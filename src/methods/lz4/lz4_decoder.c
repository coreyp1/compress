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
 * A skippable frame (magic 0x184D2A50-0x184D2A5F, then a 4-byte little-endian
 * size, then that many bytes of the writer's own data) is consumed and
 * discarded wherever one appears, and the parser returns to HEADER for
 * whatever follows.  This is not gated on `lz4.concat`: a skippable frame in
 * front of the data is a preamble, not a concatenation, and a stream that is
 * nothing but skippable frames decodes to nothing rather than erroring.
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
 */

#include "lz4_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/stream.h>
#include <stdlib.h>
#include <string.h>

//
// Helper: Read options and configure decoder
//

/**
 * @brief Start the match history from the dictionary, or from nothing.
 *
 * LZ4 Frame Format: with linked blocks the dictionary is what precedes the
 * first block and the frame's own output takes over from there, so this runs
 * once per frame.  With independent blocks each block starts from the
 * dictionary again, so it runs before every block.  That difference was
 * settled by reading liblz4's own dictionary-compressed output rather than
 * inferred from the specification's wording.
 *
 * With no dictionary this empties the history, which is what both modes did
 * before dictionaries existed.
 */
static void lz4_decoder_seed_history(lz4_decoder_state_t * state) {
  state->history_size = 0;
  if (state->dictionary_size == 0 || !state->history_buffer) {
    return;
  }
  size_t take = state->dictionary_size;
  if (take > state->history_capacity) {
    take = state->history_capacity;
  }
  memcpy(state->history_buffer,
      state->dictionary + state->dictionary_size - take, take);
  state->history_size = take;
}

static gcomp_status_t lz4_decoder_read_options(gcomp_options_t * options,
    lz4_decoder_state_t * state, const gcomp_allocator_t * alloc) {
  uint64_t u64_val;
  int bool_val;

  // Set defaults
  state->dictionary = NULL;
  state->dictionary_size = 0;
  state->concat_enabled = LZ4_DEFAULT_CONCAT;
  state->max_output_bytes = LZ4_DEFAULT_MAX_OUTPUT_BYTES;
  state->max_expansion_ratio = LZ4_DEFAULT_MAX_EXPANSION_RATIO;
  state->max_block_bytes = LZ4_DEFAULT_BLOCK_SIZE;
  state->max_memory_bytes = LZ4_DEFAULT_MAX_MEMORY_BYTES;

  if (!options) {
    return GCOMP_OK;
  }

  // Read lz4.dictionary.  Only the tail is kept: the match offset is two
  // bytes, so a dictionary longer than LZ4_HISTORY_SIZE has an unreachable
  // head, and this is what liblz4 does with an over-long one too.
  {
    const void * dict_data = NULL;
    size_t dict_size = 0;
    if (gcomp_options_get_bytes(options, "lz4.dictionary", &dict_data,
            &dict_size) == GCOMP_OK &&
        dict_data && dict_size > 0) {
      const uint8_t * tail = (const uint8_t *)dict_data;
      if (dict_size > LZ4_HISTORY_SIZE) {
        tail += dict_size - LZ4_HISTORY_SIZE;
        dict_size = LZ4_HISTORY_SIZE;
      }
      state->dictionary = (uint8_t *)gcomp_malloc(alloc, dict_size);
      if (!state->dictionary) {
        return GCOMP_ERR_MEMORY;
      }
      memcpy(state->dictionary, tail, dict_size);
      state->dictionary_size = dict_size;
      gcomp_memory_track_alloc(&state->mem_tracker, dict_size);
    }
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
  // Deliberately the same parser gcomp_lz4_peek_frame_info() uses.  A reader
  // that peeks at a header and then decodes the frame must not be told two
  // different things about the same bytes, and the only way to be sure of
  // that is for there to be one parser.
  size_t header_size = 0;
  return lz4_parse_frame_header(state->header_accum,
      state->header_accum_pos, &state->header, &header_size);
}

//
// Internal API Implementation
//

gcomp_status_t lz4_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder) {

  if (!decoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Get allocator from registry
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);

  // Allocate state
  lz4_decoder_state_t * state =
      (lz4_decoder_state_t *)gcomp_calloc(alloc, 1, sizeof(lz4_decoder_state_t));
  if (!state) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_MEMORY, "failed to allocate lz4 decoder state");
  }
  state->allocator = alloc;

  // Track memory usage (tracker is zero-initialized by calloc)
  gcomp_memory_track_alloc(&state->mem_tracker, sizeof(lz4_decoder_state_t));

  // Read options
  gcomp_status_t status = lz4_decoder_read_options(options, state, alloc);
  if (status != GCOMP_OK) {
    gcomp_free(alloc, state);
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
  // Not state->dictionary: lz4_decoder_read_options() has already run and
  // copied it, and zeroing it here would leak the copy and lose the option.

  // Set initial stage
  state->stage = LZ4_DEC_STAGE_HEADER;
  state->header_stage = LZ4_HEADER_MAGIC;
  state->header_accum_pos = 0;
  state->skippable_remaining = 0;
  state->skippable_variant = 0;
  state->skippable_size = 0;
  state->skippable_delivered = 0;
  state->skippable_cb = NULL;
  state->skippable_ctx = NULL;
  state->frames_completed = 0;
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
  const gcomp_allocator_t * alloc = state->allocator;

  if (state->dictionary) {
    gcomp_free(alloc, state->dictionary);
    gcomp_memory_track_free(&state->mem_tracker, state->dictionary_size);
    state->dictionary = NULL;
    state->dictionary_size = 0;
  }
  if (state->history_buffer) {
    gcomp_free(alloc, state->history_buffer);
  }
  if (state->output_buffer) {
    gcomp_free(alloc, state->output_buffer);
  }
  if (state->block_buffer) {
    gcomp_free(alloc, state->block_buffer);
  }

  gcomp_free(alloc, state);
  decoder->method_state = NULL;
}

/**
 * @brief Retire a fully consumed skippable frame.
 *
 * A skippable frame produces no output and carries no history, so whatever
 * follows -- another skippable frame, a data frame, or the end of the stream
 * -- starts from a clean header parse.
 */
static void lz4_decoder_finish_skippable(lz4_decoder_state_t * state) {
  state->frames_completed++;
  state->stage = LZ4_DEC_STAGE_HEADER;
  state->header_stage = LZ4_HEADER_MAGIC;
  state->header_accum_pos = 0;
}

/**
 * @brief Hand already-decompressed bytes to the caller.
 *
 * A block is decompressed into state->output_buffer in one piece and then
 * copied out as the caller's buffer allows.  That copy used to move one byte
 * per iteration, in three separate places, and was 42.9% of an LZ4 decode --
 * more than the decompressor itself.  Copying a byte at a time is roughly
 * eight instructions of loop and bounds checking per byte; memcpy moves
 * sixteen or thirty-two at once.
 *
 * @return Non-zero once everything buffered has been delivered.
 */
static int lz4_drain_output(
    lz4_decoder_state_t * state, gcomp_buffer_t * output) {
  size_t pending = state->output_buffer_len - state->output_buffer_pos;
  if (pending > 0u) {
    size_t space = output->size - output->used;
    size_t n = (pending < space) ? pending : space;
    if (n > 0u) {
      memcpy((uint8_t *)output->data + output->used,
          state->output_buffer + state->output_buffer_pos, n);
      output->used += n;
      state->output_buffer_pos += n;
    }
  }
  return state->output_buffer_pos >= state->output_buffer_len;
}

gcomp_status_t lz4_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!decoder || !decoder->method_state || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }
  // Check data pointers if size > 0
  if ((input->size > 0 && !input->data) ||
      (output->size > 0 && !output->data)) {
    return GCOMP_ERR_INVALID_ARG;
  }

  lz4_decoder_state_t * state = (lz4_decoder_state_t *)decoder->method_state;

  if (state->stage == LZ4_DEC_STAGE_ERROR) {
    return GCOMP_ERR_INTERNAL;
  }

  // First, drain any pending output
  if (state->output_buffer_pos < state->output_buffer_len) {
    if (!lz4_drain_output(state, output)) {
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
          uint32_t magic = gcomp_read_le32(state->header_accum);
          if (LZ4_IS_SKIPPABLE_MAGIC(magic)) {
            // LZ4 Frame Format, "Skippable Frames".  Not gated on
            // lz4.concat: a skippable frame is a frame *type* the format
            // defines, and one sitting in front of the data -- an
            // application's own preamble -- has no completed frame before it
            // for concatenation to be a question about.  lz4.concat keeps its
            // own meaning, which is whether to carry on past a finished data
            // frame.
            state->skippable_variant = (unsigned)(magic & 0x0Fu);
            state->header_accum_pos = 0;
            state->stage = LZ4_DEC_STAGE_SKIPPABLE_SIZE;
            break;
          }
          if (magic != LZ4_MAGIC) {
            state->stage = LZ4_DEC_STAGE_ERROR;
            return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
                "invalid lz4 magic: 0x%08X (expected 0x%08X, or 0x%08X-0x%08X "
                "for a skippable frame)",
                magic, LZ4_MAGIC, LZ4_SKIPPABLE_MAGIC,
                LZ4_SKIPPABLE_MAGIC | 0x0FU);
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
          const gcomp_allocator_t * alloc = state->allocator;
          if (!state->block_buffer || state->block_buffer_size < block_size) {
            if (state->block_buffer) {
              gcomp_free(alloc, state->block_buffer);
              gcomp_memory_track_free(
                  &state->mem_tracker, state->block_buffer_size);
            }
            state->block_buffer = (uint8_t *)gcomp_malloc(alloc, block_size);
            state->block_buffer_size = block_size;
            if (state->block_buffer) {
              gcomp_memory_track_alloc(&state->mem_tracker, block_size);
            }
          }
          if (!state->output_buffer || state->output_buffer_size < block_size) {
            if (state->output_buffer) {
              gcomp_free(alloc, state->output_buffer);
              gcomp_memory_track_free(
                  &state->mem_tracker, state->output_buffer_size);
            }
            state->output_buffer = (uint8_t *)gcomp_malloc(alloc, block_size);
            state->output_buffer_size = block_size;
            if (state->output_buffer) {
              gcomp_memory_track_alloc(&state->mem_tracker, block_size);
            }
          }

          if (!state->block_buffer || !state->output_buffer) {
            // Clean up any successful allocations before returning error
            if (state->block_buffer) {
              gcomp_free(alloc, state->block_buffer);
              gcomp_memory_track_free(
                  &state->mem_tracker, state->block_buffer_size);
              state->block_buffer = NULL;
              state->block_buffer_size = 0;
            }
            if (state->output_buffer) {
              gcomp_free(alloc, state->output_buffer);
              gcomp_memory_track_free(
                  &state->mem_tracker, state->output_buffer_size);
              state->output_buffer = NULL;
              state->output_buffer_size = 0;
            }
            state->stage = LZ4_DEC_STAGE_ERROR;
            return gcomp_decoder_set_error(decoder, GCOMP_ERR_MEMORY,
                "failed to allocate lz4 block buffers (%u bytes)", block_size);
          }

          // Allocate the history buffer for dependent blocks, and for any
          // frame decoded against a dictionary -- an independent block reads
          // the dictionary out of the same buffer.
          if (!state->header.block_independence ||
              state->dictionary_size > 0) {
            if (!state->history_buffer ||
                state->history_capacity < LZ4_HISTORY_SIZE) {
              if (state->history_buffer) {
                gcomp_free(alloc, state->history_buffer);
                gcomp_memory_track_free(
                    &state->mem_tracker, state->history_capacity);
              }
              state->history_capacity = LZ4_HISTORY_SIZE;
              state->history_buffer =
                  (uint8_t *)gcomp_malloc(alloc, state->history_capacity);
              if (!state->history_buffer) {
                // Clean up block and output buffers before returning error
                if (state->block_buffer) {
                  gcomp_free(alloc, state->block_buffer);
                  gcomp_memory_track_free(
                      &state->mem_tracker, state->block_buffer_size);
                  state->block_buffer = NULL;
                  state->block_buffer_size = 0;
                }
                if (state->output_buffer) {
                  gcomp_free(alloc, state->output_buffer);
                  gcomp_memory_track_free(
                      &state->mem_tracker, state->output_buffer_size);
                  state->output_buffer = NULL;
                  state->output_buffer_size = 0;
                }
                state->history_capacity = 0;
                state->stage = LZ4_DEC_STAGE_ERROR;
                return gcomp_decoder_set_error(decoder, GCOMP_ERR_MEMORY,
                    "failed to allocate lz4 history buffer (%zu bytes)",
                    LZ4_HISTORY_SIZE);
              }
              gcomp_memory_track_alloc(
                  &state->mem_tracker, state->history_capacity);
            }
            lz4_decoder_seed_history(state);
          }

          // Check memory limit through the core helper: see the note in
          // lz4_encoder.c about where "0 means unlimited" lives.
          if (gcomp_memory_check_limit(
                  &state->mem_tracker, state->max_memory_bytes) != GCOMP_OK) {
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
            state->frames_completed++;
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
          // An independent block may reference the dictionary but not the
          // blocks before it, so the history goes back to the dictionary
          // alone -- and to nothing at all when there is no dictionary, which
          // is what this did before.
          if (state->header.block_independence) {
            lz4_decoder_seed_history(state);
          }

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

        // Check output limit.  This compared directly against the limit
        // and so treated 0 as "no output permitted" rather than "unlimited",
        // which is what limits.h documents.
        if (gcomp_limits_check_output((size_t)state->total_output_bytes,
                state->max_output_bytes) != GCOMP_OK) {
          state->stage = LZ4_DEC_STAGE_ERROR;
          return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
              "lz4 output size %llu exceeds limit %llu",
              (unsigned long long)state->total_output_bytes,
              (unsigned long long)state->max_output_bytes);
        }

        // Check expansion ratio using proper multiplication-based check
        if (state->max_expansion_ratio > 0) {
          gcomp_status_t ratio_status = gcomp_limits_check_expansion_ratio(
              state->total_input_bytes, state->total_output_bytes,
              state->max_expansion_ratio);
          if (ratio_status != GCOMP_OK) {
            state->stage = LZ4_DEC_STAGE_ERROR;
            return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
                "lz4 expansion ratio exceeds limit %llu (input=%llu, output=%llu)",
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
      if (!lz4_drain_output(state, output)) {
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
        uint32_t expected = gcomp_read_le32(state->block_checksum_buf);
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
        uint32_t expected = gcomp_read_le32(state->content_checksum_buf);
        uint32_t computed = gcomp_xxhash32_finalize(&state->content_hash);
        if (expected != computed) {
          state->stage = LZ4_DEC_STAGE_ERROR;
          return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
              "lz4 content checksum mismatch: expected 0x%08X, computed 0x%08X",
              expected, computed);
        }
        state->stage = LZ4_DEC_STAGE_DONE;
        state->frames_completed++;
      }
      break;
    }

    case LZ4_DEC_STAGE_SKIPPABLE_SIZE: {
      while (state->header_accum_pos < 4 && input->used < input->size) {
        state->header_accum[state->header_accum_pos++] =
            ((const uint8_t *)input->data)[input->used++];
      }
      if (state->header_accum_pos < 4) {
        break; // Need more input
      }
      state->skippable_remaining = gcomp_read_le32(state->header_accum);
      state->header_accum_pos = 0;
      state->skippable_size = state->skippable_remaining;
      state->skippable_delivered = 0;
      if (state->skippable_remaining == 0) {
        // A zero-length payload finishes the frame here.  It cannot be left
        // to the SKIPPABLE_DATA case: reading the size may have consumed the
        // last of the input, and the loop would then exit before that case
        // ever ran, leaving a complete frame looking truncated.
        //
        // The callback still runs once.  A frame with no payload is not a
        // frame with nothing to say -- its variant may be the whole message.
        if (state->skippable_cb) {
          gcomp_status_t cb_status = state->skippable_cb(state->skippable_ctx,
              state->skippable_variant, 0, 0, NULL, 0);
          if (cb_status != GCOMP_OK) {
            state->stage = LZ4_DEC_STAGE_ERROR;
            return gcomp_decoder_set_error(decoder, cb_status,
                "skippable frame callback stopped the decode (variant %u, "
                "empty payload)",
                state->skippable_variant);
          }
        }
        lz4_decoder_finish_skippable(state);
        break;
      }
      state->stage = LZ4_DEC_STAGE_SKIPPABLE_DATA;
      break;
    }

    case LZ4_DEC_STAGE_SKIPPABLE_DATA: {
      // The payload is the writer's business, not ours; consume and discard.
      size_t available = input->size - input->used;
      size_t take = (available < state->skippable_remaining)
          ? available
          : (size_t)state->skippable_remaining;

      // Hand the bytes over before discarding them.  The chunk points into
      // the caller's own input buffer, so nothing is copied and nothing is
      // held.  On refusal the input is left unconsumed, so the decoder does
      // not half-swallow a frame it is about to stop on.
      if (state->skippable_cb && take > 0) {
        gcomp_status_t cb_status = state->skippable_cb(state->skippable_ctx,
            state->skippable_variant, state->skippable_size,
            state->skippable_delivered,
            (const uint8_t *)input->data + input->used, take);
        if (cb_status != GCOMP_OK) {
          state->stage = LZ4_DEC_STAGE_ERROR;
          return gcomp_decoder_set_error(decoder, cb_status,
              "skippable frame callback stopped the decode (variant %u, "
              "%llu of %llu payload bytes delivered)",
              state->skippable_variant,
              (unsigned long long)state->skippable_delivered,
              (unsigned long long)state->skippable_size);
        }
      }

      input->used += take;
      state->skippable_delivered += take;
      state->skippable_remaining -= (uint32_t)take;
      if (state->skippable_remaining > 0) {
        break; // Need more input
      }
      lz4_decoder_finish_skippable(state);
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
        // A following frame is a new frame: its first block sees the
        // dictionary, not the frame before it.
        lz4_decoder_seed_history(state);
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
  if (!lz4_drain_output(state, output)) {
    return GCOMP_OK;
  }

  if (state->stage == LZ4_DEC_STAGE_DONE) {
    return GCOMP_OK;
  }

  // A stream may legally end on a skippable frame -- including one that is the
  // whole stream, which decodes to nothing.  That leaves the parser waiting on
  // the next magic number with nothing accumulated, which is indistinguishable
  // from a stream cut off before a frame began except by whether any frame has
  // finished.
  if (state->stage == LZ4_DEC_STAGE_HEADER &&
      state->header_stage == LZ4_HEADER_MAGIC &&
      state->header_accum_pos == 0 && state->frames_completed > 0) {
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

  case LZ4_DEC_STAGE_SKIPPABLE_SIZE:
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
        "lz4 stream truncated in skippable frame size (%zu of 4 bytes)",
        state->header_accum_pos);

  case LZ4_DEC_STAGE_SKIPPABLE_DATA:
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
        "lz4 stream truncated in skippable frame payload (%u bytes "
        "remaining)",
        state->skippable_remaining);

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

  // Keep buffers allocated for reuse (consistent with deflate/gzip behavior).
  // Buffers will be resized on next header parse if the new stream has a
  // different block size requirement.

  // Reset buffer usage state (not the allocations)
  state->block_buffer_pos = 0;
  state->output_buffer_pos = 0;
  state->output_buffer_len = 0;
  lz4_decoder_seed_history(state);

  // Memory tracker reflects retained allocations
  state->mem_tracker.current_bytes = sizeof(lz4_decoder_state_t);
  if (state->block_buffer) {
    state->mem_tracker.current_bytes += state->block_buffer_size;
  }
  if (state->output_buffer) {
    state->mem_tracker.current_bytes += state->output_buffer_size;
  }
  if (state->history_buffer) {
    state->mem_tracker.current_bytes += state->history_capacity;
  }

  // Reset parsing state
  state->stage = LZ4_DEC_STAGE_HEADER;
  state->header_stage = LZ4_HEADER_MAGIC;
  state->header_accum_pos = 0;
  // Per-frame skippable tracking resets; the registered callback does not.
  // It describes how the caller is using this decoder, not the stream it was
  // reading, and a caller that resets to decode another stream still wants to
  // be told about its skippable frames.
  state->skippable_remaining = 0;
  state->skippable_variant = 0;
  state->skippable_size = 0;
  state->skippable_delivered = 0;
  state->frames_completed = 0;
  state->block_size_buf_pos = 0;
  state->block_bytes_remaining = 0;
  state->block_checksum_buf_pos = 0;
  state->content_checksum_buf_pos = 0;
  state->total_input_bytes = 0;
  state->total_output_bytes = 0;

  // Clear header info
  memset(&state->header, 0, sizeof(state->header));

  return GCOMP_OK;
}

//
// Skippable frame reporting
//

gcomp_status_t gcomp_lz4_decoder_on_skippable_frame(gcomp_decoder_t * decoder,
    gcomp_lz4_skippable_cb callback, void * ctx) {
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // The state pointer is method-specific, so registering on another method's
  // decoder would write through a pointer to something else entirely.
  if (!decoder->method || !decoder->method->name ||
      strcmp(decoder->method->name, "lz4") != 0) {
    return GCOMP_ERR_INVALID_ARG;
  }

  lz4_decoder_state_t * state = (lz4_decoder_state_t *)decoder->method_state;
  state->skippable_cb = callback;
  state->skippable_ctx = ctx;
  return GCOMP_OK;
}
