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
 * `lit_width` is the width of a literal code: GIF calls it the "LZW minimum
 * code size" and writes it in front of every image's data (89a 22); TIFF fixes
 * it at 8 (TIFF 6.0 section 13).  Everything else here follows from it -
 * CLEAR is the first code above the literals, EOI the one after that, and the
 * stream opens one bit wider than a literal so that both of them are
 * expressible.
 *
 * These three used to ignore `lit_width` and return 256, 257 and 9, on the
 * stated grounds that this "matches common GIF and TIFF LZW streams".  It
 * matches TIFF, whose literals are always 8 bits.  It does not match GIF: of
 * 121 GIFs installed on one Debian machine, 71 contained at least one image
 * below width 8, and every one of those 71 failed to decode while no file of
 * only width-8 images did.  Widths 2, 3, 4, 6 and 7 were rejected outright;
 * width 5 was worse, returning success after writing 8 bytes of a 128-byte
 * image, because a wrong code width can happen to hit EOI early.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>
#include "lzw_profile.h"
#include <string.h>

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
  // The same rule in both profiles: CLEAR sits directly above the literals.
  // At TIFF's fixed width of 8 it is 256, which is what section 13 names.
  (void)profile;
  return 1u << lit_width;
}

uint32_t lzw_profile_eoi_code(lzw_profile_id_t profile, unsigned lit_width) {
  (void)profile;
  return (1u << lit_width) + 1u;
}

unsigned lzw_profile_initial_code_bits(
    lzw_profile_id_t profile, unsigned lit_width) {
  // One wider than a literal, so that CLEAR and EOI can be written at all.
  (void)profile;
  return lit_width + 1u;
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
