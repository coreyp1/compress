/**
 * @file gzip_format.c
 *
 * RFC 1952 format helpers for the gzip method.
 *
 * This file provides helper functions for constructing and managing gzip
 * headers and trailers according to RFC 1952 ("GZIP file format specification
 * version 4.3").
 *
 * ## Gzip Header Structure (RFC 1952 Section 2.3)
 *
 * ```
 * Offset  Size  Field       Description
 * ------  ----  -----       -----------
 *    0      1   ID1         Magic byte 1 (0x1F)
 *    1      1   ID2         Magic byte 2 (0x8B)
 *    2      1   CM          Compression method (8 = deflate)
 *    3      1   FLG         Flags (see below)
 *    4      4   MTIME       Modification time (Unix timestamp, little-endian)
 *    8      1   XFL         Extra flags (2=max compression, 4=fastest)
 *    9      1   OS          Operating system (see RFC 1952 §2.3.1)
 *
 * Optional fields follow based on FLG bits:
 *
 *   If FEXTRA (bit 2) set:
 *   10+     2   XLEN        Extra field length (little-endian)
 *   12+  XLEN   extra       Extra field data
 *
 *   If FNAME (bit 3) set:
 *   ...     ?   name        Original filename (Latin-1, NUL-terminated)
 *
 *   If FCOMMENT (bit 4) set:
 *   ...     ?   comment     File comment (Latin-1, NUL-terminated)
 *
 *   If FHCRC (bit 1) set:
 *   ...     2   CRC16       CRC16 of header bytes (lower 16 bits of CRC32)
 * ```
 *
 * ## FLG Byte Bits
 *
 *   Bit 0: FTEXT    - File is probably ASCII text (informational)
 *   Bit 1: FHCRC    - Header CRC16 present
 *   Bit 2: FEXTRA   - Extra field present
 *   Bit 3: FNAME    - Original filename present
 *   Bit 4: FCOMMENT - File comment present
 *   Bits 5-7: Reserved (must be zero)
 *
 * ## Gzip Trailer Structure (RFC 1952 Section 2.3.1)
 *
 * ```
 * Offset  Size  Field       Description
 * ------  ----  -----       -----------
 *    0      4   CRC32       CRC32 of uncompressed data (little-endian)
 *    4      4   ISIZE       Original uncompressed size mod 2^32 (little-endian)
 * ```
 *
 * ## String Encoding Note (RFC 1952 Section 2.3)
 *
 * FNAME and FCOMMENT fields must be encoded in ISO 8859-1 (Latin-1) and
 * cannot contain embedded NUL bytes (only the terminating NUL). Since this
 * implementation uses C strings via the options API, embedded NULs would
 * naturally truncate the string at that point. The use of strlen() to
 * determine field lengths ensures RFC compliance.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "gzip_internal.h"
#include <ghoti.io/compress/crc32.h>
#include <ghoti.io/compress/errors.h>
#include <stdlib.h>
#include <string.h>

/**
 * Write a complete gzip header to a buffer.
 *
 * Constructs the RFC 1952 header based on the provided header_info structure.
 * The FLG byte in info determines which optional fields are written. The
 * caller is responsible for ensuring the FLG byte is consistent with the
 * populated fields (e.g., if FNAME is set in FLG, info->name should be valid).
 *
 * If FHCRC is set in FLG, this function computes the header CRC16 by taking
 * the lower 16 bits of the CRC32 of all header bytes up to (but not including)
 * the CRC16 field itself.
 *
 * @param info        Header information structure
 * @param buf         Output buffer (must be at least GZIP_HEADER_MAX_SIZE)
 * @param buf_size    Size of output buffer
 * @param header_len_out  Receives the actual header length written
 * @return GCOMP_OK on success, GCOMP_ERR_INVALID_ARG if buffer too small
 */
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
  buf[pos++] = GZIP_ID1;        // ID1
  buf[pos++] = GZIP_ID2;        // ID2
  buf[pos++] = GZIP_CM_DEFLATE; // CM
  buf[pos++] = info->flg;       // FLG
  gzip_write_le32(buf + pos, info->mtime);
  pos += 4;
  buf[pos++] = info->xfl; // XFL
  buf[pos++] = info->os;  // OS

  // Track CRC for header (if FHCRC will be written)
  if (info->flg & GZIP_FLG_FHCRC) {
    header_crc = gcomp_crc32_update(header_crc, buf, pos);
  }

  // FEXTRA
  if (info->flg & GZIP_FLG_FEXTRA) {
    gzip_write_le16(buf + pos, (uint16_t)info->extra_len);
    if (info->flg & GZIP_FLG_FHCRC) {
      header_crc = gcomp_crc32_update(header_crc, buf + pos, 2);
    }
    pos += 2;

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
    gzip_write_le16(buf + pos, crc16);
    pos += 2;
  }

  *header_len_out = pos;
  return GCOMP_OK;
}

/**
 * Write the 8-byte gzip trailer to a buffer.
 *
 * The trailer consists of:
 * - CRC32: 32-bit CRC of the uncompressed data (already finalized)
 * - ISIZE: Original uncompressed size modulo 2^32
 *
 * Both values are stored in little-endian byte order.
 *
 * @param crc32  Finalized CRC32 of uncompressed data (call gcomp_crc32_finalize
 * first)
 * @param isize  Uncompressed size mod 2^32
 * @param buf    Output buffer (must be at least GZIP_TRAILER_SIZE = 8 bytes)
 */
void gzip_write_trailer(uint32_t crc32, uint32_t isize, uint8_t * buf) {
  if (!buf) {
    return;
  }

  gzip_write_le32(buf, crc32);     // CRC32
  gzip_write_le32(buf + 4, isize); // ISIZE
}

/**
 * Free dynamically allocated members of a gzip_header_info_t structure.
 *
 * This function frees the extra, name, and comment fields if they are
 * non-NULL, and resets the pointers to NULL. The structure itself is not
 * freed (it may be stack-allocated or embedded in another structure).
 *
 * Safe to call with NULL or with already-freed members.
 *
 * @param info  Header info structure to clean up (may be NULL)
 */
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

/**
 * Extract options to pass through to the inner deflate encoder/decoder.
 *
 * Creates a clone of the source options for pass-through to deflate. The
 * deflate method will ignore unknown keys (like gzip.*) via its schema
 * validation, while accepting deflate.* and limits.* keys.
 *
 * @param src Source options (may be NULL)
 * @param dst_out Receives cloned options (set to NULL if src is NULL)
 * @return GCOMP_OK on success, GCOMP_ERR_MEMORY on allocation failure
 */
gcomp_status_t gzip_extract_passthrough_options(
    const gcomp_options_t * src, gcomp_options_t ** dst_out) {
  // For now, just clone the entire options object
  // The deflate method will ignore unknown keys with its schema
  if (!src) {
    *dst_out = NULL;
    return GCOMP_OK;
  }
  return gcomp_options_clone(src, dst_out);
}
