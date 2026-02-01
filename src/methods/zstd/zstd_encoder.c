/**
 * @file zstd_encoder.c
 *
 * Zstandard encoder implementation for the Ghoti.io Compress library.
 *
 * This file implements the streaming Zstd encoder, which produces
 * Zstandard frame format output.
 *
 * ## State Machine
 *
 * The encoder progresses through these stages:
 * 1. HEADER: Write frame header (magic + header descriptor)
 * 2. BLOCKS: Write compressed data blocks
 * 3. CHECKSUM: Write content checksum (if enabled)
 * 4. DONE: Frame complete
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include "zstd_internal.h"
#include <string.h>

//
// Helper Functions
//

/**
 * @brief Get window log from compression level.
 */
uint8_t zstd_level_to_window_log(int level) {
  // Simple mapping - higher levels get larger windows
  if (level <= 3) {
    return 17; // 128 KB
  }
  else if (level <= 6) {
    return 19; // 512 KB
  }
  else if (level <= 10) {
    return 21; // 2 MB
  }
  else if (level <= 16) {
    return 23; // 8 MB
  }
  else {
    return 25; // 32 MB
  }
}

/**
 * @brief Compute window size from window log.
 */
uint32_t zstd_window_log_to_size(uint8_t window_log) {
  if (window_log < ZSTD_WINDOW_LOG_MIN || window_log > ZSTD_WINDOW_LOG_MAX) {
    return 0;
  }
  return 1U << window_log;
}

//
// Initialization
//

gcomp_status_t zstd_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  (void)registry;

  if (!encoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Get allocator from registry
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);
  gcomp_status_t status = GCOMP_OK;

  // Allocate state structure
  zstd_encoder_state_t * state = gcomp_calloc(alloc, 1, sizeof(*state));
  if (!state) {
    return GCOMP_ERR_MEMORY;
  }
  state->allocator = alloc;

  // Initialize memory tracker
  gcomp_memory_track_alloc(&state->mem_tracker, sizeof(*state));

  // Read options
  int64_t level = ZSTD_LEVEL_DEFAULT;
  int checksum = 0;
  uint64_t window_log = 0;
  uint64_t content_size = 0;
  bool content_size_present = false;
  uint64_t max_memory = ZSTD_DEFAULT_MAX_MEMORY_BYTES;

  if (options) {
    gcomp_options_get_int64(options, "zstd.level", &level);
    gcomp_options_get_bool(options, "zstd.checksum", &checksum);
    gcomp_options_get_uint64(options, "zstd.window_log", &window_log);
    if (gcomp_options_get_uint64(options, "zstd.content_size", &content_size) ==
        GCOMP_OK) {
      content_size_present = true;
    }
    gcomp_options_get_uint64(options, "limits.max_memory_bytes", &max_memory);
  }

  // Validate and set compression level
  if (level < ZSTD_LEVEL_MIN || level > ZSTD_LEVEL_MAX) {
    status = GCOMP_ERR_INVALID_ARG;
    goto cleanup;
  }
  state->compression_level = (int)level;

  // Determine window log
  uint8_t effective_window_log;
  if (window_log == 0) {
    effective_window_log = zstd_level_to_window_log(state->compression_level);
  }
  else {
    if (window_log < ZSTD_WINDOW_LOG_MIN || window_log > ZSTD_WINDOW_LOG_MAX) {
      status = GCOMP_ERR_INVALID_ARG;
      goto cleanup;
    }
    effective_window_log = (uint8_t)window_log;
  }

  // Set up frame header
  state->header.window_log = effective_window_log;
  state->header.window_size = zstd_window_log_to_size(effective_window_log);
  state->header.content_checksum = (checksum != 0);
  state->header.content_size_present = content_size_present;
  state->header.content_size = content_size;
  state->header.single_segment =
      content_size_present && (content_size <= state->header.window_size);
  state->checksum_enabled = (checksum != 0);
  state->max_memory_bytes = max_memory;

  // Initialize repeat offsets
  state->rep_offset_1 = ZSTD_REP_OFFSET_1_INIT;
  state->rep_offset_2 = ZSTD_REP_OFFSET_2_INIT;
  state->rep_offset_3 = ZSTD_REP_OFFSET_3_INIT;

  // Initialize content hash if checksum enabled
  if (checksum) {
    gcomp_xxhash64_reset(&state->content_hash, 0);
  }

  // Allocate block buffer
  size_t block_buffer_size = ZSTD_BLOCK_SIZE_MAX;
  state->block_buffer = gcomp_malloc(alloc, block_buffer_size);
  if (!state->block_buffer) {
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  state->block_buffer_capacity = block_buffer_size;
  gcomp_memory_track_alloc(&state->mem_tracker, block_buffer_size);

  // Allocate compressed buffer (may be larger than input for incompressible)
  size_t compressed_buffer_size =
      block_buffer_size + ZSTD_BLOCK_HEADER_SIZE + 256; // Some overhead
  state->compressed_buffer = gcomp_malloc(alloc, compressed_buffer_size);
  if (!state->compressed_buffer) {
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  state->compressed_buffer_capacity = compressed_buffer_size;
  gcomp_memory_track_alloc(&state->mem_tracker, compressed_buffer_size);

  // Check memory limits
  if (state->mem_tracker.current_bytes > max_memory) {
    status = GCOMP_ERR_LIMIT;
    goto cleanup;
  }

  // Set initial stage
  state->stage = ZSTD_ENC_STAGE_HEADER;

  // Build frame header
  status = zstd_write_frame_header(&state->header, state->header_buf,
      sizeof(state->header_buf), &state->header_len);
  if (status != GCOMP_OK) {
    goto cleanup;
  }

  encoder->method_state = state;
  return GCOMP_OK;

cleanup:
  if (state) {
    if (state->block_buffer) {
      gcomp_free(alloc, state->block_buffer);
    }
    if (state->compressed_buffer) {
      gcomp_free(alloc, state->compressed_buffer);
    }
    if (state->hash_table) {
      gcomp_free(alloc, state->hash_table);
    }
    gcomp_free(alloc, state);
  }
  return status;
}

//
// Destroy
//

void zstd_encoder_destroy(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return;
  }

  zstd_encoder_state_t * state = encoder->method_state;
  const gcomp_allocator_t * alloc = state->allocator;

  if (state->block_buffer) {
    gcomp_free(alloc, state->block_buffer);
  }
  if (state->compressed_buffer) {
    gcomp_free(alloc, state->compressed_buffer);
  }
  if (state->hash_table) {
    gcomp_free(alloc, state->hash_table);
  }
  gcomp_free(alloc, state);
  encoder->method_state = NULL;
}

//
// Update
//

gcomp_status_t zstd_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!encoder || !encoder->method_state || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Buffer validation (SAFE-1)
  if (input->size > 0 && !input->data) {
    gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "input data is NULL with size > 0");
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output->size > 0 && !output->data) {
    gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "output data is NULL with size > 0");
    return GCOMP_ERR_INVALID_ARG;
  }

  zstd_encoder_state_t * state = encoder->method_state;
  const uint8_t * in_ptr = (const uint8_t *)input->data;
  uint8_t * out_ptr = (uint8_t *)output->data;

  if (state->stage == ZSTD_ENC_STAGE_ERROR) {
    return GCOMP_ERR_INTERNAL;
  }
  if (state->stage == ZSTD_ENC_STAGE_DONE) {
    return GCOMP_OK;
  }

  // Write header if needed
  if (state->stage == ZSTD_ENC_STAGE_HEADER) {
    while (
        state->header_pos < state->header_len && output->used < output->size) {
      out_ptr[output->used++] = state->header_buf[state->header_pos++];
    }
    if (state->header_pos >= state->header_len) {
      state->stage = ZSTD_ENC_STAGE_BLOCKS;
    }
    else {
      return GCOMP_OK; // Need more output space
    }
  }

  // Process input in BLOCKS stage
  if (state->stage == ZSTD_ENC_STAGE_BLOCKS) {
    // First, drain any pending compressed output
    while (state->compressed_buffer_pos < state->compressed_buffer_len &&
        output->used < output->size) {
      out_ptr[output->used++] =
          state->compressed_buffer[state->compressed_buffer_pos++];
    }
    if (state->compressed_buffer_pos < state->compressed_buffer_len) {
      return GCOMP_OK; // Need more output space
    }

    // Buffer input data
    while (input->used < input->size) {
      size_t to_copy = input->size - input->used;
      size_t space = state->block_buffer_capacity - state->block_buffer_pos;
      if (to_copy > space) {
        to_copy = space;
      }
      memcpy(state->block_buffer + state->block_buffer_pos,
          in_ptr + input->used, to_copy);
      state->block_buffer_pos += to_copy;
      input->used += to_copy;
      state->total_input_bytes += to_copy;

      // Update content hash if enabled
      if (state->checksum_enabled) {
        gcomp_xxhash64_update(&state->content_hash,
            state->block_buffer + state->block_buffer_pos - to_copy, to_copy);
      }

      // If block buffer is full, compress and output
      if (state->block_buffer_pos >= state->block_buffer_capacity) {
        uint8_t block_type;
        size_t compressed_len;
        size_t input_len = state->block_buffer_pos;
        gcomp_status_t status = zstd_block_compress(state, state->block_buffer,
            input_len, state->compressed_buffer + 3,
            state->compressed_buffer_capacity - 3, &compressed_len,
            &block_type);
        if (status != GCOMP_OK) {
          state->stage = ZSTD_ENC_STAGE_ERROR;
          gcomp_encoder_set_error(encoder, status, "block compression failed");
          return status;
        }

        // Write block header
        // For RLE blocks, the header size field is the regenerated size
        uint32_t header_size = (block_type == ZSTD_BLOCK_TYPE_RLE)
            ? (uint32_t)input_len
            : (uint32_t)compressed_len;
        zstd_write_block_header(
            state->compressed_buffer, false, block_type, header_size);
        state->compressed_buffer_len = 3 + compressed_len;
        state->compressed_buffer_pos = 0;

        // Output compressed block
        while (state->compressed_buffer_pos < state->compressed_buffer_len &&
            output->used < output->size) {
          out_ptr[output->used++] =
              state->compressed_buffer[state->compressed_buffer_pos++];
        }

        state->block_buffer_pos = 0;

        // If we couldn't output everything, return and continue later
        if (state->compressed_buffer_pos < state->compressed_buffer_len) {
          return GCOMP_OK;
        }
      }
    }
  }

  return GCOMP_OK;
}

//
// Finish
//

gcomp_status_t zstd_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  if (!encoder || !encoder->method_state || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Buffer validation (SAFE-1)
  if (output->size > 0 && !output->data) {
    gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "output data is NULL with size > 0");
    return GCOMP_ERR_INVALID_ARG;
  }

  zstd_encoder_state_t * state = encoder->method_state;
  uint8_t * out_ptr = (uint8_t *)output->data;

  if (state->stage == ZSTD_ENC_STAGE_ERROR) {
    return GCOMP_ERR_INTERNAL;
  }
  if (state->stage == ZSTD_ENC_STAGE_DONE) {
    return GCOMP_OK;
  }

  state->finish_called = true;

  // First, drain any remaining header
  if (state->stage == ZSTD_ENC_STAGE_HEADER) {
    while (
        state->header_pos < state->header_len && output->used < output->size) {
      out_ptr[output->used++] = state->header_buf[state->header_pos++];
    }
    if (state->header_pos >= state->header_len) {
      state->stage = ZSTD_ENC_STAGE_BLOCKS;
    }
    else {
      return GCOMP_OK;
    }
  }

  // Flush remaining buffered data as final block
  if (state->stage == ZSTD_ENC_STAGE_BLOCKS) {
    // Drain pending compressed output
    while (state->compressed_buffer_pos < state->compressed_buffer_len &&
        output->used < output->size) {
      out_ptr[output->used++] =
          state->compressed_buffer[state->compressed_buffer_pos++];
    }
    if (state->compressed_buffer_pos < state->compressed_buffer_len) {
      return GCOMP_OK;
    }

    if (!state->blocks_finished) {
      // Compress remaining data as final block
      if (state->block_buffer_pos > 0) {
        uint8_t block_type;
        size_t compressed_len;
        size_t input_len = state->block_buffer_pos;
        gcomp_status_t status = zstd_block_compress(state, state->block_buffer,
            input_len, state->compressed_buffer + 3,
            state->compressed_buffer_capacity - 3, &compressed_len,
            &block_type);
        if (status != GCOMP_OK) {
          state->stage = ZSTD_ENC_STAGE_ERROR;
          gcomp_encoder_set_error(
              encoder, status, "final block compression failed");
          return status;
        }

        // Write block header (last block)
        // For RLE blocks, the header size field is the regenerated size
        // (input_len) For RAW blocks, it's the compressed size (same as
        // input_len)
        uint32_t header_size = (block_type == ZSTD_BLOCK_TYPE_RLE)
            ? (uint32_t)input_len
            : (uint32_t)compressed_len;
        zstd_write_block_header(
            state->compressed_buffer, true, block_type, header_size);
        state->compressed_buffer_len = 3 + compressed_len;
        state->compressed_buffer_pos = 0;
        state->block_buffer_pos = 0;
      }
      else {
        // No remaining data - write empty last block
        zstd_write_block_header(
            state->compressed_buffer, true, ZSTD_BLOCK_TYPE_RAW, 0);
        state->compressed_buffer_len = 3;
        state->compressed_buffer_pos = 0;
      }

      state->blocks_finished = true;
    }

    // Output final block
    while (state->compressed_buffer_pos < state->compressed_buffer_len &&
        output->used < output->size) {
      out_ptr[output->used++] =
          state->compressed_buffer[state->compressed_buffer_pos++];
    }
    if (state->compressed_buffer_pos < state->compressed_buffer_len) {
      return GCOMP_OK;
    }

    // Prepare checksum if enabled
    if (state->checksum_enabled) {
      uint64_t hash = gcomp_xxhash64_finalize(&state->content_hash);
      gcomp_write_le32(state->checksum_buf, (uint32_t)(hash & 0xFFFFFFFF));
      state->checksum_pos = 0;
      state->stage = ZSTD_ENC_STAGE_CHECKSUM;
    }
    else {
      state->stage = ZSTD_ENC_STAGE_DONE;
      return GCOMP_OK;
    }
  }

  // Output checksum
  if (state->stage == ZSTD_ENC_STAGE_CHECKSUM) {
    while (state->checksum_pos < ZSTD_CONTENT_CHECKSUM_SIZE &&
        output->used < output->size) {
      out_ptr[output->used++] = state->checksum_buf[state->checksum_pos++];
    }
    if (state->checksum_pos >= ZSTD_CONTENT_CHECKSUM_SIZE) {
      state->stage = ZSTD_ENC_STAGE_DONE;
      return GCOMP_OK;
    }
  }

  return GCOMP_OK;
}

//
// Reset
//

gcomp_status_t zstd_encoder_reset(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }

  zstd_encoder_state_t * state = encoder->method_state;

  // Reset stage
  state->stage = ZSTD_ENC_STAGE_HEADER;

  // Reset positions (BP-4: retain buffers)
  state->header_pos = 0;
  state->checksum_pos = 0;
  state->block_buffer_pos = 0;
  state->compressed_buffer_pos = 0;
  state->compressed_buffer_len = 0;

  // Reset counters
  state->total_input_bytes = 0;
  state->finish_called = false;
  state->blocks_finished = false;

  // Reset repeat offsets
  state->rep_offset_1 = ZSTD_REP_OFFSET_1_INIT;
  state->rep_offset_2 = ZSTD_REP_OFFSET_2_INIT;
  state->rep_offset_3 = ZSTD_REP_OFFSET_3_INIT;

  // Reset content hash if enabled
  if (state->checksum_enabled) {
    gcomp_xxhash64_reset(&state->content_hash, 0);
  }

  // Rebuild frame header
  gcomp_status_t status = zstd_write_frame_header(&state->header,
      state->header_buf, sizeof(state->header_buf), &state->header_len);

  return status;
}
