/**
 * @file zstd_block.c
 *
 * Zstandard block compression/decompression for the Ghoti.io Compress library.
 *
 * ## Block Types
 *
 * Zstd frames consist of one or more blocks. Each block has a 3-byte header:
 * - Bit 0: Last_Block flag (1 = this is the final block)
 * - Bits 1-2: Block_Type (0=Raw, 1=RLE, 2=Compressed, 3=Reserved)
 * - Bits 3-23: Block_Size (21 bits, max ~2MB)
 *
 * | Type | Encoding | Block_Size Meaning |
 * |------|----------|-------------------|
 * | Raw  | Uncompressed | Bytes to copy |
 * | RLE  | Single byte  | Times to repeat |
 * | Compressed | FSE+Huffman | Compressed size |
 * | Reserved | Invalid | N/A |
 *
 * ## Block Selection Strategy (Encoder)
 *
 * The encoder chooses block type based on data characteristics:
 *
 * 1. **RLE Block**: If all bytes in the block are identical
 *    - Most efficient: 1 byte of data regardless of block size
 *    - Condition: block[0] == block[1] == ... == block[n-1]
 *
 * 2. **Compressed Block**: If compression is beneficial
 *    - Contains: literals section + sequences section
 *    - Used when compressed_size < raw_size
 *
 * 3. **Raw Block**: Fallback when compression doesn't help
 *    - Direct copy of input bytes
 *    - Used when compressed_size >= raw_size
 *
 * ## Compressed Block Structure
 *
 * ```
 * [Literals Section]
 *   - Header (1-5 bytes): type, sizes
 *   - Data: raw, RLE, or Huffman-compressed literals
 *
 * [Sequences Section]
 *   - Header: num_sequences, compression modes
 *   - FSE tables (if not predefined)
 *   - Bitstream: FSE-encoded (literal_len, offset, match_len) tuples
 * ```
 *
 * Reference: RFC 8878 Section 3.1 (Blocks)
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include "zstd_internal.h"
#include <string.h>

//
// Raw Block
//

gcomp_status_t zstd_block_decompress_raw(const uint8_t * input,
    size_t input_len, uint8_t * output, size_t output_cap,
    size_t * output_len_out) {
  if (!input || !output || !output_len_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (input_len > output_cap) {
    return GCOMP_ERR_LIMIT;
  }

  memcpy(output, input, input_len);
  *output_len_out = input_len;
  return GCOMP_OK;
}

//
// RLE Block
//

gcomp_status_t zstd_block_decompress_rle(const uint8_t * input,
    size_t input_len, uint8_t * output, size_t output_cap,
    size_t * output_len_out, uint32_t regenerated_size) {
  if (!input || !output || !output_len_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (input_len < 1) {
    return GCOMP_ERR_CORRUPT;
  }

  if (regenerated_size > output_cap) {
    return GCOMP_ERR_LIMIT;
  }

  uint8_t byte = input[0];
  memset(output, byte, regenerated_size);
  *output_len_out = regenerated_size;
  return GCOMP_OK;
}

//
// Compressed Block
//

gcomp_status_t zstd_block_decompress_compressed(zstd_decoder_state_t * state,
    const uint8_t * input, size_t input_len, uint8_t * output,
    size_t output_cap, size_t * output_len_out) {
  if (!state || !input || !output || !output_len_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (input_len == 0) {
    *output_len_out = 0;
    return GCOMP_OK;
  }

  // Allocate temporary buffer for literals
  // Maximum literals size is the block size (128KB)
  uint8_t * literals_buf = gcomp_malloc(state->allocator, ZSTD_BLOCK_SIZE_MAX);
  if (!literals_buf) {
    return GCOMP_ERR_MEMORY;
  }

  gcomp_status_t status;
  size_t literals_size;
  size_t literals_consumed;

  // 1. Decode literals section
  status = zstd_literals_decode(state, input, input_len, literals_buf,
      ZSTD_BLOCK_SIZE_MAX, &literals_size, &literals_consumed);
  if (status != GCOMP_OK) {
    gcomp_free(state->allocator, literals_buf);
    return status;
  }

  // 2. Decode sequences section and execute
  const uint8_t * sequences_data = input + literals_consumed;
  size_t sequences_data_size = input_len - literals_consumed;
  size_t output_size;
  size_t sequences_consumed;

  status = zstd_sequences_decode(state, sequences_data, sequences_data_size,
      literals_buf, literals_size, output, output_cap, &output_size,
      &sequences_consumed);

  gcomp_free(state->allocator, literals_buf);

  if (status != GCOMP_OK) {
    return status;
  }

  *output_len_out = output_size;
  return GCOMP_OK;
}

//
// Block Compression
//

// Maximum sequences per block (block_size / min_match)
#define MAX_SEQUENCES_PER_BLOCK (ZSTD_BLOCK_SIZE_MAX / 3)

// Minimum input size to attempt compression (small blocks rarely compress well)
#define MIN_COMPRESSION_SIZE 64

gcomp_status_t zstd_block_compress(zstd_encoder_state_t * state,
    const uint8_t * input, size_t input_len, uint8_t * output,
    size_t output_cap, size_t * output_len_out, uint8_t * type_out) {
  if (!input || !output || !output_len_out || !type_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Handle empty input
  if (input_len == 0) {
    *output_len_out = 0;
    *type_out = ZSTD_BLOCK_TYPE_RAW;
    return GCOMP_OK;
  }

  // Check if RLE is beneficial (all same bytes)
  bool all_same = true;
  if (input_len > 1) {
    uint8_t first_byte = input[0];
    for (size_t i = 1; i < input_len && all_same; i++) {
      if (input[i] != first_byte) {
        all_same = false;
      }
    }
  }

  if (all_same) {
    // Use RLE block
    if (output_cap < 1) {
      return GCOMP_ERR_LIMIT;
    }
    output[0] = input[0];
    *output_len_out = 1;
    *type_out = ZSTD_BLOCK_TYPE_RLE;
    return GCOMP_OK;
  }

  // Try compressed block if we have a match finder and sufficient input
  if (state && state->match_finder && state->seq_buffer &&
      state->literals_buffer && input_len >= MIN_COMPRESSION_SIZE) {
    size_t num_sequences = 0;
    size_t literals_size = 0;
    const uint8_t * mf_data = input;
    size_t mf_data_size = input_len;
    size_t dict_prefix = 0;

    if (state->dict_parsed.content && state->dict_parsed.content_size > 0 &&
        state->dict_block_buffer) {
      size_t copy_len = state->dict_parsed.content_size;
      if (copy_len + input_len <= state->dict_block_buffer_capacity) {
        memcpy(state->dict_block_buffer, state->dict_parsed.content, copy_len);
        memcpy(state->dict_block_buffer + copy_len, input, input_len);
        mf_data = state->dict_block_buffer;
        mf_data_size = copy_len + input_len;
        dict_prefix = copy_len;
      }
    }

    gcomp_status_t status =
        zstd_mf_generate_sequences(state->match_finder, mf_data, mf_data_size,
            dict_prefix, state->seq_buffer, state->seq_buffer_capacity,
            &num_sequences, state->literals_buffer, &literals_size,
            &state->rep_offset_1, &state->rep_offset_2, &state->rep_offset_3);

    if (status == GCOMP_OK && num_sequences > 0) {
      // We have sequences - try to compress

      // Encode literals section (with Huffman compression when beneficial)
      size_t literals_encoded_size = 0;
      status = zstd_literals_encode_compressed(state->literals_buffer,
          literals_size, output, output_cap, &literals_encoded_size);

      if (status == GCOMP_OK) {
        // Encode sequences section
        size_t sequences_encoded_size = 0;
        status = zstd_sequences_encode_predefined(state->seq_buffer,
            num_sequences, output + literals_encoded_size,
            output_cap - literals_encoded_size, &sequences_encoded_size);

        if (status == GCOMP_OK) {
          size_t compressed_size =
              literals_encoded_size + sequences_encoded_size;

          // Only use compressed block if it's actually smaller
          if (compressed_size < input_len) {
            *output_len_out = compressed_size;
            *type_out = ZSTD_BLOCK_TYPE_COMPRESSED;
            return GCOMP_OK;
          }
        }
      }
    }
    // If compression failed or didn't help, fall through to raw block
  }

  // Use raw block (no compression or compression not beneficial)
  if (input_len > output_cap) {
    return GCOMP_ERR_LIMIT;
  }

  memcpy(output, input, input_len);
  *output_len_out = input_len;
  *type_out = ZSTD_BLOCK_TYPE_RAW;
  return GCOMP_OK;
}
