/**
 * @file gzip_format.c
 *
 * RFC 1952 format helpers for the gzip method.
 *
 * This file provides helper functions for:
 * - Building gzip headers
 * - Building gzip trailers
 * - Cleanup of header info structures
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "gzip_internal.h"
#include <ghoti.io/compress/crc32.h>
#include <ghoti.io/compress/errors.h>
#include <stdlib.h>
#include <string.h>

//
// Header Writer
//

gcomp_status_t gzip_write_header(const gzip_header_info_t * info, uint8_t * buf,
    size_t buf_size, size_t * header_len_out) {
  if (!info || !buf || !header_len_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  size_t pos = 0;
  uint32_t header_crc = GCOMP_CRC32_INIT;

  // Calculate required size first
  size_t required = GZIP_HEADER_MIN_SIZE;
  if (info->flg & GZIP_FLG_FEXTRA) {
    required += 2 + info->extra_len; // XLEN + data
  }
  if (info->flg & GZIP_FLG_FNAME) {
    required += info->name ? strlen(info->name) + 1 : 1;
  }
  if (info->flg & GZIP_FLG_FCOMMENT) {
    required += info->comment ? strlen(info->comment) + 1 : 1;
  }
  if (info->flg & GZIP_FLG_FHCRC) {
    required += 2;
  }

  if (required > buf_size) {
    return GCOMP_ERR_INVALID_ARG; // Buffer too small
  }

  // Write fixed header (10 bytes)
  buf[pos++] = GZIP_ID1;                     // ID1
  buf[pos++] = GZIP_ID2;                     // ID2
  buf[pos++] = GZIP_CM_DEFLATE;              // CM
  buf[pos++] = info->flg;                    // FLG
  buf[pos++] = (uint8_t)(info->mtime);       // MTIME byte 0
  buf[pos++] = (uint8_t)(info->mtime >> 8);  // MTIME byte 1
  buf[pos++] = (uint8_t)(info->mtime >> 16); // MTIME byte 2
  buf[pos++] = (uint8_t)(info->mtime >> 24); // MTIME byte 3
  buf[pos++] = info->xfl;                    // XFL
  buf[pos++] = info->os;                     // OS

  // Track CRC for header (if FHCRC will be written)
  if (info->flg & GZIP_FLG_FHCRC) {
    header_crc = gcomp_crc32_update(header_crc, buf, pos);
  }

  // FEXTRA
  if (info->flg & GZIP_FLG_FEXTRA) {
    uint16_t xlen = (uint16_t)info->extra_len;
    buf[pos++] = (uint8_t)(xlen);
    buf[pos++] = (uint8_t)(xlen >> 8);
    if (info->flg & GZIP_FLG_FHCRC) {
      header_crc = gcomp_crc32_update(header_crc, buf + pos - 2, 2);
    }

    if (info->extra && info->extra_len > 0) {
      memcpy(buf + pos, info->extra, info->extra_len);
      if (info->flg & GZIP_FLG_FHCRC) {
        header_crc = gcomp_crc32_update(header_crc, buf + pos, info->extra_len);
      }
      pos += info->extra_len;
    }
  }

  // FNAME (null-terminated)
  // Per RFC 1952, FNAME must be Latin-1 encoded and cannot contain
  // embedded NUL bytes (only the terminator). Since we use C strings,
  // strlen() naturally handles this constraint.
  if (info->flg & GZIP_FLG_FNAME) {
    if (info->name) {
      size_t len = strlen(info->name) + 1; // Include null terminator
      memcpy(buf + pos, info->name, len);
      if (info->flg & GZIP_FLG_FHCRC) {
        header_crc = gcomp_crc32_update(header_crc, buf + pos, len);
      }
      pos += len;
    }
    else {
      buf[pos++] = 0; // Empty name (just null terminator)
      if (info->flg & GZIP_FLG_FHCRC) {
        header_crc = gcomp_crc32_update(header_crc, buf + pos - 1, 1);
      }
    }
  }

  // FCOMMENT (null-terminated)
  // Per RFC 1952, FCOMMENT must be Latin-1 encoded and cannot contain
  // embedded NUL bytes (only the terminator). Since we use C strings,
  // strlen() naturally handles this constraint.
  if (info->flg & GZIP_FLG_FCOMMENT) {
    if (info->comment) {
      size_t len = strlen(info->comment) + 1; // Include null terminator
      memcpy(buf + pos, info->comment, len);
      if (info->flg & GZIP_FLG_FHCRC) {
        header_crc = gcomp_crc32_update(header_crc, buf + pos, len);
      }
      pos += len;
    }
    else {
      buf[pos++] = 0; // Empty comment (just null terminator)
      if (info->flg & GZIP_FLG_FHCRC) {
        header_crc = gcomp_crc32_update(header_crc, buf + pos - 1, 1);
      }
    }
  }

  // FHCRC (CRC16 of header so far)
  if (info->flg & GZIP_FLG_FHCRC) {
    uint32_t final_crc = gcomp_crc32_finalize(header_crc);
    uint16_t crc16 = (uint16_t)(final_crc & 0xFFFF);
    buf[pos++] = (uint8_t)(crc16);
    buf[pos++] = (uint8_t)(crc16 >> 8);
  }

  *header_len_out = pos;
  return GCOMP_OK;
}

//
// Trailer Writer
//

void gzip_write_trailer(uint32_t crc32, uint32_t isize, uint8_t * buf) {
  if (!buf) {
    return;
  }

  // CRC32 (little-endian)
  buf[0] = (uint8_t)(crc32);
  buf[1] = (uint8_t)(crc32 >> 8);
  buf[2] = (uint8_t)(crc32 >> 16);
  buf[3] = (uint8_t)(crc32 >> 24);

  // ISIZE (little-endian)
  buf[4] = (uint8_t)(isize);
  buf[5] = (uint8_t)(isize >> 8);
  buf[6] = (uint8_t)(isize >> 16);
  buf[7] = (uint8_t)(isize >> 24);
}

//
// Header Info Cleanup
//

void gzip_header_info_free(gzip_header_info_t * info) {
  if (!info) {
    return;
  }

  if (info->extra) {
    free(info->extra);
    info->extra = NULL;
    info->extra_len = 0;
  }

  if (info->name) {
    free(info->name);
    info->name = NULL;
  }

  if (info->comment) {
    free(info->comment);
    info->comment = NULL;
  }
}
