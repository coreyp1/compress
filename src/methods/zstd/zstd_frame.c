/**
 * @file zstd_frame.c
 *
 * Zstandard frame format helpers for the Ghoti.io Compress library.
 *
 * ## Frame Format Structure (RFC 8878)
 *
 * A Zstd frame consists of:
 * 1. **Magic Number** (4 bytes): 0xFD2FB528 (little-endian)
 * 2. **Frame Header**: Frame_Header_Descriptor (1 byte) plus optional
 *    Window_Descriptor, Dictionary_ID, Frame_Content_Size
 * 3. **Data Blocks**: One or more blocks, each with 3-byte header + payload
 * 4. **Content Checksum** (optional, 4 bytes): xxHash64 of decompressed
 *    content, low 32 bits (per spec)
 *
 * ## xxHash64 Usage
 *
 * When the frame header has Content_Checksum_Flag set, the encoder computes
 * xxHash64 over all decompressed bytes and writes the low 32 bits at frame
 * end. The decoder validates this checksum and returns GCOMP_ERR_CORRUPT on
 * mismatch. See zstd_encoder.c (content_hash) and zstd_decoder.c
 * (content_hash, content_checksum).
 *
 * This file provides:
 * - Building and parsing frame headers (zstd_write_frame_header,
 *   zstd_parse_frame_header in decoder)
 * - Building and parsing block headers (zstd_write_block_header,
 *   zstd_parse_block_header)
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/compress/macros.h>
#include "zstd_internal.h"
#include <string.h>

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

  // Dictionary ID flag (0 = none, 1 = 1 byte, 2 = 2 bytes, 3 = 4 bytes)
  if (header->dict_id != 0) {
    if (header->dict_id <= 0xFF) {
      fhd |= 1;
    }
    else if (header->dict_id <= 0xFFFF) {
      fhd |= 2;
    }
    else {
      fhd |= 3;
    }
  }

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

  // Write dictionary ID (if present)
  if (header->dict_id != 0) {
    uint8_t did_flag = fhd & ZSTD_FHD_DICT_ID_FLAG_MASK;
    if (did_flag == 1) {
      buf[pos++] = (uint8_t)header->dict_id;
    }
    else if (did_flag == 2) {
      gcomp_write_le16(buf + pos, (uint16_t)header->dict_id);
      pos += 2;
    }
    else if (did_flag == 3) {
      gcomp_write_le32(buf + pos, header->dict_id);
      pos += 4;
    }
  }

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

  // Block header is 3 bytes (24 bits) per RFC 8878:
  // - Bit 0: Last_Block
  // - Bits 1-2: Block_Type (0=raw, 1=RLE, 2=compressed, 3=reserved)
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

//
// Frame Header: parsing
//
// One parser, used by the decoder and by gcomp_peek().  Two would be two
// readings of RFC 8878 section 3.1.1.1, and the one a caller was shown would
// not have to agree with the one that decoded the bytes.  The LZ4 decoder
// already states that rule where it parses its own frame descriptor; this is
// the same rule applied to Zstandard.
//
// What lives here is only what the bytes say.  What to *do* about it - whether
// a declared window is larger than this decoder will hold, whether a named
// dictionary is present - is policy, and stays with the decoder that has the
// limits and the dictionary to check against.
//

size_t zstd_dict_id_size(uint8_t flag) {
  static const size_t sizes[] = {0, 1, 2, 4};
  return sizes[flag & 0x03];
}

size_t zstd_fcs_size(uint8_t flag, bool single_segment) {
  if (single_segment && flag == 0) {
    // RFC 8878 section 3.1.1.1.4: with Single_Segment_Flag set, a
    // Frame_Content_Size_Flag of 0 still means one byte of content size.
    return 1;
  }
  static const size_t sizes[] = {0, 2, 4, 8};
  return sizes[flag & 0x03];
}

size_t zstd_frame_header_length(uint8_t fhd) {
  bool single_segment = (fhd & ZSTD_FHD_SINGLE_SEGMENT_FLAG) != 0;
  uint8_t dict_id_flag = fhd & ZSTD_FHD_DICT_ID_FLAG_MASK;
  uint8_t fcs_flag = (fhd & ZSTD_FHD_FCS_FLAG_MASK) >> ZSTD_FHD_FCS_FLAG_SHIFT;

  // Magic_Number (4) and Frame_Header_Descriptor (1), then the optional
  // fields.  Each term is bounded by a constant - the largest total is
  // 4 + 1 + 1 + 4 + 8 = 18 - so this cannot overflow and needs no checked
  // arithmetic to say so.
  size_t len = 5u;
  if (!single_segment) {
    len += 1u; // Window_Descriptor
  }
  len += zstd_dict_id_size(dict_id_flag);
  len += zstd_fcs_size(fcs_flag, single_segment);
  return len;
}

gcomp_status_t zstd_frame_header_parse(const uint8_t * buf, size_t buf_size,
    zstd_frame_header_t * header_out, uint64_t * window_size_out,
    size_t * needed_out) {
  if (!buf || !header_out || !window_size_out || !needed_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *window_size_out = 0;
  *needed_out = ZSTD_FRAME_HEADER_MIN_SIZE;
  if (buf_size < ZSTD_FRAME_HEADER_MIN_SIZE) {
    return GCOMP_ERR_LIMIT;
  }

  uint32_t magic = gcomp_read_le32(buf);
  if (magic != ZSTD_MAGIC) {
    return GCOMP_ERR_CORRUPT;
  }

  uint8_t fhd = buf[4];
  // RFC 8878 section 3.1.1.1.1: bit 3 is reserved and "must be zero".
  if (fhd & ZSTD_FHD_RESERVED_BIT) {
    return GCOMP_ERR_CORRUPT;
  }

  size_t header_len = zstd_frame_header_length(fhd);
  *needed_out = header_len;
  if (buf_size < header_len) {
    return GCOMP_ERR_LIMIT;
  }

  zstd_frame_header_t h;
  memset(&h, 0, sizeof(h));
  h.descriptor = fhd;
  h.dict_id_flag = fhd & ZSTD_FHD_DICT_ID_FLAG_MASK;
  h.content_checksum = (fhd & ZSTD_FHD_CHECKSUM_FLAG) != 0;
  h.single_segment = (fhd & ZSTD_FHD_SINGLE_SEGMENT_FLAG) != 0;
  h.fcs_flag = (fhd & ZSTD_FHD_FCS_FLAG_MASK) >> ZSTD_FHD_FCS_FLAG_SHIFT;

  size_t pos = 5;
  uint64_t window_size = 0;

  if (!h.single_segment) {
    // RFC 8878 section 3.1.1.1.2:
    //   Window_Log = 10 + Exponent
    //   Window_Base = 1 << Window_Log
    //   Window_Size = Window_Base + (Window_Base / 8) * Mantissa
    //
    // The exponent is five bits, so a frame may legally declare a window log
    // of up to 41.  Computed in 64 bits because a 32-bit shift by that much is
    // undefined - which it once was here, and a nine-byte frame header reached
    // it.  Whether the result is acceptable is the caller's decision, not this
    // function's: an enormous window is a well-formed frame asking for more
    // history than a particular decoder will hold.
    uint8_t wd = buf[pos++];
    unsigned exponent = (wd >> 3) & 0x1Fu;
    unsigned mantissa = wd & 0x07u;
    unsigned window_log = exponent + 10u;
    uint64_t base = (uint64_t)1u << window_log;
    window_size = base + (base / 8u) * mantissa;
    h.window_log = (uint8_t)window_log;
    h.window_size =
        (window_size > UINT32_MAX) ? UINT32_MAX : (uint32_t)window_size;
  }

  size_t dict_id_len = zstd_dict_id_size(h.dict_id_flag);
  switch (dict_id_len) {
  case 1: h.dict_id = buf[pos]; break;
  case 2: h.dict_id = gcomp_read_le16(buf + pos); break;
  case 4: h.dict_id = gcomp_read_le32(buf + pos); break;
  default: break;
  }
  pos += dict_id_len;

  size_t fcs_len = zstd_fcs_size(h.fcs_flag, h.single_segment);
  h.content_size_present = (fcs_len > 0);
  switch (fcs_len) {
  case 1: h.content_size = buf[pos]; break;
  // RFC 8878 section 3.1.1.1.4: the two-byte form carries the value minus 256.
  case 2: h.content_size = (uint64_t)gcomp_read_le16(buf + pos) + 256u; break;
  case 4: h.content_size = gcomp_read_le32(buf + pos); break;
  case 8: h.content_size = gcomp_read_le64(buf + pos); break;
  default: break;
  }
  pos += fcs_len;

  if (h.single_segment) {
    // Section 3.1.1.1.2: with no Window_Descriptor, "Window_Size is
    // Frame_Content_Size" - the whole content is one segment.
    window_size = h.content_size;
    h.window_size =
        (window_size > UINT32_MAX) ? UINT32_MAX : (uint32_t)window_size;
  }

  *header_out = h;
  *window_size_out = window_size;
  *needed_out = pos;
  return GCOMP_OK;
}
