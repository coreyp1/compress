/**
 * @file zstd_literals.c
 *
 * Literals section decoder for the Ghoti.io Compress library.
 *
 * ## Literals Section Format
 *
 * The literals section in a zstd compressed block contains the literal bytes
 * that will be copied to output. Literals are encoded in one of four ways:
 *
 * - Raw_Literals_Block: Uncompressed literals
 * - RLE_Literals_Block: Single byte repeated
 * - Compressed_Literals_Block: Huffman encoded
 * - Treeless_Literals_Block: Huffman encoded with previous tree
 *
 * ## Header Format
 *
 * The header is 1-5 bytes depending on literals type and size:
 * - Bits 0-1: Block type (raw=0, RLE=1, compressed=2, treeless=3)
 * - Bits 2-3: Size format (determines header size)
 * - Remaining bits: Regenerated size, compressed size, stream count
 *
 * Reference:
 * https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include "zstd_internal.h"
#include <string.h>

//
// Literals Constants
//

#define LITERALS_TYPE_RAW 0        ///< Raw uncompressed literals
#define LITERALS_TYPE_RLE 1        ///< RLE single byte repeated
#define LITERALS_TYPE_COMPRESSED 2 ///< Huffman compressed
#define LITERALS_TYPE_TREELESS 3   ///< Huffman with previous tree

#define LITERALS_SIZE_FORMAT_1STREAM_5BIT 0  ///< 1 stream, 5-bit size
#define LITERALS_SIZE_FORMAT_1STREAM_12BIT 1 ///< 1 stream, 12-bit size
#define LITERALS_SIZE_FORMAT_1STREAM_20BIT 2 ///< 1 stream, 20-bit size
#define LITERALS_SIZE_FORMAT_4STREAMS 3      ///< 4 streams, 14-bit sizes

// Note: HUF_MAX_TABLE_SIZE is defined in zstd_internal.h

//
// Literals Header Parsing
//

typedef struct {
  uint8_t type;              ///< Literals type (raw/RLE/compressed/treeless)
  uint8_t size_format;       ///< Size format (0-3)
  uint32_t regenerated_size; ///< Decompressed size
  uint32_t compressed_size;  ///< Compressed size (0 for raw/RLE)
  bool four_streams;         ///< True if 4-stream mode
} zstd_literals_header_t;

/**
 * @brief Parse literals section header.
 *
 * @param src Source data
 * @param src_size Source size
 * @param header Output header structure
 * @param bytes_read_out Output: bytes consumed for header
 * @return GCOMP_OK on success
 */
static gcomp_status_t zstd_literals_parse_header(const uint8_t * src,
    size_t src_size, zstd_literals_header_t * header, size_t * bytes_read_out) {
  if (!src || !header || !bytes_read_out || src_size < 1) {
    return GCOMP_ERR_INVALID_ARG;
  }

  uint8_t byte0 = src[0];
  header->type = byte0 & 0x03;
  header->size_format = (byte0 >> 2) & 0x03;
  header->four_streams = false;
  header->compressed_size = 0;

  size_t header_size = 0;

  switch (header->type) {
  case LITERALS_TYPE_RAW:
  case LITERALS_TYPE_RLE:
    // Raw and RLE have the same header format
    switch (header->size_format) {
    case 0:
    case 2:
      // 1 byte header, 5-bit size
      header->regenerated_size = byte0 >> 3;
      header_size = 1;
      break;

    case 1:
      // 2 byte header, 12-bit size
      if (src_size < 2) {
        return GCOMP_ERR_CORRUPT;
      }
      header->regenerated_size = (byte0 >> 4) | ((uint32_t)src[1] << 4);
      header_size = 2;
      break;

    case 3:
      // 3 byte header, 20-bit size
      if (src_size < 3) {
        return GCOMP_ERR_CORRUPT;
      }
      header->regenerated_size =
          (byte0 >> 4) | ((uint32_t)src[1] << 4) | ((uint32_t)src[2] << 12);
      header_size = 3;
      break;
    }
    break;

  case LITERALS_TYPE_COMPRESSED:
  case LITERALS_TYPE_TREELESS:
    // Compressed and treeless have similar header format
    switch (header->size_format) {
    case 0:
    case 1:
      // Single stream: 1+1 or 2+2 bytes
      if (header->size_format == 0) {
        // 2 bytes total: 6-bit sizes (10+10 bits)
        if (src_size < 2) {
          return GCOMP_ERR_CORRUPT;
        }
        // Note: spec says this is rare/impossible for compressed literals
        header->regenerated_size = (byte0 >> 4) | ((src[1] & 0x3F) << 4);
        header->compressed_size = src[1] >> 6;
        header_size = 2;
        header->four_streams = false;
      }
      else {
        // 3 bytes total: 10+10 bit sizes
        if (src_size < 3) {
          return GCOMP_ERR_CORRUPT;
        }
        header->regenerated_size =
            (byte0 >> 4) | ((uint32_t)(src[1] & 0x3F) << 4);
        header->compressed_size = (src[1] >> 6) | ((uint32_t)src[2] << 2);
        header_size = 3;
        header->four_streams = false;
      }
      break;

    case 2:
      // 4 streams: 3 bytes, 14+14 bit sizes
      if (src_size < 4) {
        return GCOMP_ERR_CORRUPT;
      }
      header->regenerated_size = (byte0 >> 4) |
          ((uint32_t)(src[1] & 0x3F) << 4) | ((uint32_t)(src[2] & 0x03) << 10);
      header->compressed_size = (src[2] >> 2) | ((uint32_t)src[3] << 6);
      header_size = 4;
      header->four_streams = true;
      break;

    case 3:
      // 4 streams: 4 bytes, 18+18 bit sizes
      if (src_size < 5) {
        return GCOMP_ERR_CORRUPT;
      }
      header->regenerated_size = (byte0 >> 4) | ((uint32_t)src[1] << 4) |
          ((uint32_t)(src[2] & 0x03) << 12);
      header->compressed_size =
          (src[2] >> 2) | ((uint32_t)src[3] << 6) | ((uint32_t)src[4] << 14);
      header_size = 5;
      header->four_streams = true;
      break;
    }
    break;

  default:
    return GCOMP_ERR_CORRUPT;
  }

  *bytes_read_out = header_size;
  return GCOMP_OK;
}

//
// Public API
//

gcomp_status_t zstd_literals_decode(zstd_decoder_state_t * state,
    const uint8_t * src, size_t src_size, uint8_t * dst, size_t dst_capacity,
    size_t * regenerated_size_out, size_t * bytes_read_out) {
  if (!state || !src || !dst || !regenerated_size_out || !bytes_read_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Parse header
  zstd_literals_header_t header;
  size_t header_size;

  gcomp_status_t status =
      zstd_literals_parse_header(src, src_size, &header, &header_size);
  if (status != GCOMP_OK) {
    return status;
  }

  // Validate sizes
  if (header.regenerated_size > dst_capacity) {
    return GCOMP_ERR_LIMIT;
  }

  const uint8_t * literals_data = src + header_size;
  size_t literals_data_size = src_size - header_size;

  switch (header.type) {
  case LITERALS_TYPE_RAW:
    // Raw literals: copy directly
    if (literals_data_size < header.regenerated_size) {
      return GCOMP_ERR_CORRUPT;
    }
    memcpy(dst, literals_data, header.regenerated_size);
    *regenerated_size_out = header.regenerated_size;
    *bytes_read_out = header_size + header.regenerated_size;
    break;

  case LITERALS_TYPE_RLE:
    // RLE literals: repeat single byte
    if (literals_data_size < 1) {
      return GCOMP_ERR_CORRUPT;
    }
    memset(dst, literals_data[0], header.regenerated_size);
    *regenerated_size_out = header.regenerated_size;
    *bytes_read_out = header_size + 1;
    break;

  case LITERALS_TYPE_COMPRESSED:
    // Compressed literals: read Huffman tree and decode
    {
      // Read Huffman table
      unsigned max_bits;
      size_t huf_header_size;

      // Allocate Huffman table if not present
      if (!state->huf_table) {
        state->huf_table = gcomp_malloc(
            state->allocator, HUF_MAX_TABLE_SIZE * sizeof(zstd_huf_entry_t));
        if (!state->huf_table) {
          return GCOMP_ERR_MEMORY;
        }
        state->huf_table_size = HUF_MAX_TABLE_SIZE;
        gcomp_memory_track_alloc(
            &state->mem_tracker, HUF_MAX_TABLE_SIZE * sizeof(zstd_huf_entry_t));
      }

      status = zstd_huf_read_table(literals_data, literals_data_size,
          state->huf_table, state->huf_table_size, &max_bits, &huf_header_size);
      if (status != GCOMP_OK) {
        return status;
      }

      state->huf_table_valid = true;
      state->huf_max_bits = max_bits;

      // Decode literals
      const uint8_t * huf_stream = literals_data + huf_header_size;
      size_t huf_stream_size = header.compressed_size - huf_header_size;

      size_t decoded_size;
      if (header.four_streams) {
        // 4-stream mode: read jump table (6 bytes)
        if (huf_stream_size < 6) {
          return GCOMP_ERR_CORRUPT;
        }
        uint32_t jump_table[3];
        jump_table[0] = gcomp_read_le16(huf_stream);
        jump_table[1] = gcomp_read_le16(huf_stream + 2);
        jump_table[2] = gcomp_read_le16(huf_stream + 4);

        status = zstd_huf_decode_4streams(state->huf_table, max_bits,
            huf_stream + 6, huf_stream_size - 6, jump_table, dst,
            header.regenerated_size, &decoded_size);
      }
      else {
        status = zstd_huf_decode_1stream(state->huf_table, max_bits, huf_stream,
            huf_stream_size, dst, header.regenerated_size, &decoded_size);
      }

      if (status != GCOMP_OK) {
        return status;
      }

      if (decoded_size != header.regenerated_size) {
        return GCOMP_ERR_CORRUPT;
      }

      *regenerated_size_out = header.regenerated_size;
      *bytes_read_out = header_size + header.compressed_size;
    }
    break;

  case LITERALS_TYPE_TREELESS:
    // Treeless literals: use previous Huffman tree
    {
      if (!state->huf_table_valid) {
        return GCOMP_ERR_CORRUPT;
      }

      // Decode using existing Huffman table
      size_t decoded_size;
      if (header.four_streams) {
        // 4-stream mode: read jump table (6 bytes)
        if (literals_data_size < 6) {
          return GCOMP_ERR_CORRUPT;
        }
        uint32_t jump_table[3];
        jump_table[0] = gcomp_read_le16(literals_data);
        jump_table[1] = gcomp_read_le16(literals_data + 2);
        jump_table[2] = gcomp_read_le16(literals_data + 4);

        status = zstd_huf_decode_4streams(state->huf_table, state->huf_max_bits,
            literals_data + 6, header.compressed_size - 6, jump_table, dst,
            header.regenerated_size, &decoded_size);
      }
      else {
        status = zstd_huf_decode_1stream(state->huf_table, state->huf_max_bits,
            literals_data, header.compressed_size, dst, header.regenerated_size,
            &decoded_size);
      }

      if (status != GCOMP_OK) {
        return status;
      }

      if (decoded_size != header.regenerated_size) {
        return GCOMP_ERR_CORRUPT;
      }

      *regenerated_size_out = header.regenerated_size;
      *bytes_read_out = header_size + header.compressed_size;
    }
    break;

  default:
    return GCOMP_ERR_CORRUPT;
  }

  return GCOMP_OK;
}

size_t zstd_literals_header_size(const uint8_t * src, size_t src_size) {
  if (!src || src_size < 1) {
    return 0;
  }

  uint8_t byte0 = src[0];
  uint8_t type = byte0 & 0x03;
  uint8_t size_format = (byte0 >> 2) & 0x03;

  switch (type) {
  case LITERALS_TYPE_RAW:
  case LITERALS_TYPE_RLE:
    switch (size_format) {
    case 0:
    case 2:
      return 1;
    case 1:
      return 2;
    case 3:
      return 3;
    }
    break;

  case LITERALS_TYPE_COMPRESSED:
  case LITERALS_TYPE_TREELESS:
    switch (size_format) {
    case 0:
      return 2;
    case 1:
      return 3;
    case 2:
      return 4;
    case 3:
      return 5;
    }
    break;
  }

  return 0;
}
