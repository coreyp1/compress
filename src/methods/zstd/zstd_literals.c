/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Compress.
 *
 * Ghoti.io Compress is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Compress is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file zstd_literals.c
 *
 * Literals section encoder and decoder for the Ghoti.io Compress library.
 *
 * ## Literals Section Format
 *
 * The literals section in a zstd compressed block contains the literal bytes
 * that will be copied to output. Literals are encoded in one of four ways:
 *
 * | Type | Value | Description |
 * |------|-------|-------------|
 * | Raw_Literals_Block | 0 | Uncompressed literals, copied directly |
 * | RLE_Literals_Block | 1 | Single byte repeated N times |
 * | Compressed_Literals_Block | 2 | Huffman encoded with inline tree |
 * | Treeless_Literals_Block | 3 | Huffman encoded, reuse previous tree |
 *
 * ## Header Format
 *
 * The header is 1-5 bytes depending on literals type and size:
 * - Bits 0-1: Block type (raw=0, RLE=1, compressed=2, treeless=3)
 * - Bits 2-3: Size format (determines header size and field widths)
 * - Remaining bits: Regenerated size, compressed size, stream count
 *
 * ### Header sizes by type and format:
 *
 * | Type | Format | Bytes | Regen Size | Comp Size |
 * |------|--------|-------|------------|-----------|
 * | Raw/RLE | 0,2 | 1 | 5 bits | N/A |
 * | Raw/RLE | 1 | 2 | 12 bits | N/A |
 * | Raw/RLE | 3 | 3 | 20 bits | N/A |
 * | Compressed | 0 | 2 | 10 bits | 6 bits |
 * | Compressed | 1 | 3 | 10 bits | 10 bits |
 * | Compressed | 2 | 4 | 14 bits | 14 bits |
 * | Compressed | 3 | 5 | 18 bits | 18 bits |
 *
 * ## Encoder Behavior
 *
 * The encoder automatically selects the best literals encoding:
 *
 * 1. **Small inputs (<32 bytes)**: Use raw encoding (Huffman overhead too high)
 *
 * 2. **Build Huffman table**: Count symbol frequencies, build optimal tree
 *
 * 3. **Estimate compression**: Calculate weights_size + bitstream_size
 *
 * 4. **Decision threshold**: Use Huffman only if savings > 10%
 *    - If compressed_size + header >= raw_size: use raw
 *    - Otherwise: use Huffman compressed
 *
 * This adaptive selection ensures we never make data larger by attempting
 * compression on data that doesn't benefit from it.
 *
 * ## Decoder Behavior
 *
 * The decoder handles all four literal types and supports:
 * - Single-stream Huffman decoding (Size_Format 0)
 * - Four-stream Huffman decoding (Size_Format 1, 2 or 3; jump
 *   table + 4 concatenated streams; encoder uses this when appropriate)
 * - Treeless mode (reuse Huffman table from previous block)
 * - FSE-compressed Huffman weights (header_byte < 128; used when >127
 *   symbols; encoder uses this when the weight table has >127 symbols)
 *
 * Reference:
 * https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/compress/macros.h>
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
      // Single stream: 3 bytes total, 10+10 bit sizes
      // Per RFC 8878, size_format 0 for compressed = single stream with both
      // sizes Parse as 24-bit little-endian: bits 0-1: type, 2-3: fmt, 4-13:
      // regen, 14-23: comp
      if (src_size < 3) {
        return GCOMP_ERR_CORRUPT;
      }
      {
        uint32_t val = (uint32_t)byte0 | ((uint32_t)src[1] << 8) |
            ((uint32_t)src[2] << 16);
        header->regenerated_size = (val >> 4) & 0x3FF; // 10 bits
        header->compressed_size = (val >> 14) & 0x3FF; // 10 bits
      }
      header_size = 3;
      header->four_streams = false;
      break;

    case 1:
      // 4 streams: 3 bytes total, 10+10 bit sizes
      // Parse same as case 0 but four_streams = true
      if (src_size < 3) {
        return GCOMP_ERR_CORRUPT;
      }
      {
        uint32_t val = (uint32_t)byte0 | ((uint32_t)src[1] << 8) |
            ((uint32_t)src[2] << 16);
        header->regenerated_size = (val >> 4) & 0x3FF; // 10 bits
        header->compressed_size = (val >> 14) & 0x3FF; // 10 bits
      }
      header_size = 3;
      header->four_streams = true;
      break;

    case 2:
      // 4 streams: 4 bytes, 14+14 bit sizes
      // Parse as 32-bit little-endian value:
      // bits 0-1: type, bits 2-3: size_format, bits 4-17: regen, bits 18-31:
      // comp
      if (src_size < 4) {
        return GCOMP_ERR_CORRUPT;
      }
      {
        uint32_t val = (uint32_t)byte0 | ((uint32_t)src[1] << 8) |
            ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
        header->regenerated_size = (val >> 4) & 0x3FFF; // 14 bits
        header->compressed_size = (val >> 18) & 0x3FFF; // 14 bits
      }
      header_size = 4;
      header->four_streams = true;
      break;

    case 3:
      // 4 streams: 5 bytes, 18+18 bit sizes
      // Parse as 40-bit little-endian value:
      // bits 0-1: type, bits 2-3: size_format, bits 4-21: regen, bits 22-39:
      // comp
      if (src_size < 5) {
        return GCOMP_ERR_CORRUPT;
      }
      {
        uint64_t val = (uint64_t)byte0 | ((uint64_t)src[1] << 8) |
            ((uint64_t)src[2] << 16) | ((uint64_t)src[3] << 24) |
            ((uint64_t)src[4] << 32);
        header->regenerated_size = (val >> 4) & 0x3FFFF; // 18 bits
        header->compressed_size = (val >> 22) & 0x3FFFF; // 18 bits
      }
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

  // RFC 8878 section 3.1.1.3.1.1: for a Compressed_Literals_Block or a
  // Treeless_Literals_Block the header carries Compressed_Size, the length of
  // everything that follows the header -- the Huffman tree description, when
  // present, plus the stream or streams.  It is a field of the input, so it
  // can name more bytes than the block actually holds.
  //
  // Nothing below validated it.  Every later size was derived from it by
  // subtraction, so an oversized value either ran the Huffman reader off the
  // end of the block or, where the subtraction went negative, wrapped to a
  // size_t near SIZE_MAX that the reader then indexed from.
  if ((header.type == LITERALS_TYPE_COMPRESSED ||
          header.type == LITERALS_TYPE_TREELESS) &&
      header.compressed_size > literals_data_size) {
    return GCOMP_ERR_CORRUPT;
  }

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

      // The tree description lives inside Compressed_Size, so that -- not the
      // rest of the block -- is the bound the reader is given.  Passing the
      // larger literals_data_size let a truncated description keep reading
      // into the sequences section that follows.
      status = zstd_huf_read_table(literals_data, header.compressed_size,
          state->huf_table, state->huf_table_size, &max_bits, &huf_header_size);
      if (status != GCOMP_OK) {
        return status;
      }

      // zstd_huf_read_table stops at the end of the description, which by the
      // line above cannot exceed Compressed_Size; the check is kept so the
      // subtraction can never wrap if that ever stops being true.
      if (huf_header_size > header.compressed_size) {
        return GCOMP_ERR_CORRUPT;
      }

      state->huf_table_valid = true;
      state->huf_max_bits = max_bits;

      // Decode literals
      const uint8_t * huf_stream = literals_data + huf_header_size;
      size_t huf_stream_size = header.compressed_size - huf_header_size;

      size_t decoded_size;
      if (header.four_streams) {
        // RFC 8878 section 4.2.2: whenever the literals are carried in four
        // streams the 6-byte Jump_Table is present.  It is not conditional on
        // Regenerated_Size -- the reference encoder picks four streams well
        // below 1024 bytes, and skipping the table there consumed the jump
        // offsets as literal bits.
        uint32_t jump_table[3];

        if (huf_stream_size < 6) {
          return GCOMP_ERR_CORRUPT;
        }
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
        // RFC 8878 section 4.2.2: four streams always carry the 6-byte
        // Jump_Table, at any Regenerated_Size.  See the same correction in
        // the Compressed_Literals_Block path above.
        uint32_t jump_table[3];

        if (literals_data_size < 6 || header.compressed_size < 6) {
          return GCOMP_ERR_CORRUPT;
        }
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

//============================================================================
// Literals Encoding
//============================================================================
//
// The literals encoder chooses between raw and Huffman encoding based on
// the estimated compression benefit. The decision process:
//
//   Input literals
//         │
//         ▼
//   ┌─────────────┐
//   │ Size < 32?  │───Yes───► Raw encoding (Huffman overhead too high)
//   └─────────────┘
//         │ No
//         ▼
//   ┌─────────────────────┐
//   │ Build Huffman table │
//   │ from frequencies    │
//   └─────────────────────┘
//         │
//         ▼
//   ┌─────────────────────┐
//   │ Only 1 symbol?      │───Yes───► Raw encoding (nothing to compress)
//   └─────────────────────┘
//         │ No
//         ▼
//   ┌─────────────────────┐
//   │ Write weights +     │
//   │ encode bitstream    │
//   └─────────────────────┘
//         │
//         ▼
//   ┌─────────────────────────────┐
//   │ compressed_size + 5 <       │───No───► Raw encoding (no benefit)
//   │ literals_size * 0.9 ?       │
//   └─────────────────────────────┘
//         │ Yes
//         ▼
//   Huffman compressed output
//
// IMPLEMENTATION HEURISTICS (not format requirements):
//
// - 32-byte minimum: The Huffman weights header adds ~num_symbols/2 bytes
//   of overhead. For small inputs, this overhead exceeds any compression
//   benefit. The 32-byte threshold is a tunable heuristic.
//
// - 10% savings threshold: We require at least 10% size reduction to use
//   Huffman. This avoids marginal compression that adds decoder complexity
//   for minimal benefit. This threshold is also a tunable heuristic.
//
// The Zstd format (RFC 8878) allows encoders complete freedom to choose
// raw, RLE, or Huffman encoding for any literals section. Other encoders
// may use different heuristics.
//
//============================================================================

//
// Encoder: Raw Literals
//

gcomp_status_t zstd_literals_encode_raw(const uint8_t * literals,
    size_t literals_size, uint8_t * output, size_t output_cap,
    size_t * output_len_out);

//
// Encoder: Compressed Literals (Huffman)
//

/**
 * @brief Encode literals using Huffman compression.
 *
 * This function:
 * 1. Counts symbol frequencies
 * 2. Builds a Huffman encoding table
 * 3. Writes the Huffman weights header
 * 4. Encodes the literals as a backward bitstream
 *
 * The output format is:
 * - Literals header (3-5 bytes for compressed type)
 * - Huffman weights description
 * - Compressed bitstream
 *
 * @param alloc Allocator for scratch memory; NULL uses the default
 * @param literals Input literal bytes
 * @param literals_size Number of literals
 * @param output Output buffer
 * @param output_cap Output capacity
 * @param output_len_out Output: bytes written
 * @return GCOMP_OK on success, error code on failure
 */
gcomp_status_t zstd_literals_encode_compressed(
    const gcomp_allocator_t * alloc, gcomp_stepdown_tally_t * stepdowns,
    const uint8_t * literals, size_t literals_size, uint8_t * output,
    size_t output_cap, size_t * output_len_out) {
  if (!output || !output_len_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Handle empty or very small input - use raw encoding instead.
  // The 32-byte threshold is a tunable heuristic, not a format requirement.
  // Huffman overhead (weights header) rarely pays off for tiny inputs.
  if (literals_size < 32 || !literals) {
    gcomp_stepdown_note(stepdowns, GCOMP_STEPDOWN_LITERALS_NOT_WORTH_CODING);
    return zstd_literals_encode_raw(
        literals, literals_size, output, output_cap, output_len_out);
  }

  // Count symbol frequencies
  uint32_t freq[256] = {0};
  for (size_t i = 0; i < literals_size; i++) {
    freq[literals[i]]++;
  }

  // Build Huffman encoding table
  zstd_huf_enc_table_t huf_table;
  gcomp_status_t status =
      zstd_huf_build_enc_table(alloc, freq, &huf_table);
  if (status != GCOMP_OK) {
    // Fall back to raw encoding
    gcomp_stepdown_note(stepdowns,
        status == GCOMP_ERR_MEMORY ? GCOMP_STEPDOWN_NO_MEMORY
                                   : GCOMP_STEPDOWN_CODE_REJECTED);
    return zstd_literals_encode_raw(
        literals, literals_size, output, output_cap, output_len_out);
  }

  // Check if compression is worthwhile (estimate compressed size)
  // If all symbols have same frequency, Huffman won't help
  if (huf_table.num_symbols <= 1 || huf_table.max_bits == 0) {
    gcomp_stepdown_note(stepdowns, GCOMP_STEPDOWN_LITERALS_NOT_WORTH_CODING);
    return zstd_literals_encode_raw(
        literals, literals_size, output, output_cap, output_len_out);
  }

  // Temporary buffers for weights and compressed stream
  // Reserve space: header (5 bytes max) + weights + compressed data
  size_t header_offset = 5; // We'll write the header last
  uint8_t * weights_buf = output + header_offset;
  size_t weights_cap = output_cap - header_offset;

  // Write Huffman weights
  size_t weights_size;
  status = zstd_huf_write_weights(
      &huf_table, weights_buf, weights_cap, &weights_size);
  if (status != GCOMP_OK) {
    // Fall back to raw encoding.
    //
    // GCOMP_ERR_UNSUPPORTED here is a decision, not a failure.  The only
    // alphabets zstd_huf_write_weights() cannot describe are the ones where
    // a Huffman code would buy nothing: every used symbol sharing one code
    // length, which is a flat code costing exactly what storing costs, and a
    // single-symbol alphabet, which belongs in an RLE literals block.  See
    // the two GCOMP_ERR_UNSUPPORTED returns there.
    gcomp_stepdown_note(stepdowns,
        status == GCOMP_ERR_UNSUPPORTED
            ? GCOMP_STEPDOWN_LITERALS_NOT_WORTH_CODING
            : status == GCOMP_ERR_LIMIT ? GCOMP_STEPDOWN_NO_ROOM
                                        : GCOMP_STEPDOWN_ENCODE_FAILED);
    return zstd_literals_encode_raw(
        literals, literals_size, output, output_cap, output_len_out);
  }

  // Encode literals as Huffman bitstream (1-stream or 4-stream)
  // Per RFC 8878: use 4 streams when regenerated_size >= 1024 for faster
  // decoding. 4-stream layout: jump table (6 bytes) + 4 concatenated streams.
  uint8_t * stream_buf = weights_buf + weights_size;
  size_t stream_cap = weights_cap - weights_size;

  size_t stream_size;
  if (literals_size >= 1024) {
    status = zstd_huf_encode_4streams(&huf_table, literals, literals_size,
        stream_buf, stream_cap, &stream_size);
  }
  else {
    status = zstd_huf_encode_1stream(&huf_table, literals, literals_size,
        stream_buf, stream_cap, &stream_size);
  }
  if (status != GCOMP_OK) {
    // Fall back to raw encoding.
    //
    // GCOMP_ERR_UNSUPPORTED here is a decision, not a failure.  The only
    // alphabets zstd_huf_write_weights() cannot describe are the ones where
    // a Huffman code would buy nothing: every used symbol sharing one code
    // length, which is a flat code costing exactly what storing costs, and a
    // single-symbol alphabet, which belongs in an RLE literals block.  See
    // the two GCOMP_ERR_UNSUPPORTED returns there.
    gcomp_stepdown_note(stepdowns,
        status == GCOMP_ERR_UNSUPPORTED
            ? GCOMP_STEPDOWN_LITERALS_NOT_WORTH_CODING
            : status == GCOMP_ERR_LIMIT ? GCOMP_STEPDOWN_NO_ROOM
                                        : GCOMP_STEPDOWN_ENCODE_FAILED);
    return zstd_literals_encode_raw(
        literals, literals_size, output, output_cap, output_len_out);
  }

  // Total compressed size = weights + stream(s)
  size_t compressed_size = weights_size + stream_size;

  // Check if compression is beneficial: the weights, the bitstream and the
  // largest literals header have to come to less than the literals did.
  //
  // There is no percentage threshold here, whatever the comment on this line
  // used to say -- one byte saved is one byte kept.  Requiring a margin would
  // mean choosing, for some block, to write the larger of two encodings the
  // decoder reads identically.
  if (compressed_size + 5 >= literals_size) {
    // No savings (or negative savings), use raw encoding
    gcomp_stepdown_note(stepdowns, GCOMP_STEPDOWN_LITERALS_NOT_WORTH_CODING);
    return zstd_literals_encode_raw(
        literals, literals_size, output, output_cap, output_len_out);
  }

  // Write literals header for compressed type
  // Type = 2 (compressed). size_format 0 = single stream, 1/2/3 = 4 streams.
  // Decoder uses four_streams = true only for format 1, 2, 3.
  size_t header_size;
  uint8_t header_buf[5];
  bool use_4streams = (literals_size >= 1024);

  // RFC 8878 section 3.1.1.3.1.1: the literals header is one little-endian
  // bit field -- type in bits 0-1, size_format in 2-3, Regenerated_Size and
  // then Compressed_Size in the bits above, each as wide as the format says.
  // Writing it as hand-packed bytes had dropped bits 12-13 of
  // Regenerated_Size in the 14-bit form and written bits 10-11 twice.
  {
    unsigned fmt;
    unsigned size_bits;

    if (literals_size <= 1023 && compressed_size <= 1023) {
      fmt = use_4streams ? 1u : 0u;
      size_bits = 10;
      header_size = 3;
    }
    else if (literals_size <= 16383 && compressed_size <= 16383) {
      fmt = 2; // 14-bit sizes; size_format 2 and 3 are always four streams
      size_bits = 14;
      header_size = 4;
    }
    else if (literals_size <= 262143 && compressed_size <= 262143) {
      fmt = 3;
      size_bits = 18;
      header_size = 5;
    }
    else {
      return GCOMP_ERR_LIMIT;
    }

    uint64_t val = (uint64_t)LITERALS_TYPE_COMPRESSED | ((uint64_t)fmt << 2) |
        ((uint64_t)literals_size << 4) |
        ((uint64_t)compressed_size << (4 + size_bits));

    for (size_t i = 0; i < header_size; i++) {
      header_buf[i] = (uint8_t)(val >> (i * 8));
    }
  }

  // Move data to make room for header (we wrote at offset 5)
  // Move weights+stream from offset 5 to offset header_size
  if (header_size < header_offset) {
    memmove(output + header_size, output + header_offset, compressed_size);
  }

  // Write header at beginning
  memcpy(output, header_buf, header_size);

  *output_len_out = header_size + compressed_size;
  return GCOMP_OK;
}

gcomp_status_t zstd_literals_encode_raw(const uint8_t * literals,
    size_t literals_size, uint8_t * output, size_t output_cap,
    size_t * output_len_out) {
  if (!output || !output_len_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Handle empty literals
  if (literals_size == 0 || !literals) {
    // Write minimal header: type=raw (0), size_format=0, size=0
    if (output_cap < 1) {
      return GCOMP_ERR_LIMIT;
    }
    output[0] = 0x00; // type=0, size_format=0, size=0
    *output_len_out = 1;
    return GCOMP_OK;
  }

  // Determine header size based on literals size
  size_t header_size;
  size_t total_size;

  if (literals_size <= 31) {
    // 1-byte header: 5-bit size (size_format=0)
    header_size = 1;
    total_size = 1 + literals_size;
  }
  else if (literals_size <= 4095) {
    // 2-byte header: 12-bit size (size_format=1)
    header_size = 2;
    total_size = 2 + literals_size;
  }
  else if (literals_size <= 1048575) {
    // 3-byte header: 20-bit size (size_format=3)
    header_size = 3;
    total_size = 3 + literals_size;
  }
  else {
    // Too large for raw literals
    return GCOMP_ERR_LIMIT;
  }

  if (output_cap < total_size) {
    return GCOMP_ERR_LIMIT;
  }

  // Write header
  // Type = 0 (raw), format encodes in bits 2-3
  if (header_size == 1) {
    // 1-byte: type(2) | format(2) | size(5) = size << 3
    output[0] = (uint8_t)((literals_size << 3) | (0 << 2) | LITERALS_TYPE_RAW);
  }
  else if (header_size == 2) {
    // 2-byte: byte0 = type(2) | format(2) | size_lo(4)
    //         byte1 = size_hi(8)
    output[0] =
        (uint8_t)(((literals_size & 0x0F) << 4) | (1 << 2) | LITERALS_TYPE_RAW);
    output[1] = (uint8_t)(literals_size >> 4);
  }
  else {
    // 3-byte: byte0 = type(2) | format(2) | size_lo(4)
    //         byte1 = size_mid(8)
    //         byte2 = size_hi(8)
    output[0] =
        (uint8_t)(((literals_size & 0x0F) << 4) | (3 << 2) | LITERALS_TYPE_RAW);
    output[1] = (uint8_t)(literals_size >> 4);
    output[2] = (uint8_t)(literals_size >> 12);
  }

  // Copy literals
  memcpy(output + header_size, literals, literals_size);

  *output_len_out = total_size;
  return GCOMP_OK;
}
