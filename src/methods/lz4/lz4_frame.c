/**
 * @file lz4_frame.c
 *
 * LZ4 frame format helpers.
 *
 * This file provides functions for building and parsing LZ4 frame headers
 * and block metadata.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "lz4_internal.h"
#include <ghoti.io/compress/xxhash32.h>
#include <string.h>

//
// Block Size Conversion
//

uint32_t lz4_block_code_to_size(uint8_t code) {
  switch (code) {
  case LZ4_BLOCK_MAX_64KB:
    return LZ4_BLOCK_SIZE_64KB;
  case LZ4_BLOCK_MAX_256KB:
    return LZ4_BLOCK_SIZE_256KB;
  case LZ4_BLOCK_MAX_1MB:
    return LZ4_BLOCK_SIZE_1MB;
  case LZ4_BLOCK_MAX_4MB:
    return LZ4_BLOCK_SIZE_4MB;
  default:
    return 0;
  }
}

uint8_t lz4_size_to_block_code(uint32_t size) {
  switch (size) {
  case LZ4_BLOCK_SIZE_64KB:
    return LZ4_BLOCK_MAX_64KB;
  case LZ4_BLOCK_SIZE_256KB:
    return LZ4_BLOCK_MAX_256KB;
  case LZ4_BLOCK_SIZE_1MB:
    return LZ4_BLOCK_MAX_1MB;
  case LZ4_BLOCK_SIZE_4MB:
    return LZ4_BLOCK_MAX_4MB;
  default:
    return 0;
  }
}

//
// Frame Header Building
//

gcomp_status_t lz4_write_frame_header(const lz4_frame_header_t * header,
    uint8_t * buf, size_t buf_size, size_t * header_len_out) {
  if (!header || !buf || !header_len_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  size_t pos = 0;

  // Calculate required size
  size_t required = 4 + 2 + 1; // Magic + FLG + BD + HC
  if (header->content_size_present) {
    required += 8;
  }
  if (header->dict_id_present) {
    required += 4;
  }

  if (buf_size < required) {
    return GCOMP_ERR_LIMIT;
  }

  // Magic number
  lz4_write_le32(buf + pos, LZ4_MAGIC);
  pos += 4;

  // FLG byte
  buf[pos++] = header->flg;

  // BD byte
  buf[pos++] = header->bd;

  // Content size (if present)
  if (header->content_size_present) {
    lz4_write_le64(buf + pos, header->content_size);
    pos += 8;
  }

  // Dictionary ID (if present)
  if (header->dict_id_present) {
    lz4_write_le32(buf + pos, header->dict_id);
    pos += 4;
  }

  // Header checksum: (xxHash32(FLG..end) >> 8) & 0xFF
  // The checksum covers bytes from FLG to just before HC (exclusive of magic)
  uint32_t hash = gcomp_xxhash32(buf + 4, pos - 4, 0);
  buf[pos++] = (uint8_t)((hash >> 8) & 0xFF);

  *header_len_out = pos;
  return GCOMP_OK;
}

//
// Block Size Parsing
//

gcomp_status_t lz4_parse_block_size(
    const uint8_t * buf, uint32_t * size_out, bool * uncompressed_out) {
  if (!buf || !size_out || !uncompressed_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  uint32_t raw = lz4_read_le32(buf);

  *uncompressed_out = (raw & LZ4_BLOCK_UNCOMPRESSED_FLAG) != 0;
  *size_out = raw & LZ4_BLOCK_SIZE_MASK;

  return GCOMP_OK;
}
