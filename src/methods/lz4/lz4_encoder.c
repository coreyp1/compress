/**
 * @file lz4_encoder.c
 *
 * LZ4 frame format encoder implementation.
 *
 * This file implements the streaming encoder for LZ4 frame format compression.
 *
 * ## Encoder State Machine
 *
 * 1. HEADER: Write frame header with configuration
 * 2. BLOCKS: Compress input data into blocks
 * 3. END_MARK: Write 4-byte end mark (0x00000000)
 * 4. TRAILER: Write content checksum (if enabled)
 * 5. DONE: Frame complete
 *
 * ## Memory Management
 *
 * The encoder allocates:
 * - Block buffer (block_size bytes) for collecting input
 * - Compressed buffer (block_size + overhead) for block output
 * - Hash table for match finding
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "lz4_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <stdlib.h>
#include <string.h>

//
// Helper: Read options and configure encoder
//

static gcomp_status_t lz4_encoder_read_options(
    gcomp_options_t * options, lz4_encoder_state_t * state) {
  uint64_t u64_val;
  int bool_val;

  // Set defaults
  state->header.block_max_size = LZ4_DEFAULT_BLOCK_SIZE;
  state->header.block_checksum = LZ4_DEFAULT_BLOCK_CHECKSUM;
  state->header.content_checksum = LZ4_DEFAULT_CONTENT_CHECKSUM;
  state->header.block_independence = LZ4_DEFAULT_INDEPENDENT_BLOCKS;
  state->header.content_size_present = false;
  state->header.content_size = 0;
  state->header.dict_id_present = false;
  state->header.dict_id = 0;
  state->max_memory_bytes = LZ4_DEFAULT_MAX_MEMORY_BYTES;

  if (!options) {
    return GCOMP_OK;
  }

  // Read lz4.block_size
  if (gcomp_options_get_uint64(options, "lz4.block_size", &u64_val) ==
      GCOMP_OK) {
    state->header.block_max_size = (uint32_t)u64_val;
  }

  // Read lz4.block_checksum
  if (gcomp_options_get_bool(options, "lz4.block_checksum", &bool_val) ==
      GCOMP_OK) {
    state->header.block_checksum = bool_val != 0;
  }

  // Read lz4.content_checksum
  if (gcomp_options_get_bool(options, "lz4.content_checksum", &bool_val) ==
      GCOMP_OK) {
    state->header.content_checksum = bool_val != 0;
  }

  // Read lz4.independent_blocks
  if (gcomp_options_get_bool(options, "lz4.independent_blocks", &bool_val) ==
      GCOMP_OK) {
    state->header.block_independence = bool_val != 0;
  }

  // Read lz4.content_size (optional)
  if (gcomp_options_get_uint64(options, "lz4.content_size", &u64_val) ==
      GCOMP_OK) {
    if (u64_val > 0) {
      state->header.content_size_present = true;
      state->header.content_size = u64_val;
    }
  }

  // Read lz4.dictionary_id (optional)
  if (gcomp_options_get_uint64(options, "lz4.dictionary_id", &u64_val) ==
      GCOMP_OK) {
    if (u64_val > 0) {
      state->header.dict_id_present = true;
      state->header.dict_id = (uint32_t)u64_val;
    }
  }

  // Read limits.max_memory_bytes
  if (gcomp_options_get_uint64(options, "limits.max_memory_bytes", &u64_val) ==
      GCOMP_OK) {
    state->max_memory_bytes = u64_val;
  }

  return GCOMP_OK;
}

//
// Internal API Implementation
//

gcomp_status_t lz4_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  (void)registry;

  if (!encoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Allocate state
  lz4_encoder_state_t * state =
      (lz4_encoder_state_t *)calloc(1, sizeof(lz4_encoder_state_t));
  if (!state) {
    return GCOMP_ERR_MEMORY;
  }

  // Track memory usage (tracker is zero-initialized by calloc)
  gcomp_memory_track_alloc(&state->mem_tracker, sizeof(lz4_encoder_state_t));

  // Read options
  gcomp_status_t status = lz4_encoder_read_options(options, state);
  if (status != GCOMP_OK) {
    free(state);
    return status;
  }

  // Allocate block buffer
  state->block_buffer_size = state->header.block_max_size;
  state->block_buffer = (uint8_t *)malloc(state->block_buffer_size);
  if (!state->block_buffer) {
    free(state);
    return GCOMP_ERR_MEMORY;
  }
  gcomp_memory_track_alloc(&state->mem_tracker, state->block_buffer_size);
  state->block_buffer_pos = 0;

  // Allocate compressed buffer (block + header + checksum overhead)
  state->compressed_buffer_size = state->header.block_max_size +
      LZ4_BLOCK_HEADER_SIZE + LZ4_BLOCK_CHECKSUM_SIZE + 16; // Extra for safety
  state->compressed_buffer = (uint8_t *)malloc(state->compressed_buffer_size);
  if (!state->compressed_buffer) {
    free(state->block_buffer);
    free(state);
    return GCOMP_ERR_MEMORY;
  }
  gcomp_memory_track_alloc(&state->mem_tracker, state->compressed_buffer_size);
  state->compressed_buffer_pos = 0;

  // Allocate hash table for compression (64KB entries)
  state->hash_table_size = 65536;
  state->hash_table =
      (uint32_t *)malloc(state->hash_table_size * sizeof(uint32_t));
  if (!state->hash_table) {
    free(state->compressed_buffer);
    free(state->block_buffer);
    free(state);
    return GCOMP_ERR_MEMORY;
  }
  gcomp_memory_track_alloc(
      &state->mem_tracker, state->hash_table_size * sizeof(uint32_t));
  memset(state->hash_table, 0, state->hash_table_size * sizeof(uint32_t));

  // Check memory limit
  if (state->max_memory_bytes > 0 &&
      state->mem_tracker.current_bytes > state->max_memory_bytes) {
    free(state->hash_table);
    free(state->compressed_buffer);
    free(state->block_buffer);
    free(state);
    return GCOMP_ERR_MEMORY;
  }

  // Build FLG and BD bytes
  state->header.flg = LZ4_FLG_VERSION_VALUE; // Version 01
  if (state->header.block_independence) {
    state->header.flg |= LZ4_FLG_B_INDEP;
  }
  if (state->header.block_checksum) {
    state->header.flg |= LZ4_FLG_B_CHECKSUM;
  }
  if (state->header.content_size_present) {
    state->header.flg |= LZ4_FLG_C_SIZE;
  }
  if (state->header.content_checksum) {
    state->header.flg |= LZ4_FLG_C_CHECKSUM;
  }
  if (state->header.dict_id_present) {
    state->header.flg |= LZ4_FLG_DICT_ID;
  }

  // BD byte: block max size
  uint8_t block_code = lz4_size_to_block_code(state->header.block_max_size);
  state->header.bd = (uint8_t)(block_code << LZ4_BD_BLOCK_MAX_SHIFT);

  // Build frame header
  status = lz4_write_frame_header(&state->header, state->header_buf,
      sizeof(state->header_buf), &state->header_len);
  if (status != GCOMP_OK) {
    free(state->hash_table);
    free(state->compressed_buffer);
    free(state->block_buffer);
    free(state);
    return status;
  }
  state->header_pos = 0;

  // Initialize content checksum if enabled
  if (state->header.content_checksum) {
    gcomp_xxhash32_reset(&state->content_hash, 0);
  }

  // Set initial stage
  state->stage = LZ4_ENC_STAGE_HEADER;
  state->total_input_bytes = 0;
  state->finish_called = false;
  state->blocks_finished = false;

  encoder->method_state = state;
  return GCOMP_OK;
}

void lz4_encoder_destroy(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return;
  }

  lz4_encoder_state_t * state = (lz4_encoder_state_t *)encoder->method_state;

  if (state->hash_table) {
    free(state->hash_table);
  }
  if (state->compressed_buffer) {
    free(state->compressed_buffer);
  }
  if (state->block_buffer) {
    free(state->block_buffer);
  }

  free(state);
  encoder->method_state = NULL;
}

gcomp_status_t lz4_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!encoder || !encoder->method_state || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  lz4_encoder_state_t * state = (lz4_encoder_state_t *)encoder->method_state;

  if (state->stage == LZ4_ENC_STAGE_ERROR) {
    return GCOMP_ERR_INTERNAL;
  }

  if (state->finish_called) {
    return GCOMP_ERR_INTERNAL;
  }

  // Write header first
  if (state->stage == LZ4_ENC_STAGE_HEADER) {
    while (
        state->header_pos < state->header_len && output->used < output->size) {
      ((uint8_t *)output->data)[output->used++] =
          state->header_buf[state->header_pos++];
    }
    if (state->header_pos >= state->header_len) {
      state->stage = LZ4_ENC_STAGE_BLOCKS;
    }
    if (output->used >= output->size) {
      return GCOMP_OK; // Output full, continue later
    }
  }

  // Process input data
  if (state->stage == LZ4_ENC_STAGE_BLOCKS) {
    while (input->used < input->size) {
      // Buffer input data
      size_t available = input->size - input->used;
      size_t space = state->block_buffer_size - state->block_buffer_pos;
      size_t to_copy = (available < space) ? available : space;

      memcpy(state->block_buffer + state->block_buffer_pos,
          (const uint8_t *)input->data + input->used, to_copy);
      state->block_buffer_pos += to_copy;
      input->used += to_copy;
      state->total_input_bytes += to_copy;

      // Update content checksum
      if (state->header.content_checksum) {
        gcomp_xxhash32_update(&state->content_hash,
            (const uint8_t *)input->data + input->used - to_copy, to_copy);
      }

      // If block is full, compress and output it
      if (state->block_buffer_pos >= state->block_buffer_size) {
        // Compress block
        size_t compressed_len;
        gcomp_status_t status =
            lz4_block_compress(state->block_buffer, state->block_buffer_pos,
                state->compressed_buffer + 4, state->compressed_buffer_size - 4,
                &compressed_len, state->hash_table, state->hash_table_size);

        if (status != GCOMP_OK || compressed_len >= state->block_buffer_pos) {
          // Compression didn't help, store uncompressed
          uint32_t block_size =
              (uint32_t)state->block_buffer_pos | LZ4_BLOCK_UNCOMPRESSED_FLAG;
          lz4_write_le32(state->compressed_buffer, block_size);
          memcpy(state->compressed_buffer + 4, state->block_buffer,
              state->block_buffer_pos);
          compressed_len = state->block_buffer_pos;
        }
        else {
          // Use compressed data
          lz4_write_le32(state->compressed_buffer, (uint32_t)compressed_len);
        }

        // Add block checksum if enabled
        size_t total_block_len = 4 + compressed_len;
        if (state->header.block_checksum) {
          uint32_t checksum =
              gcomp_xxhash32(state->compressed_buffer + 4, compressed_len, 0);
          lz4_write_le32(state->compressed_buffer + total_block_len, checksum);
          total_block_len += 4;
        }

        // Output compressed block
        size_t block_pos = 0;
        while (block_pos < total_block_len && output->used < output->size) {
          ((uint8_t *)output->data)[output->used++] =
              state->compressed_buffer[block_pos++];
        }

        // If we couldn't output the whole block, we need to buffer it
        // For simplicity, require output buffer to be large enough
        if (block_pos < total_block_len) {
          // TODO: Handle partial block output
          state->stage = LZ4_ENC_STAGE_ERROR;
          return GCOMP_ERR_LIMIT;
        }

        // Reset block buffer
        state->block_buffer_pos = 0;

        // Clear hash table for independent blocks
        if (state->header.block_independence) {
          memset(
              state->hash_table, 0, state->hash_table_size * sizeof(uint32_t));
        }
      }
    }
  }

  return GCOMP_OK;
}

gcomp_status_t lz4_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  if (!encoder || !encoder->method_state || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  lz4_encoder_state_t * state = (lz4_encoder_state_t *)encoder->method_state;

  if (state->stage == LZ4_ENC_STAGE_ERROR) {
    return GCOMP_ERR_INTERNAL;
  }

  state->finish_called = true;

  // First, output any remaining header bytes
  if (state->stage == LZ4_ENC_STAGE_HEADER) {
    while (
        state->header_pos < state->header_len && output->used < output->size) {
      ((uint8_t *)output->data)[output->used++] =
          state->header_buf[state->header_pos++];
    }
    if (state->header_pos >= state->header_len) {
      state->stage = LZ4_ENC_STAGE_BLOCKS;
    }
    if (output->used >= output->size && state->stage == LZ4_ENC_STAGE_HEADER) {
      return GCOMP_OK; // Need more output space
    }
  }

  // Flush any remaining buffered data as a final block
  if (state->stage == LZ4_ENC_STAGE_BLOCKS && !state->blocks_finished) {
    if (state->block_buffer_pos > 0) {
      // Compress final block
      size_t compressed_len;
      gcomp_status_t status =
          lz4_block_compress(state->block_buffer, state->block_buffer_pos,
              state->compressed_buffer + 4, state->compressed_buffer_size - 4,
              &compressed_len, state->hash_table, state->hash_table_size);

      if (status != GCOMP_OK || compressed_len >= state->block_buffer_pos) {
        // Store uncompressed
        uint32_t block_size =
            (uint32_t)state->block_buffer_pos | LZ4_BLOCK_UNCOMPRESSED_FLAG;
        lz4_write_le32(state->compressed_buffer, block_size);
        memcpy(state->compressed_buffer + 4, state->block_buffer,
            state->block_buffer_pos);
        compressed_len = state->block_buffer_pos;
      }
      else {
        lz4_write_le32(state->compressed_buffer, (uint32_t)compressed_len);
      }

      // Add block checksum if enabled
      size_t total_block_len = 4 + compressed_len;
      if (state->header.block_checksum) {
        uint32_t checksum =
            gcomp_xxhash32(state->compressed_buffer + 4, compressed_len, 0);
        lz4_write_le32(state->compressed_buffer + total_block_len, checksum);
        total_block_len += 4;
      }

      // Output final block
      size_t block_pos = 0;
      while (block_pos < total_block_len && output->used < output->size) {
        ((uint8_t *)output->data)[output->used++] =
            state->compressed_buffer[block_pos++];
      }

      if (block_pos < total_block_len) {
        return GCOMP_OK;
      }

      state->block_buffer_pos = 0;
    }
    state->blocks_finished = true;
    state->stage = LZ4_ENC_STAGE_END_MARK;

    // Prepare end mark
    lz4_write_le32(state->end_mark_buf, LZ4_END_MARK);
    state->end_mark_pos = 0;
  }

  // Output end mark
  if (state->stage == LZ4_ENC_STAGE_END_MARK) {
    while (state->end_mark_pos < 4 && output->used < output->size) {
      ((uint8_t *)output->data)[output->used++] =
          state->end_mark_buf[state->end_mark_pos++];
    }
    if (state->end_mark_pos >= 4) {
      state->stage = LZ4_ENC_STAGE_TRAILER;

      // Prepare trailer (content checksum if enabled)
      state->trailer_len = 0;
      state->trailer_pos = 0;
      if (state->header.content_checksum) {
        uint32_t checksum = gcomp_xxhash32_finalize(&state->content_hash);
        lz4_write_le32(state->trailer_buf, checksum);
        state->trailer_len = 4;
      }
    }
    if (output->used >= output->size &&
        state->stage == LZ4_ENC_STAGE_END_MARK) {
      return GCOMP_OK;
    }
  }

  // Output trailer
  if (state->stage == LZ4_ENC_STAGE_TRAILER) {
    while (state->trailer_pos < state->trailer_len &&
        output->used < output->size) {
      ((uint8_t *)output->data)[output->used++] =
          state->trailer_buf[state->trailer_pos++];
    }
    if (state->trailer_pos >= state->trailer_len) {
      state->stage = LZ4_ENC_STAGE_DONE;
    }
    if (output->used >= output->size && state->stage == LZ4_ENC_STAGE_TRAILER) {
      return GCOMP_OK;
    }
  }

  if (state->stage == LZ4_ENC_STAGE_DONE) {
    return GCOMP_OK;
  }

  return GCOMP_OK;
}

gcomp_status_t lz4_encoder_reset(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }

  lz4_encoder_state_t * state = (lz4_encoder_state_t *)encoder->method_state;

  // Reset state
  state->stage = LZ4_ENC_STAGE_HEADER;
  state->header_pos = 0;
  state->block_buffer_pos = 0;
  state->compressed_buffer_pos = 0;
  state->end_mark_pos = 0;
  state->trailer_pos = 0;
  state->total_input_bytes = 0;
  state->finish_called = false;
  state->blocks_finished = false;

  // Clear hash table
  memset(state->hash_table, 0, state->hash_table_size * sizeof(uint32_t));

  // Reset content checksum
  if (state->header.content_checksum) {
    gcomp_xxhash32_reset(&state->content_hash, 0);
  }

  return GCOMP_OK;
}
