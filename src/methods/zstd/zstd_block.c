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

#include <ghoti.io/compress/macros.h>
#include "../../core/stepdown.h"
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

/**
 * @brief Move the match finder's window forward, dropping the oldest bytes.
 *
 * History older than the declared window size cannot be pointed at by a
 * sequence, so it is dropped rather than kept.  The match finder's tables
 * index positions in this window, so they move with it.
 *
 * @param state Encoder state.
 * @param shift Bytes to drop from the front.
 */
static void zstd_block_slide_window(
    zstd_encoder_state_t * state, size_t shift) {
  if (!state->mf_window || shift == 0) {
    return;
  }
  if (shift >= state->mf_window_len) {
    state->mf_window_len = 0;
    zstd_mf_reset(state->match_finder);
    return;
  }
  memmove(state->mf_window, state->mf_window + shift,
      state->mf_window_len - shift);
  state->mf_window_len -= shift;
  zstd_mf_slide(state->match_finder, shift);
}

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

  // Why this block ends up raw, if it does.  It starts as "did not try",
  // becomes "something went wrong" the moment the compressed path is entered,
  // and only a branch that has actually priced the alternatives may set it
  // back to a chosen reason.  See src/core/stepdown.h.
  gcomp_stepdown_t stepdown = GCOMP_STEPDOWN_TOO_SMALL_TO_TRY;

  // Try compressed block if we have a match finder and sufficient input
  if (state && state->match_finder && state->seq_buffer &&
      state->literals_buffer && input_len >= MIN_COMPRESSION_SIZE) {
    stepdown = GCOMP_STEPDOWN_ENCODE_FAILED;
    size_t num_sequences = 0;
    size_t literals_size = 0;
    const uint8_t * mf_data = input;
    size_t mf_data_size = input_len;
    size_t start_pos = 0;
    bool index_prefix = false;

    if (state->mf_window && input_len <= state->mf_window_capacity) {
      // Stream path: the block is appended to the history already in the
      // window, and the match finder's tables already describe that history.
      if (state->mf_window_len + input_len > state->mf_window_capacity) {
        size_t shift =
            state->mf_window_len + input_len - state->mf_window_capacity;
        zstd_block_slide_window(state, shift);
      }
      memcpy(state->mf_window + state->mf_window_len, input, input_len);
      mf_data = state->mf_window;
      start_pos = state->mf_window_len;
      mf_data_size = state->mf_window_len + input_len;
    }
    else if (state->dict_parsed.content && state->dict_parsed.content_size > 0 &&
        state->dict_block_buffer) {
      // Job path: a parallel job compresses its block on its own, so the
      // dictionary is prepended to it and indexed for that block alone.
      size_t copy_len = state->dict_parsed.content_size;
      if (copy_len + input_len <= state->dict_block_buffer_capacity) {
        memcpy(state->dict_block_buffer, state->dict_parsed.content, copy_len);
        memcpy(state->dict_block_buffer + copy_len, input, input_len);
        mf_data = state->dict_block_buffer;
        mf_data_size = copy_len + input_len;
        start_pos = copy_len;
        index_prefix = true;
      }
    }

    if (index_prefix) {
      zstd_mf_index_range(
          state->match_finder, mf_data, 0, start_pos, mf_data_size);
    }

    gcomp_status_t status =
        zstd_mf_generate_sequences(state->match_finder, mf_data, mf_data_size,
            start_pos, state->seq_buffer, state->seq_buffer_capacity,
            &num_sequences, state->literals_buffer, &literals_size,
            &state->rep_offset_1, &state->rep_offset_2, &state->rep_offset_3);

    if (state->mf_window && mf_data == state->mf_window) {
      // Keep the block as history for the next one, dropping whatever no
      // longer fits inside the declared window.
      state->mf_window_len = mf_data_size;
      if (state->mf_window_len > state->mf_window_max) {
        zstd_block_slide_window(
            state, state->mf_window_len - state->mf_window_max);
      }
    }

    // A block needs either sequences or literals, and neither one on its own
    // disqualifies it.  RFC 8878 section 3.1.1.3 lets a Compressed_Block
    // carry Huffman-coded literals with a Sequences_Section of zero
    // sequences, and it equally lets one carry sequences with an empty
    // Literals_Section.
    //
    // Both halves of that have been got wrong here.  Requiring
    // num_sequences > 0 sent every match-less block to a raw block -- a
    // skewed-alphabet file that the reference encoder takes to 36% came out
    // at 100.003% of input.  Requiring literals_size > 0 then did the same to
    // every block that matched *everything*, which became reachable the
    // moment sequences could reach back into earlier blocks: a megabyte of
    // words drawn at random from a thirteen-word vocabulary went from 189,076
    // bytes to 833,633, six of its eight blocks stored raw, because after the
    // first block there was nothing left to emit as a literal.
    if (status == GCOMP_OK && (num_sequences > 0 || literals_size > 0)) {
      // Encode literals section (with Huffman compression when beneficial)
      size_t literals_encoded_size = 0;
      status = zstd_literals_encode_compressed(state->allocator,
          &state->stepdowns, state->literals_buffer, literals_size, output,
          output_cap, &literals_encoded_size);

      if (status == GCOMP_OK) {
        // Encode sequences section
        size_t sequences_encoded_size = 0;
        status = zstd_sequences_encode(state->seq_buffer,
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
          // Priced, and the raw block won.
          stepdown = GCOMP_STEPDOWN_STORED_IS_SMALLER;
        }
      }
    }

    // Anything else that lands here is a failure, and `stepdown` still says
    // so.  That default is the point: the branch that goes wrong is the one
    // nobody thought to record, and requiring a branch to *claim* success
    // catches the ones nobody wrote down.  The first version of this counter
    // noted each branch instead, and the defect it was written to catch -
    // requiring literals_size > 0, which sent every block that matched
    // everything to a raw block - walked straight past it.
    if (status == GCOMP_ERR_MEMORY) {
      stepdown = GCOMP_STEPDOWN_NO_MEMORY;
    }
    else if (status == GCOMP_ERR_LIMIT) {
      stepdown = GCOMP_STEPDOWN_NO_ROOM;
    }

    // If compression failed or didn't help, fall through to raw block
  }

  gcomp_stepdown_note(state ? &state->stepdowns : NULL, stepdown);

  // Use raw block (no compression or compression not beneficial)
  if (input_len > output_cap) {
    return GCOMP_ERR_LIMIT;
  }

  memcpy(output, input, input_len);
  *output_len_out = input_len;
  *type_out = ZSTD_BLOCK_TYPE_RAW;
  return GCOMP_OK;
}
