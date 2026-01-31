/**
 * @file xxhash32.h
 *
 * xxHash32 computation for the Ghoti.io Compress library.
 *
 * This module provides xxHash32 computation for LZ4 frame format checksums.
 * xxHash32 is a fast, non-cryptographic hash function used by LZ4 for:
 * - Header checksum (single byte, masked from full hash)
 * - Block checksum (optional, full 32-bit hash)
 * - Content checksum (optional, full 32-bit hash)
 *
 * Reference: https://github.com/Cyan4973/xxHash
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_XXHASH32_H
#define GHOTI_IO_GCOMP_XXHASH32_H

#include <ghoti.io/compress/macros.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief xxHash32 state structure for incremental computation.
 *
 * This structure holds the internal state for streaming xxHash32 computation.
 * Initialize with gcomp_xxhash32_reset() before use.
 */
typedef struct {
  uint32_t total_len; ///< Total bytes processed
  uint32_t v1;        ///< Accumulator 1
  uint32_t v2;        ///< Accumulator 2
  uint32_t v3;        ///< Accumulator 3
  uint32_t v4;        ///< Accumulator 4
  uint32_t mem32[4];  ///< Buffer for partial block
  uint32_t memsize;   ///< Bytes in buffer
  uint32_t seed;      ///< Hash seed
} gcomp_xxhash32_state_t;

/**
 * @brief Default seed for LZ4 xxHash32 computation.
 *
 * LZ4 frame format uses seed 0 for all xxHash32 computations.
 */
#define GCOMP_XXHASH32_SEED_DEFAULT 0

/**
 * @brief Compute xxHash32 for a buffer (one-shot).
 *
 * Computes the xxHash32 hash for the given buffer in a single call.
 * This is the most efficient method for data that is available all at once.
 *
 * @param data Pointer to the data buffer
 * @param len Length of the data buffer in bytes
 * @param seed Hash seed (use GCOMP_XXHASH32_SEED_DEFAULT for LZ4)
 * @return The 32-bit xxHash32 value
 *
 * Example:
 * @code
 * const uint8_t data[] = {0x01, 0x02, 0x03};
 * uint32_t hash = gcomp_xxhash32(data, sizeof(data), 0);
 * @endcode
 */
GCOMP_API uint32_t gcomp_xxhash32(const void * data, size_t len, uint32_t seed);

/**
 * @brief Initialize/reset xxHash32 state for incremental computation.
 *
 * Initializes or resets the state structure for streaming computation.
 * Call this before the first gcomp_xxhash32_update() call.
 *
 * @param state Pointer to the state structure
 * @param seed Hash seed (use GCOMP_XXHASH32_SEED_DEFAULT for LZ4)
 *
 * Example:
 * @code
 * gcomp_xxhash32_state_t state;
 * gcomp_xxhash32_reset(&state, 0);
 * gcomp_xxhash32_update(&state, chunk1, len1);
 * gcomp_xxhash32_update(&state, chunk2, len2);
 * uint32_t hash = gcomp_xxhash32_finalize(&state);
 * @endcode
 */
GCOMP_API void gcomp_xxhash32_reset(
    gcomp_xxhash32_state_t * state, uint32_t seed);

/**
 * @brief Update xxHash32 computation with more data.
 *
 * Updates the xxHash32 computation with additional data. This allows for
 * incremental computation of xxHash32 across multiple buffers.
 *
 * @param state Pointer to the state structure (must be initialized)
 * @param data Pointer to the data buffer (may be NULL if len is 0)
 * @param len Length of the data buffer in bytes
 *
 * Example:
 * @code
 * gcomp_xxhash32_state_t state;
 * gcomp_xxhash32_reset(&state, 0);
 * gcomp_xxhash32_update(&state, chunk1, len1);
 * gcomp_xxhash32_update(&state, chunk2, len2);
 * uint32_t hash = gcomp_xxhash32_finalize(&state);
 * @endcode
 */
GCOMP_API void gcomp_xxhash32_update(
    gcomp_xxhash32_state_t * state, const void * data, size_t len);

/**
 * @brief Finalize xxHash32 computation and return the hash.
 *
 * Completes the xxHash32 computation and returns the final hash value.
 * The state is not modified, so you can continue updating after finalize
 * if needed (though this is uncommon).
 *
 * @param state Pointer to the state structure
 * @return The 32-bit xxHash32 value
 *
 * Example:
 * @code
 * gcomp_xxhash32_state_t state;
 * gcomp_xxhash32_reset(&state, 0);
 * gcomp_xxhash32_update(&state, data, len);
 * uint32_t hash = gcomp_xxhash32_finalize(&state);
 * @endcode
 */
GCOMP_API uint32_t gcomp_xxhash32_finalize(
    const gcomp_xxhash32_state_t * state);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_XXHASH32_H
