/**
 * @file zlib_format.c
 *
 * Building and parsing the RFC 1950 header.
 *
 * Two bytes, and every bit of them is specified in section 2.2.  Small enough
 * to get subtly wrong, which is why each field is handled explicitly here
 * rather than assembled inline at the call site.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>

#include "zlib_internal.h"

uint8_t zlib_flevel_for_level(int level) {
  // FLEVEL is "a hint as to the compression level used" and RFC 1950 says
  // outright that it "need not be set correctly" -- a decompressor must not
  // depend on it.  It is set correctly anyway, because a stream that claims
  // something false about itself is a trap for whoever reads it next, and
  // because matching zlib byte for byte makes interoperability testable.
  //
  // These are zlib's own boundaries, from deflate.c: below 2 is "fastest",
  // 2 to 5 "fast", exactly 6 "default", and 7 and up "maximum".
  if (level < 2) {
    return 0u;
  }
  if (level < 6) {
    return 1u;
  }
  if (level == 6) {
    return 2u;
  }
  return 3u;
}

gcomp_status_t zlib_write_header(
    unsigned window_bits, int level, uint8_t * out) {
  if (!out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (window_bits < ZLIB_WINDOW_BITS_MIN ||
      window_bits > ZLIB_WINDOW_BITS_MAX) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // CINFO is log2 of the window size minus eight, so a 32 KiB window is 7 --
  // the largest the format allows for deflate.
  uint8_t cinfo = (uint8_t)(window_bits - 8u);
  uint8_t cmf =
      (uint8_t)((cinfo << ZLIB_CMF_CINFO_SHIFT) | ZLIB_CM_DEFLATE);

  // FDICT stays clear: a preset dictionary would need the deflate encoder to
  // accept one, and it does not.  See the note in zlib.h.
  uint8_t flg = (uint8_t)(zlib_flevel_for_level(level) << ZLIB_FLG_FLEVEL_SHIFT);

  // FCHECK is whatever makes the pair a multiple of 31.  Five bits is always
  // enough: the remainder is at most 30, and subtracting it from 31 lands
  // inside the field.
  unsigned pair = ((unsigned)cmf << 8) | flg;
  unsigned remainder = pair % ZLIB_HEADER_MODULUS;
  if (remainder != 0u) {
    flg = (uint8_t)(flg | (ZLIB_HEADER_MODULUS - remainder));
  }

  out[0] = cmf;
  out[1] = flg;
  return GCOMP_OK;
}

gcomp_status_t zlib_parse_header(const uint8_t * data, zlib_header_t * out) {
  if (!data || !out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  uint8_t cmf = data[0];
  uint8_t flg = data[1];

  uint8_t cm = (uint8_t)(cmf & ZLIB_CMF_CM_MASK);
  if (cm != ZLIB_CM_DEFLATE) {
    return GCOMP_ERR_CORRUPT;
  }

  uint8_t cinfo = (uint8_t)(cmf >> ZLIB_CMF_CINFO_SHIFT);
  if (cinfo > ZLIB_CINFO_MAX) {
    // RFC 1950 section 2.2: "CINFO above 7 are not allowed in this version of
    // the specification."  A larger window than 32 KiB is not something to
    // attempt and fail at halfway through.
    return GCOMP_ERR_CORRUPT;
  }

  // The check that makes a damaged header cheap to spot.  It is worth doing
  // before anything else is trusted, because the two bytes above are only
  // meaningful if the pair is well formed.
  unsigned pair = ((unsigned)cmf << 8) | flg;
  if ((pair % ZLIB_HEADER_MODULUS) != 0u) {
    return GCOMP_ERR_CORRUPT;
  }

  out->cmf = cmf;
  out->flg = flg;
  out->cm = cm;
  out->cinfo = cinfo;
  out->flevel = (uint8_t)(flg >> ZLIB_FLG_FLEVEL_SHIFT);
  out->fdict = (flg & ZLIB_FLG_FDICT) != 0u;
  out->dict_id = 0u;
  out->window_bits = (uint8_t)(cinfo + 8u);
  return GCOMP_OK;
}
