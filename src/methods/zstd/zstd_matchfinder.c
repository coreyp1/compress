/**
 * @file zstd_matchfinder.c
 *
 * Match finding for Zstandard encoder in the Ghoti.io Compress library.
 *
 * ## Algorithm Overview
 *
 * This implements a hash chain match finder for LZ77-style compression:
 * - Each position is hashed using a 4-byte hash
 * - Hash collisions form a chain (linked list)
 * - To find matches, we look up the hash and follow the chain
 * - Chain depth is tuned by compression level
 *
 * ## Compression Levels
 *
 * - Levels 1-3: Fast mode, shallow search (4-16 iterations)
 * - Levels 4-6: Normal mode (32-64 iterations)
 * - Levels 7-12: Better compression (128-256 iterations)
 * - Levels 13-22: Best compression (512+ iterations)
 *
 * Reference:
 * https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include "zstd_internal.h"
#include <string.h>

//
// Match Finder Constants
//

#define MF_MIN_MATCH 3           ///< Minimum match length
#define MF_HASH_LOG_DEFAULT 17   ///< Default hash table log (128K entries)
#define MF_HASH_LOG_MIN 12       ///< Minimum hash table log
#define MF_HASH_LOG_MAX 20       ///< Maximum hash table log
#define MF_CHAIN_LOG_DEFAULT 16  ///< Default chain table log (64K entries)
#define MF_MAX_DISTANCE 0x7FFFFF ///< Maximum match distance (~8MB for blocks)

//
// Hash Functions
//

/**
 * @brief Compute hash for 4 bytes.
 */
static inline uint32_t zstd_mf_hash4(const uint8_t * data, unsigned hash_log) {
  // Simple multiplicative hash
  uint32_t val = gcomp_read_le32(data);
  return (val * 0x9E3779B1U) >> (32 - hash_log);
}

/**
 * @brief Compute hash for 5 bytes (better distribution for larger tables).
 */
static inline uint32_t zstd_mf_hash5(const uint8_t * data, unsigned hash_log) {
  uint64_t val = gcomp_read_le32(data);
  val |= ((uint64_t)data[4]) << 32;
  return (uint32_t)((val * 0x870DD5849ULL) >> (40 - hash_log));
}

//
// Search Depth by Level
//

static unsigned zstd_mf_get_search_depth(int level) {
  if (level <= 1) {
    return 4;
  }
  if (level <= 3) {
    return 16;
  }
  if (level <= 6) {
    return 64;
  }
  if (level <= 12) {
    return 256;
  }
  if (level <= 16) {
    return 512;
  }
  return 1024; // Levels 17-22
}

//
// Match Finder Implementation
//

/**
 * @brief Initialize match finder.
 */
gcomp_status_t zstd_mf_init(zstd_match_finder_t * mf,
    const gcomp_allocator_t * alloc, int level, size_t window_size,
    gcomp_memory_tracker_t * mem_tracker) {
  if (!mf || !alloc) {
    return GCOMP_ERR_INVALID_ARG;
  }

  memset(mf, 0, sizeof(*mf));

  // Determine hash log based on window size and level
  unsigned hash_log = MF_HASH_LOG_DEFAULT;
  if (level <= 3) {
    hash_log = 14; // 16K entries for fast levels
  }
  else if (level <= 6) {
    hash_log = 16; // 64K entries
  }
  else if (level <= 12) {
    hash_log = 17; // 128K entries
  }
  else {
    hash_log = 18; // 256K entries for high levels
  }

  // Clamp to window size (no point having more hash entries than positions)
  while (hash_log > MF_HASH_LOG_MIN && ((size_t)1 << hash_log) > window_size) {
    hash_log--;
  }

  mf->hash_log = hash_log;
  mf->hash_size = (size_t)1 << hash_log;
  mf->window_size = window_size;
  mf->search_depth = zstd_mf_get_search_depth(level);

  // Chain table size = window size (or max block size)
  mf->chain_size =
      (window_size < ZSTD_BLOCK_SIZE_MAX) ? window_size : ZSTD_BLOCK_SIZE_MAX;

  // Allocate hash table
  mf->hash_table = gcomp_malloc(alloc, mf->hash_size * sizeof(uint32_t));
  if (!mf->hash_table) {
    return GCOMP_ERR_MEMORY;
  }
  memset(mf->hash_table, 0, mf->hash_size * sizeof(uint32_t));
  if (mem_tracker) {
    gcomp_memory_track_alloc(mem_tracker, mf->hash_size * sizeof(uint32_t));
  }

  // Allocate chain table
  mf->chain_table = gcomp_malloc(alloc, mf->chain_size * sizeof(uint32_t));
  if (!mf->chain_table) {
    gcomp_free(alloc, mf->hash_table);
    mf->hash_table = NULL;
    return GCOMP_ERR_MEMORY;
  }
  memset(mf->chain_table, 0, mf->chain_size * sizeof(uint32_t));
  if (mem_tracker) {
    gcomp_memory_track_alloc(mem_tracker, mf->chain_size * sizeof(uint32_t));
  }

  return GCOMP_OK;
}

/**
 * @brief Destroy match finder.
 */
void zstd_mf_destroy(zstd_match_finder_t * mf, const gcomp_allocator_t * alloc,
    gcomp_memory_tracker_t * mem_tracker) {
  if (!mf) {
    return;
  }

  if (mf->hash_table) {
    if (mem_tracker) {
      gcomp_memory_track_free(mem_tracker, mf->hash_size * sizeof(uint32_t));
    }
    gcomp_free(alloc, mf->hash_table);
    mf->hash_table = NULL;
  }

  if (mf->chain_table) {
    if (mem_tracker) {
      gcomp_memory_track_free(mem_tracker, mf->chain_size * sizeof(uint32_t));
    }
    gcomp_free(alloc, mf->chain_table);
    mf->chain_table = NULL;
  }
}

/**
 * @brief Reset match finder for new block.
 */
void zstd_mf_reset(zstd_match_finder_t * mf) {
  if (!mf) {
    return;
  }

  if (mf->hash_table) {
    memset(mf->hash_table, 0, mf->hash_size * sizeof(uint32_t));
  }
  if (mf->chain_table) {
    memset(mf->chain_table, 0, mf->chain_size * sizeof(uint32_t));
  }
}

//
// Match Structure
//

typedef struct {
  uint32_t offset; ///< Match offset (distance back)
  uint32_t length; ///< Match length
} zstd_match_t;

/**
 * @brief Count how many bytes match at two positions.
 */
static inline size_t zstd_mf_count_match(
    const uint8_t * p1, const uint8_t * p2, const uint8_t * p1_end) {
  const uint8_t * anchor = p1;
  while (p1 < p1_end && *p1 == *p2) {
    p1++;
    p2++;
  }
  return (size_t)(p1 - anchor);
}

/**
 * @brief Find best match at current position.
 *
 * @param mf Match finder context
 * @param data Input data
 * @param pos Current position
 * @param data_size Total data size
 * @param match_out Output: best match found
 * @return true if match found, false otherwise
 */
static bool zstd_mf_find_match(zstd_match_finder_t * mf, const uint8_t * data,
    size_t pos, size_t data_size, zstd_match_t * match_out) {
  if (pos + MF_MIN_MATCH > data_size) {
    return false;
  }

  // Hash the current position
  uint32_t hash = zstd_mf_hash4(data + pos, mf->hash_log);
  uint32_t chain_pos = mf->hash_table[hash];

  // Update hash table with current position (for future references)
  mf->hash_table[hash] = (uint32_t)(pos + 1); // +1 so 0 means "no entry"

  // Update chain table
  if (pos < mf->chain_size) {
    mf->chain_table[pos] = chain_pos;
  }

  // No previous position at this hash
  if (chain_pos == 0) {
    return false;
  }
  chain_pos--; // Convert back from 1-based to 0-based

  // Search the chain for best match
  size_t best_len = MF_MIN_MATCH - 1;
  size_t best_offset = 0;
  const uint8_t * const limit = data + data_size;
  unsigned depth = mf->search_depth;

  while (depth > 0 && chain_pos < pos) {
    size_t offset = pos - chain_pos;

    // Check if offset is too large
    if (offset > MF_MAX_DISTANCE || offset > pos) {
      break;
    }

    // Quick check: compare first and last bytes before full comparison
    if (data[chain_pos] == data[pos] &&
        data[chain_pos + best_len] == data[pos + best_len]) {
      // Count matching bytes
      size_t match_len =
          zstd_mf_count_match(data + pos, data + chain_pos, limit);

      if (match_len > best_len) {
        best_len = match_len;
        best_offset = offset;

        // Early exit for very long matches
        if (match_len >= 128) {
          break;
        }
      }
    }

    // Follow chain
    if (chain_pos < mf->chain_size) {
      uint32_t next = mf->chain_table[chain_pos];
      if (next == 0 || next - 1 >= chain_pos) {
        break; // End of chain or invalid
      }
      chain_pos = next - 1;
    }
    else {
      break;
    }

    depth--;
  }

  if (best_len >= MF_MIN_MATCH && best_offset > 0) {
    match_out->offset = (uint32_t)best_offset;
    match_out->length = (uint32_t)best_len;
    return true;
  }

  return false;
}

/**
 * @brief Insert position into hash table without searching.
 *
 * Used for positions we're skipping (e.g., inside a match).
 */
static void zstd_mf_insert(zstd_match_finder_t * mf, const uint8_t * data,
    size_t pos, size_t data_size) {
  if (pos + MF_MIN_MATCH > data_size) {
    return;
  }

  uint32_t hash = zstd_mf_hash4(data + pos, mf->hash_log);
  uint32_t prev = mf->hash_table[hash];
  mf->hash_table[hash] = (uint32_t)(pos + 1);

  if (pos < mf->chain_size) {
    mf->chain_table[pos] = prev;
  }
}

//
// Sequence Generation
//

/**
 * @brief Generate sequences from input data using match finder.
 *
 * @param mf Match finder context
 * @param data Input data
 * @param data_size Input size
 * @param sequences Output sequence array
 * @param max_sequences Maximum sequences to generate
 * @param num_sequences_out Output: number of sequences generated
 * @param literals_out Output: literals buffer (caller provides)
 * @param literals_size_out Output: total literals size
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_mf_generate_sequences(zstd_match_finder_t * mf,
    const uint8_t * data, size_t data_size, zstd_sequence_t * sequences,
    size_t max_sequences, size_t * num_sequences_out, uint8_t * literals_out,
    size_t * literals_size_out, uint32_t * rep_offset_1,
    uint32_t * rep_offset_2, uint32_t * rep_offset_3) {
  if (!mf || !data || !sequences || !num_sequences_out || !literals_out ||
      !literals_size_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Reset match finder for this block
  zstd_mf_reset(mf);

  size_t pos = 0;
  size_t lit_start = 0;
  size_t num_seq = 0;
  size_t lit_pos = 0;

  // Current repeat offsets
  uint32_t rep1 = rep_offset_1 ? *rep_offset_1 : ZSTD_REP_OFFSET_1_INIT;
  uint32_t rep2 = rep_offset_2 ? *rep_offset_2 : ZSTD_REP_OFFSET_2_INIT;
  uint32_t rep3 = rep_offset_3 ? *rep_offset_3 : ZSTD_REP_OFFSET_3_INIT;

  while (pos < data_size && num_seq < max_sequences) {
    zstd_match_t match;

    // Try to find a match
    if (zstd_mf_find_match(mf, data, pos, data_size, &match)) {
      // Convert to encoded offset (zstd uses codes 1-3 for repeat offsets)
      uint32_t encoded_offset = match.offset;

      if (match.offset == rep1) {
        encoded_offset = 1;
        // rep1 stays the same
      }
      else if (match.offset == rep2) {
        encoded_offset = 2;
        // Swap rep1 and rep2
        uint32_t tmp = rep2;
        rep2 = rep1;
        rep1 = tmp;
      }
      else if (match.offset == rep3) {
        encoded_offset = 3;
        // Rotate offsets
        uint32_t tmp = rep3;
        rep3 = rep2;
        rep2 = rep1;
        rep1 = tmp;
      }
      else {
        // New offset: add 3 (offset codes 1-3 are for repeat offsets)
        encoded_offset = match.offset + 3;
        // Update repeat offsets
        rep3 = rep2;
        rep2 = rep1;
        rep1 = match.offset;
      }

      // Copy literals before this match
      size_t lit_len = pos - lit_start;
      memcpy(literals_out + lit_pos, data + lit_start, lit_len);
      lit_pos += lit_len;

      // Record sequence
      sequences[num_seq].lit_length = (uint32_t)lit_len;
      sequences[num_seq].match_offset = encoded_offset;
      sequences[num_seq].match_length = match.length;
      num_seq++;

      // Advance position
      // Insert intermediate positions into hash table for better chaining
      size_t match_end = pos + match.length;
      for (size_t i = pos + 1; i < match_end && i + MF_MIN_MATCH <= data_size;
           i++) {
        zstd_mf_insert(mf, data, i, data_size);
      }

      pos = match_end;
      lit_start = pos;
    }
    else {
      // No match found, advance by one
      pos++;
    }
  }

  // Copy remaining literals
  if (lit_start < data_size) {
    size_t remaining = data_size - lit_start;
    memcpy(literals_out + lit_pos, data + lit_start, remaining);
    lit_pos += remaining;
  }

  *num_sequences_out = num_seq;
  *literals_size_out = lit_pos;

  // Update repeat offsets
  if (rep_offset_1)
    *rep_offset_1 = rep1;
  if (rep_offset_2)
    *rep_offset_2 = rep2;
  if (rep_offset_3)
    *rep_offset_3 = rep3;

  return GCOMP_OK;
}
