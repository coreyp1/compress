/**
 * @file xxhash32.c
 *
 * Implementation of xxHash32 computation for the Ghoti.io Compress library.
 *
 * ## Algorithm
 *
 * xxHash32 is a fast, non-cryptographic hash function designed for speed.
 * It processes data in 16-byte blocks using four parallel accumulators,
 * then combines them with any remaining bytes.
 *
 * ## API Design
 *
 * The xxHash32 API provides both one-shot and streaming interfaces:
 *
 * One-shot:
 * ```c
 * uint32_t hash = gcomp_xxhash32(data, len, 0);
 * ```
 *
 * Streaming:
 * ```c
 * gcomp_xxhash32_state_t state;
 * gcomp_xxhash32_reset(&state, 0);
 * gcomp_xxhash32_update(&state, data1, len1);
 * gcomp_xxhash32_update(&state, data2, len2);
 * uint32_t hash = gcomp_xxhash32_finalize(&state);
 * ```
 *
 * ## LZ4 Usage
 *
 * LZ4 frame format uses xxHash32 with seed 0 for:
 * - Header checksum: (xxhash32 >> 8) & 0xFF (second byte of hash)
 * - Block checksum: full 32-bit hash
 * - Content checksum: full 32-bit hash
 *
 * Reference: https://github.com/Cyan4973/xxHash
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/compress/xxhash32.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

//
// xxHash32 Prime Constants
//

#define XXHASH32_PRIME1 0x9E3779B1U
#define XXHASH32_PRIME2 0x85EBCA77U
#define XXHASH32_PRIME3 0xC2B2AE3DU
#define XXHASH32_PRIME4 0x27D4EB2FU
#define XXHASH32_PRIME5 0x165667B1U

//
// Helper Functions
//

/**
 * @brief Rotate left a 32-bit value.
 */
static inline uint32_t xxhash32_rotl(uint32_t x, int r) {
  return (x << r) | (x >> (32 - r));
}

/**
 * @brief Read a 32-bit little-endian value from memory.
 */
static inline uint32_t xxhash32_read_le32(const uint8_t * ptr) {
  return (uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8) | ((uint32_t)ptr[2] << 16) |
      ((uint32_t)ptr[3] << 24);
}

/**
 * @brief Process a single lane (accumulator round).
 */
static inline uint32_t xxhash32_round(uint32_t acc, uint32_t input) {
  acc += input * XXHASH32_PRIME2;
  acc = xxhash32_rotl(acc, 13);
  acc *= XXHASH32_PRIME1;
  return acc;
}

/**
 * @brief Avalanche mix for finalization.
 */
static inline uint32_t xxhash32_avalanche(uint32_t hash) {
  hash ^= hash >> 15;
  hash *= XXHASH32_PRIME2;
  hash ^= hash >> 13;
  hash *= XXHASH32_PRIME3;
  hash ^= hash >> 16;
  return hash;
}

//
// One-Shot Interface
//

uint32_t gcomp_xxhash32(const void * data, size_t len, uint32_t seed) {
  const uint8_t * p = (const uint8_t *)data;
  const uint8_t * end = p + len;
  uint32_t hash;

  if (len >= 16) {
    // Process full 16-byte blocks with four accumulators
    uint32_t v1 = seed + XXHASH32_PRIME1 + XXHASH32_PRIME2;
    uint32_t v2 = seed + XXHASH32_PRIME2;
    uint32_t v3 = seed;
    uint32_t v4 = seed - XXHASH32_PRIME1;

    const uint8_t * limit = end - 16;
    do {
      v1 = xxhash32_round(v1, xxhash32_read_le32(p));
      p += 4;
      v2 = xxhash32_round(v2, xxhash32_read_le32(p));
      p += 4;
      v3 = xxhash32_round(v3, xxhash32_read_le32(p));
      p += 4;
      v4 = xxhash32_round(v4, xxhash32_read_le32(p));
      p += 4;
    } while (p <= limit);

    // Combine accumulators
    hash = xxhash32_rotl(v1, 1) + xxhash32_rotl(v2, 7) + xxhash32_rotl(v3, 12) +
        xxhash32_rotl(v4, 18);
  }
  else {
    // Small input: seed + PRIME5
    hash = seed + XXHASH32_PRIME5;
  }

  // Add total length
  hash += (uint32_t)len;

  // Process remaining 4-byte chunks
  while (p + 4 <= end) {
    hash += xxhash32_read_le32(p) * XXHASH32_PRIME3;
    hash = xxhash32_rotl(hash, 17) * XXHASH32_PRIME4;
    p += 4;
  }

  // Process remaining bytes
  while (p < end) {
    hash += (*p) * XXHASH32_PRIME5;
    hash = xxhash32_rotl(hash, 11) * XXHASH32_PRIME1;
    p++;
  }

  // Final avalanche
  return xxhash32_avalanche(hash);
}

//
// Streaming Interface
//

void gcomp_xxhash32_reset(gcomp_xxhash32_state_t * state, uint32_t seed) {
  if (!state) {
    return;
  }

  state->total_len = 0;
  state->seed = seed;
  state->memsize = 0;

  // Initialize accumulators
  state->v1 = seed + XXHASH32_PRIME1 + XXHASH32_PRIME2;
  state->v2 = seed + XXHASH32_PRIME2;
  state->v3 = seed;
  state->v4 = seed - XXHASH32_PRIME1;
}

void gcomp_xxhash32_update(
    gcomp_xxhash32_state_t * state, const void * data, size_t len) {
  if (!state || !data || len == 0) {
    return;
  }

  const uint8_t * p = (const uint8_t *)data;
  const uint8_t * end = p + len;

  state->total_len += (uint32_t)len;

  // If we have buffered data, try to complete a 16-byte block
  if (state->memsize > 0) {
    uint32_t needed = 16 - state->memsize;
    if (len < needed) {
      // Not enough to complete a block, just buffer
      memcpy((uint8_t *)state->mem32 + state->memsize, p, len);
      state->memsize += (uint32_t)len;
      return;
    }

    // Complete the buffered block
    memcpy((uint8_t *)state->mem32 + state->memsize, p, needed);
    p += needed;

    // Process the buffered block
    state->v1 = xxhash32_round(state->v1, state->mem32[0]);
    state->v2 = xxhash32_round(state->v2, state->mem32[1]);
    state->v3 = xxhash32_round(state->v3, state->mem32[2]);
    state->v4 = xxhash32_round(state->v4, state->mem32[3]);

    state->memsize = 0;
  }

  // Process full 16-byte blocks
  if (p + 16 <= end) {
    const uint8_t * limit = end - 16;
    do {
      state->v1 = xxhash32_round(state->v1, xxhash32_read_le32(p));
      p += 4;
      state->v2 = xxhash32_round(state->v2, xxhash32_read_le32(p));
      p += 4;
      state->v3 = xxhash32_round(state->v3, xxhash32_read_le32(p));
      p += 4;
      state->v4 = xxhash32_round(state->v4, xxhash32_read_le32(p));
      p += 4;
    } while (p <= limit);
  }

  // Buffer remaining bytes
  if (p < end) {
    size_t remaining = (size_t)(end - p);
    memcpy(state->mem32, p, remaining);
    state->memsize = (uint32_t)remaining;
  }
}

uint32_t gcomp_xxhash32_finalize(const gcomp_xxhash32_state_t * state) {
  if (!state) {
    return 0;
  }

  uint32_t hash;

  if (state->total_len >= 16) {
    // Combine accumulators
    hash = xxhash32_rotl(state->v1, 1) + xxhash32_rotl(state->v2, 7) +
        xxhash32_rotl(state->v3, 12) + xxhash32_rotl(state->v4, 18);
  }
  else {
    // Small input: seed + PRIME5
    hash = state->seed + XXHASH32_PRIME5;
  }

  // Add total length
  hash += state->total_len;

  // Process buffered data
  const uint8_t * p = (const uint8_t *)state->mem32;
  const uint8_t * end = p + state->memsize;

  // Process remaining 4-byte chunks
  while (p + 4 <= end) {
    hash += xxhash32_read_le32(p) * XXHASH32_PRIME3;
    hash = xxhash32_rotl(hash, 17) * XXHASH32_PRIME4;
    p += 4;
  }

  // Process remaining bytes
  while (p < end) {
    hash += (*p) * XXHASH32_PRIME5;
    hash = xxhash32_rotl(hash, 11) * XXHASH32_PRIME1;
    p++;
  }

  // Final avalanche
  return xxhash32_avalanche(hash);
}
