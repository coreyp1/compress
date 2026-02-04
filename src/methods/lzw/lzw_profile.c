/**
 * @file lzw_profile.c
 *
 * LZW profile: GIF and TIFF (bit order, CLEAR/EOI, code-size increment rules).
 *
 * RATIONALE
 * =========
 *
 * LZW has multiple deployed on-the-wire variants. For this library the profile
 * determines:
 *
 * - Bit packing order (LSB for GIF, MSB for TIFF)
 * - When code width increases as the table grows (GIF vs TIFF rule)
 *
 * Note: the public options include `lzw.lit_width` for compatibility and future
 * extension. For the reference profiles implemented here, CLEAR/EOI are fixed
 * to 256/257 and the starting code width is 9 bits, which matches common GIF
 * and TIFF LZW streams. `lzw.lit_width` primarily affects defaults and schema
 * validation at the method layer.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "lzw_profile.h"
#include <string.h>

#define LZW_CLEAR_DEFAULT 256u
#define LZW_EOI_DEFAULT 257u

lzw_profile_id_t lzw_profile_from_string(const char * format) {
  if (!format) {
    return LZW_PROFILE_UNKNOWN;
  }
  if (strcmp(format, "gif") == 0) {
    return LZW_PROFILE_GIF;
  }
  if (strcmp(format, "tiff") == 0) {
    return LZW_PROFILE_TIFF;
  }
  return LZW_PROFILE_UNKNOWN;
}

uint32_t lzw_profile_clear_code(lzw_profile_id_t profile, unsigned lit_width) {
  (void)profile;
  (void)lit_width;
  return LZW_CLEAR_DEFAULT;
}

uint32_t lzw_profile_eoi_code(lzw_profile_id_t profile, unsigned lit_width) {
  (void)profile;
  (void)lit_width;
  return LZW_EOI_DEFAULT;
}

unsigned lzw_profile_initial_code_bits(
    lzw_profile_id_t profile, unsigned lit_width) {
  (void)profile;
  (void)lit_width;
  return 9;
}

lzw_bitio_order_t lzw_profile_bit_order(lzw_profile_id_t profile) {
  return (profile == LZW_PROFILE_TIFF) ? LZW_BITIO_MSB : LZW_BITIO_LSB;
}

int lzw_profile_should_increment_bits(
    lzw_profile_id_t profile, uint32_t next_code, unsigned current_bits) {
  if (current_bits >= 12) {
    return 0;
  }
  uint32_t threshold = 1u << current_bits;
  if (profile == LZW_PROFILE_GIF) {
    return (next_code == threshold) ? 1 : 0;
  }
  if (profile == LZW_PROFILE_TIFF) {
    return (next_code == threshold - 1u) ? 1 : 0;
  }
  return 0;
}
