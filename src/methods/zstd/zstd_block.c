/**
 * @file zstd_block.c
 *
 * Zstandard block compression/decompression for the Ghoti.io Compress library.
 *
 * This file implements block-level operations for the Zstd format:
 * - Raw block: direct copy
 * - RLE block: single byte repeated
 * - Compressed block: FSE + Huffman encoded (TODO: full implementation)
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

// TODO: Implement full FSE + Huffman decompression
// For now, this is a stub that returns an error

gcomp_status_t zstd_block_decompress_compressed(zstd_decoder_state_t * state,
    const uint8_t * input, size_t input_len, uint8_t * output,
    size_t output_cap, size_t * output_len_out) {
  (void)state;
  (void)input;
  (void)input_len;
  (void)output;
  (void)output_cap;
  (void)output_len_out;

  // TODO: Implement full compressed block decompression
  // This requires:
  // 1. Literals section parsing (raw, RLE, compressed, treeless)
  // 2. Sequences section parsing
  // 3. FSE table decoding
  // 4. Huffman table decoding
  // 5. Sequence execution (literal copies + match copies)

  (void)input_len; // suppress unused parameter warning
  return GCOMP_ERR_UNSUPPORTED;
}

//
// Block Compression
//

// For now, we only support raw blocks
// Full compression requires FSE + Huffman encoding

gcomp_status_t zstd_block_compress(zstd_encoder_state_t * state,
    const uint8_t * input, size_t input_len, uint8_t * output,
    size_t output_cap, size_t * output_len_out, uint8_t * type_out) {
  (void)state;

  if (!input || !output || !output_len_out || !type_out) {
    return GCOMP_ERR_INVALID_ARG;
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

  if (all_same && input_len > 0) {
    // Use RLE block
    if (output_cap < 1) {
      return GCOMP_ERR_LIMIT;
    }
    output[0] = input[0];
    *output_len_out = 1;
    *type_out = ZSTD_BLOCK_TYPE_RLE;
    return GCOMP_OK;
  }

  // Use raw block (no compression)
  if (input_len > output_cap) {
    return GCOMP_ERR_LIMIT;
  }

  memcpy(output, input, input_len);
  *output_len_out = input_len;
  *type_out = ZSTD_BLOCK_TYPE_RAW;
  return GCOMP_OK;
}
