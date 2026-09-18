/**
 * @file zstd_sequences_private.h
 *
 * Shared tables and symbol-to-code conversions for zstd sequences.
 * Included by zstd_sequences.c, zstd_sequences_encode.c and zstd_optimal.c.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_ZSTD_ZSTD_SEQUENCES_PRIVATE_H
#define GHOTI_IO_GCOMP_SRC_METHODS_ZSTD_ZSTD_SEQUENCES_PRIVATE_H

#include <ghoti.io/compress/macros.h>

#include <stdint.h>

#define ZSTD_SEQ_LL_CODES 36
#define ZSTD_SEQ_ML_CODES 53

extern const uint32_t zstd_seq_ll_baseline[ZSTD_SEQ_LL_CODES];
extern const uint8_t zstd_seq_ll_extra_bits[ZSTD_SEQ_LL_CODES];
extern const uint32_t zstd_seq_ml_baseline[ZSTD_SEQ_ML_CODES];
extern const uint8_t zstd_seq_ml_extra_bits[ZSTD_SEQ_ML_CODES];

//
// Symbol to Code Conversion (RFC 8878 section 3.1.1.3.2.1)
//
// A literal length, match length or offset value is written as a code
// followed by a fixed number of raw bits.  These map the value to its code;
// zstd_seq_*_baseline and zstd_seq_*_extra_bits above give the rest.
//

static inline uint8_t zstd_enc_get_ll_code(uint32_t ll) {
  if (ll < 16) {
    return (uint8_t)ll;
  }
  if (ll < 18) {
    return 16;
  }
  if (ll < 20) {
    return 17;
  }
  if (ll < 22) {
    return 18;
  }
  if (ll < 24) {
    return 19;
  }
  if (ll < 28) {
    return 20;
  }
  if (ll < 32) {
    return 21;
  }
  if (ll < 40) {
    return 22;
  }
  if (ll < 48) {
    return 23;
  }
  if (ll < 64) {
    return 24;
  }
  if (ll < 128) {
    return 25;
  }
  if (ll < 256) {
    return 26;
  }
  if (ll < 512) {
    return 27;
  }
  if (ll < 1024) {
    return 28;
  }
  if (ll < 2048) {
    return 29;
  }
  if (ll < 4096) {
    return 30;
  }
  if (ll < 8192) {
    return 31;
  }
  if (ll < 16384) {
    return 32;
  }
  if (ll < 32768) {
    return 33;
  }
  if (ll < 65536) {
    return 34;
  }
  return 35;
}

static inline uint8_t zstd_enc_get_ml_code(uint32_t ml) {
  if (ml < 3) {
    return 0;
  }
  ml -= 3;
  if (ml < 32) {
    return (uint8_t)ml;
  }
  if (ml < 34) {
    return 32;
  }
  if (ml < 36) {
    return 33;
  }
  if (ml < 38) {
    return 34;
  }
  if (ml < 40) {
    return 35;
  }
  if (ml < 44) {
    return 36;
  }
  if (ml < 48) {
    return 37;
  }
  if (ml < 56) {
    return 38;
  }
  if (ml < 64) {
    return 39;
  }
  if (ml < 80) {
    return 40;
  }
  if (ml < 96) {
    return 41;
  }
  if (ml < 128) {
    return 42;
  }
  if (ml < 256) {
    return 43;
  }
  if (ml < 512) {
    return 44;
  }
  if (ml < 1024) {
    return 45;
  }
  if (ml < 2048) {
    return 46;
  }
  if (ml < 4096) {
    return 47;
  }
  if (ml < 8192) {
    return 48;
  }
  if (ml < 16384) {
    return 49;
  }
  if (ml < 32768) {
    return 50;
  }
  if (ml < 65536) {
    return 51;
  }
  return 52;
}

static inline uint8_t zstd_enc_get_of_code(uint32_t offset) {
  if (offset == 0) {
    return 0;
  }
  // of_code = highest set bit position (floor(log2(offset)))
  return (uint8_t)(31 - __builtin_clz(offset));
}

#endif /* GHOTI_IO_GCOMP_SRC_METHODS_ZSTD_ZSTD_SEQUENCES_PRIVATE_H */
