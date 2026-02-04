/**
 * @file lzw_profile.h
 *
 * LZW profile interface: CLEAR/END semantics, initial/max code width,
 * code-width growth, bit-packing order (LSB vs MSB). Two reference
 * profiles: GIF (LSB, 8 to 12 bits) and TIFF (MSB, 9 to 12 bits).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_LZW_PROFILE_H
#define GHOTI_IO_GCOMP_LZW_PROFILE_H

#include "lzw_bitio.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LZW_PROFILE_GIF,
  LZW_PROFILE_TIFF,
  LZW_PROFILE_UNKNOWN,
} lzw_profile_id_t;

/**
 * @brief Resolve profile from format string ("gif" or "tiff").
 */
lzw_profile_id_t lzw_profile_from_string(const char * format);

/**
 * @brief CLEAR code (e.g. 256).
 */
uint32_t lzw_profile_clear_code(lzw_profile_id_t profile, unsigned lit_width);

/**
 * @brief EOI (end-of-information) code (e.g. 257).
 */
uint32_t lzw_profile_eoi_code(lzw_profile_id_t profile, unsigned lit_width);

/**
 * @brief Initial code width in bits for data codes (GIF: 9 after CLEAR/EOI;
 * TIFF: 9).
 */
unsigned lzw_profile_initial_code_bits(
    lzw_profile_id_t profile, unsigned lit_width);

/**
 * @brief Bit order for stream (GIF = LSB, TIFF = MSB).
 */
lzw_bitio_order_t lzw_profile_bit_order(lzw_profile_id_t profile);

/**
 * @brief Whether to increment code width after adding this next_code.
 *
 * GIF: when next_code == 2^current_bits.
 * TIFF: when next_code == 2^current_bits - 1.
 */
int lzw_profile_should_increment_bits(
    lzw_profile_id_t profile, uint32_t next_code, unsigned current_bits);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_LZW_PROFILE_H
