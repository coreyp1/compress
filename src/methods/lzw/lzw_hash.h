/**
 * @file lzw_hash.h
 *
 * LZW encoder hash table: (prefix, byte) → code for O(1) lookup.
 * Encoder-only; core table layout (prefix_code[], append_char[]) unchanged.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_LZW_HASH_H
#define GHOTI_IO_GCOMP_LZW_HASH_H

#include "lzw_core.h"
#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque hash table for encoder (prefix, byte) → code. */
typedef struct lzw_encoder_hash_s lzw_encoder_hash_t;

/**
 * Initialize hash table for given capacity (number of codes, e.g. 4096).
 * Table size is 2 * capacity for load factor < 1.
 *
 * @param out           Output: created hash table (or NULL on failure).
 * @param allocator     Allocator for the table.
 * @param capacity      Max codes (2^max_code_bits).
 * @param mem_tracker   If non-NULL, track allocation here.
 * @param bytes_tracked  Output: bytes to pass to gcomp_memory_track_alloc
 * (optional).
 * @return GCOMP_OK on success.
 */
gcomp_status_t lzw_encoder_hash_init(lzw_encoder_hash_t ** out,
    const gcomp_allocator_t * allocator, uint32_t capacity,
    gcomp_memory_tracker_t * mem_tracker, size_t * bytes_tracked);

/**
 * Free hash table and set *h to NULL.
 */
void lzw_encoder_hash_destroy(lzw_encoder_hash_t ** h,
    const gcomp_allocator_t * allocator, gcomp_memory_tracker_t * mem_tracker,
    size_t bytes_tracked);

/**
 * Clear all entries (e.g. after CLEAR). Table remains allocated.
 */
void lzw_encoder_hash_reset(lzw_encoder_hash_t * h);

/**
 * Find code for (prefix, byte). Returns 1 if found, 0 if not; code_out set when
 * found.
 */
int lzw_encoder_hash_find(lzw_encoder_hash_t * h,
    const lzw_core_encoder_t * core, uint32_t prefix, uint8_t byte,
    uint32_t * code_out);

/**
 * Insert (prefix, byte) → code. Call after adding to core table.
 */
void lzw_encoder_hash_insert(
    lzw_encoder_hash_t * h, uint32_t prefix, uint8_t byte, uint32_t code);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_LZW_HASH_H
