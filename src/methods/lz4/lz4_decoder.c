/**
 * @file lz4_decoder.c
 *
 * LZ4 frame format decoder implementation.
 *
 * This file implements the streaming decoder for LZ4 frame format
 * decompression. The decoder accepts input conforming to the LZ4 Frame
 * Format specification:
 * https://github.com/lz4/lz4/blob/dev/doc/lz4_Frame_format.md
 *
 * ## Decoder State Machine
 *
 * The decoder is implemented as a state machine that processes input
 * incrementally, allowing it to work with arbitrarily small input/output
 * buffers:
 *
 * ```
 *   ┌──────────┐
 *   │  HEADER  │◄─── init(), parse magic/FLG/BD/optional fields/HC
 *   └────┬─────┘
 *        │ header complete
 *        v
 *   ┌────────────┐
 *   │ BLOCK_SIZE │◄──────────────────────────────────────┐
 *   └─────┬──────┘                                       │
 *         │ size > 0                                     │
 *         v                                              │
 *   ┌────────────┐                                       │
 *   │ BLOCK_DATA │──── decompress block                  │
 *   └─────┬──────┘                                       │
 *         │                                              │
 *         v                                              │
 *   ┌────────────────┐ (if block checksum enabled)       │
 *   │ BLOCK_CHECKSUM │───────────────────────────────────┘
 *   └────────────────┘
 *         │ size == 0 (end mark)
 *         v
 *   ┌──────────────────┐ (if content checksum enabled)
 *   │ CONTENT_CHECKSUM │
 *   └────────┬─────────┘
 *            │
 *            v
 *   ┌──────────┐
 *   │   DONE   │──── if concat enabled, may return to HEADER
 *   └──────────┘
 * ```
 *
 * ## Header Parsing Sub-State Machine
 *
 * Header parsing has its own state machine within the HEADER stage:
 *
 * 1. **MAGIC**: Read 4-byte magic number (0x184D2204)
 * 2. **FLG_BD**: Read FLG and BD bytes
 * 3. **CONTENT_SIZE**: Read 8-byte content size (if FLG.C_SIZE set)
 * 4. **DICT_ID**: Read 4-byte dictionary ID (if FLG.DICT_ID set)
 * 5. **HC**: Read 1-byte header checksum
 *
 * ## Block Decompression
 *
 * For each block:
 * 1. Read 4-byte block size (little-endian)
 *    - High bit (0x80000000) indicates uncompressed block
 *    - Size 0 indicates end of frame
 * 2. Read block data into buffer
 * 3. Decompress using `lz4_block_decompress()` (or copy if uncompressed)
 * 4. Update content checksum if enabled
 * 5. Update history buffer for dependent blocks
 *
 * ## History Buffer (Dependent Blocks)
 *
 * When `lz4.independent_blocks=false` in the frame header, blocks can
 * reference data from previous blocks. The decoder maintains a sliding
 * window history buffer:
 *
 * - Capacity: 64KB (LZ4_HISTORY_SIZE) - maximum back-reference distance
 * - After each block, the last 64KB of output is retained as history
 * - The history slides forward as new data is produced
 *
 * For independent blocks, no history is needed and the buffer may be NULL.
 *
 * ## Concatenated Frames
 *
 * When `lz4.concat=true`, after reaching DONE state:
 * - If more input is available, the decoder returns to HEADER state
 * - Each frame is validated independently (checksums, content size)
 * - Output is continuous across frames
 * - Limits apply to cumulative output across all frames
 *
 * ## Safety Limits
 *
 * The decoder enforces multiple safety limits:
 *
 * | Limit | Check Point | Error |
 * |-------|-------------|-------|
 * | max_output_bytes | After each block decompression | GCOMP_ERR_LIMIT |
 * | max_block_bytes | When reading block size | GCOMP_ERR_LIMIT |
 * | max_expansion_ratio | After each block decompression | GCOMP_ERR_LIMIT |
 * | max_memory_bytes | During init allocation | GCOMP_ERR_MEMORY |
 *
 * ## Error Handling
 *
 * When an error occurs, the decoder enters the ERROR stage and subsequent
 * update() calls return GCOMP_ERR_INTERNAL. Use reset() to recover and
 * start a new stream. Error details are available via
 * `gcomp_decoder_get_error_detail()`.
 *
 * ## Thread Safety
 *
 * A single decoder instance is NOT thread-safe. Each thread should have
 * its own decoder instance.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "lz4_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/stream.h>
#include <stdlib.h>
#include <string.h>

//
// Helper: Read options and configure decoder
//

static gcomp_status_t lz4_decoder_read_options(
    gcomp_options_t * options, lz4_decoder_state_t * state) {
  uint64_t u64_val;
  int bool_val;

  // Set defaults
  state->concat_enabled = LZ4_DEFAULT_CONCAT;
  state->max_output_bytes = LZ4_DEFAULT_MAX_OUTPUT_BYTES;
  state->max_expansion_ratio = LZ4_DEFAULT_MAX_EXPANSION_RATIO;
  state->max_block_bytes = LZ4_DEFAULT_BLOCK_SIZE;
  state->max_memory_bytes = LZ4_DEFAULT_MAX_MEMORY_BYTES;

  if (!options) {
    return GCOMP_OK;
  }

  // Read lz4.concat
  if (gcomp_options_get_bool(options, "lz4.concat", &bool_val) == GCOMP_OK) {
    state->concat_enabled = bool_val != 0;
  }

  // Read limits.max_output_bytes
  if (gcomp_options_get_uint64(options, "limits.max_output_bytes", &u64_val) ==
      GCOMP_OK) {
    state->max_output_bytes = u64_val;
  }

  // Read limits.max_expansion_ratio
  if (gcomp_options_get_uint64(
          options, "limits.max_expansion_ratio", &u64_val) == GCOMP_OK) {
    state->max_expansion_ratio = u64_val;
  }

  // Read limits.max_block_bytes
  if (gcomp_options_get_uint64(options, "limits.max_block_bytes", &u64_val) ==
      GCOMP_OK) {
    state->max_block_bytes = u64_val;
  }

  // Read limits.max_memory_bytes
  if (gcomp_options_get_uint64(options, "limits.max_memory_bytes", &u64_val) ==
      GCOMP_OK) {
    state->max_memory_bytes = u64_val;
  }

  return GCOMP_OK;
}

//
// Helper: Parse frame header
//

static gcomp_status_t lz4_decoder_parse_header(lz4_decoder_state_t * state) {
  const uint8_t * buf = state->header_accum;
  size_t pos = 0;

  // Magic number (already validated during accumulation)
  pos += 4;

  // FLG byte
  uint8_t flg = buf[pos++];
  state->header.flg = flg;

  // Validate version bits (must be 01)
  if ((flg & LZ4_FLG_VERSION_MASK) != LZ4_FLG_VERSION_VALUE) {
    return GCOMP_ERR_CORRUPT;
  }

  // Check reserved bit
  if (flg & LZ4_FLG_RESERVED) {
    return GCOMP_ERR_CORRUPT;
  }

  // Extract flags
  state->header.block_independence = (flg & LZ4_FLG_B_INDEP) != 0;
  state->header.block_checksum = (flg & LZ4_FLG_B_CHECKSUM) != 0;
  state->header.content_size_present = (flg & LZ4_FLG_C_SIZE) != 0;
  state->header.content_checksum = (flg & LZ4_FLG_C_CHECKSUM) != 0;
  state->header.dict_id_present = (flg & LZ4_FLG_DICT_ID) != 0;

  // BD byte
  uint8_t bd = buf[pos++];
  state->header.bd = bd;

  // Check reserved bits
  if (bd & LZ4_BD_RESERVED) {
    return GCOMP_ERR_CORRUPT;
  }

  // Extract block max size
  uint8_t block_code = (bd & LZ4_BD_BLOCK_MAX_MASK) >> LZ4_BD_BLOCK_MAX_SHIFT;
  state->header.block_max_size = lz4_block_code_to_size(block_code);
  if (state->header.block_max_size == 0) {
    return GCOMP_ERR_CORRUPT;
  }

  // Content size (if present)
  if (state->header.content_size_present) {
    state->header.content_size = lz4_read_le64(buf + pos);
    pos += 8;
  }
  else {
    state->header.content_size = 0;
  }

  // Dictionary ID (if present)
  if (state->header.dict_id_present) {
    state->header.dict_id = lz4_read_le32(buf + pos);
    pos += 4;
  }
  else {
    state->header.dict_id = 0;
  }

  // Header checksum (1 byte)
  uint8_t expected_hc = buf[pos];

  // Compute header checksum: xxHash32 of FLG..{optional fields} >> 8 & 0xFF
  uint32_t hash = gcomp_xxhash32(buf + 4, pos - 4, 0);
  uint8_t computed_hc = (uint8_t)((hash >> 8) & 0xFF);

  if (expected_hc != computed_hc) {
    return GCOMP_ERR_CORRUPT;
  }

  return GCOMP_OK;
}

//
// Internal API Implementation
//

gcomp_status_t lz4_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder) {
  (void)registry;

  if (!decoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Allocate state
  lz4_decoder_state_t * state =
      (lz4_decoder_state_t *)calloc(1, sizeof(lz4_decoder_state_t));
  if (!state) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_MEMORY, "failed to allocate lz4 decoder state");
  }

  // Track memory usage (tracker is zero-initialized by calloc)
  gcomp_memory_track_alloc(&state->mem_tracker, sizeof(lz4_decoder_state_t));

  // Read options
  gcomp_status_t status = lz4_decoder_read_options(options, state);
  if (status != GCOMP_OK) {
    free(state);
    return status;
  }

  // Initial buffers will be allocated when we know the block size from header
  state->block_buffer = NULL;
  state->block_buffer_size = 0;
  state->output_buffer = NULL;
  state->output_buffer_size = 0;
  state->history_buffer = NULL;
  state->history_size = 0;
  state->history_capacity = 0;

  // Set initial stage
  state->stage = LZ4_DEC_STAGE_HEADER;
  state->header_stage = LZ4_HEADER_MAGIC;
  state->header_accum_pos = 0;
  state->total_input_bytes = 0;
  state->total_output_bytes = 0;

  // Initialize header hash for checksum computation
  gcomp_xxhash32_reset(&state->header_hash, 0);

  decoder->method_state = state;
  return GCOMP_OK;
}

void lz4_decoder_destroy(gcomp_decoder_t * decoder) {
  if (!decoder || !decoder->method_state) {
    return;
  }

  lz4_decoder_state_t * state = (lz4_decoder_state_t *)decoder->method_state;

  if (state->history_buffer) {
    free(state->history_buffer);
  }
  if (state->output_buffer) {
    free(state->output_buffer);
  }
  if (state->block_buffer) {
    free(state->block_buffer);
  }

  free(state);
  decoder->method_state = NULL;
}

gcomp_status_t lz4_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!decoder || !decoder->method_state || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  lz4_decoder_state_t * state = (lz4_decoder_state_t *)decoder->method_state;

  if (state->stage == LZ4_DEC_STAGE_ERROR) {
    return GCOMP_ERR_INTERNAL;
  }

  // First, drain any pending output
  if (state->output_buffer_pos < state->output_buffer_len) {
    while (state->output_buffer_pos < state->output_buffer_len &&
        output->used < output->size) {
      ((uint8_t *)output->data)[output->used++] =
          state->output_buffer[state->output_buffer_pos++];
    }
    if (state->output_buffer_pos < state->output_buffer_len) {
      return GCOMP_OK; // Need more output space
    }
  }

  while (input->used < input->size || state->stage == LZ4_DEC_STAGE_DONE) {
    switch (state->stage) {
    case LZ4_DEC_STAGE_HEADER: {
      // Accumulate header bytes
      switch (state->header_stage) {
      case LZ4_HEADER_MAGIC:
        while (state->header_accum_pos < 4 && input->used < input->size) {
          state->header_accum[state->header_accum_pos++] =
              ((const uint8_t *)input->data)[input->used++];
        }
        if (state->header_accum_pos >= 4) {
          // Validate magic
          uint32_t magic = lz4_read_le32(state->header_accum);
          if (magic != LZ4_MAGIC) {
            state->stage = LZ4_DEC_STAGE_ERROR;
            return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
                "invalid lz4 magic: 0x%08X (expected 0x%08X)", magic,
                LZ4_MAGIC);
          }
          state->header_stage = LZ4_HEADER_FLG_BD;
        }
        break;

      case LZ4_HEADER_FLG_BD:
        while (state->header_accum_pos < 6 && input->used < input->size) {
          state->header_accum[state->header_accum_pos++] =
              ((const uint8_t *)input->data)[input->used++];
        }
        if (state->header_accum_pos >= 6) {
          uint8_t flg = state->header_accum[4];
          state->header.content_size_present = (flg & LZ4_FLG_C_SIZE) != 0;
          state->header.dict_id_present = (flg & LZ4_FLG_DICT_ID) != 0;

          if (state->header.content_size_present) {
            state->header_stage = LZ4_HEADER_CONTENT_SIZE;
          }
          else if (state->header.dict_id_present) {
            state->header_stage = LZ4_HEADER_DICT_ID;
          }
          else {
            state->header_stage = LZ4_HEADER_HC;
          }
        }
        break;

      case LZ4_HEADER_CONTENT_SIZE: {
        size_t target = 6 + 8;
        while (state->header_accum_pos < target && input->used < input->size) {
          state->header_accum[state->header_accum_pos++] =
              ((const uint8_t *)input->data)[input->used++];
        }
        if (state->header_accum_pos >= target) {
          if (state->header.dict_id_present) {
            state->header_stage = LZ4_HEADER_DICT_ID;
          }
          else {
            state->header_stage = LZ4_HEADER_HC;
          }
        }
        break;
      }

      case LZ4_HEADER_DICT_ID: {
        size_t target = 6 + (state->header.content_size_present ? 8 : 0) + 4;
        while (state->header_accum_pos < target && input->used < input->size) {
          state->header_accum[state->header_accum_pos++] =
              ((const uint8_t *)input->data)[input->used++];
        }
        if (state->header_accum_pos >= target) {
          state->header_stage = LZ4_HEADER_HC;
        }
        break;
      }

      case LZ4_HEADER_HC: {
        size_t target = 6 + (state->header.content_size_present ? 8 : 0) +
            (state->header.dict_id_present ? 4 : 0) + 1;
        while (state->header_accum_pos < target && input->used < input->size) {
          state->header_accum[state->header_accum_pos++] =
              ((const uint8_t *)input->data)[input->used++];
        }
        if (state->header_accum_pos >= target) {
          // Parse and validate header
          gcomp_status_t status = lz4_decoder_parse_header(state);
          if (status != GCOMP_OK) {
            state->stage = LZ4_DEC_STAGE_ERROR;
            // Determine specific error based on what failed
            uint8_t flg = state->header_accum[4];
            if ((flg & LZ4_FLG_VERSION_MASK) != LZ4_FLG_VERSION_VALUE) {
              return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
                  "invalid lz4 FLG version bits: 0x%02X (expected 0x40)",
                  flg & LZ4_FLG_VERSION_MASK);
            }
            if (flg & LZ4_FLG_RESERVED) {
              return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
                  "invalid lz4 FLG byte: reserved bit set (0x%02X)", flg);
            }
            uint8_t bd = state->header_accum[5];
            if (bd & LZ4_BD_RESERVED) {
              return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
                  "invalid lz4 BD byte: reserved bits set (0x%02X)", bd);
            }
            return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
                "lz4 header checksum mismatch or invalid block size");
          }
          state->header_stage = LZ4_HEADER_DONE;

          // Allocate buffers now that we know block size
          uint32_t block_size = state->header.block_max_size;
          if (block_size > state->max_block_bytes) {
            state->stage = LZ4_DEC_STAGE_ERROR;
            return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
                "lz4 block max size %u exceeds limit %llu", block_size,
                (unsigned long long)state->max_block_bytes);
          }

          // Reuse existing buffers if large enough, otherwise reallocate
          if (!state->block_buffer || state->block_buffer_size < block_size) {
            if (state->block_buffer) {
              free(state->block_buffer);
              gcomp_memory_track_free(
                  &state->mem_tracker, state->block_buffer_size);
            }
            state->block_buffer = (uint8_t *)malloc(block_size);
            state->block_buffer_size = block_size;
            if (state->block_buffer) {
              gcomp_memory_track_alloc(&state->mem_tracker, block_size);
            }
          }
          if (!state->output_buffer || state->output_buffer_size < block_size) {
            if (state->output_buffer) {
              free(state->output_buffer);
              gcomp_memory_track_free(
                  &state->mem_tracker, state->output_buffer_size);
            }
            state->output_buffer = (uint8_t *)malloc(block_size);
            state->output_buffer_size = block_size;
            if (state->output_buffer) {
              gcomp_memory_track_alloc(&state->mem_tracker, block_size);
            }
          }

          if (!state->block_buffer || !state->output_buffer) {
            state->stage = LZ4_DEC_STAGE_ERROR;
            return gcomp_decoder_set_error(decoder, GCOMP_ERR_MEMORY,
                "failed to allocate lz4 block buffers (%u bytes)", block_size);
          }

          // Allocate history buffer for dependent blocks
          if (!state->header.block_independence) {
            if (!state->history_buffer ||
                state->history_capacity < LZ4_HISTORY_SIZE) {
              if (state->history_buffer) {
                free(state->history_buffer);
                gcomp_memory_track_free(
                    &state->mem_tracker, state->history_capacity);
              }
              state->history_capacity = LZ4_HISTORY_SIZE;
              state->history_buffer =
                  (uint8_t *)malloc(state->history_capacity);
              if (!state->history_buffer) {
                state->stage = LZ4_DEC_STAGE_ERROR;
                return gcomp_decoder_set_error(decoder, GCOMP_ERR_MEMORY,
                    "failed to allocate lz4 history buffer (%zu bytes)",
                    state->history_capacity);
              }
              gcomp_memory_track_alloc(
                  &state->mem_tracker, state->history_capacity);
            }
            state->history_size = 0;
          }

          // Check memory limit
          if (state->max_memory_bytes > 0 &&
              state->mem_tracker.current_bytes > state->max_memory_bytes) {
            state->stage = LZ4_DEC_STAGE_ERROR;
            return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
                "lz4 decoder memory usage %llu exceeds limit %llu",
                (unsigned long long)state->mem_tracker.current_bytes,
                (unsigned long long)state->max_memory_bytes);
          }

          // Initialize content checksum if enabled
          if (state->header.content_checksum) {
            gcomp_xxhash32_reset(&state->content_hash, 0);
          }

          state->stage = LZ4_DEC_STAGE_BLOCK_SIZE;
          state->block_size_buf_pos = 0;
        }
        break;
      }

      case LZ4_HEADER_DONE:
        // Should not reach here
        break;
      }
      break;
    }

    case LZ4_DEC_STAGE_BLOCK_SIZE: {
      // Accumulate 4-byte block size
      while (state->block_size_buf_pos < 4 && input->used < input->size) {
        state->block_size_buf[state->block_size_buf_pos++] =
            ((const uint8_t *)input->data)[input->used++];
      }
      if (state->block_size_buf_pos >= 4) {
        gcomp_status_t status = lz4_parse_block_size(state->block_size_buf,
            &state->current_block_size, &state->current_block_uncompressed);
        if (status != GCOMP_OK) {
          state->stage = LZ4_DEC_STAGE_ERROR;
          return gcomp_decoder_set_error(
              decoder, status, "failed to parse lz4 block size field");
        }

        if (state->current_block_size == 0) {
          // End mark - validate content size if present in header
          if (state->header.content_size_present &&
              state->total_output_bytes != state->header.content_size) {
            state->stage = LZ4_DEC_STAGE_ERROR;
            return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
                "lz4 content size mismatch: header specified %llu bytes, "
                "got %llu bytes",
                (unsigned long long)state->header.content_size,
                (unsigned long long)state->total_output_bytes);
          }
          // Go to content checksum or done
          if (state->header.content_checksum) {
            state->stage = LZ4_DEC_STAGE_CONTENT_CHECKSUM;
            state->content_checksum_buf_pos = 0;
          }
          else {
            state->stage = LZ4_DEC_STAGE_DONE;
          }
        }
        else {
          // Check block size limit
          if (state->current_block_size > state->max_block_bytes) {
            state->stage = LZ4_DEC_STAGE_ERROR;
            return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
                "lz4 block size %u exceeds limit %llu",
                state->current_block_size,
                (unsigned long long)state->max_block_bytes);
          }
          state->block_bytes_remaining = state->current_block_size;
          state->block_buffer_pos = 0;
          state->stage = LZ4_DEC_STAGE_BLOCK_DATA;

          // Initialize block hash for checksum
          if (state->header.block_checksum) {
            gcomp_xxhash32_reset(&state->block_hash, 0);
          }
        }
        state->block_size_buf_pos = 0;
      }
      break;
    }

    case LZ4_DEC_STAGE_BLOCK_DATA: {
      // Accumulate block data
      while (state->block_bytes_remaining > 0 && input->used < input->size) {
        size_t to_read = state->block_bytes_remaining;
        size_t available = input->size - input->used;
        if (to_read > available) {
          to_read = available;
        }

        memcpy(state->block_buffer + state->block_buffer_pos,
            (const uint8_t *)input->data + input->used, to_read);
        input->used += to_read;
        state->block_buffer_pos += to_read;
        state->block_bytes_remaining -= to_read;
        state->total_input_bytes += to_read;
      }

      if (state->block_bytes_remaining == 0) {
        // Block complete, decompress it
        size_t decompressed_len;
        if (state->current_block_uncompressed) {
          // Uncompressed block - just copy
          memcpy(state->output_buffer, state->block_buffer,
              state->block_buffer_pos);
          decompressed_len = state->block_buffer_pos;
        }
        else {
          // Decompress block
          gcomp_status_t status = lz4_block_decompress(state->block_buffer,
              state->block_buffer_pos, state->output_buffer,
              state->output_buffer_size, &decompressed_len,
              state->history_buffer, state->history_size);
          if (status != GCOMP_OK) {
            state->stage = LZ4_DEC_STAGE_ERROR;
            if (status == GCOMP_ERR_CORRUPT) {
              return gcomp_decoder_set_error(decoder, status,
                  "corrupt lz4 block data (invalid match offset or bounds)");
            }
            if (status == GCOMP_ERR_LIMIT) {
              return gcomp_decoder_set_error(decoder, status,
                  "lz4 block decompressed size exceeds buffer capacity");
            }
            return gcomp_decoder_set_error(
                decoder, status, "lz4 block decompression failed");
          }
        }

        // Update output tracking
        state->output_buffer_pos = 0;
        state->output_buffer_len = decompressed_len;
        state->total_output_bytes += decompressed_len;

        // Check output limit
        if (state->total_output_bytes > state->max_output_bytes) {
          state->stage = LZ4_DEC_STAGE_ERROR;
          return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
              "lz4 output size %llu exceeds limit %llu",
              (unsigned long long)state->total_output_bytes,
              (unsigned long long)state->max_output_bytes);
        }

        // Check expansion ratio
        if (state->total_input_bytes > 0 && state->max_expansion_ratio > 0) {
          uint64_t ratio = state->total_output_bytes / state->total_input_bytes;
          if (ratio > state->max_expansion_ratio) {
            state->stage = LZ4_DEC_STAGE_ERROR;
            return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
                "lz4 expansion ratio %llu exceeds limit %llu "
                "(input=%llu, output=%llu)",
                (unsigned long long)ratio,
                (unsigned long long)state->max_expansion_ratio,
                (unsigned long long)state->total_input_bytes,
                (unsigned long long)state->total_output_bytes);
          }
        }

        // Update content checksum
        if (state->header.content_checksum) {
          gcomp_xxhash32_update(
              &state->content_hash, state->output_buffer, decompressed_len);
        }

        // Update history for dependent blocks
        if (!state->header.block_independence && state->history_buffer) {
          size_t to_keep = decompressed_len;
          if (to_keep > state->history_capacity) {
            to_keep = state->history_capacity;
          }
          // Shift history and add new data
          if (state->history_size + to_keep > state->history_capacity) {
            size_t shift =
                state->history_size + to_keep - state->history_capacity;
            memmove(state->history_buffer, state->history_buffer + shift,
                state->history_size - shift);
            state->history_size -= shift;
          }
          memcpy(state->history_buffer + state->history_size,
              state->output_buffer + decompressed_len - to_keep, to_keep);
          state->history_size += to_keep;
        }

        if (state->header.block_checksum) {
          state->stage = LZ4_DEC_STAGE_BLOCK_CHECKSUM;
          state->block_checksum_buf_pos = 0;
        }
        else {
          state->stage = LZ4_DEC_STAGE_BLOCK_SIZE;
          state->block_size_buf_pos = 0;
        }
      }

      // Output decompressed data
      while (state->output_buffer_pos < state->output_buffer_len &&
          output->used < output->size) {
        ((uint8_t *)output->data)[output->used++] =
            state->output_buffer[state->output_buffer_pos++];
      }
      if (state->output_buffer_pos < state->output_buffer_len) {
        return GCOMP_OK; // Need more output space
      }
      break;
    }

    case LZ4_DEC_STAGE_BLOCK_CHECKSUM: {
      // Accumulate 4-byte block checksum
      while (state->block_checksum_buf_pos < 4 && input->used < input->size) {
        state->block_checksum_buf[state->block_checksum_buf_pos++] =
            ((const uint8_t *)input->data)[input->used++];
      }
      if (state->block_checksum_buf_pos >= 4) {
        uint32_t expected = lz4_read_le32(state->block_checksum_buf);
        uint32_t computed =
            gcomp_xxhash32(state->block_buffer, state->current_block_size, 0);
        if (expected != computed) {
          state->stage = LZ4_DEC_STAGE_ERROR;
          return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
              "lz4 block checksum mismatch: expected 0x%08X, computed 0x%08X",
              expected, computed);
        }
        state->stage = LZ4_DEC_STAGE_BLOCK_SIZE;
        state->block_size_buf_pos = 0;
        state->block_checksum_buf_pos = 0;
      }
      break;
    }

    case LZ4_DEC_STAGE_CONTENT_CHECKSUM: {
      // Accumulate 4-byte content checksum
      while (state->content_checksum_buf_pos < 4 && input->used < input->size) {
        state->content_checksum_buf[state->content_checksum_buf_pos++] =
            ((const uint8_t *)input->data)[input->used++];
      }
      if (state->content_checksum_buf_pos >= 4) {
        uint32_t expected = lz4_read_le32(state->content_checksum_buf);
        uint32_t computed = gcomp_xxhash32_finalize(&state->content_hash);
        if (expected != computed) {
          state->stage = LZ4_DEC_STAGE_ERROR;
          return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
              "lz4 content checksum mismatch: expected 0x%08X, computed 0x%08X",
              expected, computed);
        }
        state->stage = LZ4_DEC_STAGE_DONE;
      }
      break;
    }

    case LZ4_DEC_STAGE_DONE:
      // Check for concatenated frames
      if (state->concat_enabled && input->used < input->size) {
        // Reset for next frame
        state->stage = LZ4_DEC_STAGE_HEADER;
        state->header_stage = LZ4_HEADER_MAGIC;
        state->header_accum_pos = 0;
        if (state->header.content_checksum) {
          gcomp_xxhash32_reset(&state->content_hash, 0);
        }
        if (!state->header.block_independence && state->history_buffer) {
          state->history_size = 0;
        }
      }
      else {
        return GCOMP_OK;
      }
      break;

    case LZ4_DEC_STAGE_ERROR:
      return GCOMP_ERR_INTERNAL;
    }
  }

  return GCOMP_OK;
}

gcomp_status_t lz4_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  if (!decoder || !decoder->method_state || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  lz4_decoder_state_t * state = (lz4_decoder_state_t *)decoder->method_state;

  // Drain any pending output
  while (state->output_buffer_pos < state->output_buffer_len &&
      output->used < output->size) {
    ((uint8_t *)output->data)[output->used++] =
        state->output_buffer[state->output_buffer_pos++];
  }
  if (state->output_buffer_pos < state->output_buffer_len) {
    return GCOMP_OK;
  }

  if (state->stage == LZ4_DEC_STAGE_DONE) {
    return GCOMP_OK;
  }

  if (state->stage == LZ4_DEC_STAGE_ERROR) {
    return GCOMP_ERR_INTERNAL;
  }

  // Incomplete stream - provide specific error message based on stage
  switch (state->stage) {
  case LZ4_DEC_STAGE_HEADER:
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
        "lz4 stream truncated in header (sub-stage %d, pos %zu)",
        state->header_stage, state->header_accum_pos);

  case LZ4_DEC_STAGE_BLOCK_SIZE:
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
        "lz4 stream truncated in block size (%zu of 4 bytes)",
        state->block_size_buf_pos);

  case LZ4_DEC_STAGE_BLOCK_DATA:
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
        "lz4 stream truncated in block data (%zu bytes remaining)",
        state->block_bytes_remaining);

  case LZ4_DEC_STAGE_BLOCK_CHECKSUM:
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
        "lz4 stream truncated in block checksum (%zu of 4 bytes)",
        state->block_checksum_buf_pos);

  case LZ4_DEC_STAGE_CONTENT_CHECKSUM:
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
        "lz4 stream truncated in content checksum (%zu of 4 bytes)",
        state->content_checksum_buf_pos);

  default:
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
        "lz4 stream incomplete (unexpected stage %d)", state->stage);
  }
}

gcomp_status_t lz4_decoder_reset(gcomp_decoder_t * decoder) {
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }

  lz4_decoder_state_t * state = (lz4_decoder_state_t *)decoder->method_state;

  // Free dynamically allocated buffers (they'll be reallocated for next stream)
  if (state->block_buffer) {
    free(state->block_buffer);
    state->block_buffer = NULL;
    state->block_buffer_size = 0;
  }
  if (state->output_buffer) {
    free(state->output_buffer);
    state->output_buffer = NULL;
    state->output_buffer_size = 0;
  }
  if (state->history_buffer) {
    free(state->history_buffer);
    state->history_buffer = NULL;
    state->history_size = 0;
    state->history_capacity = 0;
  }

  // Reset memory tracker (buffers freed, only state struct remains)
  state->mem_tracker.current_bytes = sizeof(lz4_decoder_state_t);

  // Reset parsing state
  state->stage = LZ4_DEC_STAGE_HEADER;
  state->header_stage = LZ4_HEADER_MAGIC;
  state->header_accum_pos = 0;
  state->block_size_buf_pos = 0;
  state->block_buffer_pos = 0;
  state->block_bytes_remaining = 0;
  state->block_checksum_buf_pos = 0;
  state->content_checksum_buf_pos = 0;
  state->output_buffer_pos = 0;
  state->output_buffer_len = 0;
  state->total_input_bytes = 0;
  state->total_output_bytes = 0;

  // Clear header info
  memset(&state->header, 0, sizeof(state->header));

  return GCOMP_OK;
}
