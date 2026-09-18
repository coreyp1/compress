/**
 * @file deflate_encode.c
 *
 * Streaming DEFLATE (RFC 1951) encoder for the Ghoti.io Compress library.
 *
 * Implements multiple compression strategies based on level:
 * - Level 0: Stored blocks (no compression, data copied verbatim)
 * - Levels 1-3: Fixed Huffman codes with LZ77 (shorter hash chains)
 * - Levels 4-9: Dynamic Huffman codes with LZ77 (optimal code lengths from
 *   symbol frequency histograms, longer hash chains for better matching)
 *
 * The encoder maintains a sliding window for LZ77 back-references and uses
 * hash chains for efficient match finding. Dynamic Huffman blocks are built
 * by collecting symbol frequencies during LZ77 matching, then constructing
 * optimal length-limited (15-bit max) Huffman codes.
 *
 * See the "Dynamic Huffman Encoding" section below for algorithm details.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>
#include "../../core/alloc_internal.h"
#include "../../core/huffman_lengths.h"
#include "../../core/stepdown.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include "bitwriter.h"
#include "deflate_internal.h"
#include "huffman.h"
#include <ghoti.io/compress/limits.h>
#include <stdint.h>
#include <string.h>

//
// Constants (RFC 1951)
//

#define DEFLATE_WINDOW_BITS_DEFAULT 15u
#define DEFLATE_WINDOW_BITS_MIN 8u
#define DEFLATE_WINDOW_BITS_MAX 15u

#define DEFLATE_MAX_STORED_BLOCK 65535u
#define DEFLATE_MAX_LITLEN_SYMBOLS 288u
#define DEFLATE_MAX_DIST_SYMBOLS 32u
#define DEFLATE_MIN_MATCH_LENGTH 3u
#define DEFLATE_MAX_MATCH_LENGTH 258u
#define DEFLATE_MAX_DISTANCE 32768u

// Distance past which a three-byte match stops paying for itself.
#define DEFLATE_TOO_FAR 4096u

// The window holds history and lookahead in one circular buffer, so every
// byte of lookahead is a byte of history the encoder does not have.  A match
// is at most DEFLATE_MAX_MATCH_LENGTH bytes and needs three more to be worth
// looking for, so anything past that is lookahead held for no reason.
//
// Refilling in batches of DEFLATE_REFILL_LOOKAHEAD amortises the copy without
// giving up a meaningful amount of history: 1 KB out of a 32 KB window leaves
// 97% of the legal distance range reachable.
#define DEFLATE_MIN_LOOKAHEAD \
  (DEFLATE_MAX_MATCH_LENGTH + DEFLATE_MIN_MATCH_LENGTH + 1u)
#define DEFLATE_REFILL_LOOKAHEAD 1024u

// Hash chain configuration
#define DEFLATE_HASH_BITS 15u
#define DEFLATE_HASH_SIZE (1u << DEFLATE_HASH_BITS)
#define DEFLATE_HASH_MASK (DEFLATE_HASH_SIZE - 1u)
#define DEFLATE_NIL 0u

//
// Encoder state machine
//

typedef enum {
  DEFLATE_ENC_STAGE_INIT = 0,
  DEFLATE_ENC_STAGE_ACCEPTING,
  DEFLATE_ENC_STAGE_FLUSHING,
  DEFLATE_ENC_STAGE_DONE,
} gcomp_deflate_encoder_stage_t;

//
// Compression strategies
// ======================
//
// The `deflate.strategy` option controls how the encoder finds and encodes
// matches. Each strategy optimizes for different data characteristics:
//
// DEFLATE_STRATEGY_DEFAULT (strategy="default")
// ---------------------------------------------
// Standard LZ77 with hash-chain match finding, suitable for most data.
// - Uses hash chains to find repeated byte sequences in the sliding window
// - Chain length varies by compression level (8/32/64 at L1-3/L4-6/L7-9)
// - Levels 4 and up defer a match one byte to see whether the next position
//   starts a longer one; levels 1 to 3 take what they find.  This is where
//   zlib switches from deflate_fast to deflate_slow, and it is the same
//   trade: across a 19 MB corpus of source, prose, XML, binaries and images
//   it is worth 2.3% at level 6 and 2.6% at level 9, for about half the
//   encode throughput on text.
// - Chooses fixed or dynamic Huffman based on compression level
// - Good balance of speed and compression for general-purpose data
//
// DEFLATE_STRATEGY_FILTERED (strategy="filtered")
// -----------------------------------------------
// Optimized for pre-filtered data like PNG filter output.
// PNG filters (Sub, Up, Average, Paeth) produce data where:
// - Values cluster around zero (differences between adjacent pixels)
// - Short runs of the same value are common, broken by occasional outliers
// - A match at one position is often beaten by a longer one a byte later
//
// Implementation differences from DEFAULT:
// - Defers a match at every level, including 1 to 3: a match is held back one
//   byte to see whether the next position starts a longer one, and the search
//   that position performs anyway is what settles it.  See
//   deflate_find_match()'s caller.
// - Uses the deferral threshold of the levels that defer, so that deferring
//   at levels 1 to 3 is worth doing.  See max_lazy in that caller.
// - Nothing else.  It searches exactly as hard as DEFAULT: same hash chain
//   lengths, same everything.
//
// Since DEFAULT defers from level 4 up, FILTERED is identical to it at levels
// 4 to 9 and differs only at levels 1 to 3.  What it offers there is the fast
// levels' search effort with the slow levels' deferral: on 2 MB of
// filter-shaped bytes it reaches 22.428% against DEFAULT's 25.065%, and on
// 12 MB of source, prose, XML and binaries 25.954% against 26.680%, for about
// 10% of the encode throughput on general data and none of it on filtered.
//
// It used to search four times as deep as DEFAULT as well - 16/128/256
// against 4/32/128 - on the reasoning that filtered data hides longer
// patterns behind short chains.  Measured across 52 files of real PNG
// filtered rows, 7.3 MB, that is not where the win is:
//
//     chain   with deferral   without
//        32      31.22%       32.91%
//        64      31.13%       33.42%
//       128      31.13%       33.41%
//       256      31.12%       33.40%
//
// Chain length is worth 0.1 points across a factor of eight.  Deferral is
// worth 1.7.  So the chains came back down and the strategy is DEFAULT plus
// deferral, which is the same relationship zlib's levels 4-9 have to its
// levels 1-3.
//
// Note that this is still not what zlib's Z_FILTERED does.  zlib *reduces*
// effort there - it discards matches shorter than six bytes and leans on
// Huffman coding - so Z_FILTERED is faster than its default.  This one is
// slower than its default.  The name is shared; the meaning is not.  That
// rule was measured here too and is not worth having: it costs 0.3% on
// filter-shaped data and 6.1% on general data.
//
// DEFLATE_STRATEGY_HUFFMAN_ONLY (strategy="huffman_only")
// -------------------------------------------------------
// Skip LZ77 entirely; emit all input bytes as literals.
// - No hash table lookups or match searching
// - Only entropy encoding via Huffman codes
// - Extremely fast encoding, minimal compression
//
// Use cases:
// - Pre-compressed data (JPEG, PNG, ZIP contents) where LZ77 finds few matches
// - High-entropy data (random, encrypted) where searching is wasted effort
// - When encoding speed is more important than compression ratio
//
// Note: Even with huffman_only, the encoder builds optimal dynamic Huffman
// codes from literal byte frequencies (at levels 4+), providing some
// compression for non-uniform byte distributions.
//
// DEFLATE_STRATEGY_RLE (strategy="rle")
// ------------------------------------
// Run-length encoding: only find matches at distance 1.
// - No hash chains needed; just check if current byte equals previous byte
// - Very fast O(n) matching with no memory overhead
// - Good compression for data with long runs of repeated bytes
//
// Use cases:
// - Simple graphics (icons, diagrams) with solid color regions
// - Sparse data (arrays with many zeros)
// - Any data dominated by repeated byte patterns
//
// Implementation: At each position, scans forward while bytes match the
// immediately preceding byte, up to DEFLATE_MAX_MATCH_LENGTH (258).
//
// DEFLATE_STRATEGY_FIXED (strategy="fixed")
// ----------------------------------------
// Always use fixed Huffman tables, skip dynamic tree building.
// - Avoids the overhead of computing optimal Huffman codes
// - Avoids transmitting custom Huffman tree in block header
// - Faster encoding, slightly worse compression ratio
//
// Combines with any compression level:
// - Level 0 + fixed: stored blocks (fixed has no effect)
// - Level 1-9 + fixed: LZ77 matching at that level, but fixed Huffman output
//
// Useful when:
// - Encoding many small blocks where tree overhead dominates
// - Very speed-sensitive applications
// - Data that compresses similarly with fixed vs dynamic codes
//

typedef enum {
  DEFLATE_STRATEGY_DEFAULT = 0,
  DEFLATE_STRATEGY_FILTERED,
  DEFLATE_STRATEGY_HUFFMAN_ONLY,
  DEFLATE_STRATEGY_RLE,
  DEFLATE_STRATEGY_FIXED,
} gcomp_deflate_strategy_t;

/**
 * @brief LZ77 match result.
 */
typedef struct {
  uint32_t length;   ///< Match length (0 if no match found).
  uint32_t distance; ///< Match distance (1-based).
} deflate_match_t;

typedef struct gcomp_deflate_encoder_state_s {
  //
  // Allocator (for internal memory operations)
  //
  const gcomp_allocator_t * allocator;

  //
  // Configuration
  //
  int level;
  size_t window_bits;
  size_t window_size;
  /**
   * @brief window_size - 1, for wrapping indices into the circular window.
   *
   * window_size is always a power of two (it is `1 << window_bits`, and
   * RFC 1951 section 3.2.1 bounds window_bits to 8..15), so wrapping is a
   * mask rather than a division.  The compiler cannot make that rewrite on
   * its own because window_size is a runtime value, so `% window_size` had
   * been compiling to a 64-bit `div` - including two per byte compared in
   * the innermost loop of deflate_find_match().
   */
  size_t window_mask;
  gcomp_deflate_strategy_t strategy;

  //
  // Limits
  //
  uint64_t max_memory_bytes;

  //
  // Memory tracking
  //
  gcomp_memory_tracker_t mem_tracker;

  /**
   * @brief Times this encoder settled for a weaker encoding, and why.
   *
   * Read by tests, which assert that nothing was forced.  See
   * src/core/stepdown.h for why a count is needed at all.
   */
  gcomp_stepdown_tally_t stepdowns;

  /**
   * @brief Input bytes the buffered symbols stand for.
   *
   * Only used to decide when to close a block early so that a stored block
   * stays reachable; deflate_block_input_length() is what the block writer
   * trusts.  A drift here costs a block boundary, never a wrong block.
   */
  size_t block_input_len;

  //
  // State machine
  //
  gcomp_deflate_encoder_stage_t stage;
  int final_block_written;

  //
  // Sliding window buffer for LZ77
  //
  uint8_t * window;
  size_t window_pos;  ///< Next write position (circular index).
  size_t window_fill; ///< Total bytes written (capped at window_size).
  size_t lookahead;   ///< Bytes available for matching.

  /**
   * @brief A match found at the previous position and not yet emitted.
   *
   * Lazy matching asks whether the byte at p is better spent as a literal,
   * because the match starting at p+1 is longer than the one starting at p.
   * Answering it needs the search at p+1, so the match at p is held here
   * while the encoder moves on one byte; the next iteration's own search is
   * the one that settles it.  @ref lazy_length is 0 when nothing is held.
   *
   * It lives in the encoder state, not on the stack, because the batch loop
   * can return to the caller between the two positions.  Carrying it means
   * streaming a stream in small pieces produces the same bytes as encoding it
   * in one call, which is the property the previous arrangement could not
   * have offered had it kept anything at all.
   *
   * The deferred position has already been consumed from @ref lookahead and
   * entered into the hash chains, so emitting the match consumes only its
   * remaining @ref lazy_length - 1 bytes.
   */
  uint32_t lazy_length;
  uint32_t lazy_distance; ///< Distance of the held match; valid with length.
  size_t total_in;    ///< Total bytes written to window (for hash validity).

  //
  // Hash chain for LZ77 match finding
  // ==================================
  //
  // The encoder uses hash chains to efficiently find repeated byte sequences
  // in the sliding window. For each 3-byte sequence, a hash value is computed.
  // Positions with the same hash are linked together in a chain, allowing
  // quick traversal of potential match candidates.
  //
  // Data structures:
  // - hash_head[hash]: Buffer index of most recent position with this hash
  // - hash_prev[idx]:  Buffer index of previous position in same chain
  // - hash_pos[idx]:   Stream position when this entry was inserted (for
  //                    validity checking - entries older than window_size
  //                    bytes are stale)
  // - hash_at[idx]:    Hash value that was used when inserting at this buffer
  //                    index (for proactive invalidation - see below)
  //
  // HASH CHAIN INVALIDATION (Critical for correctness)
  // ---------------------------------------------------
  //
  // Because the sliding window is circular, buffer indices are reused when
  // the window wraps. This creates a subtle corruption problem:
  //
  // Consider buffer index 100:
  //   1. First use: hash("abc")=500 → hash_head[500]=100, hash_prev[100]=...
  //   2. Window wraps, index 100 now contains different data
  //   3. Second use: hash("xyz")=700 → hash_head[700]=100
  //
  // Problem: hash_head[500] still points to 100, but 100 is now in chain 700!
  // If we search chain 500, we'll follow hash_prev[100] into chain 700's
  // history, causing incorrect matches or infinite loops.
  //
  // Solution: Proactive invalidation using hash_at[]:
  //   - When inserting at buffer index idx with new hash:
  //     1. Check old_hash = hash_at[idx]
  //     2. If hash_head[old_hash] == idx (this index is head of old chain)
  //        AND old_hash != new_hash, set hash_head[old_hash] = NIL
  //     3. This ensures the old chain doesn't dangle into the wrong chain
  //
  // The check "old_hash != new_hash" is important: if we're re-inserting into
  // the same chain (same data at same position), we don't want to invalidate
  // the chain head, as that would corrupt hash_prev linkage.
  //
  uint16_t * hash_head; ///< Head of each hash chain (hash → buffer index).
  uint16_t * hash_prev; ///< Previous link in hash chain (buffer index → index).
  size_t * hash_pos;    ///< Stream position when entry was inserted.
  uint16_t * hash_at;   ///< Hash value at each buffer position (for proactive
                        ///< invalidation when buffer indices are reused).
  uint32_t hash_value; ///< Running hash value.

  //
  // Output bitstream
  //
  gcomp_deflate_bitwriter_t bitwriter;

  //
  // Block buffering for stored blocks (level 0)
  //
  uint8_t * block_buffer;
  size_t block_buffer_size;
  size_t block_buffer_used;

  //
  // Symbol buffer for Huffman encoding
  //
  uint16_t * lit_buf;  ///< Literal/length symbols.
  uint16_t * dist_buf; ///< Distance values (0 for literals).
  size_t sym_buf_size; ///< Capacity of symbol buffers.
  size_t sym_buf_used; ///< Number of symbols buffered.

  //
  // Histograms for dynamic Huffman
  //
  uint32_t * lit_freq;  ///< Literal/length frequencies.
  uint32_t * dist_freq; ///< Distance frequencies.

  //
  // Fixed Huffman codes (precomputed)
  //
  uint16_t fixed_lit_codes[DEFLATE_MAX_LITLEN_SYMBOLS];
  uint8_t fixed_lit_lens[DEFLATE_MAX_LITLEN_SYMBOLS];
  uint16_t fixed_dist_codes[DEFLATE_MAX_DIST_SYMBOLS];
  uint8_t fixed_dist_lens[DEFLATE_MAX_DIST_SYMBOLS];
  int fixed_ready;

  //
  // Finish buffer for incremental output during finish()
  //
  // When finish() is called, the entire final output is rendered to this
  // internal buffer first, then copied incrementally to the user's output
  // buffer. This allows finish() to work with arbitrarily small output
  // buffers (even 1 byte at a time) without corrupting the output stream.
  //
  uint8_t * finish_buf;     ///< Buffer holding rendered finish output.
  size_t finish_buf_size;   ///< Allocated size of finish_buf.
  size_t finish_buf_used;   ///< Bytes written to finish_buf.
  size_t finish_buf_copied; ///< Bytes already copied to user output.
  int finish_buf_ready;     ///< Non-zero if finish output is fully rendered.
  /**
   * Output staged by update() before it is handed to the caller.
   *
   * Block flushes used to be written straight into the caller's buffer, so a
   * flush that did not fit returned GCOMP_ERR_LIMIT *after* the input it came
   * from had already been consumed - an unrecoverable state rather than a
   * retryable one, which made streaming through a bounded output buffer
   * impossible. Blocks are now rendered here first and copied out as space
   * allows, exactly as finish() already did with finish_buf.
   */
  uint8_t * pending_buf;    ///< Output staged by update() before delivery.
  size_t pending_size;      ///< Allocated size of pending_buf.
  size_t pending_used;      ///< Bytes rendered into pending_buf.
  size_t pending_copied;    ///< Bytes of pending_buf already delivered.
} gcomp_deflate_encoder_state_t;

/**
 * @brief Whether deflate_find_match() may compare eight bytes at a time.
 *
 * Requires a little-endian target and __builtin_ctzll, so that the index of
 * the first differing byte in `a ^ b` is `ctz(diff) / 8`.  Everything else
 * falls back to the byte-at-a-time loop, which is what the word loop is
 * checked against.
 *
 * Define GCOMP_DEFLATE_NO_WORD_COMPARE to force the byte path on a platform
 * that would otherwise qualify; the tests build both.
 */
#if !defined(GCOMP_DEFLATE_NO_WORD_COMPARE) &&                                 \
    (defined(__GNUC__) || defined(__clang__)) &&                               \
    defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) &&             \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define GCOMP_DEFLATE_WORD_COMPARE 1
#endif

//
// Hash function for LZ77
//

static uint32_t deflate_hash_update(uint32_t h, uint8_t b) {
  // Simple multiplicative hash
  return ((h << 5u) ^ (h >> (DEFLATE_HASH_BITS - 5u)) ^ b) & DEFLATE_HASH_MASK;
}

/**
 * @brief Compute hash of 3 bytes from a circular buffer.
 *
 * Handles wrapping around the end of the circular window buffer.
 */
static uint32_t deflate_hash_3bytes_wrap(
    const uint8_t * data, size_t pos, size_t window_mask) {
  uint32_t h = 0;
  h = deflate_hash_update(h, data[pos & window_mask]);
  h = deflate_hash_update(h, data[(pos + 1) & window_mask]);
  h = deflate_hash_update(h, data[(pos + 2) & window_mask]);
  return h;
}

//
// Forward declarations
//

static gcomp_status_t deflate_build_fixed_codes(
    gcomp_deflate_encoder_state_t * st);
/**
 * @brief How many input bytes the buffered symbols stand for.
 *
 * Derived from the symbols rather than tracked alongside them, so it cannot
 * drift out of step with what was actually recorded.
 *
 * @param st Encoder state.
 * @return Total bytes the current block covers.
 */
static size_t deflate_block_input_length(
    const gcomp_deflate_encoder_state_t * st) {
  size_t total = 0;
  for (size_t i = 0; i < st->sym_buf_used; i++) {
    total += (st->dist_buf[i] != 0) ? (size_t)st->lit_buf[i] : 1u;
  }
  return total;
}

/**
 * @brief Write the block as a stored block, taking the bytes from the window.
 *
 * RFC 1951 section 3.2.4: three header bits, padding to the next byte
 * boundary, LEN and its complement, then the bytes themselves.  Nothing is
 * compressed, so this is the ceiling on what a block can cost - about five
 * bytes over its own length - and it is the answer whenever the coded forms
 * would cost more.
 *
 * The bytes come from the sliding window.  They are the @p data_len bytes
 * ending where the encoder has reached, which is @ref gcomp_deflate_encoder_state_t::lookahead
 * bytes before the end of what has been read.  The caller checks that they
 * are still in the window before asking.
 *
 * @param st Encoder state.
 * @param final Non-zero if this is the last block in the stream.
 * @param data_len Bytes to store; at most 65535.
 * @return GCOMP_OK, or a bit writer error.
 */
static gcomp_status_t deflate_flush_stored_block_from_window(
    gcomp_deflate_encoder_state_t * st, int final, size_t data_len) {
  gcomp_status_t s = gcomp_deflate_bitwriter_write_bits(
      &st->bitwriter, final ? 1u : 0u, 1);
  if (s != GCOMP_OK) {
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, 0u, 2); // BTYPE=00
  if (s != GCOMP_OK) {
    return s;
  }
  s = gcomp_deflate_bitwriter_flush_to_byte(&st->bitwriter);
  if (s != GCOMP_OK) {
    return s;
  }

  uint16_t len = (uint16_t)data_len;
  uint16_t nlen = (uint16_t)(~len);
  const uint16_t header[4] = {(uint16_t)(len & 0xFF),
      (uint16_t)((len >> 8) & 0xFF), (uint16_t)(nlen & 0xFF),
      (uint16_t)((nlen >> 8) & 0xFF)};
  for (size_t i = 0; i < 4; i++) {
    s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, header[i], 8);
    if (s != GCOMP_OK) {
      return s;
    }
  }

  // The block ends at the encoder's position, which is `lookahead` bytes
  // behind where the window has been filled to.
  size_t end = (st->window_pos + st->window_size - st->lookahead) &
      st->window_mask;
  size_t start = (end + st->window_size - data_len) & st->window_mask;
  for (size_t i = 0; i < data_len; i++) {
    s = gcomp_deflate_bitwriter_write_bits(
        &st->bitwriter, st->window[(start + i) & st->window_mask], 8);
    if (s != GCOMP_OK) {
      return s;
    }
  }

  st->sym_buf_used = 0;
  st->block_input_len = 0;
  if (st->lit_freq) {
    memset(st->lit_freq, 0, DEFLATE_MAX_LITLEN_SYMBOLS * sizeof(uint32_t));
  }
  if (st->dist_freq) {
    memset(st->dist_freq, 0, DEFLATE_MAX_DIST_SYMBOLS * sizeof(uint32_t));
  }
  return GCOMP_OK;
}

static gcomp_status_t deflate_flush_stored_block(
    gcomp_deflate_encoder_state_t * st, int final);
static gcomp_status_t deflate_flush_fixed_block(
    gcomp_deflate_encoder_state_t * st, int final);
static gcomp_status_t deflate_flush_dynamic_block(
    gcomp_deflate_encoder_state_t * st, int final);

//
// Length/Distance encoding tables (RFC 1951)
//

static const uint16_t k_len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17,
    19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t k_len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2,
    2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

static const uint16_t k_dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33,
    49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577};
static const uint8_t k_dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5,
    5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/**
 * @brief Find the length code (257..285) for a given match length (3..258).
 */
/**
 * @brief Length and distance code lookups (RFC 1951 section 3.2.5).
 *
 * These were linear scans over k_len_base and k_dist_base, run up to three
 * times per emitted match - once to count symbols and again in whichever
 * block writer runs - and together they were about 8% of the encoder's
 * instruction count.  They are lookups now.
 *
 * @ref k_len_code is indexed by `length - 3`, covering lengths 3..258.
 *
 * Distances need 32768 entries to index directly, so they are split the way
 * zlib splits them: @ref k_dist_code_low covers 1..256 by `distance - 1`,
 * and @ref k_dist_code_high covers 257..32768 by `(distance - 1) >> 7`.  The
 * high half works because every distance code from 257 up spans a whole
 * number of 128-wide buckets.
 *
 * Both tables were generated from the same k_len_base/k_dist_base the scans
 * used.  DeflateEncodeCodeTables in the test suite re-derives the scan for
 * every one of the 256 lengths and 32768 distances and checks it against the
 * table, so the tables cannot drift from the bases they came from.
 */
static const uint8_t k_len_code[256] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 12, 12, 13,
    13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 16, 16, 16,
    16, 17, 17, 17, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 18, 18, 19,
    19, 19, 19, 19, 19, 19, 19, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    20, 20, 20, 20, 20, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
    22, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    28
};

static const uint8_t k_dist_code_low[256] = {
    0, 1, 2, 3, 4, 4, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 9,
    9, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 13, 13, 13,
    13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
    13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15
};

static const uint8_t k_dist_code_high[256] = {
    0, 14, 16, 17, 18, 18, 19, 19, 20, 20, 20, 20, 21, 21, 21, 21, 22, 22, 22,
    22, 22, 22, 22, 22, 23, 23, 23, 23, 23, 23, 23, 23, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    27, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29
};

uint32_t gcomp_deflate_length_code(uint32_t length) {
  if (length < 3 || length > 258) {
    return 0; // Invalid
  }
  return 257u + k_len_code[length - 3u];
}

/**
 * @brief Find the distance code (0..29) for a given distance (1..32768).
 */
uint32_t gcomp_deflate_distance_code(uint32_t distance) {
  if (distance < 1 || distance > 32768) {
    return 0; // Invalid
  }
  return (distance <= 256u) ? k_dist_code_low[distance - 1u]
                            : k_dist_code_high[(distance - 1u) >> 7u];
}

//
// Fixed Huffman codes (RFC 1951, Section 3.2.6)
//

static uint16_t reverse_code(uint16_t code, uint32_t bits) {
  uint16_t r = 0;
  for (uint32_t i = 0; i < bits; i++) {
    r = (uint16_t)((r << 1) | (code & 1));
    code >>= 1;
  }
  return r;
}

static gcomp_status_t deflate_build_fixed_codes(
    gcomp_deflate_encoder_state_t * st) {
  if (!st) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Build fixed literal/length code lengths per RFC 1951
  uint8_t lit_lengths[DEFLATE_MAX_LITLEN_SYMBOLS];
  for (int i = 0; i <= 143; i++) {
    lit_lengths[i] = 8;
  }
  for (int i = 144; i <= 255; i++) {
    lit_lengths[i] = 9;
  }
  for (int i = 256; i <= 279; i++) {
    lit_lengths[i] = 7;
  }
  for (int i = 280; i <= 287; i++) {
    lit_lengths[i] = 8;
  }

  // Build canonical codes
  gcomp_status_t status = gcomp_deflate_huffman_build_codes(lit_lengths,
      DEFLATE_MAX_LITLEN_SYMBOLS, 15, st->fixed_lit_codes, st->fixed_lit_lens);
  if (status != GCOMP_OK) {
    return status;
  }

  // Reverse codes for LSB-first output
  for (int i = 0; i < (int)DEFLATE_MAX_LITLEN_SYMBOLS; i++) {
    if (st->fixed_lit_lens[i] > 0) {
      st->fixed_lit_codes[i] =
          reverse_code(st->fixed_lit_codes[i], st->fixed_lit_lens[i]);
    }
  }

  // Build fixed distance codes (all 5-bit codes)
  uint8_t dist_lengths[DEFLATE_MAX_DIST_SYMBOLS];
  for (int i = 0; i < (int)DEFLATE_MAX_DIST_SYMBOLS; i++) {
    dist_lengths[i] = 5;
  }

  status = gcomp_deflate_huffman_build_codes(dist_lengths,
      DEFLATE_MAX_DIST_SYMBOLS, 15, st->fixed_dist_codes, st->fixed_dist_lens);
  if (status != GCOMP_OK) {
    return status;
  }

  // Reverse codes for LSB-first output
  for (int i = 0; i < (int)DEFLATE_MAX_DIST_SYMBOLS; i++) {
    if (st->fixed_dist_lens[i] > 0) {
      st->fixed_dist_codes[i] =
          reverse_code(st->fixed_dist_codes[i], st->fixed_dist_lens[i]);
    }
  }

  st->fixed_ready = 1;
  return GCOMP_OK;
}

/*
 * ===========================================================================
 * LZ77 Match Finding with Hash Chain
 * ===========================================================================
 *
 * DEFLATE uses LZ77 compression: replace repeated byte sequences with
 * (length, distance) pairs that reference earlier occurrences.
 *
 * Data Structures
 * ---------------
 * - window[window_size]: Circular buffer holding recent input bytes.
 * - window_pos: Next write position (wraps at window_size).
 * - total_in: Total bytes ever written to window (monotonically increasing).
 * - lookahead: Bytes in window not yet encoded.
 *
 * Hash Chain
 * ----------
 * To find matches efficiently, we maintain a hash chain:
 *
 * - hash_head[HASH_SIZE]: For each hash value, the buffer index of the most
 *   recent position that hashed to that value.
 *
 * - hash_prev[window_size]: Linked list of previous positions with the same
 *   hash. hash_prev[i] points to the previous position that had the same
 *   3-byte hash as position i.
 *
 * - hash_pos[window_size]: Stream position when each buffer index was last
 *   inserted. This is CRITICAL for validity checking (see below).
 *
 * The Circular Buffer Problem
 * ---------------------------
 * The window is circular: after writing to index (window_size-1), we wrap
 * to index 0 and overwrite old data. This creates a subtle bug:
 *
 *   1. At stream position 100, we write byte 'A' to window[100].
 *   2. We insert hash chain entry: hash_head[h] = 100.
 *   3. Later, at stream position 32868 (= 32768 + 100), we write byte 'X'
 *      to window[100] (same index due to wrap).
 *   4. The old hash entry still says "look at index 100" but that now
 *      contains 'X', not 'A'!
 *
 * Solution: Stream Position Tracking
 * ----------------------------------
 * When inserting, we record hash_pos[idx] = stream_pos (the true position).
 * When matching, we check:
 *
 *   stream_dist = current_stream_pos - hash_pos[match_idx]
 *   buf_dist = (current_buf_idx - match_idx + window_size) % window_size
 *
 *   if (stream_dist != buf_dist):
 *       The entry is stale (buffer wrapped) - skip it
 *
 * This ensures we only use matches that are actually valid in the current
 * window contents.
 *
 * ===========================================================================
 */

/**
 * @brief Find the best match for the current position.
 *
 * Walks the hash chain for the current 3-byte sequence, checking each
 * candidate against the actual window contents. Uses stream position
 * tracking to skip stale entries from before the circular buffer wrapped.
 *
 * @param st Encoder state
 * @param pos Position in circular window buffer
 * @param stream_pos Current position in the total input stream
 * @param max_chain Maximum hash chain length to search
 * @return Match with length >= 3, or length == 0 if no match found
 */
static deflate_match_t deflate_find_match(gcomp_deflate_encoder_state_t * st,
    size_t pos, size_t stream_pos, int max_chain) {
  deflate_match_t result = {0, 0};

  if (!st || !st->window || st->lookahead < DEFLATE_MIN_MATCH_LENGTH) {
    return result;
  }

  size_t scan = pos & st->window_mask;
  const uint8_t * data = st->window;
  size_t max_len = st->lookahead;
  if (max_len > DEFLATE_MAX_MATCH_LENGTH) {
    max_len = DEFLATE_MAX_MATCH_LENGTH;
  }

  uint32_t hash = deflate_hash_3bytes_wrap(data, scan, st->window_mask);
  uint16_t cur = st->hash_head[hash];
  int chain_count = 0;

  // The byte one past the end of the best match so far, which any candidate
  // that is going to beat it has to match.  Kept here rather than reloaded per
  // candidate: it only changes when result.length does.
  uint8_t probe_byte = 0;

  // A chain is built by prepending, so following it must walk strictly
  // backwards through the stream.  When it stops doing that the chain has
  // left this hash's history: a window slot is reused every window_size
  // bytes, and a slot reused for a different hash still has the hash_prev
  // link from its previous life, so a walk that reaches it continues into
  // some other hash's older entries.
  //
  // Those entries are not candidates - they were stored because different
  // bytes hashed to a different bucket - so the walk was doing nothing but
  // reading memory at random.  On incompressible data it did that all the way
  // to max_chain on every position: the mean walk at level 9 was 106.9 of a
  // possible 128, where a clean chain over a 32 KB window with 32768 buckets
  // is about one.  Stopping at the break takes it to 2.8, and the encoder's
  // output does not change - on 2 MB of random bytes, 4.7 MB of C source and
  // an XML registry it is byte for byte identical, because a candidate from
  // the wrong bucket had no reason to match anyway.
  size_t chain_bound = stream_pos;
  while (cur != DEFLATE_NIL && chain_count < max_chain) {
    size_t match_idx = cur;

    // Check if this hash entry is still valid (not overwritten in circular
    // buf), and that the chain is still going backwards.
    size_t match_stream_pos = st->hash_pos[match_idx];
    if (match_stream_pos >= chain_bound) {
      break;
    }
    chain_bound = match_stream_pos;

    // Then the cheapest rejection.  Only a candidate that matches at the byte
    // just past the end of the best match so far can beat it, and one load
    // settles that.
    //
    // This cannot change which match is chosen: a candidate it rejects has
    // length <= result.length, and the code below only takes one whose length
    // is strictly greater.  A stale entry that survives the probe is still
    // caught by the checks that follow.
    //
    // result.length < max_len always holds here, because the loop breaks as
    // soon as a match reaches max_len, so `scan + result.length` is still
    // inside the lookahead.
    if (result.length >= DEFLATE_MIN_MATCH_LENGTH &&
        data[(match_idx + result.length) & st->window_mask] != probe_byte) {
      cur = st->hash_prev[cur];
      chain_count++;
      continue;
    }

    size_t stream_dist = stream_pos - match_stream_pos;

    // Staleness must be measured against total_in, not stream_pos.  The
    // window is filled ahead of the encoding position, so window[idx] holds
    // the most recent byte written there - the largest position <= total_in-1
    // congruent to idx.  hash_pos[idx] still names the byte that *was* there
    // when the entry was inserted.  The two agree only while
    // total_in - hash_pos[idx] <= window_size; beyond that the refill has
    // already overwritten those bytes with lookahead data.
    //
    // Using stream_pos here left a blind spot exactly `lookahead` bytes wide:
    // entries whose data had been overwritten still passed, and the buf_dist
    // check below could not catch them (buf_dist is stream_dist modulo
    // window_size, so it agrees for any distance under one window).  The
    // encoder then matched against future data and emitted a distance that
    // decodes to the wrong bytes.
    if (stream_dist > DEFLATE_MAX_DISTANCE ||
        st->total_in - match_stream_pos > st->window_size) {
      // Entry has been overwritten or is too far back - skip
      cur = st->hash_prev[cur];
      chain_count++;
      continue;
    }

    // Calculate circular buffer distance for byte comparisons
    size_t buf_dist = (scan >= match_idx)
        ? (scan - match_idx)
        : (st->window_size - match_idx + scan);

    // The stream distance should equal the buffer distance for valid matches
    if (buf_dist != stream_dist) {
      cur = st->hash_prev[cur];
      chain_count++;
      continue;
    }

    // Check match length.
    size_t len = 0;

#ifdef GCOMP_DEFLATE_WORD_COMPARE
    // Both cursors run forward from scan and match_idx until one of them
    // reaches the end of the circular window and wraps.  Up to that point the
    // two runs are flat, so eight bytes can be compared at once; the loop
    // below finishes the tail and handles the wrap.
    //
    // The bound keeps the loads in bounds without a separate check:
    // len + 8 <= flat <= window_size - scan means scan + len + 8 <=
    // window_size, and the same for match_idx.  A match is at most 258 bytes
    // (RFC 1951 section 3.2.5) against a window of at least 256, so a wrap is
    // rare and the tail loop is short.
    {
      size_t scan_room = st->window_size - scan;
      size_t match_room = st->window_size - match_idx;
      size_t flat = scan_room < match_room ? scan_room : match_room;
      if (flat > max_len) {
        flat = max_len;
      }
      while (len + 8u <= flat) {
        uint64_t a;
        uint64_t b;
        memcpy(&a, data + scan + len, sizeof(a));
        memcpy(&b, data + match_idx + len, sizeof(b));
        uint64_t diff = a ^ b;
        if (diff) {
          // Little-endian: the lowest set bit sits in the first byte that
          // differs.  Land on it and let the byte loop below stop there.
          len += (size_t)(__builtin_ctzll(diff) / 8u);
          break;
        }
        len += 8u;
      }
    }
#endif

    while (len < max_len &&
        data[(scan + len) & st->window_mask] ==
            data[(match_idx + len) & st->window_mask]) {
      len++;
    }

    if (len >= DEFLATE_MIN_MATCH_LENGTH && len > result.length) {
      result.length = (uint32_t)len;
      result.distance = (uint32_t)stream_dist;
      probe_byte = data[(scan + len) & st->window_mask];

      if (len >= max_len) {
        break; // Max length found
      }
    }

    cur = st->hash_prev[cur];
    chain_count++;
  }

  return result;
}

/**
 * @brief Insert a position into the hash chain.
 *
 * Updates the hash chain so that future searches can find this position.
 * Records both the buffer index (in hash_head/hash_prev) and the stream
 * position (in hash_pos) so that stale entries can be detected after the
 * circular buffer wraps.
 *
 * ## Proactive Hash Chain Invalidation
 *
 * This function implements proactive invalidation to prevent hash chain
 * corruption when buffer indices are reused. The problem and solution:
 *
 * **Problem**: When the circular window buffer wraps, a buffer index that
 * was previously part of hash chain A may be reused for data that hashes
 * to chain B. If hash_head[A] still points to this index, searches on
 * chain A will incorrectly follow hash_prev into chain B's history.
 *
 * **Solution**: Before inserting at index `idx` with hash `new_hash`:
 * 1. Look up `old_hash = hash_at[idx]` (the hash from the previous insert)
 * 2. If `hash_head[old_hash] == idx` AND `old_hash != new_hash`:
 *    - This index is still the head of the old chain
 *    - Set `hash_head[old_hash] = hash_prev[idx]`, which unlinks this index
 *      from the old chain and leaves the rest of it intact
 * 3. The condition `old_hash != new_hash` is critical: if we're reinserting
 *    into the same chain, invalidating would corrupt hash_prev linkage.
 *
 * This proactive approach ensures hash chains are always clean, avoiding
 * the need for expensive validation during match searches.
 *
 * @param st Encoder state
 * @param pos Position in circular window buffer
 * @param stream_pos Position in the total input stream (for validity checking)
 */
static void deflate_insert_hash(
    gcomp_deflate_encoder_state_t * st, size_t pos, size_t stream_pos) {
  if (!st || !st->window || st->lookahead < 3) {
    return;
  }

  size_t idx = pos & st->window_mask;
  uint32_t hash = deflate_hash_3bytes_wrap(st->window, idx, st->window_mask);

  // Proactive invalidation: If this buffer index was previously the head of
  // a different hash chain, clear that chain head to prevent corruption.
  // See function documentation above for detailed explanation.
  uint16_t old_hash = st->hash_at[idx];
  if (old_hash != hash && st->hash_head[old_hash] == idx) {
    // Splice this index out of its old chain rather than discarding the
    // chain.  hash_prev[idx] still names the entry that was behind it, and
    // those entries are older positions in the same window - exactly the
    // candidates a search on old_hash wants.  Setting the head to NIL threw
    // all of them away.
    //
    // Measured, this changes nothing: the case fires on 0% to 5% of inserts
    // depending on the data, and the corpus came out byte for byte identical
    // either way.  It is here because keeping the older entries is free and
    // discarding them is not defensible, not because it was costing anything
    // that could be found.
    st->hash_head[old_hash] = st->hash_prev[idx];
  }

  // Standard hash chain insertion: prepend to chain, record metadata
  st->hash_prev[idx] = st->hash_head[hash];
  st->hash_head[hash] = (uint16_t)idx;
  st->hash_pos[idx] = stream_pos;
  st->hash_at[idx] = (uint16_t)hash;
}

//
// Block Flushing
//

static gcomp_status_t deflate_flush_stored_block(
    gcomp_deflate_encoder_state_t * st, int final) {
  if (!st) {
    return GCOMP_ERR_INVALID_ARG;
  }

  size_t data_len = st->block_buffer_used;
  if (data_len > DEFLATE_MAX_STORED_BLOCK) {
    data_len = DEFLATE_MAX_STORED_BLOCK;
  }

  gcomp_status_t s;

  // Write block header: BFINAL (1 bit), BTYPE=00 (2 bits)
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, final ? 1u : 0u, 1);
  if (s != GCOMP_OK) {
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, 0u, 2); // BTYPE=00
  if (s != GCOMP_OK) {
    return s;
  }

  // Align to byte boundary
  s = gcomp_deflate_bitwriter_flush_to_byte(&st->bitwriter);
  if (s != GCOMP_OK) {
    return s;
  }

  // Write LEN and NLEN
  uint16_t len = (uint16_t)data_len;
  uint16_t nlen = (uint16_t)(~len);

  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, len & 0xFF, 8);
  if (s != GCOMP_OK) {
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, (len >> 8) & 0xFF, 8);
  if (s != GCOMP_OK) {
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, nlen & 0xFF, 8);
  if (s != GCOMP_OK) {
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, (nlen >> 8) & 0xFF, 8);
  if (s != GCOMP_OK) {
    return s;
  }

  // Write data bytes
  for (size_t i = 0; i < data_len; i++) {
    s = gcomp_deflate_bitwriter_write_bits(
        &st->bitwriter, st->block_buffer[i], 8);
    if (s != GCOMP_OK) {
      return s;
    }
  }

  // Remove written data from buffer
  if (data_len < st->block_buffer_used) {
    memmove(st->block_buffer, st->block_buffer + data_len,
        st->block_buffer_used - data_len);
  }
  st->block_buffer_used -= data_len;

  return GCOMP_OK;
}

static gcomp_status_t deflate_write_symbol(
    gcomp_deflate_encoder_state_t * st, uint16_t code, uint8_t len) {
  return gcomp_deflate_bitwriter_write_bits(&st->bitwriter, code, len);
}

static gcomp_status_t deflate_flush_fixed_block(
    gcomp_deflate_encoder_state_t * st, int final) {
  if (!st || !st->fixed_ready) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t s;

  // Write block header: BFINAL (1 bit), BTYPE=01 (2 bits) = fixed Huffman
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, final ? 1u : 0u, 1);
  if (s != GCOMP_OK) {
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, 1u, 2); // BTYPE=01
  if (s != GCOMP_OK) {
    return s;
  }

  // Write all buffered symbols
  for (size_t i = 0; i < st->sym_buf_used; i++) {
    uint16_t lit = st->lit_buf[i];
    uint16_t dist = st->dist_buf[i];

    if (dist == 0) {
      // Literal byte
      s = deflate_write_symbol(
          st, st->fixed_lit_codes[lit], st->fixed_lit_lens[lit]);
      if (s != GCOMP_OK) {
        return s;
      }
    }
    else {
      // Length/distance pair
      // lit contains the length (3..258)
      uint32_t len_code = gcomp_deflate_length_code(lit);
      uint32_t len_sym = len_code - 257;

      s = deflate_write_symbol(
          st, st->fixed_lit_codes[len_code], st->fixed_lit_lens[len_code]);
      if (s != GCOMP_OK) {
        return s;
      }

      // Write length extra bits
      if (k_len_extra[len_sym] > 0) {
        uint32_t extra = lit - k_len_base[len_sym];
        s = gcomp_deflate_bitwriter_write_bits(
            &st->bitwriter, extra, k_len_extra[len_sym]);
        if (s != GCOMP_OK) {
          return s;
        }
      }

      // Write distance code
      uint32_t dist_code = gcomp_deflate_distance_code(dist);
      s = deflate_write_symbol(
          st, st->fixed_dist_codes[dist_code], st->fixed_dist_lens[dist_code]);
      if (s != GCOMP_OK) {
        return s;
      }

      // Write distance extra bits
      if (k_dist_extra[dist_code] > 0) {
        uint32_t extra = dist - k_dist_base[dist_code];
        s = gcomp_deflate_bitwriter_write_bits(
            &st->bitwriter, extra, k_dist_extra[dist_code]);
        if (s != GCOMP_OK) {
          return s;
        }
      }
    }
  }

  // Write end-of-block symbol (256)
  s = deflate_write_symbol(
      st, st->fixed_lit_codes[256], st->fixed_lit_lens[256]);
  if (s != GCOMP_OK) {
    return s;
  }

  // Reset for next block
  st->sym_buf_used = 0;
  st->block_input_len = 0;
  if (st->lit_freq) {
    memset(st->lit_freq, 0, DEFLATE_MAX_LITLEN_SYMBOLS * sizeof(uint32_t));
  }
  if (st->dist_freq) {
    memset(st->dist_freq, 0, DEFLATE_MAX_DIST_SYMBOLS * sizeof(uint32_t));
  }
  return GCOMP_OK;
}

/*
 * ===========================================================================
 * Dynamic Huffman Encoding (RFC 1951 Section 3.2.7)
 * ===========================================================================
 *
 * OVERVIEW
 * --------
 * For compression levels 4-9, we build optimal Huffman codes based on actual
 * symbol frequencies observed in the data. Dynamic Huffman typically achieves
 * better compression than fixed Huffman because the code lengths are tailored
 * to the specific input data's statistical properties.
 *
 * ALGORITHM STEPS
 * ---------------
 * 1. FREQUENCY COLLECTION (during LZ77 matching in encoder_update):
 *    - lit_freq[0..255]: Count of each literal byte
 *    - lit_freq[257..285]: Count of each length code (match lengths 3-258)
 *    - dist_freq[0..29]: Count of each distance code (distances 1-32768)
 *    - lit_freq[256] is incremented for the end-of-block marker
 *
 * 2. CODE LENGTH CONSTRUCTION (build_code_lengths):
 *    - Hand the frequencies to the shared package-merge implementation in
 *      src/core/huffman_lengths.c
 *    - It returns the code lengths that encode these frequencies in the
 *      fewest bits of any code whose longest code word fits the cap - 15 bits
 *      for the literal/length and distance alphabets, 7 for the code-length
 *      alphabet - and that always describe a complete code
 *
 * 3. (no separate limiting step)
 *    - The cap is part of what package-merge solves, so there is no clamp to
 *      repair afterwards.  Deciding the cap and the code separately is what
 *      this encoder used to do, and it cost bits: a clamp cannot see which
 *      lengthening elsewhere is cheapest, so it spent the code space in the
 *      wrong place
 *
 * 4. CODE LENGTH ENCODING (RFC 1951 Section 3.2.7):
 *    - Combine literal/length and distance code lengths into one sequence
 *    - Run-length encode using the code-length alphabet:
 *      * 0-15: Literal code length values
 *      * 16: Copy previous code length 3-6 times (2 extra bits)
 *      * 17: Repeat code length 0 for 3-10 times (3 extra bits)
 *      * 18: Repeat code length 0 for 11-138 times (7 extra bits)
 *    - Build a Huffman tree for the code-length symbols themselves
 *
 * 5. BLOCK HEADER WRITING:
 *    - BFINAL (1 bit): 1 if this is the final block
 *    - BTYPE (2 bits): 10 binary = dynamic Huffman
 *    - HLIT (5 bits): Number of literal/length codes - 257 (range 0-29)
 *    - HDIST (5 bits): Number of distance codes - 1 (range 0-29)
 *    - HCLEN (4 bits): Number of code-length codes - 4 (range 0-15)
 *    - Code-length code lengths (3 bits each, in permuted order k_cl_order)
 *    - Encoded code lengths for literal/length alphabet
 *    - Encoded code lengths for distance alphabet
 *
 * 6. DATA ENCODING:
 *    - Emit each buffered symbol using its dynamic Huffman code
 *    - End with end-of-block symbol (256)
 *
 * DESIGN RATIONALE
 * ----------------
 * - Package-merge is O(n * max_bits) and is exact under the cap; the 15-bit
 *   limit is mandated by RFC 1951 section 3.2.7
 * - Fallback to fixed Huffman occurs on memory allocation failure
 * - Empty distance trees are valid when no LZ77 matches are used (e.g.,
 *   incompressible data or very short inputs)
 *
 * MEMORY LAYOUT
 * -------------
 * - lit_freq: 288 uint32_t entries (allocated for levels > 3)
 * - dist_freq: 32 uint32_t entries (allocated for levels > 3)
 * - Temporary allocations in build_code_lengths: 2 * used * max_bits chain
 *   records, where `used` counts the symbols that actually occur
 *
 * ===========================================================================
 */

/**
 * @brief Build code lengths for an alphabet, none longer than @p max_bits.
 *
 * Thin wrapper over the shared package-merge implementation in
 * src/core/huffman_lengths.c, which is where the algorithm and the reason
 * for it are documented.  The lengths it returns are optimal under the cap
 * and always describe a complete code.
 *
 * @param alloc Allocator for scratch memory.
 * @param freq Frequency of each symbol.
 * @param num_symbols Size of the alphabet.
 * @param lengths Receives one length per symbol.
 * @param max_bits Longest code word allowed: 15 for the literal/length and
 *        distance alphabets, 7 for the code-length alphabet (RFC 1951
 *        sections 3.2.2 and 3.2.7).
 * @return GCOMP_OK, or GCOMP_ERR_MEMORY when scratch space cannot be
 *         allocated.  A caller that cannot proceed without lengths emits a
 *         fixed-Huffman block instead, which needs none.
 */
static gcomp_status_t build_code_lengths(const gcomp_allocator_t * alloc,
    const uint32_t * freq, size_t num_symbols, uint8_t * lengths,
    unsigned max_bits) {
  return gcomp_huffman_code_lengths(
      alloc, freq, num_symbols, max_bits, lengths);
}

/**
 * Code length alphabet transmission order (RFC 1951 Section 3.2.7).
 *
 * The code lengths for the code-length alphabet are transmitted in this
 * permuted order to maximize trailing zeros (which can be omitted via HCLEN).
 * The most commonly used code-length symbols (0, 17, 18 for runs of zeros,
 * and small literal lengths like 1-4) appear at positions that allow HCLEN
 * to exclude the rarely-used symbols at the end.
 */
static const uint8_t k_cl_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/**
 * @brief Run-length encode code lengths using the code-length alphabet.
 *
 * Compresses a sequence of code lengths by encoding runs:
 *   0-15: Literal code length value (no extra bits)
 *   16:   Copy previous code length 3-6 times (2 extra bits: 0-3)
 *   17:   Repeat code length 0 for 3-10 times (3 extra bits: 0-7)
 *   18:   Repeat code length 0 for 11-138 times (7 extra bits: 0-127)
 *
 * @param all_lengths   Array of code lengths to encode
 * @param total_codes   Number of code lengths in the array
 * @param cl_symbols    Output: symbols (0-18) to emit
 * @param cl_extra      Output: extra bits for each symbol
 * @return Number of symbols written to cl_symbols/cl_extra
 */
static size_t deflate_rle_encode_lengths(const uint8_t * all_lengths,
    size_t total_codes, uint8_t * cl_symbols, uint8_t * cl_extra) {
  size_t cl_count = 0;
  size_t i = 0;

  while (i < total_codes) {
    uint8_t len = all_lengths[i];

    // Count consecutive occurrences of this length
    size_t run = 1;
    while (i + run < total_codes && all_lengths[i + run] == len) {
      run++;
    }

    if (len == 0) {
      // Encode run of zeros
      while (run > 0) {
        if (run >= 11) {
          size_t emit = (run > 138) ? 138 : run;
          cl_symbols[cl_count] = 18;
          cl_extra[cl_count] = (uint8_t)(emit - 11);
          cl_count++;
          run -= emit;
          i += emit;
        }
        else if (run >= 3) {
          cl_symbols[cl_count] = 17;
          cl_extra[cl_count] = (uint8_t)(run - 3);
          cl_count++;
          i += run;
          run = 0;
        }
        else {
          cl_symbols[cl_count] = 0;
          cl_extra[cl_count] = 0;
          cl_count++;
          run--;
          i++;
        }
      }
    }
    else {
      // Non-zero length: emit it first
      cl_symbols[cl_count] = len;
      cl_extra[cl_count] = 0;
      cl_count++;
      i++;
      run--;

      // Then encode repeats using symbol 16
      while (run >= 3) {
        size_t emit = (run > 6) ? 6 : run;
        cl_symbols[cl_count] = 16;
        cl_extra[cl_count] = (uint8_t)(emit - 3);
        cl_count++;
        run -= emit;
        i += emit;
      }

      // Remaining repeats (0-2) will be handled in next iteration
    }
  }

  return cl_count;
}

/**
 * @brief Ensure code-length alphabet is complete for zlib compatibility.
 *
 * RFC 1951 allows incomplete Huffman trees (Kraft sum < 2^max_bits), but
 * zlib's inflate_table() function rejects incomplete trees for the CODES
 * type (code-length alphabet). This is stricter than the RFC requires.
 *
 * Background: The Kraft inequality states that for a valid prefix code,
 * sum(2^(-length_i)) <= 1. An "under-subscribed" or "incomplete" tree has
 * sum < 1, meaning some bit patterns don't decode to any symbol.
 *
 * Solution: If the code-length alphabet is under-subscribed, add unused
 * symbols with appropriate code lengths to make the Kraft sum exactly 2^7.
 * We prefer symbols late in k_cl_order[] to minimize HCLEN.
 *
 * @param cl_lengths  Code lengths array (19 elements), modified in place
 */
/**
 * @brief Write all buffered symbols using dynamic Huffman codes.
 *
 * Writes literals and length/distance pairs from the symbol buffer using
 * the provided Huffman codes. Also writes the end-of-block symbol.
 *
 * @param st           Encoder state with symbol buffer and bitwriter
 * @param lit_codes    Huffman codes for literal/length symbols
 * @param lit_lengths  Code lengths for literal/length symbols
 * @param dist_codes   Huffman codes for distance symbols
 * @param dist_lengths Code lengths for distance symbols
 * @return GCOMP_OK on success, error code on failure
 */
static gcomp_status_t deflate_write_dynamic_block_data(
    gcomp_deflate_encoder_state_t * st, const uint16_t * lit_codes,
    const uint8_t * lit_lengths, const uint16_t * dist_codes,
    const uint8_t * dist_lengths) {
  gcomp_status_t s;

  // Write all buffered symbols using the dynamic codes
  for (size_t i = 0; i < st->sym_buf_used; i++) {
    uint16_t lit = st->lit_buf[i];
    uint16_t dist = st->dist_buf[i];

    if (dist == 0) {
      // Literal byte
      s = gcomp_deflate_bitwriter_write_bits(
          &st->bitwriter, lit_codes[lit], lit_lengths[lit]);
      if (s != GCOMP_OK) {
        return s;
      }
    }
    else {
      // Length/distance pair
      uint32_t len_code = gcomp_deflate_length_code(lit);
      uint32_t len_sym = len_code - 257;

      s = gcomp_deflate_bitwriter_write_bits(
          &st->bitwriter, lit_codes[len_code], lit_lengths[len_code]);
      if (s != GCOMP_OK) {
        return s;
      }

      // Write length extra bits
      if (k_len_extra[len_sym] > 0) {
        uint32_t extra = lit - k_len_base[len_sym];
        s = gcomp_deflate_bitwriter_write_bits(
            &st->bitwriter, extra, k_len_extra[len_sym]);
        if (s != GCOMP_OK) {
          return s;
        }
      }

      // Write distance code
      uint32_t dist_code = gcomp_deflate_distance_code(dist);
      s = gcomp_deflate_bitwriter_write_bits(
          &st->bitwriter, dist_codes[dist_code], dist_lengths[dist_code]);
      if (s != GCOMP_OK) {
        return s;
      }

      // Write distance extra bits
      if (k_dist_extra[dist_code] > 0) {
        uint32_t extra = dist - k_dist_base[dist_code];
        s = gcomp_deflate_bitwriter_write_bits(
            &st->bitwriter, extra, k_dist_extra[dist_code]);
        if (s != GCOMP_OK) {
          return s;
        }
      }
    }
  }

  // Write end-of-block symbol (256)
  s = gcomp_deflate_bitwriter_write_bits(
      &st->bitwriter, lit_codes[256], lit_lengths[256]);
  if (s != GCOMP_OK) {
    return s;
  }

  return GCOMP_OK;
}

static void deflate_ensure_cl_kraft_complete(uint8_t * cl_lengths) {
  // Calculate current Kraft sum (scaled by 2^7 = 128)
  uint32_t kraft = 0;
  for (int i = 0; i < 19; i++) {
    if (cl_lengths[i] > 0) {
      kraft += 1u << (7 - cl_lengths[i]);
    }
  }

  // If under-subscribed (kraft < 128), fill remaining space
  if (kraft < 128) {
    uint32_t remaining = 128 - kraft;

    // Find unused symbols to fill the space
    // Prefer symbols that come late in k_cl_order (to minimize HCLEN)
    // Symbols at positions 15-18 in k_cl_order are: 2, 14, 1, 15
    while (remaining > 0) {
      int best_ord = -1;
      int best_len = 0;
      uint32_t best_contrib = 0;

      // Find the best unused symbol and length that fits
      for (int ord = 18; ord >= 0; ord--) {
        uint8_t sym = k_cl_order[ord];
        if (cl_lengths[sym] == 0) {
          for (int len = 7; len >= 1; len--) {
            uint32_t contribution = 1u << (7 - len);
            if (contribution <= remaining && contribution > best_contrib) {
              best_ord = ord;
              best_len = len;
              best_contrib = contribution;
            }
          }
        }
      }

      if (best_ord < 0) {
        // No suitable unused symbol found - should not happen in practice
        break;
      }

      uint8_t sym = k_cl_order[best_ord];
      cl_lengths[sym] = (uint8_t)best_len;
      remaining -= best_contrib;
    }
  }
}

/**
 * @brief Write a complete dynamic Huffman block.
 *
 * This function performs the following steps:
 * 1. Builds optimal code lengths from the frequency histograms
 * 2. Calculates HLIT, HDIST (number of codes to transmit)
 * 3. Run-length encodes the code lengths using symbols 16, 17, 18
 * 4. Builds a Huffman tree for the code-length alphabet
 * 5. Writes the complete block header (BFINAL, BTYPE, HLIT, HDIST, HCLEN)
 * 6. Writes the code-length code lengths in permuted order
 * 7. Writes the encoded literal/length and distance code lengths
 * 8. Writes all buffered symbols using the dynamic Huffman codes
 * 9. Writes the end-of-block symbol (256)
 *
 * Falls back to fixed Huffman on memory allocation failure or if no
 * frequency data is available.
 *
 * @param st    Encoder state with frequency histograms and symbol buffer
 * @param final Non-zero if this is the final block (sets BFINAL bit)
 * @return GCOMP_OK on success, error code on failure
 */
static gcomp_status_t deflate_flush_dynamic_block(
    gcomp_deflate_encoder_state_t * st, int final) {
  if (!st || !st->lit_freq) {
    // No frequency histograms means no code to build from, which only
    // happens if they could not be allocated.
    gcomp_stepdown_note(st ? &st->stepdowns : NULL, GCOMP_STEPDOWN_NO_MEMORY);
    return deflate_flush_fixed_block(st, final);
  }

  gcomp_status_t s;

  // Ensure end-of-block symbol is counted
  st->lit_freq[256]++;

  // Build code lengths for literal/length alphabet
  uint8_t lit_lengths[DEFLATE_MAX_LITLEN_SYMBOLS];
  s = build_code_lengths(
      st->allocator, st->lit_freq, DEFLATE_MAX_LITLEN_SYMBOLS, lit_lengths, 15);
  if (s != GCOMP_OK) {
    // Without lengths there is no dynamic block to write.  The fixed code is
    // defined by RFC 1951 section 3.2.6 and needs no scratch memory, so it
    // remains available when this does not.
    gcomp_stepdown_note(&st->stepdowns, GCOMP_STEPDOWN_NO_MEMORY);
    st->lit_freq[256]--;
    return deflate_flush_fixed_block(st, final);
  }

  // Ensure end-of-block (256) has a code
  if (lit_lengths[256] == 0) {
    lit_lengths[256] = 1;
  }

  // Build code lengths for distance alphabet
  uint8_t dist_lengths[DEFLATE_MAX_DIST_SYMBOLS];
  s = build_code_lengths(
      st->allocator, st->dist_freq, DEFLATE_MAX_DIST_SYMBOLS, dist_lengths, 15);
  if (s != GCOMP_OK) {
    gcomp_stepdown_note(&st->stepdowns, GCOMP_STEPDOWN_NO_MEMORY);
    st->lit_freq[256]--;
    return deflate_flush_fixed_block(st, final);
  }

  // Determine HLIT (number of literal/length codes - 257)
  int hlit = DEFLATE_MAX_LITLEN_SYMBOLS - 257;
  while (hlit > 0 && lit_lengths[256 + hlit] == 0) {
    hlit--;
  }

  // Determine HDIST (number of distance codes - 1)
  int hdist = DEFLATE_MAX_DIST_SYMBOLS - 1;
  while (hdist > 0 && dist_lengths[hdist] == 0) {
    hdist--;
  }

  // A block that uses no distances at all still has to declare a distance
  // code, and RFC 1951 section 3.2.7 names the case: "If only one distance
  // code is used, it is encoded using one bit ... Note that in this case
  // there is an incomplete Huffman tree with only one code."
  //
  // The condition has to be "no distance code is used", not "distance code 0
  // is not used".  Testing the latter - which is what this did - gave a
  // length-1 code to distance code 0 on top of a complete code for the
  // distances the block does use, and a complete code plus another one-bit
  // code is over-subscribed.  gcomp_deflate_huffman_build_codes() then
  // refused it and this function fell back to a fixed-Huffman block, quietly,
  // for the whole block.
  //
  // Distance code 0 is a match at distance 1, so run-heavy data has it and
  // structured text often does not.  On an XML registry every block took the
  // fallback: 135,514 bytes where the dynamic code its own frequencies called
  // for would have cost 113,265, against zlib's 112,703.  The parse was
  // already as good as zlib's - 30,519 matches averaging 39.8 bytes against
  // 30,320 averaging 40.1 - and all of the difference was this.
  int any_distance_used = 0;
  for (size_t j = 0; j < DEFLATE_MAX_DIST_SYMBOLS; j++) {
    if (dist_lengths[j] > 0) {
      any_distance_used = 1;
      break;
    }
  }
  if (!any_distance_used) {
    dist_lengths[0] = 1;
  }

  // Combine lit/dist lengths for encoding
  size_t total_codes = (size_t)(257 + hlit + 1 + hdist);
  uint8_t * all_lengths = (uint8_t *)gcomp_malloc(st->allocator, total_codes);
  if (!all_lengths) {
    // Fall back to fixed on memory error
    gcomp_stepdown_note(&st->stepdowns, GCOMP_STEPDOWN_NO_MEMORY);
    st->lit_freq[256]--;
    return deflate_flush_fixed_block(st, final);
  }

  memcpy(all_lengths, lit_lengths, 257 + hlit);
  memcpy(all_lengths + 257 + hlit, dist_lengths, 1 + hdist);

  // Run-length encode the code lengths
  uint8_t
      cl_symbols[DEFLATE_MAX_LITLEN_SYMBOLS + DEFLATE_MAX_DIST_SYMBOLS + 32];
  uint8_t cl_extra[DEFLATE_MAX_LITLEN_SYMBOLS + DEFLATE_MAX_DIST_SYMBOLS + 32];
  size_t cl_count = deflate_rle_encode_lengths(
      all_lengths, total_codes, cl_symbols, cl_extra);

  // Build code length Huffman tree
  uint32_t cl_freq[19] = {0};
  for (size_t i = 0; i < cl_count; i++) {
    cl_freq[cl_symbols[i]]++;
  }

  uint8_t cl_lengths[19];
  s = build_code_lengths(st->allocator, cl_freq, 19, cl_lengths, 7);
  if (s != GCOMP_OK) {
    gcomp_stepdown_note(&st->stepdowns, GCOMP_STEPDOWN_NO_MEMORY);
    gcomp_free(st->allocator, all_lengths);
    st->lit_freq[256]--;
    return deflate_flush_fixed_block(st, final);
  }

  // Ensure code-length alphabet is complete for zlib compatibility
  deflate_ensure_cl_kraft_complete(cl_lengths);

  // Determine HCLEN (number of code length codes - 4)
  int hclen = 19 - 4;
  while (hclen > 0 && cl_lengths[k_cl_order[hclen + 3]] == 0) {
    hclen--;
  }

  // Price this block both ways and take the cheaper.
  //
  // RFC 1951 section 3.2.6 defines a fixed code that costs no header at all;
  // section 3.2.7's dynamic code costs one but fits the block's own symbol
  // frequencies.  Which wins depends on the block, so the block decides.  A
  // level cannot: a short block, or one whose symbols are close to uniform,
  // pays more for the table than the table saves.
  //
  // Extra bits for length and distance codes are identical under both
  // codings, so they are left out of both sides rather than counted twice.
  {
    static const uint8_t k_cl_extra_bits[19] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 2, 3, 7};

    uint64_t dynamic_bits = 3u + 5u + 5u + 4u + 3u * (uint64_t)(hclen + 4);
    for (size_t i = 0; i < cl_count; i++) {
      dynamic_bits += cl_lengths[cl_symbols[i]] + k_cl_extra_bits[cl_symbols[i]];
    }
    for (size_t i = 0; i < DEFLATE_MAX_LITLEN_SYMBOLS; i++) {
      dynamic_bits += (uint64_t)st->lit_freq[i] * lit_lengths[i];
    }
    for (size_t i = 0; i < DEFLATE_MAX_DIST_SYMBOLS; i++) {
      dynamic_bits += (uint64_t)st->dist_freq[i] * dist_lengths[i];
    }

    // The fixed code of RFC 1951 section 3.2.6: literals 0-143 and 280-287
    // are eight bits, 144-255 are nine, 256-279 are seven, and every distance
    // code is five.
    uint64_t fixed_bits = 3u;
    for (size_t i = 0; i < DEFLATE_MAX_LITLEN_SYMBOLS; i++) {
      unsigned len = (i < 144u) ? 8u : (i < 256u) ? 9u : (i < 280u) ? 7u : 8u;
      fixed_bits += (uint64_t)st->lit_freq[i] * len;
    }
    for (size_t i = 0; i < DEFLATE_MAX_DIST_SYMBOLS; i++) {
      fixed_bits += (uint64_t)st->dist_freq[i] * 5u;
    }

    // And against storing the block uncompressed.  RFC 1951 section 3.2.4
    // costs three header bits, up to seven more to reach a byte boundary,
    // four bytes of LEN and its complement, and then the bytes themselves.
    // That is the ceiling on what a block can cost, and without it an encoder
    // can expand its input: level 1 used to make a JPEG 1.6% larger, and
    // nothing prevented it in general.
    //
    // The bytes come from the sliding window, so they have to still be in it,
    // and a stored block's length field is sixteen bits.  Neither bound binds
    // in the case that matters - incompressible data is nearly all literals,
    // so the block covers about one byte per symbol - but a block of long
    // matches can exceed both, and then there is nothing to store from.
    uint64_t stored_bits = 0;
    size_t block_input = deflate_block_input_length(st);
    size_t window_behind = (st->window_fill < st->window_size)
        ? st->window_fill
        : st->window_size;
    window_behind = (window_behind > st->lookahead)
        ? window_behind - st->lookahead
        : 0u;
    if (block_input > 0 && block_input <= 65535u &&
        block_input <= window_behind) {
      stored_bits = 3u + 7u + 32u + 8u * (uint64_t)block_input;
    }

    if (stored_bits > 0 && stored_bits < dynamic_bits &&
        (!st->fixed_ready || stored_bits < fixed_bits)) {
      gcomp_stepdown_note(&st->stepdowns, GCOMP_STEPDOWN_STORED_IS_SMALLER);
      gcomp_free(st->allocator, all_lengths);
      return deflate_flush_stored_block_from_window(st, final, block_input);
    }

    if (fixed_bits <= dynamic_bits && st->fixed_ready) {
      gcomp_stepdown_note(&st->stepdowns, GCOMP_STEPDOWN_FIXED_IS_SMALLER);
      gcomp_free(st->allocator, all_lengths);
      return deflate_flush_fixed_block(st, final);
    }
  }

  // Build canonical codes for code lengths
  uint16_t cl_codes[19];
  s = gcomp_deflate_huffman_build_codes(cl_lengths, 19, 7, cl_codes, NULL);
  if (s != GCOMP_OK) {
    gcomp_stepdown_note(&st->stepdowns, GCOMP_STEPDOWN_CODE_REJECTED);
    gcomp_free(st->allocator, all_lengths);
    st->lit_freq[256]--;
    return deflate_flush_fixed_block(st, final);
  }

  // Reverse codes for LSB-first output
  for (int j = 0; j < 19; j++) {
    if (cl_lengths[j] > 0) {
      cl_codes[j] = reverse_code(cl_codes[j], cl_lengths[j]);
    }
  }

  // Build canonical codes for lit/len and dist
  uint16_t lit_codes[DEFLATE_MAX_LITLEN_SYMBOLS];
  s = gcomp_deflate_huffman_build_codes(
      lit_lengths, DEFLATE_MAX_LITLEN_SYMBOLS, 15, lit_codes, NULL);
  if (s != GCOMP_OK) {
    gcomp_stepdown_note(&st->stepdowns, GCOMP_STEPDOWN_CODE_REJECTED);
    gcomp_free(st->allocator, all_lengths);
    st->lit_freq[256]--;
    return deflate_flush_fixed_block(st, final);
  }
  for (size_t j = 0; j < DEFLATE_MAX_LITLEN_SYMBOLS; j++) {
    if (lit_lengths[j] > 0) {
      lit_codes[j] = reverse_code(lit_codes[j], lit_lengths[j]);
    }
  }

  uint16_t dist_codes[DEFLATE_MAX_DIST_SYMBOLS];
  s = gcomp_deflate_huffman_build_codes(
      dist_lengths, DEFLATE_MAX_DIST_SYMBOLS, 15, dist_codes, NULL);
  if (s != GCOMP_OK) {
    gcomp_stepdown_note(&st->stepdowns, GCOMP_STEPDOWN_CODE_REJECTED);
    gcomp_free(st->allocator, all_lengths);
    st->lit_freq[256]--;
    return deflate_flush_fixed_block(st, final);
  }
  for (size_t j = 0; j < DEFLATE_MAX_DIST_SYMBOLS; j++) {
    if (dist_lengths[j] > 0) {
      dist_codes[j] = reverse_code(dist_codes[j], dist_lengths[j]);
    }
  }

  // Write block header: BFINAL (1 bit), BTYPE=10 (2 bits) = dynamic Huffman
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, final ? 1u : 0u, 1);
  if (s != GCOMP_OK) {
    gcomp_free(st->allocator, all_lengths);
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, 2u, 2); // BTYPE=10
  if (s != GCOMP_OK) {
    gcomp_free(st->allocator, all_lengths);
    return s;
  }

  // Write HLIT (5 bits), HDIST (5 bits), HCLEN (4 bits)
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, (uint32_t)hlit, 5);
  if (s != GCOMP_OK) {
    gcomp_free(st->allocator, all_lengths);
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, (uint32_t)hdist, 5);
  if (s != GCOMP_OK) {
    gcomp_free(st->allocator, all_lengths);
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, (uint32_t)hclen, 4);
  if (s != GCOMP_OK) {
    gcomp_free(st->allocator, all_lengths);
    return s;
  }

  // Write code length code lengths (3 bits each, in permuted order)
  for (int i = 0; i < hclen + 4; i++) {
    s = gcomp_deflate_bitwriter_write_bits(
        &st->bitwriter, cl_lengths[k_cl_order[i]], 3);
    if (s != GCOMP_OK) {
      gcomp_free(st->allocator, all_lengths);
      return s;
    }
  }

  // Write the code lengths for lit/len and dist alphabets
  for (size_t i = 0; i < cl_count; i++) {
    uint8_t sym = cl_symbols[i];
    s = gcomp_deflate_bitwriter_write_bits(
        &st->bitwriter, cl_codes[sym], cl_lengths[sym]);
    if (s != GCOMP_OK) {
      gcomp_free(st->allocator, all_lengths);
      return s;
    }

    // Write extra bits for run-length symbols
    if (sym == 16) {
      s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, cl_extra[i], 2);
    }
    else if (sym == 17) {
      s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, cl_extra[i], 3);
    }
    else if (sym == 18) {
      s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, cl_extra[i], 7);
    }
    if (s != GCOMP_OK) {
      gcomp_free(st->allocator, all_lengths);
      return s;
    }
  }

  gcomp_free(st->allocator, all_lengths);

  // Write all buffered symbols and end-of-block
  s = deflate_write_dynamic_block_data(
      st, lit_codes, lit_lengths, dist_codes, dist_lengths);
  if (s != GCOMP_OK) {
    return s;
  }

  // Reset for next block
  st->sym_buf_used = 0;
  st->block_input_len = 0;
  memset(st->lit_freq, 0, DEFLATE_MAX_LITLEN_SYMBOLS * sizeof(uint32_t));
  memset(st->dist_freq, 0, DEFLATE_MAX_DIST_SYMBOLS * sizeof(uint32_t));

  return GCOMP_OK;
}

//
// Public API
//

const gcomp_stepdown_tally_t * gcomp_deflate_encoder_stepdowns(
    const gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return NULL;
  }
  const gcomp_deflate_encoder_state_t * st =
      (const gcomp_deflate_encoder_state_t *)encoder->method_state;
  return &st->stepdowns;
}

gcomp_status_t gcomp_deflate_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  if (!registry || !encoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t status = GCOMP_OK;
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);

  // Read max memory limit early so we can check it during allocation
  uint64_t max_mem =
      gcomp_limits_read_memory_max(options, GCOMP_DEFAULT_MAX_MEMORY_BYTES);

  gcomp_deflate_encoder_state_t * st =
      (gcomp_deflate_encoder_state_t *)gcomp_calloc(
          alloc, 1, sizeof(gcomp_deflate_encoder_state_t));
  if (!st) {
    return GCOMP_ERR_MEMORY;
  }

  // Store allocator for internal use
  st->allocator = alloc;

  // Initialize memory tracker and track state struct allocation
  st->mem_tracker.current_bytes = 0;
  gcomp_memory_track_alloc(
      &st->mem_tracker, sizeof(gcomp_deflate_encoder_state_t));
  st->max_memory_bytes = max_mem;

  // Read compression level
  st->level = 6; // Default
  if (options) {
    int64_t v = 0;
    if (gcomp_options_get_int64(options, "deflate.level", &v) == GCOMP_OK) {
      if (v >= 0 && v <= 9) {
        st->level = (int)v;
      }
    }
  }

  // Read window bits
  st->window_bits = DEFLATE_WINDOW_BITS_DEFAULT;
  if (options) {
    uint64_t v = 0;
    if (gcomp_options_get_uint64(options, "deflate.window_bits", &v) ==
        GCOMP_OK) {
      if (v >= DEFLATE_WINDOW_BITS_MIN && v <= DEFLATE_WINDOW_BITS_MAX) {
        st->window_bits = (size_t)v;
      }
    }
  }

  // Read compression strategy
  st->strategy = DEFLATE_STRATEGY_DEFAULT;
  if (options) {
    const char * strategy_str = NULL;
    if (gcomp_options_get_string(options, "deflate.strategy", &strategy_str) ==
            GCOMP_OK &&
        strategy_str != NULL) {
      if (strcmp(strategy_str, "default") == 0) {
        st->strategy = DEFLATE_STRATEGY_DEFAULT;
      }
      else if (strcmp(strategy_str, "filtered") == 0) {
        st->strategy = DEFLATE_STRATEGY_FILTERED;
      }
      else if (strcmp(strategy_str, "huffman_only") == 0) {
        st->strategy = DEFLATE_STRATEGY_HUFFMAN_ONLY;
      }
      else if (strcmp(strategy_str, "rle") == 0) {
        st->strategy = DEFLATE_STRATEGY_RLE;
      }
      else if (strcmp(strategy_str, "fixed") == 0) {
        st->strategy = DEFLATE_STRATEGY_FIXED;
      }
      // Invalid strategy values silently fall back to default
    }
  }

  st->window_size = (size_t)1u << st->window_bits;
  st->window_mask = st->window_size - 1u;
  st->stage = DEFLATE_ENC_STAGE_INIT;
  st->final_block_written = 0;

  // Allocate sliding window
  st->window = (uint8_t *)gcomp_malloc(alloc, st->window_size);
  if (!st->window) {
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  gcomp_memory_track_alloc(&st->mem_tracker, st->window_size);

  st->window_pos = 0;
  st->window_fill = 0;
  st->lookahead = 0;

  // Allocate hash tables for LZ77
  size_t hash_head_size = DEFLATE_HASH_SIZE * sizeof(uint16_t);
  size_t hash_prev_size = st->window_size * sizeof(uint16_t);
  size_t hash_pos_size = st->window_size * sizeof(size_t);
  size_t hash_at_size = st->window_size * sizeof(uint16_t);

  st->hash_head =
      (uint16_t *)gcomp_calloc(alloc, DEFLATE_HASH_SIZE, sizeof(uint16_t));
  st->hash_prev =
      (uint16_t *)gcomp_calloc(alloc, st->window_size, sizeof(uint16_t));
  st->hash_pos = (size_t *)gcomp_calloc(alloc, st->window_size, sizeof(size_t));
  st->hash_at =
      (uint16_t *)gcomp_calloc(alloc, st->window_size, sizeof(uint16_t));
  if (!st->hash_head || !st->hash_prev || !st->hash_pos || !st->hash_at) {
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  gcomp_memory_track_alloc(&st->mem_tracker, hash_head_size);
  gcomp_memory_track_alloc(&st->mem_tracker, hash_prev_size);
  gcomp_memory_track_alloc(&st->mem_tracker, hash_pos_size);
  gcomp_memory_track_alloc(&st->mem_tracker, hash_at_size);

  st->total_in = 0;

  // For level 0, allocate block buffer
  if (st->level == 0) {
    st->block_buffer_size = DEFLATE_MAX_STORED_BLOCK;
    st->block_buffer = (uint8_t *)gcomp_malloc(alloc, st->block_buffer_size);
    if (!st->block_buffer) {
      status = GCOMP_ERR_MEMORY;
      goto cleanup;
    }
    gcomp_memory_track_alloc(&st->mem_tracker, st->block_buffer_size);
    st->block_buffer_used = 0;
  }

  // For levels > 0, allocate symbol buffers
  if (st->level > 0) {
    // Allocate enough for a full window worth of literals
    st->sym_buf_size = st->window_size;
    size_t sym_buf_bytes = st->sym_buf_size * sizeof(uint16_t);

    st->lit_buf = (uint16_t *)gcomp_malloc(alloc, sym_buf_bytes);
    st->dist_buf = (uint16_t *)gcomp_malloc(alloc, sym_buf_bytes);
    if (!st->lit_buf || !st->dist_buf) {
      status = GCOMP_ERR_MEMORY;
      goto cleanup;
    }
    gcomp_memory_track_alloc(&st->mem_tracker, sym_buf_bytes); // lit_buf
    gcomp_memory_track_alloc(&st->mem_tracker, sym_buf_bytes); // dist_buf
    st->sym_buf_used = 0;

    // Frequency histograms for dynamic Huffman.  These used to be allocated
    // only above level 3, which is what made the fast levels emit fixed
    // Huffman blocks: deflate_flush_dynamic_block() falls back to fixed when
    // there are no frequencies to build a code from.  Counting symbols costs
    // an increment each and building the code costs one package-merge per
    // block, and on a 12 MB corpus level 1 came out 12.8% smaller for 5% of
    // the encode throughput.  There is no level at which that is a bad trade.
    {
      size_t lit_freq_size = DEFLATE_MAX_LITLEN_SYMBOLS * sizeof(uint32_t);
      size_t dist_freq_size = DEFLATE_MAX_DIST_SYMBOLS * sizeof(uint32_t);

      st->lit_freq = (uint32_t *)gcomp_calloc(
          alloc, DEFLATE_MAX_LITLEN_SYMBOLS, sizeof(uint32_t));
      st->dist_freq = (uint32_t *)gcomp_calloc(
          alloc, DEFLATE_MAX_DIST_SYMBOLS, sizeof(uint32_t));
      if (!st->lit_freq || !st->dist_freq) {
        status = GCOMP_ERR_MEMORY;
        goto cleanup;
      }
      gcomp_memory_track_alloc(&st->mem_tracker, lit_freq_size);
      gcomp_memory_track_alloc(&st->mem_tracker, dist_freq_size);
    }

    // Build fixed Huffman codes
    status = deflate_build_fixed_codes(st);
    if (status != GCOMP_OK) {
      goto cleanup;
    }
  }

  // Check memory limit after all allocations
  status = gcomp_memory_check_limit(&st->mem_tracker, st->max_memory_bytes);
  if (status != GCOMP_OK) {
    goto cleanup;
  }

  // Success path
  st->stage = DEFLATE_ENC_STAGE_ACCEPTING;
  encoder->method_state = st;
  encoder->update_fn = gcomp_deflate_encoder_update;
  encoder->finish_fn = gcomp_deflate_encoder_finish;
  encoder->reset_fn = gcomp_deflate_encoder_reset;
  return GCOMP_OK;

cleanup:
  // Clean up all allocations on error (gcomp_free handles NULL safely)
  gcomp_free(alloc, st->dist_freq);
  gcomp_free(alloc, st->lit_freq);
  gcomp_free(alloc, st->dist_buf);
  gcomp_free(alloc, st->lit_buf);
  gcomp_free(alloc, st->block_buffer);
  gcomp_free(alloc, st->hash_at);
  gcomp_free(alloc, st->hash_pos);
  gcomp_free(alloc, st->hash_prev);
  gcomp_free(alloc, st->hash_head);
  gcomp_free(alloc, st->window);
  gcomp_free(alloc, st);
  return status;
}

void gcomp_deflate_encoder_destroy(gcomp_encoder_t * encoder) {
  if (!encoder) {
    return;
  }

  gcomp_deflate_encoder_state_t * st =
      (gcomp_deflate_encoder_state_t *)encoder->method_state;
  if (!st) {
    return;
  }

  const gcomp_allocator_t * alloc =
      gcomp_registry_get_allocator(encoder->registry);

  gcomp_free(alloc, st->pending_buf);
  gcomp_free(alloc, st->finish_buf);
  gcomp_free(alloc, st->dist_freq);
  gcomp_free(alloc, st->lit_freq);
  gcomp_free(alloc, st->dist_buf);
  gcomp_free(alloc, st->lit_buf);
  gcomp_free(alloc, st->block_buffer);
  gcomp_free(alloc, st->hash_at);
  gcomp_free(alloc, st->hash_pos);
  gcomp_free(alloc, st->hash_prev);
  gcomp_free(alloc, st->hash_head);
  gcomp_free(alloc, st->window);
  gcomp_free(alloc, st);
  encoder->method_state = NULL;
}

gcomp_status_t gcomp_deflate_encoder_reset(gcomp_encoder_t * encoder) {
  if (!encoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_deflate_encoder_state_t * st =
      (gcomp_deflate_encoder_state_t *)encoder->method_state;
  if (!st) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INTERNAL, "deflate encoder state is NULL");
  }

  const gcomp_allocator_t * alloc =
      gcomp_registry_get_allocator(encoder->registry);

  // Reset state machine
  st->stage = DEFLATE_ENC_STAGE_ACCEPTING;
  st->final_block_written = 0;

  // Reset sliding window state (keep buffer allocated)
  st->window_pos = 0;
  st->window_fill = 0;
  st->lookahead = 0;
  st->total_in = 0;
  st->lazy_length = 0;
  st->lazy_distance = 0;

  // Reset hash tables (clear to zeros)
  // A reset starts a new stream, so the record of what the previous one had
  // to settle for does not carry into it.
  memset(&st->stepdowns, 0, sizeof(st->stepdowns));
  st->block_input_len = 0;

  memset(st->hash_head, 0, DEFLATE_HASH_SIZE * sizeof(uint16_t));
  memset(st->hash_prev, 0, st->window_size * sizeof(uint16_t));
  memset(st->hash_pos, 0, st->window_size * sizeof(size_t));
  memset(st->hash_at, 0, st->window_size * sizeof(uint16_t));
  st->hash_value = 0;

  // Reset bitwriter state
  gcomp_deflate_bitwriter_reset(&st->bitwriter);

  // Reset block buffer (level 0)
  if (st->block_buffer) {
    st->block_buffer_used = 0;
  }

  // Reset symbol buffers (levels > 0)
  if (st->lit_buf) {
    st->sym_buf_used = 0;
  }

  // Reset frequency histograms (levels > 3)
  if (st->lit_freq) {
    memset(st->lit_freq, 0, DEFLATE_MAX_LITLEN_SYMBOLS * sizeof(uint32_t));
  }
  if (st->dist_freq) {
    memset(st->dist_freq, 0, DEFLATE_MAX_DIST_SYMBOLS * sizeof(uint32_t));
  }

  // Free and reset finish buffer (if any partial finish was in progress)
  if (st->finish_buf) {
    gcomp_free(alloc, st->finish_buf);
    st->finish_buf = NULL;
  }
  st->finish_buf_size = 0;
  st->finish_buf_used = 0;
  st->finish_buf_copied = 0;
  st->finish_buf_ready = 0;

  // Discard anything update() had staged but not yet delivered.
  if (st->pending_buf) {
    gcomp_free(alloc, st->pending_buf);
    st->pending_buf = NULL;
  }
  st->pending_size = 0;
  st->pending_used = 0;
  st->pending_copied = 0;

  // Note: We don't reset fixed_ready because the fixed Huffman tables don't
  // need to be rebuilt - they're static and can be reused.

  return GCOMP_OK;
}

/**
 * @brief Worst-case byte size of a single flushed block.
 *
 * A Huffman block holds at most sym_buf_size symbols. Four bytes per symbol
 * is the same conservative figure deflate_estimate_finish_size() uses (the
 * true worst case is a length/distance pair at about 43 bits), plus the
 * dynamic Huffman tree, the block header and end-of-block marker, byte
 * alignment and a margin. A stored block is bounded separately by
 * DEFLATE_MAX_STORED_BLOCK plus its header.
 *
 * pending_buf is sized to this, so a flush into an empty pending_buf always
 * fits and update() can always make progress.
 *
 * @param st Encoder state.
 * @return Upper bound, in bytes, on one flushed block.
 */
static size_t deflate_max_block_bytes(
    const gcomp_deflate_encoder_state_t * st) {
  size_t huffman = (st->sym_buf_size * 4u) + 512u + 8u + 1u + 64u;
  size_t stored = (size_t)DEFLATE_MAX_STORED_BLOCK + 16u;
  return huffman > stored ? huffman : stored;
}

/**
 * @brief Copy staged output to the caller, as far as it will fit.
 *
 * @param st Encoder state.
 * @param output The caller's output buffer.
 * @return Non-zero if everything staged has been delivered.
 */
static int deflate_drain_pending(
    gcomp_deflate_encoder_state_t * st, gcomp_buffer_t * output) {
  size_t avail = st->pending_used - st->pending_copied;
  if (avail) {
    size_t space = output->size - output->used;
    size_t n = (avail < space) ? avail : space;
    if (n) {
      memcpy((uint8_t *)output->data + output->used,
          st->pending_buf + st->pending_copied, n);
      output->used += n;
      st->pending_copied += n;
    }
  }
  if (st->pending_copied >= st->pending_used) {
    st->pending_used = 0;
    st->pending_copied = 0;
    return 1;
  }
  return 0;
}

/**
 * @brief Encode one batch of input into the staging buffer.
 *
 * Consumes as much of @p input as will fit in pending_buf, stopping before any
 * flush that could overflow it. pending_buf holds at least one worst-case
 * block and is empty on entry, so a batch always makes progress.
 *
 * @param st Encoder state.
 * @param input The caller's input buffer; used is advanced by what was taken.
 * @return Status code.
 */
static gcomp_status_t deflate_encode_batch(
    gcomp_deflate_encoder_state_t * st, gcomp_buffer_t * input) {

  if (st->stage == DEFLATE_ENC_STAGE_DONE) {
    return GCOMP_OK;
  }

  // Set bitwriter output buffer, preserving any partial bits from previous
  // call.
  //
  // IMPORTANT: We use set_buffer() instead of init() because DEFLATE blocks
  // do NOT end on byte boundaries. If a previous call to update() wrote a
  // partial byte (e.g., 5 bits), we must preserve those bits. Using init()
  // would reset the bit buffer and corrupt the stream.
  //
  // The bitwriter tracks: data (output pointer), size (capacity), byte_pos
  // (full bytes written), bit_buffer (partial byte), bit_count (bits in
  // buffer). set_buffer() updates data/size/byte_pos but preserves
  // bit_buffer/bit_count.
  gcomp_status_t s = gcomp_deflate_bitwriter_set_buffer(
      &st->bitwriter, st->pending_buf, st->pending_size);
  if (s != GCOMP_OK) {
    return s;
  }

  const uint8_t * src = (const uint8_t *)input->data;
  const size_t max_block = deflate_max_block_bytes(st);
  int batch_full = 0;

  // Level 0: stored blocks (no compression)
  if (st->level == 0) {
    while (input->used < input->size) {
      // Fill block buffer
      size_t avail = input->size - input->used;
      size_t space = st->block_buffer_size - st->block_buffer_used;
      size_t copy = (avail < space) ? avail : space;

      if (copy > 0) {
        memcpy(
            st->block_buffer + st->block_buffer_used, src + input->used, copy);
        st->block_buffer_used += copy;
        input->used += copy;
      }

      // Flush if buffer is full
      if (st->block_buffer_used >= st->block_buffer_size) {
        s = deflate_flush_stored_block(st, 0);
        if (s != GCOMP_OK) {
          st->pending_used = gcomp_deflate_bitwriter_bytes_written(
              &st->bitwriter);
          return s;
        }
        // Stop the batch if the staging buffer could not hold another block.
        // The caller drains what is staged and calls again.
        if (gcomp_deflate_bitwriter_bytes_written(&st->bitwriter) + max_block >
            st->pending_size) {
          break;
        }
      }
    }
  }
  else {
    // Levels 1-9: LZ77 + Huffman compression
    //
    // Strategy affects match finding and Huffman mode:
    // - DEFAULT: Standard LZ77 with chain length based on level
    // - FILTERED: Longer chains, favors longer matches (PNG-optimized)
    // - HUFFMAN_ONLY: No LZ77, emit all bytes as literals
    // - RLE: Only find matches at distance 1
    // - FIXED: Standard LZ77 but always use fixed Huffman codes
    //
    int max_chain;
    int use_fixed_huffman;
    int skip_lz77 = (st->strategy == DEFLATE_STRATEGY_HUFFMAN_ONLY);

    // Lazy matching: hold a match back one byte to see whether the next
    // position starts a longer one.
    //
    // zlib draws this line between its deflate_fast and deflate_slow paths at
    // level 4, and this now draws it in the same place: levels 4 and up defer
    // a match, levels 1 to 3 take what they find.  A level is a statement
    // about how much work to spend, and deferring is the cheapest large gain
    // available at that price -- it costs no extra searching at all, because
    // the search that settles a held match is the one the next position was
    // going to perform anyway.
    //
    // FILTERED keeps deferring at every level: asking for it by name is
    // asking for the effort, and its levels 1 to 3 were already spending it.
    //
    // A match already at or above max_lazy is taken as it stands: the search
    // that would test it costs as much as the search that found it, and a
    // match that long has little room to improve.  The thresholds follow
    // zlib's, which spends more of this at higher levels.
    int use_lazy = (st->strategy == DEFLATE_STRATEGY_FILTERED) ||
        ((st->strategy == DEFLATE_STRATEGY_DEFAULT ||
             st->strategy == DEFLATE_STRATEGY_FIXED) &&
            st->level >= 4);
    uint32_t max_lazy = (st->level <= 3)    ? 4u
        : (st->level <= 6)                  ? 16u
        : (st->level <= 8)                  ? 32u
                                            : 258u;

    // A strategy that defers at levels 1 to 3 needs the threshold of the
    // levels that defer, not the one those levels use.
    //
    // The level-based value of 4 means "hold a match back only if it is
    // exactly three bytes long", and a three-byte match is the one case where
    // deferring cannot pay: the byte given up is a literal, the match that
    // displaces it is four bytes at best, and the sequence it replaces would
    // have been found at the next position anyway.  Each deferral traded one
    // match for one literal and saved no symbols at all.  On filter-shaped
    // bytes FILTERED came out *larger* than DEFAULT -- 507,624 against
    // 501,304 - which is the opposite of what the strategy is for, and it is
    // what led to this being written off as a strategy that had outlived its
    // reason.
    //
    // With the threshold the deferring levels use, the same file goes to
    // 448,562: 10.5% smaller than DEFAULT rather than 1.3% larger.  Over a
    // 12 MB corpus of source, prose, XML and binaries it is 25.954% against
    // DEFAULT's 26.680%, for about 15% of the encode throughput.
    if (st->strategy == DEFLATE_STRATEGY_FILTERED && st->level <= 3) {
      max_lazy = 16u;
    }

    // Determine hash chain length based on level.
    //
    // FILTERED used to quadruple this - 16/128/256 against 4/32/128 - on the
    // reasoning that filtered data hides longer patterns behind short hash
    // chains. Measured across 52 files of real PNG filtered rows, 7.3 MB, it
    // does not pay:
    //
    //   chain   with lazy matching   without
    //      32        31.22%           32.91%
    //      64        31.13%           33.42%
    //     128        31.13%           33.41%
    //     256        31.12%           33.40%
    //
    // Chain length is worth 0.1 points across a factor of eight. Lazy matching
    // is worth 1.7. So FILTERED now searches exactly as hard as DEFAULT and
    // differs from it only by holding matches back, which is the same
    // relationship zlib's levels 4-9 have to its levels 1-3. On that corpus
    // this is 2.2x the throughput of the old setting for 0.25% more bytes,
    // and the same ranking holds on a general corpus of source and binaries.
    if (st->strategy == DEFLATE_STRATEGY_RLE) {
      // RLE doesn't use hash chains (only checks distance 1)
      max_chain = 0;
    }
    else {
      max_chain = (st->level <= 3) ? 4 : (st->level <= 6) ? 32 : 128;
    }

    // Only the fixed strategy forces fixed codes now.  Every other block goes
    // to deflate_flush_dynamic_block(), which prices both codings and writes
    // whichever is smaller -- so a block whose symbols happen to suit the
    // fixed code still gets it, without a level having to guess in advance.
    use_fixed_huffman = (st->strategy == DEFLATE_STRATEGY_FIXED);

    while (input->used < input->size) {
      // Fill window with input data.
      //
      // Only up to refill_to bytes of lookahead are held at a time.  The
      // window is circular and exactly window_size bytes long, so filling it
      // to the brim - which is what this did - overwrote every byte of
      // history in one go: the encoder ran through 32 KB of lookahead with
      // nothing behind it, refilled, and started again with an empty window.
      // Matches could not cross those boundaries at all, and inside a batch
      // the reachable history was whatever had been consumed so far rather
      // than the 32 KB the format allows.
      //
      // Holding the lookahead to a kilobyte instead keeps the rest of the
      // window as history.  Across 12 MB of source, prose, XML and binaries
      // that is 8.0% fewer bytes out, and faster: the encoder finds longer
      // matches, so it emits fewer symbols for the same input.
      size_t refill_to = st->window_size / 2u;
      if (refill_to > DEFLATE_REFILL_LOOKAHEAD) {
        refill_to = DEFLATE_REFILL_LOOKAHEAD;
      }
      size_t avail = input->size - input->used;
      size_t space = (st->lookahead < refill_to) ? (refill_to - st->lookahead)
                                                 : 0u;
      size_t copy = (avail < space) ? avail : space;

      if (copy > 0) {
        for (size_t i = 0; i < copy; i++) {
          st->window[st->window_pos] = src[input->used + i];
          st->window_pos = (st->window_pos + 1) & st->window_mask;
        }
        st->lookahead += copy;
        st->total_in += copy;
        if (st->window_fill < st->window_size) {
          st->window_fill += copy;
          if (st->window_fill > st->window_size) {
            st->window_fill = st->window_size;
          }
        }
        input->used += copy;
      }

      // Process lookahead data
      // Go back for more input before the lookahead falls short of a full
      // match, so that a match near the end of a batch is not cut off by the
      // batch boundary.  With no input left there is nothing to wait for, and
      // the remaining lookahead is encoded as it stands.
      size_t refill_at = refill_to / 2u;
      if (refill_at > DEFLATE_MIN_LOOKAHEAD) {
        refill_at = DEFLATE_MIN_LOOKAHEAD;
      }
      while (st->lookahead >= DEFLATE_MIN_MATCH_LENGTH ||
          (skip_lz77 && st->lookahead > 0)) {
        if (!skip_lz77 && st->lookahead < refill_at &&
            input->used < input->size) {
          break;
        }
        // A block is closed when the symbol buffer fills, and also when it
        // is nearly all literals and has grown to what the window can still
        // hand back.
        //
        // A stored block's bytes come from the window, so they have to still
        // be in it: the block may cover at most window_size minus the
        // lookahead in front of it.  A literal-heavy block covers about one
        // byte per symbol, so with a symbol buffer the size of the window it
        // runs past that, and the stored form - the one thing that stops an
        // encoder expanding its input - is never available where it is most
        // wanted.
        //
        // Shortening every block instead costs real data real bytes: taking
        // two refills' worth off the symbol buffer was 0.34% at level 1
        // across the corpus.  The mean-coverage test keeps that cost off
        // compressible data, where blocks cover three to fourteen bytes per
        // symbol and a stored block could never have won anyway.
        size_t stored_reach = (st->window_size > 4u * DEFLATE_REFILL_LOOKAHEAD)
            ? st->window_size - 2u * DEFLATE_REFILL_LOOKAHEAD
            : st->window_size;
        // Within an eighth of one byte per symbol: essentially nothing is
        // matching.  A looser test - twice a byte per symbol - also fires on
        // data that compresses a little, such as a JPEG, where the extra
        // block header costs more than the stored form could ever save.
        int literal_heavy = st->block_input_len <
            (size_t)st->sym_buf_used + (size_t)st->sym_buf_used / 8u;
        int out_of_window_reach = literal_heavy && st->sym_buf_used > 0 &&
            st->block_input_len + DEFLATE_MAX_MATCH_LENGTH >= stored_reach;

        if (st->sym_buf_used >= st->sym_buf_size - 2 || out_of_window_reach) {
          if (use_fixed_huffman) {
            s = deflate_flush_fixed_block(st, 0);
          }
          else {
            s = deflate_flush_dynamic_block(st, 0);
          }
          if (s != GCOMP_OK) {
            st->pending_used = gcomp_deflate_bitwriter_bytes_written(
                &st->bitwriter);
            return s;
          }
          // Stop the batch if the staging buffer could not hold another
          // block. The caller drains what is staged and calls again; the
          // window, symbol buffer and partial bits all persist in st.
          if (gcomp_deflate_bitwriter_bytes_written(&st->bitwriter) +
                  max_block >
              st->pending_size) {
            batch_full = 1;
            break;
          }
        }

        size_t pos = (st->window_pos + st->window_size - st->lookahead) %
            st->window_size;
        size_t stream_pos = st->total_in - st->lookahead;

        // Strategy-specific match finding
        deflate_match_t match = {0, 0};

        if (skip_lz77) {
          // HUFFMAN_ONLY: No match finding at all
          // match stays {0, 0} - will emit literal
        }
        else if (st->strategy == DEFLATE_STRATEGY_RLE) {
          // RLE: Only look for matches at distance 1.
          //
          // window_fill > lookahead is the test for the window holding any
          // history at all, and stream_pos > 0 is not it.  The refill takes
          // the window up to window_size - lookahead bytes at a time, so the
          // lookahead can fill the window completely; the byte before pos is
          // then the last byte of the lookahead - data not yet emitted -
          // rather than the byte a distance of 1 refers to.  Reading it there
          // made the encoder emit a distance-1 match against a byte it had
          // not written, which decodes to whatever really did precede it.
          //
          // It showed up as a run continuing one byte past its end: with an
          // 8-bit window, 40,000 bytes of 1,000-byte runs decoded correctly
          // until offset 32,000 and then carried 0x1f where 0x20 belonged.
          // Both this library's decoder and zlib reproduced the same wrong
          // bytes, which is what says the fault is on this side.
          if (st->lookahead >= DEFLATE_MIN_MATCH_LENGTH && stream_pos > 0 &&
              st->window_fill > st->lookahead) {
            // Check for run at distance 1
            size_t prev_pos = (pos + st->window_size - 1) & st->window_mask;
            uint8_t run_byte = st->window[prev_pos];
            size_t run_len = 0;
            size_t max_len = st->lookahead;
            if (max_len > DEFLATE_MAX_MATCH_LENGTH) {
              max_len = DEFLATE_MAX_MATCH_LENGTH;
            }

            // Count how many bytes match the previous byte
            while (run_len < max_len &&
                st->window[(pos + run_len) & st->window_mask] == run_byte) {
              run_len++;
            }

            if (run_len >= DEFLATE_MIN_MATCH_LENGTH) {
              match.length = (uint32_t)run_len;
              match.distance = 1;
            }
          }
        }
        else if (st->lookahead >= DEFLATE_MIN_MATCH_LENGTH) {
          // DEFAULT/FILTERED/FIXED: Standard LZ77 match finding
          match = deflate_find_match(st, pos, stream_pos, max_chain);

          // A three-byte match far away is not worth its distance code.  The
          // code for a distance past 4096 carries eleven extra bits on top of
          // the code itself (RFC 1951 section 3.2.5), so the sequence costs
          // about as much as the three literals it replaces -- and unlike
          // them it interrupts the literal run, which the Huffman code was
          // about to encode cheaply.
          //
          // Measured over a 12 MB corpus: 1.0% smaller at level 1, 0.1% at
          // level 6, and faster at every level, because a discarded match is
          // one the encoder does not have to emit.  zlib draws the same line
          // in the same place.
          if (match.length == DEFLATE_MIN_MATCH_LENGTH &&
              match.distance > DEFLATE_TOO_FAR) {
            match.length = 0;
            match.distance = 0;
          }
        }

        // Lazy matching, as a deferral rather than a second search.
        //
        // The question at position p is whether p is better spent as a
        // literal because the match at p+1 is longer.  This used to answer it
        // by searching p+1 and then throwing the answer away, so the next
        // iteration searched p+1 again: two full walks of a 128-deep chain
        // for every position that found a short match.
        //
        // Now the match at p is held in the encoder state and the encoder
        // moves on, and the search the next iteration performs anyway is the
        // one that settles it.  One search per position.
        //
        // Deferring also finds matches the old arrangement could not see.  A
        // position is entered into the hash chains as it is passed, so the
        // chain searched at p+1 now contains p - which is where a match at
        // distance 1 comes from, and those are exactly the runs that PNG
        // filter output is full of.  The old lookahead search ran before p was
        // inserted and could never find one.
        if (use_lazy) {
          if (st->lazy_length >= DEFLATE_MIN_MATCH_LENGTH) {
            uint32_t held_length = st->lazy_length;
            uint32_t held_distance = st->lazy_distance;
            st->lazy_length = 0;

            if (match.length > held_length) {
              // The later match is strictly longer, so the byte the held
              // match started on is spent as a literal.  That position was
              // consumed and hashed when it was held, so only the symbol is
              // recorded here; `match` is reconsidered below.
              size_t held_pos = (pos + st->window_size - 1u) & st->window_mask;
              uint8_t lit = st->window[held_pos];
              st->lit_buf[st->sym_buf_used] = lit;
              st->dist_buf[st->sym_buf_used] = 0;
              st->sym_buf_used++;
              st->block_input_len += 1u;
              if (st->lit_freq) {
                st->lit_freq[lit]++;
              }
            }
            else {
              // Nothing better turned up; take the held match.  It began one
              // byte back, so its first byte is already accounted for and
              // only the rest is consumed here.
              st->lit_buf[st->sym_buf_used] = (uint16_t)held_length;
              st->dist_buf[st->sym_buf_used] = (uint16_t)held_distance;
              st->sym_buf_used++;
              st->block_input_len += (size_t)held_length;
              if (st->lit_freq) {
                st->lit_freq[gcomp_deflate_length_code(held_length)]++;
                st->dist_freq[gcomp_deflate_distance_code(held_distance)]++;
              }
              for (uint32_t i = 0; i + 1u < held_length && st->lookahead > 0;
                  i++) {
                if (st->lookahead >= 3) {
                  deflate_insert_hash(st, pos, stream_pos);
                }
                pos = (pos + 1) & st->window_mask;
                stream_pos++;
                st->lookahead--;
              }
              continue;
            }
          }

          // Hold this match back if there is any prospect of improving on it.
          // st->lookahead > match.length keeps a byte in hand for the next
          // position to be searched at all.
          if (match.length >= DEFLATE_MIN_MATCH_LENGTH &&
              match.length < max_lazy && st->lookahead > match.length) {
            st->lazy_length = match.length;
            st->lazy_distance = match.distance;
            if (st->lookahead >= 3) {
              deflate_insert_hash(st, pos, stream_pos);
            }
            st->lookahead--;
            continue;
          }
        }

        if (match.length >= DEFLATE_MIN_MATCH_LENGTH) {
          // Record length/distance pair
          st->lit_buf[st->sym_buf_used] = (uint16_t)match.length;
          st->dist_buf[st->sym_buf_used] = (uint16_t)match.distance;
          st->sym_buf_used++;
          st->block_input_len += (size_t)match.length;

          // Track frequencies for dynamic Huffman
          if (st->lit_freq) {
            uint32_t len_code = gcomp_deflate_length_code(match.length);
            st->lit_freq[len_code]++;
            uint32_t dist_code = gcomp_deflate_distance_code(match.distance);
            st->dist_freq[dist_code]++;
          }

          // Insert all bytes of the match into the hash table
          for (uint32_t i = 0; i < match.length && st->lookahead > 0; i++) {
            if (st->lookahead >= 3) {
              deflate_insert_hash(st, pos, stream_pos);
            }
            pos = (pos + 1) & st->window_mask;
            stream_pos++;
            st->lookahead--;
          }
        }
        else {
          // Record literal
          uint8_t lit = st->window[pos];
          st->lit_buf[st->sym_buf_used] = lit;
          st->dist_buf[st->sym_buf_used] = 0;
          st->sym_buf_used++;
          st->block_input_len += 1u;

          // Track frequencies for dynamic Huffman
          if (st->lit_freq) {
            st->lit_freq[lit]++;
          }

          // Insert byte into hash table (unless HUFFMAN_ONLY)
          if (!skip_lz77 && st->lookahead >= 3) {
            deflate_insert_hash(st, pos, stream_pos);
          }
          st->lookahead--;
        }
      }

      if (batch_full) {
        break;
      }

      // If we can't make progress, break
      if (copy == 0 && st->lookahead < DEFLATE_MIN_MATCH_LENGTH &&
          !(skip_lz77 && st->lookahead > 0)) {
        break;
      }
    }
  }

  st->pending_used = gcomp_deflate_bitwriter_bytes_written(&st->bitwriter);
  return GCOMP_OK;
}

gcomp_status_t gcomp_deflate_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!encoder || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }
  // Check data pointers if size > 0
  if ((input->size > 0 && !input->data) ||
      (output->size > 0 && !output->data)) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_deflate_encoder_state_t * st =
      (gcomp_deflate_encoder_state_t *)encoder->method_state;
  if (!st) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INTERNAL, "deflate encoder state is NULL");
  }

  if (st->stage == DEFLATE_ENC_STAGE_DONE) {
    return GCOMP_OK;
  }

  const gcomp_allocator_t * alloc =
      gcomp_registry_get_allocator(encoder->registry);

  // Rendered blocks are staged here and copied out as the caller's buffer
  // allows, so no input is ever consumed that cannot later be delivered.
  if (!st->pending_buf) {
    size_t size = deflate_max_block_bytes(st);
    st->pending_buf = (uint8_t *)gcomp_malloc(alloc, size);
    if (!st->pending_buf) {
      return gcomp_encoder_set_error(encoder, GCOMP_ERR_MEMORY,
          "failed to allocate deflate staging buffer (%zu bytes)", size);
    }
    st->pending_size = size;
    st->pending_used = 0;
    st->pending_copied = 0;
  }

  for (;;) {
    // Hand over whatever is already staged. If it does not all fit, the
    // caller must drain and call again; no input is taken this round.
    if (!deflate_drain_pending(st, output)) {
      return GCOMP_OK;
    }
    if (input->used >= input->size || output->used >= output->size) {
      return GCOMP_OK;
    }

    size_t before = input->used;
    gcomp_status_t s = deflate_encode_batch(st, input);
    if (s != GCOMP_OK) {
      // Deliver what the failed batch had already rendered, then report.
      (void)deflate_drain_pending(st, output);
      return s;
    }
    if (input->used == before && st->pending_used == 0) {
      // Neither consumed nor produced: nothing more to do this call.
      return GCOMP_OK;
    }
  }
}

/**
 * @brief Estimate the maximum size needed for finish() output.
 *
 * This is a conservative upper bound to ensure we allocate enough buffer
 * space to render the entire finish output in one pass.
 *
 * The estimate accounts for:
 * - Remaining lookahead bytes (each becomes a literal or part of a match)
 * - Buffered symbols that need to be flushed
 * - Dynamic Huffman tree overhead
 * - Block headers and end-of-block markers
 * - Byte alignment padding
 *
 * @param st Encoder state
 * @return Conservative upper bound in bytes
 */
static size_t deflate_estimate_finish_size(
    const gcomp_deflate_encoder_state_t * st) {
  // Each literal/length can be up to 15 bits (dynamic Huffman max)
  // Each distance can be up to 15 bits + 13 extra bits = 28 bits
  // Worst case for a length/distance pair: ~43 bits
  // For a literal: 15 bits
  //
  // Conservative: assume 4 bytes per symbol (32 bits) which is more than
  // enough for any symbol type.
  size_t sym_overhead = 4;

  // Count symbols: remaining lookahead + already buffered symbols
  size_t total_symbols = st->lookahead + st->sym_buf_used;

  // For level 0, each byte becomes a stored block byte (1:1 plus header)
  if (st->level == 0) {
    // Stored block: 3 bits header, byte-align, 4 bytes LEN/NLEN, then data
    // Plus we might have multiple blocks if data is large
    size_t data_bytes = st->block_buffer_used + st->lookahead;
    size_t num_blocks =
        (data_bytes + DEFLATE_MAX_STORED_BLOCK - 1) / DEFLATE_MAX_STORED_BLOCK;
    if (num_blocks == 0) {
      num_blocks = 1; // At least one final block
    }
    // Each block: up to 5 bytes header (3 bits rounded + 4 bytes LEN/NLEN)
    // plus the data
    // Check for overflow: num_blocks * 5 + data_bytes + 8
    if (num_blocks > SIZE_MAX / 5) {
      return SIZE_MAX / 2;
    }
    size_t header_bytes = num_blocks * 5;
    if (header_bytes > SIZE_MAX - data_bytes - 8) {
      return SIZE_MAX / 2;
    }
    return header_bytes + data_bytes + 8; // +8 for safety margin
  }

  // For Huffman blocks, estimate based on symbols
  // Check for overflow: total_symbols * sym_overhead
  if (total_symbols > SIZE_MAX / sym_overhead) {
    // Overflow would occur; return a large but safe value
    return SIZE_MAX / 2;
  }
  size_t symbol_bytes = total_symbols * sym_overhead;

  // Dynamic Huffman tree overhead: up to ~300 bytes for the tree encoding
  // (HLIT/HDIST/HCLEN headers, code-length codes, encoded code lengths)
  size_t tree_overhead = 512;

  // Block headers (3 bits each) and end-of-block (up to 15 bits)
  // Multiple blocks may be needed if sym_buf fills up
  size_t num_blocks = (total_symbols + st->sym_buf_size - 1) / st->sym_buf_size;
  if (num_blocks == 0) {
    num_blocks = 1;
  }

  // Check for overflow: num_blocks * (tree_overhead + 8)
  size_t per_block = tree_overhead + 8;
  if (num_blocks > SIZE_MAX / per_block) {
    return SIZE_MAX / 2;
  }
  size_t block_overhead = num_blocks * per_block;

  // Byte alignment (up to 7 bits = 1 byte)
  size_t alignment = 1;

  // Safety margin
  size_t margin = 64;

  // Check for overflow in final addition
  size_t result = symbol_bytes;
  if (result > SIZE_MAX - block_overhead) {
    return SIZE_MAX / 2;
  }
  result += block_overhead;
  if (result > SIZE_MAX - alignment - margin) {
    return SIZE_MAX / 2;
  }

  return result + alignment + margin;
}

gcomp_status_t gcomp_deflate_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  if (!encoder || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_deflate_encoder_state_t * st =
      (gcomp_deflate_encoder_state_t *)encoder->method_state;
  if (!st) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INTERNAL, "deflate encoder state is NULL");
  }

  if (st->final_block_written) {
    return GCOMP_OK;
  }

  // Anything update() staged but could not deliver must go out first, ahead
  // of the final block. Report GCOMP_ERR_LIMIT until it has all been handed
  // over, the same "call me again" contract finish() already uses below.
  if (st->pending_buf && st->pending_used > st->pending_copied) {
    if (!deflate_drain_pending(st, output)) {
      return GCOMP_ERR_LIMIT;
    }
  }

  const gcomp_allocator_t * alloc =
      gcomp_registry_get_allocator(encoder->registry);

  // If we haven't rendered the finish output yet, do so now
  if (!st->finish_buf_ready) {
    // Estimate how much buffer we need
    size_t buf_size = deflate_estimate_finish_size(st);

    // Allocate the finish buffer
    st->finish_buf = (uint8_t *)gcomp_malloc(alloc, buf_size);
    if (!st->finish_buf) {
      return gcomp_encoder_set_error(encoder, GCOMP_ERR_MEMORY,
          "failed to allocate deflate finish buffer (%zu bytes)", buf_size);
    }
    st->finish_buf_size = buf_size;
    st->finish_buf_used = 0;
    st->finish_buf_copied = 0;

    // Set up bitwriter to write to our internal buffer
    gcomp_status_t s = gcomp_deflate_bitwriter_set_buffer(
        &st->bitwriter, st->finish_buf, st->finish_buf_size);
    if (s != GCOMP_OK) {
      gcomp_free(alloc, st->finish_buf);
      st->finish_buf = NULL;
      st->finish_buf_size = 0;
      return s;
    }

    if (st->level == 0) {
      // Flush remaining stored data as final block
      s = deflate_flush_stored_block(st, 1);
    }
    else {
      // Determine whether to use fixed or dynamic Huffman
      int use_fixed_huffman =
          (st->strategy == DEFLATE_STRATEGY_FIXED) || (st->level <= 3);

      // A match held back by lazy matching has to go out before the tail is
      // flushed, or the byte it starts on is emitted twice: once as part of
      // the match that never arrives, and once as a literal below.  Nothing
      // better can turn up now - there is no next position to search.
      if (st->lazy_length >= DEFLATE_MIN_MATCH_LENGTH) {
        uint32_t held_length = st->lazy_length;
        uint32_t held_distance = st->lazy_distance;
        st->lazy_length = 0;

        if (st->sym_buf_used >= st->sym_buf_size) {
          s = use_fixed_huffman ? deflate_flush_fixed_block(st, 0)
                                : deflate_flush_dynamic_block(st, 0);
          if (s != GCOMP_OK) {
            gcomp_free(alloc, st->finish_buf);
            st->finish_buf = NULL;
            st->finish_buf_size = 0;
            return s;
          }
        }

        st->lit_buf[st->sym_buf_used] = (uint16_t)held_length;
        st->dist_buf[st->sym_buf_used] = (uint16_t)held_distance;
        st->sym_buf_used++;
        st->block_input_len += (size_t)held_length;
        if (st->lit_freq) {
          st->lit_freq[gcomp_deflate_length_code(held_length)]++;
          st->dist_freq[gcomp_deflate_distance_code(held_distance)]++;
        }

        // Its first byte was consumed when it was held, so only the rest is
        // taken off the lookahead.
        uint32_t remaining_bytes = held_length - 1u;
        if ((size_t)remaining_bytes > st->lookahead) {
          remaining_bytes = (uint32_t)st->lookahead;
        }
        st->lookahead -= remaining_bytes;
      }

      // Flush any remaining lookahead as literals
      while (st->lookahead > 0) {
        if (st->sym_buf_used >= st->sym_buf_size) {
          if (use_fixed_huffman) {
            s = deflate_flush_fixed_block(st, 0);
          }
          else {
            s = deflate_flush_dynamic_block(st, 0);
          }
          if (s != GCOMP_OK) {
            gcomp_free(alloc, st->finish_buf);
            st->finish_buf = NULL;
            st->finish_buf_size = 0;
            return s;
          }
        }

        size_t pos = (st->window_pos + st->window_size - st->lookahead) %
            st->window_size;
        uint8_t lit = st->window[pos];
        st->lit_buf[st->sym_buf_used] = lit;
        st->dist_buf[st->sym_buf_used] = 0;
        st->sym_buf_used++;
        st->block_input_len += 1u;

        // Track frequency for dynamic Huffman
        if (st->lit_freq) {
          st->lit_freq[lit]++;
        }

        st->lookahead--;
      }

      // Flush final block
      if (use_fixed_huffman) {
        s = deflate_flush_fixed_block(st, 1);
      }
      else {
        s = deflate_flush_dynamic_block(st, 1);
      }
    }

    if (s != GCOMP_OK) {
      gcomp_free(alloc, st->finish_buf);
      st->finish_buf = NULL;
      st->finish_buf_size = 0;
      return s;
    }

    // Flush bitwriter to byte boundary
    s = gcomp_deflate_bitwriter_flush_to_byte(&st->bitwriter);
    if (s != GCOMP_OK) {
      gcomp_free(alloc, st->finish_buf);
      st->finish_buf = NULL;
      st->finish_buf_size = 0;
      return s;
    }

    // Record how many bytes were written
    st->finish_buf_used = gcomp_deflate_bitwriter_bytes_written(&st->bitwriter);
    st->finish_buf_ready = 1;
  }

  // Copy from finish buffer to user output
  size_t remaining = st->finish_buf_used - st->finish_buf_copied;
  size_t out_space = output->size - output->used;
  size_t to_copy = (remaining < out_space) ? remaining : out_space;

  if (to_copy > 0) {
    memcpy((uint8_t *)output->data + output->used,
        st->finish_buf + st->finish_buf_copied, to_copy);
    output->used += to_copy;
    st->finish_buf_copied += to_copy;
  }

  // Check if we've copied everything
  if (st->finish_buf_copied >= st->finish_buf_used) {
    // All done - clean up and mark complete
    gcomp_free(alloc, st->finish_buf);
    st->finish_buf = NULL;
    st->finish_buf_size = 0;
    st->finish_buf_used = 0;
    st->finish_buf_copied = 0;
    st->finish_buf_ready = 0;
    st->final_block_written = 1;
    st->stage = DEFLATE_ENC_STAGE_DONE;
    return GCOMP_OK;
  }

  // More data to copy - caller should call finish() again
  return GCOMP_ERR_LIMIT;
}
