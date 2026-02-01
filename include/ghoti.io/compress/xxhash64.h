/**
 * @file xxhash64.h
 *
 * xxHash64 computation for the Ghoti.io Compress library.
 *
 * This module provides xxHash64 computation for Zstandard frame format
 * checksums. xxHash64 is a fast, non-cryptographic hash function used by Zstd
 * for:
 * - Content checksum (low 32 bits of full 64-bit hash)
 *
 * Reference: https://github.com/Cyan4973/xxHash
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_XXHASH64_H
#define GHOTI_IO_GCOMP_XXHASH64_H

#include <ghoti.io/compress/macros.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief xxHash64 state structure for incremental computation.
 *
 * This structure holds the internal state for streaming xxHash64 computation.
 * Initialize with gcomp_xxhash64_reset() before use.
 */
typedef struct {
  uint64_t total_len; ///< Total bytes processed
  uint64_t v1;        ///< Accumulator 1
  uint64_t v2;        ///< Accumulator 2
  uint64_t v3;        ///< Accumulator 3
  uint64_t v4;        ///< Accumulator 4
  uint64_t mem64[4];  ///< Buffer for partial block (32 bytes)
  uint32_t memsize;   ///< Bytes in buffer
  uint64_t seed;      ///< Hash seed
} gcomp_xxhash64_state_t;

/**
 * @brief Default seed for Zstd xxHash64 computation.
 *
 * Zstd frame format uses seed 0 for all xxHash64 computations.
 */
#define GCOMP_XXHASH64_SEED_DEFAULT 0

/**
 * @brief Compute xxHash64 for a buffer (one-shot).
 *
 * Computes the xxHash64 hash for the given buffer in a single call.
 * This is the most efficient method for data that is available all at once.
 *
 * @param data Pointer to the data buffer
 * @param len Length of the data buffer in bytes
 * @param seed Hash seed (use GCOMP_XXHASH64_SEED_DEFAULT for Zstd)
 * @return The 64-bit xxHash64 value
 *
 * Example:
 * @code
 * const uint8_t data[] = {0x01, 0x02, 0x03};
 * uint64_t hash = gcomp_xxhash64(data, sizeof(data), 0);
 * @endcode
 */
GCOMP_API uint64_t gcomp_xxhash64(const void * data, size_t len, uint64_t seed);

/**
 * @brief Initialize/reset xxHash64 state for incremental computation.
 *
 * Initializes or resets the state structure for streaming computation.
 * Call this before the first gcomp_xxhash64_update() call.
 *
 * @param state Pointer to the state structure
 * @param seed Hash seed (use GCOMP_XXHASH64_SEED_DEFAULT for Zstd)
 *
 * Example:
 * @code
 * gcomp_xxhash64_state_t state;
 * gcomp_xxhash64_reset(&state, 0);
 * gcomp_xxhash64_update(&state, chunk1, len1);
 * gcomp_xxhash64_update(&state, chunk2, len2);
 * uint64_t hash = gcomp_xxhash64_finalize(&state);
 * @endcode
 */
GCOMP_API void gcomp_xxhash64_reset(
    gcomp_xxhash64_state_t * state, uint64_t seed);

/**
 * @brief Update xxHash64 computation with more data.
 *
 * Updates the xxHash64 computation with additional data. This allows for
 * incremental computation of xxHash64 across multiple buffers.
 *
 * @param state Pointer to the state structure (must be initialized)
 * @param data Pointer to the data buffer (may be NULL if len is 0)
 * @param len Length of the data buffer in bytes
 *
 * Example:
 * @code
 * gcomp_xxhash64_state_t state;
 * gcomp_xxhash64_reset(&state, 0);
 * gcomp_xxhash64_update(&state, chunk1, len1);
 * gcomp_xxhash64_update(&state, chunk2, len2);
 * uint64_t hash = gcomp_xxhash64_finalize(&state);
 * @endcode
 */
GCOMP_API void gcomp_xxhash64_update(
    gcomp_xxhash64_state_t * state, const void * data, size_t len);

/**
 * @brief Finalize xxHash64 computation and return the hash.
 *
 * Completes the xxHash64 computation and returns the final hash value.
 * The state is not modified, so you can continue updating after finalize
 * if needed (though this is uncommon).
 *
 * @param state Pointer to the state structure
 * @return The 64-bit xxHash64 value
 *
 * Example:
 * @code
 * gcomp_xxhash64_state_t state;
 * gcomp_xxhash64_reset(&state, 0);
 * gcomp_xxhash64_update(&state, data, len);
 * uint64_t hash = gcomp_xxhash64_finalize(&state);
 * @endcode
 */
GCOMP_API uint64_t gcomp_xxhash64_finalize(
    const gcomp_xxhash64_state_t * state);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_XXHASH64_H
