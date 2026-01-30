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

#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "bitwriter.h"
#include "deflate_internal.h"
#include "huffman.h"
#include <ghoti.io/compress/limits.h>
#include <stdint.h>
#include <stdlib.h>
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
// - Chooses fixed or dynamic Huffman based on compression level
// - Good balance of speed and compression for general-purpose data
//
// DEFLATE_STRATEGY_FILTERED (strategy="filtered")
// -----------------------------------------------
// Optimized for pre-filtered data like PNG filter output.
// PNG filters (Sub, Up, Average, Paeth) produce data where:
// - Values cluster around zero (differences between adjacent pixels)
// - Patterns may be longer but harder to find with short hash chains
// - Longer matches tend to provide more benefit
//
// Implementation differences from DEFAULT:
// - Uses 2x longer hash chains (16/128/256 vs 8/32/64)
// - Applies lazy matching heuristic: if a short match (<32 bytes) is found,
//   checks if the next position has a longer match before committing
// - This extra effort helps find the longer patterns typical in filtered data
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
  // Configuration
  //
  int level;
  size_t window_bits;
  size_t window_size;
  gcomp_deflate_strategy_t strategy;

  //
  // Limits
  //
  uint64_t max_memory_bytes;

  //
  // Memory tracking
  //
  gcomp_memory_tracker_t mem_tracker;

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
  size_t total_in;    ///< Total bytes written to window (for hash validity).

  //
  // Hash chain for LZ77 match finding
  //
  uint16_t * hash_head; ///< Head of each hash chain.
  uint16_t * hash_prev; ///< Previous link in hash chain.
  size_t * hash_pos;    ///< Stream position when hash entry was inserted.
  uint32_t hash_value;  ///< Running hash value.

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
} gcomp_deflate_encoder_state_t;

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
    const uint8_t * data, size_t pos, size_t window_size) {
  uint32_t h = 0;
  h = deflate_hash_update(h, data[pos % window_size]);
  h = deflate_hash_update(h, data[(pos + 1) % window_size]);
  h = deflate_hash_update(h, data[(pos + 2) % window_size]);
  return h;
}

//
// Forward declarations
//

static gcomp_status_t deflate_build_fixed_codes(
    gcomp_deflate_encoder_state_t * st);
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
static uint32_t deflate_length_code(uint32_t length) {
  if (length < 3 || length > 258) {
    return 0; // Invalid
  }
  // Binary search or table lookup
  for (uint32_t i = 0; i < 29; i++) {
    uint32_t next_base = (i + 1 < 29) ? k_len_base[i + 1] : 259;
    if (length >= k_len_base[i] && length < next_base) {
      return 257 + i;
    }
  }
  return 285; // Length 258
}

/**
 * @brief Find the distance code (0..29) for a given distance (1..32768).
 */
static uint32_t deflate_distance_code(uint32_t distance) {
  if (distance < 1 || distance > 32768) {
    return 0; // Invalid
  }
  for (uint32_t i = 0; i < 30; i++) {
    uint32_t next_base = (i + 1 < 30) ? k_dist_base[i + 1] : 32769;
    if (distance >= k_dist_base[i] && distance < next_base) {
      return i;
    }
  }
  return 29; // Max distance
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

  size_t scan = pos % st->window_size;
  const uint8_t * data = st->window;
  size_t max_len = st->lookahead;
  if (max_len > DEFLATE_MAX_MATCH_LENGTH) {
    max_len = DEFLATE_MAX_MATCH_LENGTH;
  }

  uint32_t hash = deflate_hash_3bytes_wrap(data, scan, st->window_size);
  uint16_t cur = st->hash_head[hash];
  int chain_count = 0;

  while (cur != DEFLATE_NIL && chain_count < max_chain) {
    size_t match_idx = cur;

    // Check if this hash entry is still valid (not overwritten in circular buf)
    size_t match_stream_pos = st->hash_pos[match_idx];
    if (match_stream_pos >= stream_pos) {
      // Entry is from the future or current position - skip
      cur = st->hash_prev[cur];
      chain_count++;
      continue;
    }

    size_t stream_dist = stream_pos - match_stream_pos;
    if (stream_dist > st->window_size || stream_dist > DEFLATE_MAX_DISTANCE) {
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

    // Check match length
    size_t len = 0;
    while (len < max_len &&
        data[(scan + len) % st->window_size] ==
            data[(match_idx + len) % st->window_size]) {
      len++;
    }

    if (len >= DEFLATE_MIN_MATCH_LENGTH && len > result.length) {
      result.length = (uint32_t)len;
      result.distance = (uint32_t)stream_dist;

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
 * @param st Encoder state
 * @param pos Position in circular window buffer
 * @param stream_pos Position in the total input stream (for validity checking)
 */
static void deflate_insert_hash(
    gcomp_deflate_encoder_state_t * st, size_t pos, size_t stream_pos) {
  if (!st || !st->window || st->lookahead < 3) {
    return;
  }

  size_t idx = pos % st->window_size;
  uint32_t hash = deflate_hash_3bytes_wrap(st->window, idx, st->window_size);

  st->hash_prev[idx] = st->hash_head[hash];
  st->hash_head[hash] = (uint16_t)idx;
  st->hash_pos[idx] = stream_pos; // Record stream position for validity check
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
      uint32_t len_code = deflate_length_code(lit);
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
      uint32_t dist_code = deflate_distance_code(dist);
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
 * 2. HUFFMAN TREE CONSTRUCTION (build_code_lengths):
 *    - Create leaf nodes for symbols with freq > 0
 *    - Build min-heap ordered by frequency
 *    - Repeatedly extract two minimum nodes and combine into internal node
 *    - This produces an optimal prefix-free code (Huffman's algorithm)
 *
 * 3. LENGTH EXTRACTION AND LIMITING:
 *    - Walk tree depth-first to determine code length for each symbol
 *    - DEFLATE limits code lengths to 15 bits maximum
 *    - If any code exceeds 15 bits, apply length-limiting adjustment:
 *      a. Cap all lengths at 15
 *      b. Verify Kraft inequality: sum(2^(-length)) <= 1
 *      c. If over-subscribed, increment some lengths to reduce code space
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
 * - Heap-based tree construction is O(n log n) and memory-efficient
 * - The 15-bit length limit is mandated by RFC 1951; we enforce it with
 *   a post-processing step rather than package-merge for simplicity
 * - Fallback to fixed Huffman occurs on memory allocation failure
 * - Empty distance trees are valid when no LZ77 matches are used (e.g.,
 *   incompressible data or very short inputs)
 *
 * MEMORY LAYOUT
 * -------------
 * - lit_freq: 288 uint32_t entries (allocated for levels > 3)
 * - dist_freq: 32 uint32_t entries (allocated for levels > 3)
 * - Temporary allocations in build_code_lengths: ~2*num_symbols nodes
 *
 * ===========================================================================
 */

/**
 * @brief Node structure for Huffman tree construction.
 *
 * Used during the heap-based Huffman tree building process. Leaf nodes
 * represent symbols and have symbol >= 0 with left/right = -1. Internal
 * nodes combine two children and have symbol = -1.
 *
 * The nodes array is used as both storage and a min-heap (via separate
 * indices array). This avoids copying node data during heap operations.
 */
typedef struct {
  uint32_t freq;  ///< Symbol frequency (leaves) or combined freq (internal)
  int16_t symbol; ///< Symbol value (0-285) or -1 for internal nodes
  int16_t left;   ///< Index of left child in nodes array, or -1
  int16_t right;  ///< Index of right child in nodes array, or -1
} huffman_node_t;

/**
 * @brief Min-heap sift down operation.
 */
static void heap_sift_down(
    huffman_node_t * heap, int * indices, int size, int i) {
  while (1) {
    int smallest = i;
    int left = 2 * i + 1;
    int right = 2 * i + 2;

    if (left < size &&
        heap[indices[left]].freq < heap[indices[smallest]].freq) {
      smallest = left;
    }
    if (right < size &&
        heap[indices[right]].freq < heap[indices[smallest]].freq) {
      smallest = right;
    }
    if (smallest == i) {
      break;
    }
    int tmp = indices[i];
    indices[i] = indices[smallest];
    indices[smallest] = tmp;
    i = smallest;
  }
}

/**
 * @brief Min-heap sift up operation.
 */
static void heap_sift_up(huffman_node_t * heap, int * indices, int i) {
  while (i > 0) {
    int parent = (i - 1) / 2;
    if (heap[indices[parent]].freq <= heap[indices[i]].freq) {
      break;
    }
    int tmp = indices[i];
    indices[i] = indices[parent];
    indices[parent] = tmp;
    i = parent;
  }
}

/**
 * @brief Extract code lengths from Huffman tree.
 */
static void extract_lengths(
    huffman_node_t * nodes, int root, uint8_t * lengths, int depth) {
  if (root < 0) {
    return;
  }
  if (nodes[root].symbol >= 0) {
    // Leaf node
    lengths[nodes[root].symbol] = (uint8_t)(depth > 15 ? 15 : depth);
    return;
  }
  extract_lengths(nodes, nodes[root].left, lengths, depth + 1);
  extract_lengths(nodes, nodes[root].right, lengths, depth + 1);
}

/**
 * @brief Build optimal Huffman code lengths from symbol frequencies.
 *
 * Uses the classic Huffman algorithm with a min-heap to construct an optimal
 * prefix-free code. The algorithm is O(n log n) where n is the number of
 * symbols with non-zero frequency.
 *
 * @param freq       Array of symbol frequencies (freq[i] = count of symbol i)
 * @param num_symbols Number of symbols in the frequency array
 * @param lengths    Output array for code lengths (same size as freq)
 * @param max_bits   Maximum allowed code length (15 for DEFLATE)
 *
 * Special cases:
 * - 0 symbols with freq > 0: all lengths set to 0
 * - 1 symbol with freq > 0: that symbol gets length 1
 * - Memory allocation failure: fallback to uniform 8-bit lengths
 *
 * The algorithm handles the DEFLATE 15-bit length limit by:
 * 1. Building the unconstrained Huffman tree
 * 2. Capping any lengths > max_bits
 * 3. Adjusting via Kraft inequality to ensure valid prefix code
 */
static void build_code_lengths(const uint32_t * freq, size_t num_symbols,
    uint8_t * lengths, unsigned max_bits) {
  // Count non-zero frequencies
  int count = 0;
  for (size_t i = 0; i < num_symbols; i++) {
    lengths[i] = 0;
    if (freq[i] > 0) {
      count++;
    }
  }

  if (count == 0) {
    return;
  }

  if (count == 1) {
    // Single symbol gets length 1
    for (size_t i = 0; i < num_symbols; i++) {
      if (freq[i] > 0) {
        lengths[i] = 1;
        break;
      }
    }
    return;
  }

  // Allocate nodes: up to num_symbols leaves + (num_symbols-1) internal nodes
  size_t max_nodes = num_symbols * 2;
  huffman_node_t * nodes =
      (huffman_node_t *)malloc(max_nodes * sizeof(huffman_node_t));
  int * heap = (int *)malloc(max_nodes * sizeof(int));
  if (!nodes || !heap) {
    free(heap);
    free(nodes);
    // Fallback: use uniform lengths
    for (size_t i = 0; i < num_symbols; i++) {
      if (freq[i] > 0) {
        lengths[i] = 8;
      }
    }
    return;
  }

  // Initialize leaf nodes
  int node_count = 0;
  int heap_size = 0;
  for (size_t i = 0; i < num_symbols; i++) {
    if (freq[i] > 0) {
      nodes[node_count].freq = freq[i];
      nodes[node_count].symbol = (int16_t)i;
      nodes[node_count].left = -1;
      nodes[node_count].right = -1;
      heap[heap_size] = node_count;
      heap_size++;
      node_count++;
    }
  }

  // Build heap
  for (int i = heap_size / 2 - 1; i >= 0; i--) {
    heap_sift_down(nodes, heap, heap_size, i);
  }

  // Build Huffman tree
  while (heap_size > 1) {
    // Extract two minimum nodes
    int min1 = heap[0];
    heap[0] = heap[heap_size - 1];
    heap_size--;
    heap_sift_down(nodes, heap, heap_size, 0);

    int min2 = heap[0];
    heap[0] = heap[heap_size - 1];
    heap_size--;
    heap_sift_down(nodes, heap, heap_size, 0);

    // Create internal node
    nodes[node_count].freq = nodes[min1].freq + nodes[min2].freq;
    nodes[node_count].symbol = -1;
    nodes[node_count].left = (int16_t)min1;
    nodes[node_count].right = (int16_t)min2;

    // Add to heap
    heap[heap_size] = node_count;
    heap_size++;
    heap_sift_up(nodes, heap, heap_size - 1);
    node_count++;
  }

  // Extract lengths
  int root = heap[0];
  extract_lengths(nodes, root, lengths, 0);

  // Limit code lengths to max_bits using the package-merge simplification:
  // If any length > max_bits, reduce it and compensate by reducing others
  int need_adjustment = 0;
  for (size_t i = 0; i < num_symbols; i++) {
    if (lengths[i] > max_bits) {
      need_adjustment = 1;
      break;
    }
  }

  if (need_adjustment) {
    // Simple length limiting: cap at max_bits and rebalance
    // This is a simplified approach that may not be optimal but produces
    // valid codes
    for (size_t i = 0; i < num_symbols; i++) {
      if (lengths[i] > max_bits) {
        lengths[i] = (uint8_t)max_bits;
      }
    }

    // Verify/fix the code is valid (Kraft inequality)
    // Sum of 2^(-length) must be <= 1
    uint32_t kraft = 0;
    for (size_t i = 0; i < num_symbols; i++) {
      if (lengths[i] > 0) {
        kraft += 1u << (max_bits - lengths[i]);
      }
    }

    // If over-subscribed, increase some lengths
    while (kraft > (1u << max_bits)) {
      for (size_t i = 0; i < num_symbols && kraft > (1u << max_bits); i++) {
        if (lengths[i] > 0 && lengths[i] < max_bits) {
          kraft -= 1u << (max_bits - lengths[i]);
          lengths[i]++;
          kraft += 1u << (max_bits - lengths[i]);
        }
      }
    }
  }

  free(heap);
  free(nodes);
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
    // Fall back to fixed if no frequency data
    return deflate_flush_fixed_block(st, final);
  }

  gcomp_status_t s;

  // Ensure end-of-block symbol is counted
  st->lit_freq[256]++;

  // Build code lengths for literal/length alphabet
  uint8_t lit_lengths[DEFLATE_MAX_LITLEN_SYMBOLS];
  build_code_lengths(st->lit_freq, DEFLATE_MAX_LITLEN_SYMBOLS, lit_lengths, 15);

  // Ensure end-of-block (256) has a code
  if (lit_lengths[256] == 0) {
    lit_lengths[256] = 1;
  }

  // Build code lengths for distance alphabet
  uint8_t dist_lengths[DEFLATE_MAX_DIST_SYMBOLS];
  build_code_lengths(st->dist_freq, DEFLATE_MAX_DIST_SYMBOLS, dist_lengths, 15);

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

  // Ensure at least one distance code (DEFLATE requires it).
  // When no distance codes are used (e.g., huffman_only strategy), all
  // dist_lengths are 0. The while loop stops at hdist == 0, but we still
  // need to ensure dist_lengths[0] has a valid code length.
  if (dist_lengths[0] == 0) {
    dist_lengths[0] = 1;
  }

  // Combine lit/dist lengths for encoding
  size_t total_codes = (size_t)(257 + hlit + 1 + hdist);
  uint8_t * all_lengths = (uint8_t *)malloc(total_codes);
  if (!all_lengths) {
    // Fall back to fixed on memory error
    st->lit_freq[256]--;
    return deflate_flush_fixed_block(st, final);
  }

  memcpy(all_lengths, lit_lengths, 257 + hlit);
  memcpy(all_lengths + 257 + hlit, dist_lengths, 1 + hdist);

  // Run-length encode the code lengths using the code-length alphabet.
  // This compresses the sequence of code lengths by encoding runs:
  //   0-15: Literal code length value (no extra bits)
  //   16:   Copy previous code length 3-6 times (2 extra bits: 0-3)
  //   17:   Repeat code length 0 for 3-10 times (3 extra bits: 0-7)
  //   18:   Repeat code length 0 for 11-138 times (7 extra bits: 0-127)
  // The cl_symbols array stores the symbol to emit, cl_extra stores extra bits.
  uint8_t
      cl_symbols[DEFLATE_MAX_LITLEN_SYMBOLS + DEFLATE_MAX_DIST_SYMBOLS + 32];
  uint8_t cl_extra[DEFLATE_MAX_LITLEN_SYMBOLS + DEFLATE_MAX_DIST_SYMBOLS + 32];
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

  // Build code length Huffman tree
  uint32_t cl_freq[19] = {0};
  for (size_t i = 0; i < cl_count; i++) {
    cl_freq[cl_symbols[i]]++;
  }

  uint8_t cl_lengths[19];
  build_code_lengths(cl_freq, 19, cl_lengths, 7);

  // =========================================================================
  // Code-length alphabet completeness (zlib compatibility)
  // =========================================================================
  // RFC 1951 allows incomplete Huffman trees (Kraft sum < 2^max_bits), but
  // zlib's inflate_table() function rejects incomplete trees for the CODES
  // type (code-length alphabet). This is stricter than the RFC requires.
  //
  // Background: The Kraft inequality states that for a valid prefix code,
  // sum(2^(-length_i)) <= 1. An "under-subscribed" or "incomplete" tree has
  // sum < 1, meaning some bit patterns don't decode to any symbol. RFC 1951
  // permits this (unused patterns simply never appear in the stream).
  //
  // However, zlib's decoder checks: if (left > 0) return -1; // incomplete
  // This occurs specifically for the code-length alphabet (symbols 0-18 that
  // encode the lit/len and distance code lengths).
  //
  // Solution: If our code-length alphabet is under-subscribed, add unused
  // symbols with appropriate code lengths to make the Kraft sum exactly 2^7.
  // We prefer symbols late in k_cl_order[] to minimize HCLEN (the count of
  // code-length codes transmitted in the block header).
  // =========================================================================
  {
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

  // Determine HCLEN (number of code length codes - 4)
  int hclen = 19 - 4;
  while (hclen > 0 && cl_lengths[k_cl_order[hclen + 3]] == 0) {
    hclen--;
  }

  // Build canonical codes for code lengths
  uint16_t cl_codes[19];
  s = gcomp_deflate_huffman_build_codes(cl_lengths, 19, 7, cl_codes, NULL);
  if (s != GCOMP_OK) {
    free(all_lengths);
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
    free(all_lengths);
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
    free(all_lengths);
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
    free(all_lengths);
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, 2u, 2); // BTYPE=10
  if (s != GCOMP_OK) {
    free(all_lengths);
    return s;
  }

  // Write HLIT (5 bits), HDIST (5 bits), HCLEN (4 bits)
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, (uint32_t)hlit, 5);
  if (s != GCOMP_OK) {
    free(all_lengths);
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, (uint32_t)hdist, 5);
  if (s != GCOMP_OK) {
    free(all_lengths);
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, (uint32_t)hclen, 4);
  if (s != GCOMP_OK) {
    free(all_lengths);
    return s;
  }

  // Write code length code lengths (3 bits each, in permuted order)
  for (int i = 0; i < hclen + 4; i++) {
    s = gcomp_deflate_bitwriter_write_bits(
        &st->bitwriter, cl_lengths[k_cl_order[i]], 3);
    if (s != GCOMP_OK) {
      free(all_lengths);
      return s;
    }
  }

  // Write the code lengths for lit/len and dist alphabets
  for (size_t i = 0; i < cl_count; i++) {
    uint8_t sym = cl_symbols[i];
    s = gcomp_deflate_bitwriter_write_bits(
        &st->bitwriter, cl_codes[sym], cl_lengths[sym]);
    if (s != GCOMP_OK) {
      free(all_lengths);
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
      free(all_lengths);
      return s;
    }
  }

  free(all_lengths);

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
      uint32_t len_code = deflate_length_code(lit);
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
      uint32_t dist_code = deflate_distance_code(dist);
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

  // Reset for next block
  st->sym_buf_used = 0;
  memset(st->lit_freq, 0, DEFLATE_MAX_LITLEN_SYMBOLS * sizeof(uint32_t));
  memset(st->dist_freq, 0, DEFLATE_MAX_DIST_SYMBOLS * sizeof(uint32_t));

  return GCOMP_OK;
}

//
// Public API
//

gcomp_status_t gcomp_deflate_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  if (!registry || !encoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

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
  st->stage = DEFLATE_ENC_STAGE_INIT;
  st->final_block_written = 0;

  // Allocate sliding window
  st->window = (uint8_t *)gcomp_malloc(alloc, st->window_size);
  if (!st->window) {
    gcomp_free(alloc, st);
    return GCOMP_ERR_MEMORY;
  }
  gcomp_memory_track_alloc(&st->mem_tracker, st->window_size);

  st->window_pos = 0;
  st->window_fill = 0;
  st->lookahead = 0;

  // Allocate hash tables for LZ77
  size_t hash_head_size = DEFLATE_HASH_SIZE * sizeof(uint16_t);
  size_t hash_prev_size = st->window_size * sizeof(uint16_t);
  size_t hash_pos_size = st->window_size * sizeof(size_t);

  st->hash_head =
      (uint16_t *)gcomp_calloc(alloc, DEFLATE_HASH_SIZE, sizeof(uint16_t));
  st->hash_prev =
      (uint16_t *)gcomp_calloc(alloc, st->window_size, sizeof(uint16_t));
  st->hash_pos = (size_t *)gcomp_calloc(alloc, st->window_size, sizeof(size_t));
  if (!st->hash_head || !st->hash_prev || !st->hash_pos) {
    gcomp_free(alloc, st->hash_pos);
    gcomp_free(alloc, st->hash_prev);
    gcomp_free(alloc, st->hash_head);
    gcomp_free(alloc, st->window);
    gcomp_free(alloc, st);
    return GCOMP_ERR_MEMORY;
  }
  gcomp_memory_track_alloc(&st->mem_tracker, hash_head_size);
  gcomp_memory_track_alloc(&st->mem_tracker, hash_prev_size);
  gcomp_memory_track_alloc(&st->mem_tracker, hash_pos_size);

  st->total_in = 0;

  // For level 0, allocate block buffer
  if (st->level == 0) {
    st->block_buffer_size = DEFLATE_MAX_STORED_BLOCK;
    st->block_buffer = (uint8_t *)gcomp_malloc(alloc, st->block_buffer_size);
    if (!st->block_buffer) {
      gcomp_free(alloc, st->hash_pos);
      gcomp_free(alloc, st->hash_prev);
      gcomp_free(alloc, st->hash_head);
      gcomp_free(alloc, st->window);
      gcomp_free(alloc, st);
      return GCOMP_ERR_MEMORY;
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
      gcomp_free(alloc, st->dist_buf);
      gcomp_free(alloc, st->lit_buf);
      gcomp_free(alloc, st->hash_pos);
      gcomp_free(alloc, st->hash_prev);
      gcomp_free(alloc, st->hash_head);
      gcomp_free(alloc, st->window);
      gcomp_free(alloc, st);
      return GCOMP_ERR_MEMORY;
    }
    gcomp_memory_track_alloc(&st->mem_tracker, sym_buf_bytes); // lit_buf
    gcomp_memory_track_alloc(&st->mem_tracker, sym_buf_bytes); // dist_buf
    st->sym_buf_used = 0;

    // For levels > 3, allocate frequency histograms for dynamic Huffman
    if (st->level > 3) {
      size_t lit_freq_size = DEFLATE_MAX_LITLEN_SYMBOLS * sizeof(uint32_t);
      size_t dist_freq_size = DEFLATE_MAX_DIST_SYMBOLS * sizeof(uint32_t);

      st->lit_freq = (uint32_t *)gcomp_calloc(
          alloc, DEFLATE_MAX_LITLEN_SYMBOLS, sizeof(uint32_t));
      st->dist_freq = (uint32_t *)gcomp_calloc(
          alloc, DEFLATE_MAX_DIST_SYMBOLS, sizeof(uint32_t));
      if (!st->lit_freq || !st->dist_freq) {
        gcomp_free(alloc, st->dist_freq);
        gcomp_free(alloc, st->lit_freq);
        gcomp_free(alloc, st->dist_buf);
        gcomp_free(alloc, st->lit_buf);
        gcomp_free(alloc, st->hash_pos);
        gcomp_free(alloc, st->hash_prev);
        gcomp_free(alloc, st->hash_head);
        gcomp_free(alloc, st->window);
        gcomp_free(alloc, st);
        return GCOMP_ERR_MEMORY;
      }
      gcomp_memory_track_alloc(&st->mem_tracker, lit_freq_size);
      gcomp_memory_track_alloc(&st->mem_tracker, dist_freq_size);
    }

    // Build fixed Huffman codes
    gcomp_status_t s = deflate_build_fixed_codes(st);
    if (s != GCOMP_OK) {
      gcomp_free(alloc, st->dist_freq);
      gcomp_free(alloc, st->lit_freq);
      gcomp_free(alloc, st->dist_buf);
      gcomp_free(alloc, st->lit_buf);
      gcomp_free(alloc, st->hash_pos);
      gcomp_free(alloc, st->hash_prev);
      gcomp_free(alloc, st->hash_head);
      gcomp_free(alloc, st->window);
      gcomp_free(alloc, st);
      return s;
    }
  }

  // Check memory limit after all allocations
  gcomp_status_t mem_check =
      gcomp_memory_check_limit(&st->mem_tracker, st->max_memory_bytes);
  if (mem_check != GCOMP_OK) {
    gcomp_free(alloc, st->dist_freq);
    gcomp_free(alloc, st->lit_freq);
    gcomp_free(alloc, st->dist_buf);
    gcomp_free(alloc, st->lit_buf);
    gcomp_free(alloc, st->block_buffer);
    gcomp_free(alloc, st->hash_pos);
    gcomp_free(alloc, st->hash_prev);
    gcomp_free(alloc, st->hash_head);
    gcomp_free(alloc, st->window);
    gcomp_free(alloc, st);
    return mem_check;
  }

  st->stage = DEFLATE_ENC_STAGE_ACCEPTING;

  encoder->method_state = st;
  encoder->update_fn = gcomp_deflate_encoder_update;
  encoder->finish_fn = gcomp_deflate_encoder_finish;
  encoder->reset_fn = gcomp_deflate_encoder_reset;
  return GCOMP_OK;
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

  gcomp_free(alloc, st->finish_buf);
  gcomp_free(alloc, st->dist_freq);
  gcomp_free(alloc, st->lit_freq);
  gcomp_free(alloc, st->dist_buf);
  gcomp_free(alloc, st->lit_buf);
  gcomp_free(alloc, st->block_buffer);
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
    return GCOMP_ERR_INTERNAL;
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

  // Reset hash tables (clear to zeros)
  memset(st->hash_head, 0, DEFLATE_HASH_SIZE * sizeof(uint16_t));
  memset(st->hash_prev, 0, st->window_size * sizeof(uint16_t));
  memset(st->hash_pos, 0, st->window_size * sizeof(size_t));
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

  // Note: We don't reset fixed_ready because the fixed Huffman tables don't
  // need to be rebuilt - they're static and can be reused.

  return GCOMP_OK;
}

gcomp_status_t gcomp_deflate_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!encoder || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_deflate_encoder_state_t * st =
      (gcomp_deflate_encoder_state_t *)encoder->method_state;
  if (!st) {
    return GCOMP_ERR_INTERNAL;
  }

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
  gcomp_status_t s = gcomp_deflate_bitwriter_set_buffer(&st->bitwriter,
      (uint8_t *)output->data + output->used, output->size - output->used);
  if (s != GCOMP_OK) {
    return s;
  }

  const uint8_t * src = (const uint8_t *)input->data;

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
          output->used += gcomp_deflate_bitwriter_bytes_written(&st->bitwriter);
          return s;
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

    // Determine hash chain length based on level and strategy
    if (st->strategy == DEFLATE_STRATEGY_FILTERED) {
      // Filtered strategy uses longer chains for better matching
      max_chain = (st->level <= 3) ? 16 : (st->level <= 6) ? 128 : 256;
    }
    else if (st->strategy == DEFLATE_STRATEGY_RLE) {
      // RLE doesn't use hash chains (only checks distance 1)
      max_chain = 0;
    }
    else {
      // Default/Fixed: standard chain lengths
      max_chain = (st->level <= 3) ? 4 : (st->level <= 6) ? 32 : 128;
    }

    // Determine whether to use fixed or dynamic Huffman
    // FIXED strategy always uses fixed codes; otherwise depends on level
    use_fixed_huffman =
        (st->strategy == DEFLATE_STRATEGY_FIXED) || (st->level <= 3);

    while (input->used < input->size) {
      // Fill window with input data
      size_t avail = input->size - input->used;
      size_t space = st->window_size - st->lookahead;
      size_t copy = (avail < space) ? avail : space;

      if (copy > 0) {
        for (size_t i = 0; i < copy; i++) {
          st->window[st->window_pos] = src[input->used + i];
          st->window_pos = (st->window_pos + 1) % st->window_size;
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
      while (st->lookahead >= DEFLATE_MIN_MATCH_LENGTH ||
          (skip_lz77 && st->lookahead > 0)) {
        // Check if symbol buffer needs flushing
        if (st->sym_buf_used >= st->sym_buf_size - 2) {
          if (use_fixed_huffman) {
            s = deflate_flush_fixed_block(st, 0);
          }
          else {
            s = deflate_flush_dynamic_block(st, 0);
          }
          if (s != GCOMP_OK) {
            output->used +=
                gcomp_deflate_bitwriter_bytes_written(&st->bitwriter);
            return s;
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
          // RLE: Only look for matches at distance 1
          if (st->lookahead >= DEFLATE_MIN_MATCH_LENGTH && stream_pos > 0) {
            // Check for run at distance 1
            size_t prev_pos = (pos + st->window_size - 1) % st->window_size;
            uint8_t run_byte = st->window[prev_pos];
            size_t run_len = 0;
            size_t max_len = st->lookahead;
            if (max_len > DEFLATE_MAX_MATCH_LENGTH) {
              max_len = DEFLATE_MAX_MATCH_LENGTH;
            }

            // Count how many bytes match the previous byte
            while (run_len < max_len &&
                st->window[(pos + run_len) % st->window_size] == run_byte) {
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

          // FILTERED strategy: apply lazy matching heuristic
          // If we found a short match, look ahead for a better one
          if (st->strategy == DEFLATE_STRATEGY_FILTERED &&
              match.length >= DEFLATE_MIN_MATCH_LENGTH && match.length < 32 &&
              st->lookahead > match.length) {
            // Check if the next position has a longer match
            size_t next_pos = (pos + 1) % st->window_size;
            deflate_match_t next_match =
                deflate_find_match(st, next_pos, stream_pos + 1, max_chain);
            // If next match is significantly better, emit current as literal
            if (next_match.length > match.length + 1) {
              match.length = 0; // Force literal emission
            }
          }
        }

        if (match.length >= DEFLATE_MIN_MATCH_LENGTH) {
          // Record length/distance pair
          st->lit_buf[st->sym_buf_used] = (uint16_t)match.length;
          st->dist_buf[st->sym_buf_used] = (uint16_t)match.distance;
          st->sym_buf_used++;

          // Track frequencies for dynamic Huffman
          if (st->lit_freq) {
            uint32_t len_code = deflate_length_code(match.length);
            st->lit_freq[len_code]++;
            uint32_t dist_code = deflate_distance_code(match.distance);
            st->dist_freq[dist_code]++;
          }

          // Insert all bytes of the match into the hash table
          for (uint32_t i = 0; i < match.length && st->lookahead > 0; i++) {
            if (st->lookahead >= 3) {
              deflate_insert_hash(st, pos, stream_pos);
            }
            pos = (pos + 1) % st->window_size;
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

      // If we can't make progress, break
      if (copy == 0 && st->lookahead < DEFLATE_MIN_MATCH_LENGTH &&
          !(skip_lz77 && st->lookahead > 0)) {
        break;
      }
    }
  }

  output->used += gcomp_deflate_bitwriter_bytes_written(&st->bitwriter);
  return GCOMP_OK;
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
    return num_blocks * 5 + data_bytes + 8; // +8 for safety margin
  }

  // For Huffman blocks, estimate based on symbols
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
  size_t block_overhead = num_blocks * (tree_overhead + 8);

  // Byte alignment (up to 7 bits = 1 byte)
  size_t alignment = 1;

  // Safety margin
  size_t margin = 64;

  return symbol_bytes + block_overhead + alignment + margin;
}

gcomp_status_t gcomp_deflate_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  if (!encoder || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_deflate_encoder_state_t * st =
      (gcomp_deflate_encoder_state_t *)encoder->method_state;
  if (!st) {
    return GCOMP_ERR_INTERNAL;
  }

  if (st->final_block_written) {
    return GCOMP_OK;
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
      return GCOMP_ERR_MEMORY;
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
