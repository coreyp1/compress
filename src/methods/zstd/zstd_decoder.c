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
 * @file zstd_decoder.c
 *
 * Zstandard decoder implementation for the Ghoti.io Compress library.
 *
 * This file implements the streaming Zstd decoder, which parses
 * Zstandard frame format input. The decoder supports both single-frame
 * and concatenated multi-frame streams.
 *
 * ## State Machine
 *
 * The decoder progresses through these stages:
 * 1. HEADER: Parse frame header (magic + header descriptor)
 * 2. BLOCK_HEADER: Read 3-byte block header
 * 3. BLOCK_DATA: Decompress block content
 * 4. CONTENT_CHECKSUM: Read content checksum (if enabled)
 * 5. DONE: Frame complete
 *
 * ## Concatenated Frames (zstd.concat, default true)
 *
 * RFC 8878 section 3.1: "Zstandard compressed data is made up of one or more
 * frames... The decompressed content of multiple concatenated frames is the
 * concatenation of each frame's decompressed content."  Decoding every frame
 * in the input is therefore the conformant behaviour and the default.
 *
 * This defaulted to false, so a decode of two frames returned the first
 * frame's content and GCOMP_OK, with no way for the caller to tell that the
 * rest of the input had been dropped.  The option is kept, now as an explicit
 * opt-out for a caller that wants exactly one frame and intends to deal with
 * whatever follows it itself.
 *
 * When `zstd.concat` is enabled, the decoder processes multiple
 * independent zstd frames concatenated together:
 *
 * ```
 * Input:  [Frame 1][Frame 2][Frame 3]...
 * Output: [Decompressed 1][Decompressed 2][Decompressed 3]...
 * ```
 *
 * Frame transition logic (in update()):
 * 1. When stage == DONE and concat enabled and more input available:
 *    - Reset per-frame state for new frame
 *    - Keep total byte counters (for limit checking)
 *    - Drain any buffered output from previous frame
 * 2. Parse next frame starting from HEADER stage
 * 3. Repeat until all input consumed
 *
 * ### Per-Frame State Reset
 *
 * When transitioning between concatenated frames, the following must be reset:
 * - `header_accum_pos`: For accumulating new frame header
 * - `rep_offset_{1,2,3}`: Each frame starts with standard repeat offsets
 * - `frame_output_bytes`: For content size validation per frame
 * - `window_pos`, `window_size`: Each frame has independent history window
 * - `huf_table_valid`, `fse_{ll,ml,of}_ready`: entropy tables don't carry
 *   across frames, so Repeat_Mode in a new frame's first block is corrupt
 *
 * ### Not Reset Between Frames
 *
 * - `total_input_bytes`, `total_output_bytes`: Accumulate for limit checking
 * - `output_buffer`, `block_buffer`: Retained for reuse
 * - `fse_*_table`: Overwritten by each block anyway
 *
 * ## Use Case: Parallel Encoder Output
 *
 * The parallel encoder produces concatenated frames (one per job). This decoder
 * mode enables decompression of that output without external tools.
 *
 * ## Dictionary Support
 *
 * When the user provides a dictionary via `zstd.dictionary`:
 *
 * - **Window preload**: After parsing the frame header (and allocating the
 *   window buffer), the decoder copies dictionary content into the window so
 *   that the first block can reference it. This is done both when the frame
 *   header has Dictionary_ID_Flag set and when it does not (raw-dict frames
 *   from external encoders that omit the flag).
 *
 * - **dict_id validation**: If the frame header specifies a dictionary ID and
 *   the user provided a formatted dictionary, the decoder validates that
 *   dict_id matches. Raw dictionaries (dict_id 0) are accepted for any frame
 *   dict_id so external encoders that write content-derived IDs still decode.
 *
 * - **Entropy tables**: If the dictionary is formatted and includes entropy
 *   tables (Huffman, FSE), the decoder uses them for the first block (repeat
 *   mode / treeless mode). Otherwise blocks use their own inline tables.
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/compress/macros.h>
#include "zstd_internal.h"
#include <string.h>

//
// Initialization
//

gcomp_status_t zstd_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder) {
  (void)registry;

  if (!decoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Get allocator from registry
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);
  gcomp_status_t status = GCOMP_OK;

  // Allocate state structure
  zstd_decoder_state_t * state = gcomp_calloc(alloc, 1, sizeof(*state));
  if (!state) {
    gcomp_decoder_set_error(
        decoder, GCOMP_ERR_MEMORY, "failed to allocate decoder state");
    return GCOMP_ERR_MEMORY;
  }
  state->allocator = alloc;

  // Initialize memory tracker
  gcomp_memory_track_alloc(&state->mem_tracker, sizeof(*state));

  // Read options.  These initialisers, not the option schema, are what a
  // caller passing no options gets, so the conformant default has to be here
  // as well as in the schema.
  int concat = ZSTD_DEFAULT_CONCAT;
  uint64_t max_output = ZSTD_DEFAULT_MAX_OUTPUT_BYTES;
  uint64_t max_window = ZSTD_DEFAULT_MAX_WINDOW_BYTES;
  uint64_t max_memory = ZSTD_DEFAULT_MAX_MEMORY_BYTES;
  uint64_t max_expansion = ZSTD_DEFAULT_MAX_EXPANSION_RATIO;

  const void * dict_opt = NULL;
  size_t dict_opt_size = 0;

  if (options) {
    gcomp_options_get_bool(options, "zstd.concat", &concat);
    gcomp_options_get_uint64(options, "limits.max_output_bytes", &max_output);
    gcomp_options_get_uint64(options, "limits.max_window_bytes", &max_window);
    gcomp_options_get_uint64(options, "limits.max_memory_bytes", &max_memory);
    gcomp_options_get_uint64(
        options, "limits.max_expansion_ratio", &max_expansion);
    gcomp_options_get_bytes(
        options, "zstd.dictionary", &dict_opt, &dict_opt_size);
  }

  state->concat_enabled = (concat != 0);

  if (dict_opt != NULL && dict_opt_size > 0) {
    state->dict_bytes = gcomp_malloc(alloc, dict_opt_size);
    if (!state->dict_bytes) {
      status = GCOMP_ERR_MEMORY;
      goto cleanup;
    }
    memcpy(state->dict_bytes, dict_opt, dict_opt_size);
    state->dict_size = dict_opt_size;
    gcomp_memory_track_alloc(&state->mem_tracker, dict_opt_size);
    status = zstd_dict_parse(
        state->dict_bytes, state->dict_size, alloc, &state->dict_parsed);
    if (status != GCOMP_OK) {
      gcomp_free(alloc, state->dict_bytes);
      state->dict_bytes = NULL;
      state->dict_size = 0;
      goto cleanup;
    }
  }
  state->max_output_bytes = max_output;
  state->max_window_bytes = max_window;
  state->max_memory_bytes = max_memory;
  state->max_expansion_ratio = max_expansion;

  // Initialize repeat offsets
  state->rep_offset_1 = ZSTD_REP_OFFSET_1_INIT;
  state->rep_offset_2 = ZSTD_REP_OFFSET_2_INIT;
  state->rep_offset_3 = ZSTD_REP_OFFSET_3_INIT;

  // Set initial stage
  state->stage = ZSTD_DEC_STAGE_HEADER;
  state->header_stage = ZSTD_HEADER_MAGIC;

  // Explicitly initialize counters (should be 0 from calloc, but be explicit)
  state->total_input_bytes = 0;
  state->total_output_bytes = 0;
  state->frame_output_bytes = 0;

  // Allocate block buffer (will be resized based on header)
  size_t block_buffer_size = ZSTD_BLOCK_SIZE_MAX;
  state->block_buffer = gcomp_malloc(alloc, block_buffer_size);
  if (!state->block_buffer) {
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  state->block_buffer_capacity = block_buffer_size;
  gcomp_memory_track_alloc(&state->mem_tracker, block_buffer_size);

  // Allocate output buffer.  The sequence loop overshoots its copies, so the
  // allocation carries ZSTD_DECODE_SLACK spare bytes that the capacity does
  // not name; every bound the decoder checks is against the capacity, and
  // the slack is only ever reached by an overshoot.
  size_t output_buffer_size = ZSTD_BLOCK_SIZE_MAX;
  state->output_buffer =
      gcomp_malloc(alloc, output_buffer_size + ZSTD_DECODE_SLACK);
  if (!state->output_buffer) {
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  state->output_buffer_capacity = output_buffer_size;
  gcomp_memory_track_alloc(
      &state->mem_tracker, output_buffer_size + ZSTD_DECODE_SLACK);

  // Check memory limits through the core helper; see zstd_encoder.c.
  if (gcomp_memory_check_limit(&state->mem_tracker, max_memory) != GCOMP_OK) {
    status = GCOMP_ERR_LIMIT;
    goto cleanup;
  }

  decoder->method_state = state;
  return GCOMP_OK;

cleanup:
  if (decoder && status != GCOMP_OK) {
    const char * msg = "decoder initialization failed";
    if (status == GCOMP_ERR_MEMORY) {
      msg = "failed to allocate memory during decoder init";
    }
    else if (status == GCOMP_ERR_LIMIT) {
      msg = "memory limit exceeded during decoder init";
    }
    else if (status == GCOMP_ERR_CORRUPT || status == GCOMP_ERR_UNSUPPORTED) {
      msg = "dictionary parse failed";
    }
    gcomp_decoder_set_error(decoder, status, "%s", msg);
  }
  if (state) {
    zstd_dict_destroy(&state->dict_parsed);
    if (state->dict_bytes) {
      gcomp_memory_track_free(&state->mem_tracker, state->dict_size);
      gcomp_free(alloc, state->dict_bytes);
    }
    if (state->block_buffer) {
      gcomp_free(alloc, state->block_buffer);
    }
    if (state->output_buffer) {
      gcomp_free(alloc, state->output_buffer);
    }
    if (state->window_buffer) {
      gcomp_free(alloc, state->window_buffer);
    }
    gcomp_free(alloc, state);
  }
  return status;
}

//
// Destroy
//

void zstd_decoder_destroy(gcomp_decoder_t * decoder) {
  if (!decoder || !decoder->method_state) {
    return;
  }

  zstd_decoder_state_t * state = decoder->method_state;
  const gcomp_allocator_t * alloc = state->allocator;

  if (state->block_buffer) {
    gcomp_free(alloc, state->block_buffer);
  }
  if (state->output_buffer) {
    gcomp_free(alloc, state->output_buffer);
  }
  if (state->window_buffer) {
    gcomp_free(alloc, state->window_buffer);
  }
  zstd_dict_destroy(&state->dict_parsed);
  if (state->dict_bytes) {
    gcomp_memory_track_free(&state->mem_tracker, state->dict_size);
    gcomp_free(alloc, state->dict_bytes);
    state->dict_bytes = NULL;
  }
  if (state->fse_lit_table) {
    gcomp_free(alloc, state->fse_lit_table);
  }
  if (state->fse_match_table) {
    gcomp_free(alloc, state->fse_match_table);
  }
  if (state->fse_offset_table) {
    gcomp_free(alloc, state->fse_offset_table);
  }
  if (state->huf_table) {
    gcomp_free(alloc, state->huf_table);
  }
  gcomp_free(alloc, state);
  decoder->method_state = NULL;
}

//
// Header Parsing Helpers
//

/**
 * @brief Get the size of the dictionary ID field based on flag.
 */
/**
 * @brief Read the accumulated frame header, and decide what to do about it.
 *
 * The reading is zstd_frame_header_parse()'s, which gcomp_peek() also uses, so
 * a caller cannot be shown one interpretation of a header while the decode
 * acts on another.  What stays here is the part that is this decoder's own:
 * whether it will hold a window that large, whether it has the dictionary the
 * frame names, and the buffers that follow from both.
 */
static gcomp_status_t zstd_parse_frame_header(
    zstd_decoder_state_t * state, gcomp_decoder_t * decoder) {
  zstd_frame_header_t header;
  uint64_t window_size = 0;
  size_t header_len = 0;
  gcomp_status_t s = zstd_frame_header_parse(state->header_accum,
      state->header_accum_pos, &header, &window_size, &header_len);
  if (s == GCOMP_ERR_CORRUPT) {
    uint32_t magic = gcomp_read_le32(state->header_accum);
    if (magic != ZSTD_MAGIC) {
      gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
          "invalid zstd magic: expected 0x%08X, got 0x%08X", ZSTD_MAGIC,
          magic);
    }
    else {
      gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
          "reserved bit set in frame header descriptor");
    }
    return GCOMP_ERR_CORRUPT;
  }
  if (s != GCOMP_OK) {
    return gcomp_decoder_set_error(decoder, s,
        "zstd frame header needs %zu bytes, have %zu", header_len,
        state->header_accum_pos);
  }
  state->header = header;

  // RFC 8878 section 3.1.1.1.2 lets a frame declare a window log of up to 41.
  // Such a frame is not corrupt - it is well formed and asking for more
  // history than this decoder will hold - so it is GCOMP_ERR_LIMIT.  The
  // comparison is in 64 bits because the declared size need not fit in 32.
  if (window_size > state->max_window_bytes || window_size > UINT32_MAX) {
    gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
        "window size %llu exceeds limit %llu",
        (unsigned long long)window_size,
        (unsigned long long)state->max_window_bytes);
    return GCOMP_ERR_LIMIT;
  }

  if (state->header.dict_id != 0) {
    if (!state->dict_bytes) {
      gcomp_decoder_set_error(decoder, GCOMP_ERR_UNSUPPORTED,
          "zstd stream requires dictionary ID %u but no dictionary provided",
          state->header.dict_id);
      return GCOMP_ERR_UNSUPPORTED;
    }
    /* Require dict_id match for formatted dictionaries. Raw dictionaries
     * (dict_id 0) are accepted for any frame dict_id so external encoders
     * that write a content-derived ID for raw dicts still decode. */
    if (state->dict_parsed.dict_id != 0 &&
        state->dict_parsed.dict_id != state->header.dict_id) {
      gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
          "dictionary ID mismatch: frame has %u, dictionary has %u",
          state->header.dict_id, state->dict_parsed.dict_id);
      return GCOMP_ERR_CORRUPT;
    }
  }

  // Initialize content hash if checksum enabled
  if (state->header.content_checksum) {
    gcomp_xxhash64_reset(&state->content_hash, 0);
  }

  // Allocate window buffer if needed
  if (!state->window_buffer ||
      state->window_capacity < state->header.window_size) {
    if (state->window_buffer) {
      gcomp_memory_track_free(&state->mem_tracker, state->window_capacity);
      gcomp_free(state->allocator, state->window_buffer);
    }
    state->window_buffer =
        gcomp_malloc(state->allocator, state->header.window_size);
    if (!state->window_buffer) {
      gcomp_decoder_set_error(
          decoder, GCOMP_ERR_MEMORY, "failed to allocate window buffer");
      return GCOMP_ERR_MEMORY;
    }
    state->window_capacity = state->header.window_size;
    gcomp_memory_track_alloc(&state->mem_tracker, state->header.window_size);
  }
  state->window_size = 0;
  state->window_pos = 0;

  /* Preload window with dictionary content when the user provided a dictionary
   * and we have parsed content. This covers both frames with Dictionary_ID_Flag
   * set and raw-dict frames from zstd CLI/Python that omit the flag. */
  if (state->dict_bytes && state->dict_parsed.content &&
      state->dict_parsed.content_size > 0) {
    size_t copy_len = state->dict_parsed.content_size;
    if (copy_len > state->window_capacity) {
      copy_len = state->window_capacity;
    }
    memcpy(state->window_buffer, state->dict_parsed.content, copy_len);
    state->window_size = copy_len;
    state->window_pos = copy_len;
    if (state->window_pos >= state->window_capacity) {
      state->window_pos = 0;
    }
    state->rep_offset_1 = state->dict_parsed.rep_offset_1;
    state->rep_offset_2 = state->dict_parsed.rep_offset_2;
    state->rep_offset_3 = state->dict_parsed.rep_offset_3;
    if (state->dict_parsed.has_entropy_tables) {
      /* Allocate FSE/HUF tables if not yet allocated (needed for first block
       * Repeat/Treeless mode) */
      static const size_t FSE_LL_SIZE = 512;
      static const size_t FSE_ML_SIZE = 512;
      static const size_t FSE_OF_SIZE = 256;
      if (state->dict_parsed.fse_ll_table) {
        if (!state->fse_lit_table) {
          state->fse_lit_table = gcomp_malloc(
              state->allocator, FSE_LL_SIZE * sizeof(zstd_fse_entry_t));
          if (state->fse_lit_table) {
            state->fse_lit_table_size = FSE_LL_SIZE;
            gcomp_memory_track_alloc(
                &state->mem_tracker, FSE_LL_SIZE * sizeof(zstd_fse_entry_t));
          }
        }
        if (state->fse_lit_table &&
            state->fse_lit_table_size >= state->dict_parsed.fse_ll_size) {
          memcpy(state->fse_lit_table, state->dict_parsed.fse_ll_table,
              state->dict_parsed.fse_ll_size * sizeof(zstd_fse_entry_t));
          state->fse_ll_log = state->dict_parsed.fse_ll_log;
          state->fse_ll_ready = true;
        }
      }
      if (state->dict_parsed.fse_of_table) {
        if (!state->fse_offset_table) {
          state->fse_offset_table = gcomp_malloc(
              state->allocator, FSE_OF_SIZE * sizeof(zstd_fse_entry_t));
          if (state->fse_offset_table) {
            state->fse_offset_table_size = FSE_OF_SIZE;
            gcomp_memory_track_alloc(
                &state->mem_tracker, FSE_OF_SIZE * sizeof(zstd_fse_entry_t));
          }
        }
        if (state->fse_offset_table &&
            state->fse_offset_table_size >= state->dict_parsed.fse_of_size) {
          memcpy(state->fse_offset_table, state->dict_parsed.fse_of_table,
              state->dict_parsed.fse_of_size * sizeof(zstd_fse_entry_t));
          state->fse_of_log = state->dict_parsed.fse_of_log;
          state->fse_of_ready = true;
        }
      }
      if (state->dict_parsed.fse_ml_table) {
        if (!state->fse_match_table) {
          state->fse_match_table = gcomp_malloc(
              state->allocator, FSE_ML_SIZE * sizeof(zstd_fse_entry_t));
          if (state->fse_match_table) {
            state->fse_match_table_size = FSE_ML_SIZE;
            gcomp_memory_track_alloc(
                &state->mem_tracker, FSE_ML_SIZE * sizeof(zstd_fse_entry_t));
          }
        }
        if (state->fse_match_table &&
            state->fse_match_table_size >= state->dict_parsed.fse_ml_size) {
          memcpy(state->fse_match_table, state->dict_parsed.fse_ml_table,
              state->dict_parsed.fse_ml_size * sizeof(zstd_fse_entry_t));
          state->fse_ml_log = state->dict_parsed.fse_ml_log;
          state->fse_ml_ready = true;
        }
      }
      if (state->dict_parsed.huf_table) {
        if (!state->huf_table) {
          state->huf_table = gcomp_malloc(
              state->allocator, HUF_MAX_TABLE_SIZE * sizeof(zstd_huf_entry_t));
          if (state->huf_table) {
            state->huf_table_size = HUF_MAX_TABLE_SIZE;
            gcomp_memory_track_alloc(&state->mem_tracker,
                HUF_MAX_TABLE_SIZE * sizeof(zstd_huf_entry_t));
          }
        }
        if (state->huf_table &&
            state->huf_table_size >= state->dict_parsed.huf_table_size) {
          memcpy(state->huf_table, state->dict_parsed.huf_table,
              state->dict_parsed.huf_table_size * sizeof(zstd_huf_entry_t));
          state->huf_max_bits = state->dict_parsed.huf_max_bits;
          state->huf_table_valid = true;
        }
      }
    }
  }

  return GCOMP_OK;
}

//
// Update
//

/**
 * @brief Hand already-decompressed bytes to the caller.
 *
 * A block is decompressed into state->output_buffer whole and then copied out
 * as the caller's buffer allows.  That copy was three byte-at-a-time loops --
 * the same shape, and the same cost, as the ones the LZ4 decoder had.
 *
 * @return Non-zero once everything buffered has been delivered.
 */
static int zstd_drain_output(
    zstd_decoder_state_t * state, gcomp_buffer_t * output) {
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

/**
 * @brief One pass of the decoder: at most one frame header, one block, or one
 *        checksum.
 *
 * The loop that calls it is zstd_decoder_update().
 */
static gcomp_status_t zstd_decoder_step(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!decoder || !decoder->method_state || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Buffer validation
  if (input->size > 0 && !input->data) {
    gcomp_decoder_set_error(
        decoder, GCOMP_ERR_INVALID_ARG, "input data is NULL with size > 0");
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output->size > 0 && !output->data) {
    gcomp_decoder_set_error(
        decoder, GCOMP_ERR_INVALID_ARG, "output data is NULL with size > 0");
    return GCOMP_ERR_INVALID_ARG;
  }

  zstd_decoder_state_t * state = decoder->method_state;
  const uint8_t * in_ptr = (const uint8_t *)input->data;

  if (state->stage == ZSTD_DEC_STAGE_ERROR) {
    gcomp_decoder_set_error(
        decoder, GCOMP_ERR_INTERNAL, "decoder in error state");
    return GCOMP_ERR_INTERNAL;
  }

  // Handle concatenated frames: if DONE and concat enabled, check for more
  // frames
  if (state->stage == ZSTD_DEC_STAGE_DONE) {
    state->saw_data_frame = 1;
    if (state->concat_enabled && input->used < input->size) {
      // Reset for next frame (keep buffers)
      state->stage = ZSTD_DEC_STAGE_HEADER;
      state->header_stage = ZSTD_HEADER_MAGIC;
      state->header_accum_pos = 0;
      state->rep_offset_1 = ZSTD_REP_OFFSET_1_INIT;
      state->rep_offset_2 = ZSTD_REP_OFFSET_2_INIT;
      state->rep_offset_3 = ZSTD_REP_OFFSET_3_INIT;
      // Reset per-frame counter for content size validation
      state->frame_output_bytes = 0;
      // Reset window buffer for new frame (each frame starts with empty
      // history)
      state->window_pos = 0;
      state->window_size = 0;
      // Reset entropy table validity (each frame is independent): a first
      // block of the new frame asking for Repeat_Mode has nothing to repeat.
      state->huf_table_valid = false;
      state->fse_ll_ready = false;
      state->fse_ml_ready = false;
      state->fse_of_ready = false;
      // Note: don't reset total_input_bytes/total_output_bytes - they
      // accumulate across frames for limit checking
    }
    else {
      return GCOMP_OK;
    }
  }

  gcomp_status_t status = GCOMP_OK;

  // Drain any buffered output first
  if (!zstd_drain_output(state, output)) {
    return GCOMP_OK; // Need more output space
  }

  // RFC 8878 section 3.1.2: a skippable frame is magic 0x184D2A5? followed by
  // a four byte little-endian length and that many bytes of content a decoder
  // must ignore.  They are how tools attach metadata to a zstd file -- the
  // seekable format is built on them -- and they may appear anywhere a frame
  // may, including before the first data frame and between data frames.
  //
  // Nothing here implemented them: the magic check below accepted only
  // ZSTD_MAGIC, so any file carrying one was reported as corrupt even though
  // the constants for recognising one had been defined.
  if (state->stage == ZSTD_DEC_STAGE_SKIPPABLE) {
    size_t avail = input->size - input->used;
    size_t n = (avail < (size_t)state->skippable_remaining)
        ? avail
        : (size_t)state->skippable_remaining;
    input->used += n;
    state->total_input_bytes += n;
    state->skippable_remaining -= (uint32_t)n;
    if (state->skippable_remaining != 0) {
      return GCOMP_OK; // need more input
    }

    state->frames_completed++;
    state->header_accum_pos = 0;
    state->header_stage = ZSTD_HEADER_MAGIC;
    // A skippable frame carries no data, so it never counts as "the frame"
    // the caller asked for.  Before any data frame we keep reading whatever
    // the concat setting is -- otherwise a leading skippable frame would hide
    // the frame behind it.  After one, hand back to the DONE path so the
    // concat rule decides, exactly as it would between two data frames.
    state->stage =
        state->saw_data_frame ? ZSTD_DEC_STAGE_DONE : ZSTD_DEC_STAGE_HEADER;
    return GCOMP_OK;
  }

  // Parse header
  if (state->stage == ZSTD_DEC_STAGE_HEADER) {
    // Read magic number (4 bytes)
    while (state->header_accum_pos < 4 && input->used < input->size) {
      state->header_accum[state->header_accum_pos++] = in_ptr[input->used++];
      state->total_input_bytes++;
    }
    if (state->header_accum_pos < 4) {
      return GCOMP_OK;
    }

    // Verify magic and determine header length
    uint32_t magic = gcomp_read_le32(state->header_accum);
    if (magic >= ZSTD_MAGIC_SKIPPABLE_MIN &&
        magic <= ZSTD_MAGIC_SKIPPABLE_MAX) {
      // Skippable frame: four more bytes give the length of what to discard.
      while (state->header_accum_pos < 8 && input->used < input->size) {
        state->header_accum[state->header_accum_pos++] = in_ptr[input->used++];
        state->total_input_bytes++;
      }
      if (state->header_accum_pos < 8) {
        return GCOMP_OK; // need more input
      }
      state->skippable_remaining = gcomp_read_le32(state->header_accum + 4);
      state->stage = ZSTD_DEC_STAGE_SKIPPABLE;
      return GCOMP_OK;
    }
    if (magic != ZSTD_MAGIC) {
      gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
          "invalid zstd magic: expected 0x%08X, got 0x%08X", ZSTD_MAGIC, magic);
      state->stage = ZSTD_DEC_STAGE_ERROR;
      return GCOMP_ERR_CORRUPT;
    }

    // Read FHD byte
    if (state->header_accum_pos < 5 && input->used < input->size) {
      state->header_accum[state->header_accum_pos++] = in_ptr[input->used++];
      state->total_input_bytes++;
    }
    if (state->header_accum_pos < 5) {
      return GCOMP_OK;
    }

    // How much of the header to accumulate, from the same function the
    // parser uses to walk it.  Every term is bounded by a constant and the
    // largest total is eighteen bytes, so there is nothing here that can
    // overflow - which is why this no longer carries the checked arithmetic it
    // used to.
    state->header_expected_len = zstd_frame_header_length(state->header_accum[4]);

    // Read remaining header bytes
    while (state->header_accum_pos < state->header_expected_len &&
        input->used < input->size) {
      state->header_accum[state->header_accum_pos++] = in_ptr[input->used++];
      state->total_input_bytes++;
    }
    if (state->header_accum_pos < state->header_expected_len) {
      return GCOMP_OK;
    }

    // Parse the complete header
    status = zstd_parse_frame_header(state, decoder);
    if (status != GCOMP_OK) {
      state->stage = ZSTD_DEC_STAGE_ERROR;
      return status;
    }

    state->stage = ZSTD_DEC_STAGE_BLOCK_HEADER;
  }

  // Read block header
  if (state->stage == ZSTD_DEC_STAGE_BLOCK_HEADER) {
    while (state->block_header_buf_pos < ZSTD_BLOCK_HEADER_SIZE &&
        input->used < input->size) {
      state->block_header_buf[state->block_header_buf_pos++] =
          in_ptr[input->used++];
      state->total_input_bytes++;
    }
    if (state->block_header_buf_pos < ZSTD_BLOCK_HEADER_SIZE) {
      return GCOMP_OK;
    }

    // Parse block header
    status = zstd_parse_block_header(state->block_header_buf,
        &state->current_block_last, &state->current_block_type,
        &state->current_block_size);
    if (status != GCOMP_OK) {
      state->stage = ZSTD_DEC_STAGE_ERROR;
      gcomp_decoder_set_error(decoder, status, "invalid block header");
      return status;
    }

    // Validate block type
    if (state->current_block_type == ZSTD_BLOCK_TYPE_RESERVED) {
      state->stage = ZSTD_DEC_STAGE_ERROR;
      gcomp_decoder_set_error(
          decoder, GCOMP_ERR_CORRUPT, "reserved block type");
      return GCOMP_ERR_CORRUPT;
    }

    // Validate block size against window size (per zstd spec)
    if (state->current_block_size > state->header.window_size) {
      state->stage = ZSTD_DEC_STAGE_ERROR;
      gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
          "block size %u exceeds window size %u", state->current_block_size,
          state->header.window_size);
      return GCOMP_ERR_CORRUPT;
    }

    // Validate block size against maximum (128 KB per spec)
    if (state->current_block_size > ZSTD_BLOCK_SIZE_MAX) {
      state->stage = ZSTD_DEC_STAGE_ERROR;
      gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
          "block size %u exceeds maximum %u", state->current_block_size,
          ZSTD_BLOCK_SIZE_MAX);
      return GCOMP_ERR_CORRUPT;
    }

    // For RLE blocks, the input size is 1 byte (the byte to repeat)
    // For other blocks, input size equals block_size
    state->block_bytes_remaining =
        (state->current_block_type == ZSTD_BLOCK_TYPE_RLE)
        ? 1
        : state->current_block_size;
    state->block_buffer_pos = 0;
    state->block_header_buf_pos = 0;
    state->stage = ZSTD_DEC_STAGE_BLOCK_DATA;
  }

  // Read and decompress block data
  if (state->stage == ZSTD_DEC_STAGE_BLOCK_DATA) {
    // Read block data into buffer
    // For RLE, we only need 1 byte; for others, we need block_size bytes
    size_t bytes_to_read = state->block_bytes_remaining;

    // A block is up to 128 KB of compressed data (RFC 8878 section 3.1.1) and
    // this used to bring it in one byte per iteration, which was almost all
    // of what zstd_decoder_update() cost -- 1.9 million instructions out of
    // 2.1 to move bytes that had not been decompressed yet.
    if (state->block_buffer_pos < bytes_to_read) {
      size_t want = bytes_to_read - state->block_buffer_pos;
      size_t have = input->size - input->used;
      size_t n = (want < have) ? want : have;
      if (n > 0) {
        memcpy(state->block_buffer + state->block_buffer_pos, in_ptr + input->used, n);
        state->block_buffer_pos += n;
        input->used += n;
        state->total_input_bytes += n;
      }
    }
    if (state->block_buffer_pos < bytes_to_read) {
      return GCOMP_OK;
    }

    // Decompress block
    size_t decompressed_len = 0;
    switch (state->current_block_type) {
    case ZSTD_BLOCK_TYPE_RAW:
      status = zstd_block_decompress_raw(state->block_buffer,
          state->current_block_size, state->output_buffer,
          state->output_buffer_capacity, &decompressed_len);
      break;

    case ZSTD_BLOCK_TYPE_RLE:
      status = zstd_block_decompress_rle(state->block_buffer, 1,
          state->output_buffer, state->output_buffer_capacity,
          &decompressed_len, state->current_block_size);
      break;

    case ZSTD_BLOCK_TYPE_COMPRESSED:
      status = zstd_block_decompress_compressed(state, state->block_buffer,
          state->current_block_size, state->output_buffer,
          state->output_buffer_capacity, &decompressed_len);
      break;

    default:
      status = GCOMP_ERR_CORRUPT;
      break;
    }

    if (status != GCOMP_OK) {
      state->stage = ZSTD_DEC_STAGE_ERROR;
      gcomp_decoder_set_error(decoder, status, "block decompression failed");
      return status;
    }

    state->output_buffer_len = decompressed_len;
    state->output_buffer_pos = 0;

    // Update content hash if enabled
    if (state->header.content_checksum && decompressed_len > 0) {
      gcomp_xxhash64_update(
          &state->content_hash, state->output_buffer, decompressed_len);
    }

    // Update window buffer with decoded output (for cross-block match refs)
    // The window buffer is circular - we copy new data and update position
    if (decompressed_len > 0 && state->window_buffer) {
      if (decompressed_len >= state->window_capacity) {
        // New data is larger than window - just copy the last window_capacity
        // bytes
        memcpy(state->window_buffer,
            state->output_buffer + decompressed_len - state->window_capacity,
            state->window_capacity);
        state->window_pos = 0;
        state->window_size = state->window_capacity;
      }
      else {
        // Copy new data to window buffer, wrapping if needed
        size_t first_chunk =
            state->window_capacity - state->window_pos; // Space until wrap
        if (first_chunk >= decompressed_len) {
          // No wrap needed
          memcpy(state->window_buffer + state->window_pos, state->output_buffer,
              decompressed_len);
          state->window_pos += decompressed_len;
          if (state->window_pos >= state->window_capacity) {
            state->window_pos = 0;
          }
        }
        else {
          // Copy in two parts (wrap around)
          memcpy(state->window_buffer + state->window_pos, state->output_buffer,
              first_chunk);
          memcpy(state->window_buffer, state->output_buffer + first_chunk,
              decompressed_len - first_chunk);
          state->window_pos = decompressed_len - first_chunk;
        }
        // Update window_size (capped at capacity)
        state->window_size += decompressed_len;
        if (state->window_size > state->window_capacity) {
          state->window_size = state->window_capacity;
        }
      }
    }

    // Check output limits (use safe math for accumulated sizes)
    {
      uint64_t new_total;
      if (!gcu_safe_add_u64(state->total_output_bytes,
              (uint64_t)decompressed_len, &new_total)) {
        state->stage = ZSTD_DEC_STAGE_ERROR;
        gcomp_decoder_set_error(
            decoder, GCOMP_ERR_CORRUPT, "output size overflow");
        return GCOMP_ERR_CORRUPT;
      }
      state->total_output_bytes = new_total;
    }
    {
      uint64_t new_frame;
      if (!gcu_safe_add_u64(state->frame_output_bytes,
              (uint64_t)decompressed_len, &new_frame)) {
        state->stage = ZSTD_DEC_STAGE_ERROR;
        gcomp_decoder_set_error(
            decoder, GCOMP_ERR_CORRUPT, "frame output size overflow");
        return GCOMP_ERR_CORRUPT;
      }
      state->frame_output_bytes = new_frame;
    }
    if (gcomp_limits_check_output((size_t)state->total_output_bytes,
            state->max_output_bytes) != GCOMP_OK) {
      state->stage = ZSTD_DEC_STAGE_ERROR;
      gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
          "output limit exceeded: %llu > %llu",
          (unsigned long long)state->total_output_bytes,
          (unsigned long long)state->max_output_bytes);
      return GCOMP_ERR_LIMIT;
    }

    // Check expansion ratio
    if (gcomp_limits_check_expansion_ratio(state->total_input_bytes,
            state->total_output_bytes,
            state->max_expansion_ratio) != GCOMP_OK) {
      state->stage = ZSTD_DEC_STAGE_ERROR;
      gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
          "expansion ratio exceeded: %llu/%llu > %llu",
          (unsigned long long)state->total_output_bytes,
          (unsigned long long)state->total_input_bytes,
          (unsigned long long)state->max_expansion_ratio);
      return GCOMP_ERR_LIMIT;
    }

    // Output decompressed data
    (void)zstd_drain_output(state, output);

    // Determine next stage
    if (state->current_block_last) {
      // Validate content size if present in header
      // Use frame_output_bytes for per-frame validation (not total across
      // concat)
      if (state->header.content_size_present) {
        if (state->frame_output_bytes != state->header.content_size) {
          state->stage = ZSTD_DEC_STAGE_ERROR;
          gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
              "content size mismatch: expected %lu bytes, got %lu bytes",
              (unsigned long)state->header.content_size,
              (unsigned long)state->frame_output_bytes);
          return GCOMP_ERR_CORRUPT;
        }
      }

      if (state->header.content_checksum) {
        state->stage = ZSTD_DEC_STAGE_CONTENT_CHECKSUM;
        state->content_checksum_buf_pos = 0;
      }
      else {
        state->stage = ZSTD_DEC_STAGE_DONE;
        state->frames_completed++;
      }
    }
    else {
      state->stage = ZSTD_DEC_STAGE_BLOCK_HEADER;
    }
  }

  // Read content checksum
  if (state->stage == ZSTD_DEC_STAGE_CONTENT_CHECKSUM) {
    while (state->content_checksum_buf_pos < ZSTD_CONTENT_CHECKSUM_SIZE &&
        input->used < input->size) {
      state->content_checksum_buf[state->content_checksum_buf_pos++] =
          in_ptr[input->used++];
      state->total_input_bytes++;
    }
    if (state->content_checksum_buf_pos < ZSTD_CONTENT_CHECKSUM_SIZE) {
      return GCOMP_OK;
    }

    // Verify checksum
    uint32_t expected_checksum = gcomp_read_le32(state->content_checksum_buf);
    uint64_t computed_hash = gcomp_xxhash64_finalize(&state->content_hash);
    uint32_t computed_checksum = (uint32_t)(computed_hash & 0xFFFFFFFF);

    if (expected_checksum != computed_checksum) {
      state->stage = ZSTD_DEC_STAGE_ERROR;
      gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
          "content checksum mismatch: expected 0x%08X, got 0x%08X",
          expected_checksum, computed_checksum);
      return GCOMP_ERR_CORRUPT;
    }

    state->stage = ZSTD_DEC_STAGE_DONE;
    state->frames_completed++;
  }

  return GCOMP_OK;
}

/**
 * @brief Decode as much as the buffers allow.
 *
 * WHY THIS LOOPS
 * ==============
 *
 * The step above is a straight run through the stages -- frame header, block
 * header, block, checksum -- and it falls off the end after decoding a single
 * block.  So one call returned at most 128 KB (RFC 8878 section 3.1.1) and
 * left the rest of the input unread, even with the whole input in hand and
 * room to write.
 *
 * That is legal by the contract -- a caller is told to keep calling until a
 * call neither consumes nor produces -- but nobody writes that loop by
 * instinct, and the obvious code fails in a way that names the wrong culprit:
 * update() stops early, finish() finds the stream unfinished, and the error
 * reads "truncated zstd stream" about a stream that is perfectly intact.  The
 * DEFLATE decoder loops here, so this one now does too, and the two behave
 * alike.
 *
 * It also replaces a recursive call that handled the next frame when
 * concatenation is enabled: the same loop covers that, without putting a
 * frame's worth of stack frame behind every frame in the file.
 */
gcomp_status_t zstd_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!decoder || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  for (;;) {
    const size_t in_before = input->used;
    const size_t out_before = output->used;

    gcomp_status_t s = zstd_decoder_step(decoder, input, output);
    if (s != GCOMP_OK) {
      return s;
    }
    if (input->used == in_before && output->used == out_before) {
      // Nothing left to do with what it has: out of input, out of room, or
      // finished.
      return GCOMP_OK;
    }
  }
}

//
// Finish
//

gcomp_status_t zstd_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  if (!decoder || !decoder->method_state || !output) {
    if (decoder) {
      gcomp_decoder_set_error(
          decoder, GCOMP_ERR_INVALID_ARG, "decoder or output is NULL");
    }
    return GCOMP_ERR_INVALID_ARG;
  }

  // Buffer validation
  if (output->size > 0 && !output->data) {
    gcomp_decoder_set_error(
        decoder, GCOMP_ERR_INVALID_ARG, "output data is NULL with size > 0");
    return GCOMP_ERR_INVALID_ARG;
  }

  zstd_decoder_state_t * state = decoder->method_state;

  if (state->stage == ZSTD_DEC_STAGE_ERROR) {
    gcomp_decoder_set_error(
        decoder, GCOMP_ERR_INTERNAL, "decoder in error state");
    return GCOMP_ERR_INTERNAL;
  }

  // Drain any remaining buffered output
  (void)zstd_drain_output(state, output);

  // A block is decompressed whole into output_buffer and handed out as the
  // caller's buffer allows, so reaching the end of the stream and having
  // delivered it are different things.  This reported GCOMP_OK as soon as the
  // stage was DONE, whatever was still staged, and a caller following the
  // documented contract -- call finish() until it stops saying GCOMP_ERR_LIMIT
  // -- stopped on the first call and silently lost the rest of the last block.
  //
  // With an output buffer at least one block (128 KB) wide nothing was ever
  // left over and the defect was invisible; below that, every stream lost up
  // to a block minus whatever fitted.  The DEFLATE decoder already draws this
  // distinction, and finish() now behaves the same in both.
  if (state->output_buffer_pos < state->output_buffer_len) {
    return GCOMP_ERR_LIMIT;
  }

  if (state->stage == ZSTD_DEC_STAGE_DONE) {
    return GCOMP_OK;
  }

  // Sitting at a frame boundary with nothing part-read, having already
  // finished at least one frame, is a complete stream -- a file whose last
  // frame is skippable ends here, as does one made only of skippable frames.
  if (state->stage == ZSTD_DEC_STAGE_HEADER && state->header_accum_pos == 0 &&
      state->frames_completed > 0) {
    return GCOMP_OK;
  }

  // Nothing staged and not done: the input really did stop early.
  gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT, "truncated zstd stream");
  state->stage = ZSTD_DEC_STAGE_ERROR;
  return GCOMP_ERR_CORRUPT;
}

//
// Reset
//

gcomp_status_t zstd_decoder_reset(gcomp_decoder_t * decoder) {
  if (!decoder || !decoder->method_state) {
    if (decoder) {
      gcomp_decoder_set_error(
          decoder, GCOMP_ERR_INVALID_ARG, "decoder or method state is NULL");
    }
    return GCOMP_ERR_INVALID_ARG;
  }

  zstd_decoder_state_t * state = decoder->method_state;

  // Reset stage (retain buffers)
  state->stage = ZSTD_DEC_STAGE_HEADER;
  state->header_stage = ZSTD_HEADER_MAGIC;

  // Reset positions
  state->header_accum_pos = 0;
  state->block_header_buf_pos = 0;
  state->block_buffer_pos = 0;
  state->output_buffer_pos = 0;
  state->output_buffer_len = 0;
  state->content_checksum_buf_pos = 0;
  state->window_pos = 0;
  state->window_size = 0;

  // Reset counters
  state->total_input_bytes = 0;
  state->total_output_bytes = 0;
  state->frame_output_bytes = 0;
  state->skippable_remaining = 0;
  state->frames_completed = 0;
  state->saw_data_frame = 0;

  // Reset repeat offsets
  state->rep_offset_1 = ZSTD_REP_OFFSET_1_INIT;
  state->rep_offset_2 = ZSTD_REP_OFFSET_2_INIT;
  state->rep_offset_3 = ZSTD_REP_OFFSET_3_INIT;

  // Reset entropy table state
  state->huf_table_valid = false;
  state->fse_ll_ready = false;
  state->fse_ml_ready = false;
  state->fse_of_ready = false;

  return GCOMP_OK;
}
