/**
 * @file xxhash64.c
 *
 * Implementation of xxHash64 computation for the Ghoti.io Compress library.
 *
 * ## Algorithm
 *
 * xxHash64 is a fast, non-cryptographic hash function designed for speed.
 * It processes data in 32-byte blocks using four parallel accumulators,
 * then combines them with any remaining bytes.
 *
 * ## API Design
 *
 * The xxHash64 API provides both one-shot and streaming interfaces:
 *
 * One-shot:
 * ```c
 * uint64_t hash = gcomp_xxhash64(data, len, 0);
 * ```
 *
 * Streaming:
 * ```c
 * gcomp_xxhash64_state_t state;
 * gcomp_xxhash64_reset(&state, 0);
 * gcomp_xxhash64_update(&state, data1, len1);
 * gcomp_xxhash64_update(&state, data2, len2);
 * uint64_t hash = gcomp_xxhash64_finalize(&state);
 * ```
 *
 * ## Zstd Usage
 *
 * Zstd frame format uses xxHash64 with seed 0 for:
 * - Content checksum: low 32 bits of the full 64-bit hash
 *
 * Reference: https://github.com/Cyan4973/xxHash
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/compress/xxhash64.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

//
// xxHash64 Prime Constants
//

#define XXHASH64_PRIME1 0x9E3779B185EBCA87ULL
#define XXHASH64_PRIME2 0xC2B2AE3D27D4EB4FULL
#define XXHASH64_PRIME3 0x165667B19E3779F9ULL
#define XXHASH64_PRIME4 0x85EBCA77C2B2AE63ULL
#define XXHASH64_PRIME5 0x27D4EB2F165667C5ULL

//
// Helper Functions
//

/**
 * @brief Rotate left a 64-bit value.
 */
static inline uint64_t xxhash64_rotl(uint64_t x, int r) {
  return (x << r) | (x >> (64 - r));
}

/**
 * @brief Read a 64-bit little-endian value from memory.
 */
static inline uint64_t xxhash64_read_le64(const uint8_t * ptr) {
  return (uint64_t)ptr[0] | ((uint64_t)ptr[1] << 8) | ((uint64_t)ptr[2] << 16) |
      ((uint64_t)ptr[3] << 24) | ((uint64_t)ptr[4] << 32) |
      ((uint64_t)ptr[5] << 40) | ((uint64_t)ptr[6] << 48) |
      ((uint64_t)ptr[7] << 56);
}

/**
 * @brief Read a 32-bit little-endian value from memory.
 */
static inline uint32_t xxhash64_read_le32(const uint8_t * ptr) {
  return (uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8) | ((uint32_t)ptr[2] << 16) |
      ((uint32_t)ptr[3] << 24);
}

/**
 * @brief Process a single lane (accumulator round).
 */
static inline uint64_t xxhash64_round(uint64_t acc, uint64_t input) {
  acc += input * XXHASH64_PRIME2;
  acc = xxhash64_rotl(acc, 31);
  acc *= XXHASH64_PRIME1;
  return acc;
}

/**
 * @brief Merge an accumulator into the hash.
 */
static inline uint64_t xxhash64_merge_round(uint64_t acc, uint64_t val) {
  val = xxhash64_round(0, val);
  acc ^= val;
  acc = acc * XXHASH64_PRIME1 + XXHASH64_PRIME4;
  return acc;
}

/**
 * @brief Avalanche mix for finalization.
 */
static inline uint64_t xxhash64_avalanche(uint64_t hash) {
  hash ^= hash >> 33;
  hash *= XXHASH64_PRIME2;
  hash ^= hash >> 29;
  hash *= XXHASH64_PRIME3;
  hash ^= hash >> 32;
  return hash;
}

//
// One-Shot Interface
//

uint64_t gcomp_xxhash64(const void * data, size_t len, uint64_t seed) {
  const uint8_t * p = (const uint8_t *)data;
  const uint8_t * end = p + len;
  uint64_t hash;

  if (len >= 32) {
    // Process full 32-byte blocks with four accumulators
    uint64_t v1 = seed + XXHASH64_PRIME1 + XXHASH64_PRIME2;
    uint64_t v2 = seed + XXHASH64_PRIME2;
    uint64_t v3 = seed;
    uint64_t v4 = seed - XXHASH64_PRIME1;

    const uint8_t * limit = end - 32;
    do {
      v1 = xxhash64_round(v1, xxhash64_read_le64(p));
      p += 8;
      v2 = xxhash64_round(v2, xxhash64_read_le64(p));
      p += 8;
      v3 = xxhash64_round(v3, xxhash64_read_le64(p));
      p += 8;
      v4 = xxhash64_round(v4, xxhash64_read_le64(p));
      p += 8;
    } while (p <= limit);

    // Combine accumulators
    hash = xxhash64_rotl(v1, 1) + xxhash64_rotl(v2, 7) + xxhash64_rotl(v3, 12) +
        xxhash64_rotl(v4, 18);

    hash = xxhash64_merge_round(hash, v1);
    hash = xxhash64_merge_round(hash, v2);
    hash = xxhash64_merge_round(hash, v3);
    hash = xxhash64_merge_round(hash, v4);
  }
  else {
    // Small input: seed + PRIME5
    hash = seed + XXHASH64_PRIME5;
  }

  // Add total length
  hash += (uint64_t)len;

  // Process remaining 8-byte chunks
  while (p + 8 <= end) {
    uint64_t k1 = xxhash64_round(0, xxhash64_read_le64(p));
    hash ^= k1;
    hash = xxhash64_rotl(hash, 27) * XXHASH64_PRIME1 + XXHASH64_PRIME4;
    p += 8;
  }

  // Process remaining 4-byte chunk
  if (p + 4 <= end) {
    hash ^= (uint64_t)xxhash64_read_le32(p) * XXHASH64_PRIME1;
    hash = xxhash64_rotl(hash, 23) * XXHASH64_PRIME2 + XXHASH64_PRIME3;
    p += 4;
  }

  // Process remaining bytes
  while (p < end) {
    hash ^= (*p) * XXHASH64_PRIME5;
    hash = xxhash64_rotl(hash, 11) * XXHASH64_PRIME1;
    p++;
  }

  // Final avalanche
  return xxhash64_avalanche(hash);
}

//
// Streaming Interface
//

void gcomp_xxhash64_reset(gcomp_xxhash64_state_t * state, uint64_t seed) {
  if (!state) {
    return;
  }

  state->total_len = 0;
  state->seed = seed;
  state->memsize = 0;

  // Initialize accumulators
  state->v1 = seed + XXHASH64_PRIME1 + XXHASH64_PRIME2;
  state->v2 = seed + XXHASH64_PRIME2;
  state->v3 = seed;
  state->v4 = seed - XXHASH64_PRIME1;
}

void gcomp_xxhash64_update(
    gcomp_xxhash64_state_t * state, const void * data, size_t len) {
  if (!state || !data || len == 0) {
    return;
  }

  const uint8_t * p = (const uint8_t *)data;
  const uint8_t * end = p + len;

  state->total_len += len;

  // If we have buffered data, try to complete a 32-byte block
  if (state->memsize > 0) {
    uint32_t needed = 32 - state->memsize;
    if (len < needed) {
      // Not enough to complete a block, just buffer
      memcpy((uint8_t *)state->mem64 + state->memsize, p, len);
      state->memsize += (uint32_t)len;
      return;
    }

    // Complete the buffered block
    memcpy((uint8_t *)state->mem64 + state->memsize, p, needed);
    p += needed;

    // Process the buffered block
    const uint8_t * mem = (const uint8_t *)state->mem64;
    state->v1 = xxhash64_round(state->v1, xxhash64_read_le64(mem));
    state->v2 = xxhash64_round(state->v2, xxhash64_read_le64(mem + 8));
    state->v3 = xxhash64_round(state->v3, xxhash64_read_le64(mem + 16));
    state->v4 = xxhash64_round(state->v4, xxhash64_read_le64(mem + 24));

    state->memsize = 0;
  }

  // Process full 32-byte blocks
  if (p + 32 <= end) {
    const uint8_t * limit = end - 32;
    do {
      state->v1 = xxhash64_round(state->v1, xxhash64_read_le64(p));
      p += 8;
      state->v2 = xxhash64_round(state->v2, xxhash64_read_le64(p));
      p += 8;
      state->v3 = xxhash64_round(state->v3, xxhash64_read_le64(p));
      p += 8;
      state->v4 = xxhash64_round(state->v4, xxhash64_read_le64(p));
      p += 8;
    } while (p <= limit);
  }

  // Buffer remaining bytes
  if (p < end) {
    size_t remaining = (size_t)(end - p);
    memcpy(state->mem64, p, remaining);
    state->memsize = (uint32_t)remaining;
  }
}

uint64_t gcomp_xxhash64_finalize(const gcomp_xxhash64_state_t * state) {
  if (!state) {
    return 0;
  }

  uint64_t hash;

  if (state->total_len >= 32) {
    // Combine accumulators
    hash = xxhash64_rotl(state->v1, 1) + xxhash64_rotl(state->v2, 7) +
        xxhash64_rotl(state->v3, 12) + xxhash64_rotl(state->v4, 18);

    hash = xxhash64_merge_round(hash, state->v1);
    hash = xxhash64_merge_round(hash, state->v2);
    hash = xxhash64_merge_round(hash, state->v3);
    hash = xxhash64_merge_round(hash, state->v4);
  }
  else {
    // Small input: seed + PRIME5
    hash = state->seed + XXHASH64_PRIME5;
  }

  // Add total length
  hash += state->total_len;

  // Process buffered data
  const uint8_t * p = (const uint8_t *)state->mem64;
  const uint8_t * end = p + state->memsize;

  // Process remaining 8-byte chunks
  while (p + 8 <= end) {
    uint64_t k1 = xxhash64_round(0, xxhash64_read_le64(p));
    hash ^= k1;
    hash = xxhash64_rotl(hash, 27) * XXHASH64_PRIME1 + XXHASH64_PRIME4;
    p += 8;
  }

  // Process remaining 4-byte chunk
  if (p + 4 <= end) {
    hash ^= (uint64_t)xxhash64_read_le32(p) * XXHASH64_PRIME1;
    hash = xxhash64_rotl(hash, 23) * XXHASH64_PRIME2 + XXHASH64_PRIME3;
    p += 4;
  }

  // Process remaining bytes
  while (p < end) {
    hash ^= (*p) * XXHASH64_PRIME5;
    hash = xxhash64_rotl(hash, 11) * XXHASH64_PRIME1;
    p++;
  }

  // Final avalanche
  return xxhash64_avalanche(hash);
}
