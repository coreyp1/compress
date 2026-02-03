/**
 * @file zstd_sequences_private.h
 *
 * Shared tables for zstd sequences decode and encode.
 * Included only by zstd_sequences.c and zstd_sequences_encode.c.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_ZSTD_SEQUENCES_PRIVATE_H
#define GHOTI_IO_ZSTD_SEQUENCES_PRIVATE_H

#include <stdint.h>

#define ZSTD_SEQ_LL_CODES 36
#define ZSTD_SEQ_ML_CODES 53

extern const uint32_t zstd_seq_ll_baseline[ZSTD_SEQ_LL_CODES];
extern const uint8_t zstd_seq_ll_extra_bits[ZSTD_SEQ_LL_CODES];
extern const uint32_t zstd_seq_ml_baseline[ZSTD_SEQ_ML_CODES];
extern const uint8_t zstd_seq_ml_extra_bits[ZSTD_SEQ_ML_CODES];

#endif /* GHOTI_IO_ZSTD_SEQUENCES_PRIVATE_H */
