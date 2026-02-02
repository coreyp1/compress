/**
 * @file zstd_decoder.c
 *
 * Zstandard decoder implementation for the Ghoti.io Compress library.
 *
 * This file implements the streaming Zstd decoder, which parses
 * Zstandard frame format input.
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
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

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
    return GCOMP_ERR_MEMORY;
  }
  state->allocator = alloc;

  // Initialize memory tracker
  gcomp_memory_track_alloc(&state->mem_tracker, sizeof(*state));

  // Read options
  int concat = 0;
  uint64_t max_output = ZSTD_DEFAULT_MAX_OUTPUT_BYTES;
  uint64_t max_window = ZSTD_DEFAULT_MAX_WINDOW_BYTES;
  uint64_t max_memory = ZSTD_DEFAULT_MAX_MEMORY_BYTES;
  uint64_t max_expansion = ZSTD_DEFAULT_MAX_EXPANSION_RATIO;

  if (options) {
    gcomp_options_get_bool(options, "zstd.concat", &concat);
    gcomp_options_get_uint64(options, "limits.max_output_bytes", &max_output);
    gcomp_options_get_uint64(options, "limits.max_window_bytes", &max_window);
    gcomp_options_get_uint64(options, "limits.max_memory_bytes", &max_memory);
    gcomp_options_get_uint64(
        options, "limits.max_expansion_ratio", &max_expansion);
  }

  state->concat_enabled = (concat != 0);
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

  // Allocate block buffer (will be resized based on header)
  size_t block_buffer_size = ZSTD_BLOCK_SIZE_MAX;
  state->block_buffer = gcomp_malloc(alloc, block_buffer_size);
  if (!state->block_buffer) {
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  state->block_buffer_capacity = block_buffer_size;
  gcomp_memory_track_alloc(&state->mem_tracker, block_buffer_size);

  // Allocate output buffer
  size_t output_buffer_size = ZSTD_BLOCK_SIZE_MAX;
  state->output_buffer = gcomp_malloc(alloc, output_buffer_size);
  if (!state->output_buffer) {
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  state->output_buffer_capacity = output_buffer_size;
  gcomp_memory_track_alloc(&state->mem_tracker, output_buffer_size);

  // Check memory limits
  if (state->mem_tracker.current_bytes > max_memory) {
    status = GCOMP_ERR_LIMIT;
    goto cleanup;
  }

  decoder->method_state = state;
  return GCOMP_OK;

cleanup:
  if (state) {
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
static size_t zstd_dict_id_size(uint8_t flag) {
  static const size_t sizes[] = {0, 1, 2, 4};
  return sizes[flag & 0x03];
}

/**
 * @brief Get the size of the frame content size field based on flag.
 */
static size_t zstd_fcs_size(uint8_t flag, bool single_segment) {
  if (single_segment && flag == 0) {
    return 1; // Single segment with fcs_flag=0 means 1 byte
  }
  static const size_t sizes[] = {0, 2, 4, 8};
  return sizes[flag];
}

/**
 * @brief Parse frame header from accumulated buffer.
 */
static gcomp_status_t zstd_parse_frame_header(
    zstd_decoder_state_t * state, gcomp_decoder_t * decoder) {
  const uint8_t * buf = state->header_accum;
  size_t pos = 0;

  // Verify magic number
  uint32_t magic = gcomp_read_le32(buf);
  if (magic != ZSTD_MAGIC) {
    gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
        "invalid zstd magic: expected 0x%08X, got 0x%08X", ZSTD_MAGIC, magic);
    return GCOMP_ERR_CORRUPT;
  }
  pos += 4;

  // Parse frame header descriptor
  uint8_t fhd = buf[pos++];
  state->header.descriptor = fhd;

  // Check reserved/unused bits
  if (fhd & ZSTD_FHD_RESERVED_BIT) {
    gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
        "reserved bit set in frame header descriptor");
    return GCOMP_ERR_CORRUPT;
  }

  // Extract flags
  state->header.dict_id_flag = fhd & ZSTD_FHD_DICT_ID_FLAG_MASK;
  state->header.content_checksum = (fhd & ZSTD_FHD_CHECKSUM_FLAG) != 0;
  state->header.single_segment = (fhd & ZSTD_FHD_SINGLE_SEGMENT_FLAG) != 0;
  state->header.fcs_flag =
      (fhd & ZSTD_FHD_FCS_FLAG_MASK) >> ZSTD_FHD_FCS_FLAG_SHIFT;

  // Parse window descriptor (if not single segment)
  if (!state->header.single_segment) {
    uint8_t wd = buf[pos++];
    uint8_t exponent = (wd >> 3) & 0x1F;
    uint8_t mantissa = wd & 0x07;
    state->header.window_log = exponent + 10;
    uint32_t base = 1U << (exponent + 10);
    uint32_t add = (base / 8) * mantissa;
    state->header.window_size = base + add;
  }

  // Parse dictionary ID (if present)
  size_t dict_id_len = zstd_dict_id_size(state->header.dict_id_flag);
  if (dict_id_len > 0) {
    switch (dict_id_len) {
    case 1:
      state->header.dict_id = buf[pos];
      break;
    case 2:
      state->header.dict_id = gcomp_read_le16(buf + pos);
      break;
    case 4:
      state->header.dict_id = gcomp_read_le32(buf + pos);
      break;
    default:
      break;
    }
    pos += dict_id_len;
  }

  // Parse frame content size (if present)
  size_t fcs_len =
      zstd_fcs_size(state->header.fcs_flag, state->header.single_segment);
  state->header.content_size_present = (fcs_len > 0);
  if (fcs_len > 0) {
    switch (fcs_len) {
    case 1:
      state->header.content_size = buf[pos];
      break;
    case 2:
      state->header.content_size = gcomp_read_le16(buf + pos) + 256;
      break;
    case 4:
      state->header.content_size = gcomp_read_le32(buf + pos);
      break;
    case 8:
      state->header.content_size = gcomp_read_le64(buf + pos);
      break;
    default:
      break;
    }
    pos += fcs_len;

    // For single segment, window size equals content size
    if (state->header.single_segment) {
      state->header.window_size = (uint32_t)state->header.content_size;
      if (state->header.content_size > UINT32_MAX) {
        state->header.window_size = UINT32_MAX;
      }
    }
  }

  // Validate window size against limits
  if (state->header.window_size > state->max_window_bytes) {
    gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
        "window size %u exceeds limit %llu", state->header.window_size,
        (unsigned long long)state->max_window_bytes);
    return GCOMP_ERR_LIMIT;
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

  return GCOMP_OK;
}

//
// Update
//

gcomp_status_t zstd_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!decoder || !decoder->method_state || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Buffer validation (SAFE-1)
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
  uint8_t * out_ptr = (uint8_t *)output->data;

  if (state->stage == ZSTD_DEC_STAGE_ERROR) {
    return GCOMP_ERR_INTERNAL;
  }

  // Handle concatenated frames: if DONE and concat enabled, check for more
  // frames
  if (state->stage == ZSTD_DEC_STAGE_DONE) {
    if (state->concat_enabled && input->used < input->size) {
      // Reset for next frame (keep buffers per BP-4)
      state->stage = ZSTD_DEC_STAGE_HEADER;
      state->header_stage = ZSTD_HEADER_MAGIC;
      state->header_accum_pos = 0;
      state->rep_offset_1 = ZSTD_REP_OFFSET_1_INIT;
      state->rep_offset_2 = ZSTD_REP_OFFSET_2_INIT;
      state->rep_offset_3 = ZSTD_REP_OFFSET_3_INIT;
      // Note: don't reset total_input_bytes/total_output_bytes - they
      // accumulate across frames for limit checking
    }
    else {
      return GCOMP_OK;
    }
  }

  gcomp_status_t status = GCOMP_OK;

  // Drain any buffered output first
  while (state->output_buffer_pos < state->output_buffer_len &&
      output->used < output->size) {
    out_ptr[output->used++] = state->output_buffer[state->output_buffer_pos++];
  }
  if (state->output_buffer_pos < state->output_buffer_len) {
    return GCOMP_OK; // Need more output space
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

    // Calculate expected header length
    uint8_t fhd = state->header_accum[4];
    bool single_segment = (fhd & ZSTD_FHD_SINGLE_SEGMENT_FLAG) != 0;
    uint8_t dict_id_flag = fhd & ZSTD_FHD_DICT_ID_FLAG_MASK;
    uint8_t fcs_flag =
        (fhd & ZSTD_FHD_FCS_FLAG_MASK) >> ZSTD_FHD_FCS_FLAG_SHIFT;

    size_t expected_len = 5; // magic + FHD
    if (!single_segment) {
      expected_len += 1; // window descriptor
    }
    expected_len += zstd_dict_id_size(dict_id_flag);
    expected_len += zstd_fcs_size(fcs_flag, single_segment);

    state->header_expected_len = expected_len;

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
    while (
        state->block_buffer_pos < bytes_to_read && input->used < input->size) {
      state->block_buffer[state->block_buffer_pos++] = in_ptr[input->used++];
      state->total_input_bytes++;
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

    // Check output limits
    state->total_output_bytes += decompressed_len;
    if (state->total_output_bytes > state->max_output_bytes) {
      state->stage = ZSTD_DEC_STAGE_ERROR;
      gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
          "output limit exceeded: %llu > %llu",
          (unsigned long long)state->total_output_bytes,
          (unsigned long long)state->max_output_bytes);
      return GCOMP_ERR_LIMIT;
    }

    // Check expansion ratio (CORRECT-1)
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
    while (state->output_buffer_pos < state->output_buffer_len &&
        output->used < output->size) {
      out_ptr[output->used++] =
          state->output_buffer[state->output_buffer_pos++];
    }

    // Determine next stage
    if (state->current_block_last) {
      // Validate content size if present in header (Z3.3)
      if (state->header.content_size_present) {
        if (state->total_output_bytes != state->header.content_size) {
          state->stage = ZSTD_DEC_STAGE_ERROR;
          gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
              "content size mismatch: expected %lu bytes, got %lu bytes",
              (unsigned long)state->header.content_size,
              (unsigned long)state->total_output_bytes);
          return GCOMP_ERR_CORRUPT;
        }
      }

      if (state->header.content_checksum) {
        state->stage = ZSTD_DEC_STAGE_CONTENT_CHECKSUM;
        state->content_checksum_buf_pos = 0;
      }
      else {
        state->stage = ZSTD_DEC_STAGE_DONE;
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
  }

  // If we reached DONE and concat is enabled and there's more input,
  // continue processing (recursive call to handle next frame)
  if (state->stage == ZSTD_DEC_STAGE_DONE && state->concat_enabled &&
      input->used < input->size) {
    return zstd_decoder_update(decoder, input, output);
  }

  return GCOMP_OK;
}

//
// Finish
//

gcomp_status_t zstd_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  if (!decoder || !decoder->method_state || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Buffer validation (SAFE-1)
  if (output->size > 0 && !output->data) {
    gcomp_decoder_set_error(
        decoder, GCOMP_ERR_INVALID_ARG, "output data is NULL with size > 0");
    return GCOMP_ERR_INVALID_ARG;
  }

  zstd_decoder_state_t * state = decoder->method_state;
  uint8_t * out_ptr = (uint8_t *)output->data;

  if (state->stage == ZSTD_DEC_STAGE_ERROR) {
    return GCOMP_ERR_INTERNAL;
  }

  // Drain any remaining buffered output
  while (state->output_buffer_pos < state->output_buffer_len &&
      output->used < output->size) {
    out_ptr[output->used++] = state->output_buffer[state->output_buffer_pos++];
  }

  if (state->stage == ZSTD_DEC_STAGE_DONE) {
    return GCOMP_OK;
  }

  // If we're not done and there's no more output to drain, we have truncated
  // input
  if (state->output_buffer_pos >= state->output_buffer_len) {
    gcomp_decoder_set_error(
        decoder, GCOMP_ERR_CORRUPT, "truncated zstd stream");
    state->stage = ZSTD_DEC_STAGE_ERROR;
    return GCOMP_ERR_CORRUPT;
  }

  return GCOMP_OK;
}

//
// Reset
//

gcomp_status_t zstd_decoder_reset(gcomp_decoder_t * decoder) {
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }

  zstd_decoder_state_t * state = decoder->method_state;

  // Reset stage (BP-4: retain buffers)
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

  // Reset repeat offsets
  state->rep_offset_1 = ZSTD_REP_OFFSET_1_INIT;
  state->rep_offset_2 = ZSTD_REP_OFFSET_2_INIT;
  state->rep_offset_3 = ZSTD_REP_OFFSET_3_INIT;

  // Reset Huffman table state
  state->huf_table_valid = false;

  return GCOMP_OK;
}
