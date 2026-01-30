/**
 * @file deflate_encode.c
 *
 * Streaming DEFLATE (RFC 1951) encoder for the Ghoti.io Compress library.
 *
 * Implements multiple compression strategies based on level:
 * - Level 0: Stored blocks (no compression)
 * - Levels 1-3: Fixed Huffman with basic LZ77
 * - Levels 4-9: Dynamic Huffman with LZ77
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

/**
 * @brief LZ77 match result.
 */
typedef struct {
  uint32_t length;   /**< Match length (0 if no match found). */
  uint32_t distance; /**< Match distance (1-based). */
} deflate_match_t;

typedef struct gcomp_deflate_encoder_state_s {
  //
  // Configuration
  //
  int level;
  size_t window_bits;
  size_t window_size;

  //
  // State machine
  //
  gcomp_deflate_encoder_stage_t stage;
  int final_block_written;

  //
  // Sliding window buffer for LZ77
  //
  uint8_t * window;
  size_t window_pos;  /**< Next write position (circular index). */
  size_t window_fill; /**< Total bytes written (capped at window_size). */
  size_t lookahead;   /**< Bytes available for matching. */
  size_t total_in;    /**< Total bytes written to window (for hash validity). */

  //
  // Hash chain for LZ77 match finding
  //
  uint16_t * hash_head; /**< Head of each hash chain. */
  uint16_t * hash_prev; /**< Previous link in hash chain. */
  size_t * hash_pos;    /**< Stream position when hash entry was inserted. */
  uint32_t hash_value;  /**< Running hash value. */

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
  uint16_t * lit_buf;  /**< Literal/length symbols. */
  uint16_t * dist_buf; /**< Distance values (0 for literals). */
  size_t sym_buf_size; /**< Capacity of symbol buffers. */
  size_t sym_buf_used; /**< Number of symbols buffered. */

  //
  // Histograms for dynamic Huffman
  //
  uint32_t * lit_freq;  /**< Literal/length frequencies. */
  uint32_t * dist_freq; /**< Distance frequencies. */

  //
  // Fixed Huffman codes (precomputed)
  //
  uint16_t fixed_lit_codes[DEFLATE_MAX_LITLEN_SYMBOLS];
  uint8_t fixed_lit_lens[DEFLATE_MAX_LITLEN_SYMBOLS];
  uint16_t fixed_dist_codes[DEFLATE_MAX_DIST_SYMBOLS];
  uint8_t fixed_dist_lens[DEFLATE_MAX_DIST_SYMBOLS];
  int fixed_ready;
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

  st->sym_buf_used = 0;
  return GCOMP_OK;
}

static gcomp_status_t deflate_flush_dynamic_block(
    gcomp_deflate_encoder_state_t * st, int final) {
  // For now, fall back to fixed Huffman
  // TODO: Implement dynamic Huffman tree building
  return deflate_flush_fixed_block(st, final);
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

  gcomp_deflate_encoder_state_t * st =
      (gcomp_deflate_encoder_state_t *)gcomp_calloc(
          alloc, 1, sizeof(gcomp_deflate_encoder_state_t));
  if (!st) {
    return GCOMP_ERR_MEMORY;
  }

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

  st->window_size = (size_t)1u << st->window_bits;
  st->stage = DEFLATE_ENC_STAGE_INIT;
  st->final_block_written = 0;

  // Allocate sliding window
  st->window = (uint8_t *)gcomp_malloc(alloc, st->window_size);
  if (!st->window) {
    gcomp_free(alloc, st);
    return GCOMP_ERR_MEMORY;
  }
  st->window_pos = 0;
  st->window_fill = 0;
  st->lookahead = 0;

  // Allocate hash tables for LZ77
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
    st->block_buffer_used = 0;
  }

  // For levels > 0, allocate symbol buffers
  if (st->level > 0) {
    // Allocate enough for a full window worth of literals
    st->sym_buf_size = st->window_size;
    st->lit_buf =
        (uint16_t *)gcomp_malloc(alloc, st->sym_buf_size * sizeof(uint16_t));
    st->dist_buf =
        (uint16_t *)gcomp_malloc(alloc, st->sym_buf_size * sizeof(uint16_t));
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
    st->sym_buf_used = 0;

    // Build fixed Huffman codes
    gcomp_status_t s = deflate_build_fixed_codes(st);
    if (s != GCOMP_OK) {
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

  st->stage = DEFLATE_ENC_STAGE_ACCEPTING;

  encoder->method_state = st;
  encoder->update_fn = gcomp_deflate_encoder_update;
  encoder->finish_fn = gcomp_deflate_encoder_finish;
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
    int max_chain = (st->level <= 3) ? 4 : (st->level <= 6) ? 32 : 128;

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
      while (st->lookahead >= DEFLATE_MIN_MATCH_LENGTH) {
        // Check if symbol buffer needs flushing
        if (st->sym_buf_used >= st->sym_buf_size - 2) {
          if (st->level <= 3) {
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

        // Try to find a match
        deflate_match_t match =
            deflate_find_match(st, pos, stream_pos, max_chain);

        if (match.length >= DEFLATE_MIN_MATCH_LENGTH) {
          // Record length/distance pair
          st->lit_buf[st->sym_buf_used] = (uint16_t)match.length;
          st->dist_buf[st->sym_buf_used] = (uint16_t)match.distance;
          st->sym_buf_used++;

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
          st->lit_buf[st->sym_buf_used] = st->window[pos];
          st->dist_buf[st->sym_buf_used] = 0;
          st->sym_buf_used++;

          // Insert byte into hash table
          if (st->lookahead >= 3) {
            deflate_insert_hash(st, pos, stream_pos);
          }
          st->lookahead--;
        }
      }

      // If we can't make progress, break
      if (copy == 0 && st->lookahead < DEFLATE_MIN_MATCH_LENGTH) {
        break;
      }
    }
  }

  output->used += gcomp_deflate_bitwriter_bytes_written(&st->bitwriter);
  return GCOMP_OK;
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

  // Set bitwriter output buffer, preserving partial bits from update() calls.
  // See comment in gcomp_deflate_encoder_update() for why this is necessary.
  gcomp_status_t s = gcomp_deflate_bitwriter_set_buffer(&st->bitwriter,
      (uint8_t *)output->data + output->used, output->size - output->used);
  if (s != GCOMP_OK) {
    return s;
  }

  if (st->level == 0) {
    // Flush remaining stored data as final block
    s = deflate_flush_stored_block(st, 1);
  }
  else {
    // Flush any remaining lookahead as literals
    while (st->lookahead > 0) {
      if (st->sym_buf_used >= st->sym_buf_size) {
        if (st->level <= 3) {
          s = deflate_flush_fixed_block(st, 0);
        }
        else {
          s = deflate_flush_dynamic_block(st, 0);
        }
        if (s != GCOMP_OK) {
          output->used += gcomp_deflate_bitwriter_bytes_written(&st->bitwriter);
          return s;
        }
      }

      size_t pos =
          (st->window_pos + st->window_size - st->lookahead) % st->window_size;
      st->lit_buf[st->sym_buf_used] = st->window[pos];
      st->dist_buf[st->sym_buf_used] = 0;
      st->sym_buf_used++;
      st->lookahead--;
    }

    // Flush final block
    if (st->level <= 3) {
      s = deflate_flush_fixed_block(st, 1);
    }
    else {
      s = deflate_flush_dynamic_block(st, 1);
    }
  }

  if (s != GCOMP_OK) {
    output->used += gcomp_deflate_bitwriter_bytes_written(&st->bitwriter);
    return s;
  }

  // Flush bitwriter to byte boundary
  s = gcomp_deflate_bitwriter_flush_to_byte(&st->bitwriter);
  if (s != GCOMP_OK) {
    output->used += gcomp_deflate_bitwriter_bytes_written(&st->bitwriter);
    return s;
  }

  output->used += gcomp_deflate_bitwriter_bytes_written(&st->bitwriter);
  st->final_block_written = 1;
  st->stage = DEFLATE_ENC_STAGE_DONE;
  return GCOMP_OK;
}
