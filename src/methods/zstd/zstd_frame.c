/**
 * @file zstd_frame.c
 *
 * Zstandard frame format helpers for the Ghoti.io Compress library.
 *
 * This file provides functions for:
 * - Building and parsing frame headers
 * - Building and parsing block headers
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include "zstd_internal.h"

//
// Frame Header
//

gcomp_status_t zstd_write_frame_header(const zstd_frame_header_t * header,
    uint8_t * buf, size_t buf_size, size_t * header_len_out) {
  if (!header || !buf || !header_len_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  size_t pos = 0;

  // Ensure buffer is large enough for magic + max header
  if (buf_size < 4 + ZSTD_HEADER_MAX_SIZE) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Write magic number
  gcomp_write_le32(buf + pos, ZSTD_MAGIC);
  pos += 4;

  // Build frame header descriptor
  uint8_t fhd = 0;

  // Dictionary ID flag (we don't support dictionaries yet, so always 0)
  // fhd |= 0; // dict_id_flag = 0

  // Content checksum flag
  if (header->content_checksum) {
    fhd |= ZSTD_FHD_CHECKSUM_FLAG;
  }

  // Reserved bit must be 0
  // fhd |= 0;

  // Unused bit must be 0
  // fhd |= 0;

  // Single segment flag
  if (header->single_segment) {
    fhd |= ZSTD_FHD_SINGLE_SEGMENT_FLAG;
  }

  // Frame content size flag
  if (header->content_size_present) {
    if (header->content_size < 256) {
      // 1 byte (only valid with single_segment)
      fhd |= (0 << ZSTD_FHD_FCS_FLAG_SHIFT);
    }
    else if (header->content_size <= 65535 + 256) {
      // 2 bytes
      fhd |= (1 << ZSTD_FHD_FCS_FLAG_SHIFT);
    }
    else if (header->content_size <= UINT32_MAX) {
      // 4 bytes
      fhd |= (2 << ZSTD_FHD_FCS_FLAG_SHIFT);
    }
    else {
      // 8 bytes
      fhd |= (3 << ZSTD_FHD_FCS_FLAG_SHIFT);
    }
  }

  buf[pos++] = fhd;

  // Write window descriptor (if not single segment)
  if (!header->single_segment) {
    // Encode window size as exponent + mantissa
    // window_size = 2^(10 + exponent) + 2^(10 + exponent - 3) * mantissa
    // We simplify by using exponent = window_log - 10, mantissa = 0
    uint8_t exponent = header->window_log - 10;
    uint8_t mantissa = 0;
    uint8_t wd = (exponent << 3) | mantissa;
    buf[pos++] = wd;
  }

  // Write dictionary ID (none supported yet)
  // No bytes written

  // Write frame content size (if present)
  if (header->content_size_present) {
    uint8_t fcs_flag =
        (fhd & ZSTD_FHD_FCS_FLAG_MASK) >> ZSTD_FHD_FCS_FLAG_SHIFT;
    if (header->single_segment && fcs_flag == 0) {
      // 1 byte
      buf[pos++] = (uint8_t)header->content_size;
    }
    else {
      switch (fcs_flag) {
      case 0:
        // No FCS field (shouldn't happen if content_size_present)
        break;
      case 1:
        // 2 bytes
        gcomp_write_le16(buf + pos, (uint16_t)(header->content_size - 256));
        pos += 2;
        break;
      case 2:
        // 4 bytes
        gcomp_write_le32(buf + pos, (uint32_t)header->content_size);
        pos += 4;
        break;
      case 3:
        // 8 bytes
        gcomp_write_le64(buf + pos, header->content_size);
        pos += 8;
        break;
      default:
        break;
      }
    }
  }

  *header_len_out = pos;
  return GCOMP_OK;
}

//
// Block Header
//

gcomp_status_t zstd_parse_block_header(const uint8_t * buf, bool * last_out,
    uint8_t * type_out, uint32_t * size_out) {
  if (!buf || !last_out || !type_out || !size_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Block header is 3 bytes (24 bits):
  // - Bit 0: Last_Block
  // - Bits 1-2: Block_Type
  // - Bits 3-23: Block_Size (21 bits)

  uint32_t header = buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16);

  *last_out = (header & ZSTD_BLOCK_LAST_FLAG) != 0;
  *type_out = (header >> ZSTD_BLOCK_TYPE_SHIFT) & 0x03;
  *size_out = header >> ZSTD_BLOCK_SIZE_SHIFT;

  // Validate block size
  if (*size_out > ZSTD_BLOCK_SIZE_MAX) {
    return GCOMP_ERR_CORRUPT;
  }

  return GCOMP_OK;
}

void zstd_write_block_header(
    uint8_t * buf, bool last, uint8_t type, uint32_t size) {
  if (!buf) {
    return;
  }

  // Build 24-bit header
  uint32_t header = 0;
  if (last) {
    header |= ZSTD_BLOCK_LAST_FLAG;
  }
  header |= ((uint32_t)type << ZSTD_BLOCK_TYPE_SHIFT);
  header |= (size << ZSTD_BLOCK_SIZE_SHIFT);

  buf[0] = (uint8_t)(header);
  buf[1] = (uint8_t)(header >> 8);
  buf[2] = (uint8_t)(header >> 16);
}
