/**
 * @file lz4_encoder.c
 *
 * LZ4 frame format encoder implementation.
 *
 * This file implements the streaming encoder for LZ4 frame format compression.
 * The encoder produces output conforming to the LZ4 Frame Format specification:
 * https://github.com/lz4/lz4/blob/dev/doc/lz4_Frame_format.md
 *
 * ## Encoder State Machine
 *
 * The encoder progresses through these stages:
 *
 * ```
 *   ┌──────────┐
 *   │  HEADER  │◄─── init()
 *   └────┬─────┘
 *        │ header bytes emitted
 *        v
 *   ┌──────────┐
 *   │  BLOCKS  │◄─── update() collects input, emits blocks when full
 *   └────┬─────┘
 *        │ finish() called, final block emitted
 *        v
 *   ┌──────────┐
 *   │ END_MARK │──── 4 bytes: 0x00000000
 *   └────┬─────┘
 *        │
 *        v
 *   ┌──────────┐
 *   │ TRAILER  │──── content checksum (if enabled)
 *   └────┬─────┘
 *        │
 *        v
 *   ┌──────────┐
 *   │   DONE   │
 *   └──────────┘
 * ```
 *
 * ## Data Flow
 *
 * 1. **Input buffering**: Data from update() is collected in `block_buffer`
 *    until it reaches `block_size` bytes.
 *
 * 2. **Block compression**: When the buffer is full (or finish() is called),
 *    the data is compressed using `lz4_block_compress()`. If compression
 *    doesn't reduce size, the block is stored uncompressed (high bit set
 *    in block size field).
 *
 * 3. **Checksum computation**: If `lz4.content_checksum` is enabled, the
 *    xxHash32 is computed incrementally over all uncompressed input.
 *
 * 4. **Incremental output**: All output (header, blocks, trailer) supports
 *    incremental emission when the output buffer is small. State variables
 *    track progress to resume on subsequent calls.
 *
 * ## Memory Management
 *
 * The encoder allocates these buffers (tracked via `gcomp_memory_tracker_t`):
 *
 * | Buffer | Size | Purpose |
 * |--------|------|---------|
 * | block_buffer | block_size (+ 64 KB when linked) | Search window: the
 *   retained prefix, then the block being collected |
 * | compressed_buffer | block_size + overhead | Hold compressed block for
 * output | | hash_table | 64K entries (256KB) | Match finding for LZ77 |
 *
 * Total memory is checked against `limits.max_memory_bytes` after allocation.
 *
 * ## Hash Table for Match Finding
 *
 * The encoder uses a simple hash table for LZ77 match finding:
 * - Hash function: multiply first 4 bytes by prime, shift right
 * - Hash table stores position of last occurrence of each hash
 * - Only checks most recent match (no hash chains)
 * - Matches must be within 64KB window and at least 4 bytes
 *
 * For independent blocks, the hash table is cleared after each block, so a
 * block can only reference itself.  For linked blocks it is kept, and the
 * last LZ4_WINDOW_SIZE bytes of the frame are kept in front of the block
 * being filled, so a match can reach back across the block boundary -- see
 * lz4_encoder_slide_window().
 * For dependent blocks, it persists (though the current implementation
 * doesn't fully utilize cross-block matching).
 *
 * ## Thread Safety
 *
 * A single encoder instance is NOT thread-safe. Each thread should have
 * its own encoder instance. The encoder itself does not use threading;
 * parallel compression is handled at a higher level via lz4_parallel.c.
 *
 * Copyright 2026 by Corey Pennycuff
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
// Helper: Read options and configure encoder
//

/**
 * @brief Retire the block just emitted and prepare the window for the next.
 *
 * Independent blocks keep no history: the table is cleared so the next block
 * can only reference itself, which is what the Block Independence flag
 * promises.
 *
 * Linked blocks keep the last LZ4_WINDOW_SIZE bytes of what has been emitted
 * so far.  Everything older is dropped -- the match offset is two bytes, so a
 * match can never reach past it.  Hash entries are offsets from the start of
 * the window, so sliding the bytes means rebasing the entries by the same
 * amount; an entry that falls off the front is cleared.  The two must move
 * together, or an entry would name a byte the window no longer holds.
 *
 * Entries at exactly `shift` are dropped rather than rebased to 0, because 0
 * is this table's "empty" marker.  One position per slide, and it costs a
 * missed match, never a wrong one.
 */
static void lz4_encoder_seed_window(lz4_encoder_state_t * state) {
  state->block_buffer_pos = 0;
  memset(state->hash_table, 0, state->hash_table_size * sizeof(uint32_t));
  state->prefix_len = state->dictionary_size;
  if (state->dictionary_size > 0) {
    // The dictionary is already at the front of the window and is never
    // overwritten, so only the table has to be rebuilt.  That is a scan of up
    // to 64 KB per block in independent mode; liblz4 keeps a preloaded table
    // to avoid it, which would be the optimisation to make if it ever shows
    // up in a profile.
    lz4_block_index_window(state->block_buffer, state->dictionary_size,
        state->hash_table, state->hash_table_size);
  }
}

static void lz4_encoder_slide_window(lz4_encoder_state_t * state) {
  if (state->header.block_independence) {
    // An independent block starts from the dictionary alone -- and from
    // nothing at all when there is no dictionary, which is what this did
    // before dictionaries existed.
    lz4_encoder_seed_window(state);
    return;
  }

  size_t total = state->prefix_len + state->block_buffer_pos;
  size_t keep =
      (total < state->prefix_capacity) ? total : state->prefix_capacity;
  size_t shift = total - keep;

  if (shift > 0) {
    memmove(state->block_buffer, state->block_buffer + shift, keep);
    for (size_t i = 0; i < state->hash_table_size; i++) {
      uint32_t pos = state->hash_table[i];
      state->hash_table[i] = (pos > shift) ? (uint32_t)(pos - shift) : 0;
    }
  }

  state->prefix_len = keep;
  state->block_buffer_pos = 0;
}

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
  state->dictionary = NULL;
  state->dictionary_size = 0;
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

  if (!encoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Get allocator from registry
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);

  gcomp_status_t status = GCOMP_OK;
  lz4_encoder_state_t * state = NULL;

  // Allocate state
  state = (lz4_encoder_state_t *)gcomp_calloc(
      alloc, 1, sizeof(lz4_encoder_state_t));
  if (!state) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "failed to allocate lz4 encoder state");
  }
  state->allocator = alloc;

  // Track memory usage (tracker is zero-initialized by calloc)
  gcomp_memory_track_alloc(&state->mem_tracker, sizeof(lz4_encoder_state_t));

  // Read options
  status = lz4_encoder_read_options(options, state);
  if (status != GCOMP_OK) {
    goto cleanup;
  }

  // Allocate block buffer.  Linked blocks keep LZ4_WINDOW_SIZE bytes of the
  // preceding frame in front of the block being filled, so a match can reach
  // back across the block boundary; independent blocks keep nothing and the
  // buffer is exactly one block, as before.
  // How much of the dictionary we will actually hold, before anything is
  // allocated: only the tail, since the match offset is two bytes.
  const void * dict_data = NULL;
  size_t dict_size = 0;
  if (options &&
      gcomp_options_get_bytes(options, "lz4.dictionary", &dict_data,
          &dict_size) == GCOMP_OK &&
      dict_data && dict_size > 0) {
    if (dict_size > LZ4_WINDOW_SIZE) {
      dict_data = (const uint8_t *)dict_data + (dict_size - LZ4_WINDOW_SIZE);
      dict_size = LZ4_WINDOW_SIZE;
    }
  }
  else {
    dict_data = NULL;
    dict_size = 0;
  }

  // Independent blocks need a window too when there is a dictionary: such a
  // block may not reference the blocks before it, but it may reference the
  // dictionary, so the dictionary sits in front of every one of them.
  state->prefix_capacity = (state->header.block_independence && dict_size == 0)
      ? 0
      : LZ4_WINDOW_SIZE;
  state->prefix_len = 0;
  state->dictionary_size = dict_size;
  state->block_buffer_size = state->header.block_max_size;
  size_t window_bytes = state->block_buffer_size + state->prefix_capacity;
  state->block_buffer = (uint8_t *)gcomp_malloc(alloc, window_bytes);
  if (!state->block_buffer) {
    status = gcomp_encoder_set_error(encoder, GCOMP_ERR_MEMORY,
        "failed to allocate lz4 block buffer (%zu bytes)", window_bytes);
    goto cleanup;
  }
  gcomp_memory_track_alloc(&state->mem_tracker, window_bytes);
  state->block_buffer_pos = 0;
  if (dict_size > 0) {
    // The dictionary lives at the front of the window.  Block data is always
    // written after prefix_len, so with independent blocks nothing ever
    // overwrites it and re-seeding is only a matter of re-indexing.  With
    // linked blocks the window slides and the frame's own output displaces it,
    // which is what should happen.
    memcpy(state->block_buffer, dict_data, dict_size);
    state->prefix_len = dict_size;
  }

  // Allocate compressed buffer (block + header + checksum overhead)
  state->compressed_buffer_size = state->header.block_max_size +
      LZ4_BLOCK_HEADER_SIZE + LZ4_BLOCK_CHECKSUM_SIZE + 16; // Extra for safety
  state->compressed_buffer =
      (uint8_t *)gcomp_malloc(alloc, state->compressed_buffer_size);
  if (!state->compressed_buffer) {
    status = gcomp_encoder_set_error(encoder, GCOMP_ERR_MEMORY,
        "failed to allocate lz4 compressed buffer (%zu bytes)",
        state->compressed_buffer_size);
    goto cleanup;
  }
  gcomp_memory_track_alloc(&state->mem_tracker, state->compressed_buffer_size);
  state->compressed_buffer_pos = 0;
  state->compressed_buffer_len = 0;

  // Allocate hash table for compression (64KB entries)
  state->hash_table_size = 65536;
  state->hash_table = (uint32_t *)gcomp_malloc(
      alloc, state->hash_table_size * sizeof(uint32_t));
  if (!state->hash_table) {
    status = gcomp_encoder_set_error(encoder, GCOMP_ERR_MEMORY,
        "failed to allocate lz4 hash table (%zu bytes)",
        state->hash_table_size * sizeof(uint32_t));
    goto cleanup;
  }
  gcomp_memory_track_alloc(
      &state->mem_tracker, state->hash_table_size * sizeof(uint32_t));
  memset(state->hash_table, 0, state->hash_table_size * sizeof(uint32_t));

  // Make the dictionary findable before the first block is compressed.  The
  // bytes are already in the window; without this the table is empty and the
  // first block would match nothing in them.
  if (state->dictionary_size > 0) {
    lz4_block_index_window(state->block_buffer, state->dictionary_size,
        state->hash_table, state->hash_table_size);
  }

  // Check memory limit through the core helper, which is where "0 means
  // unlimited" is defined (limits.h says so for every limit option).  Every
  // open-coded copy of this comparison is a chance to forget that, and three
  // of them had.
  if (gcomp_memory_check_limit(&state->mem_tracker, state->max_memory_bytes) !=
      GCOMP_OK) {
    status = gcomp_encoder_set_error(encoder, GCOMP_ERR_MEMORY,
        "lz4 encoder memory usage %llu exceeds limit %llu",
        (unsigned long long)state->mem_tracker.current_bytes,
        (unsigned long long)state->max_memory_bytes);
    goto cleanup;
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
    goto cleanup;
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

  // Success
  encoder->method_state = state;
  return GCOMP_OK;

cleanup:
  // Single cleanup path for all error cases
  if (state) {
    gcomp_free(alloc, state->hash_table);
    gcomp_free(alloc, state->compressed_buffer);
    gcomp_free(alloc, state->block_buffer);
    gcomp_free(alloc, state);
  }
  return status;
}

void lz4_encoder_destroy(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return;
  }

  lz4_encoder_state_t * state = (lz4_encoder_state_t *)encoder->method_state;
  const gcomp_allocator_t * alloc = state->allocator;

  if (state->hash_table) {
    gcomp_free(alloc, state->hash_table);
  }
  if (state->compressed_buffer) {
    gcomp_free(alloc, state->compressed_buffer);
  }
  if (state->block_buffer) {
    gcomp_free(alloc, state->block_buffer);
  }

  gcomp_free(alloc, state);
  encoder->method_state = NULL;
}

gcomp_status_t lz4_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!encoder || !encoder->method_state || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }
  // Check data pointers if size > 0
  if ((input->size > 0 && !input->data) ||
      (output->size > 0 && !output->data)) {
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
    // If we have a partially-emitted block from a previous call, flush it
    if (state->compressed_buffer_len > 0) {
      while (state->compressed_buffer_pos < state->compressed_buffer_len &&
          output->used < output->size) {
        ((uint8_t *)output->data)[output->used++] =
            state->compressed_buffer[state->compressed_buffer_pos++];
      }
      if (state->compressed_buffer_pos < state->compressed_buffer_len) {
        // Still have pending block bytes; caller should provide more space
        return GCOMP_OK;
      }

      // Finished emitting this block
      state->compressed_buffer_len = 0;
      state->compressed_buffer_pos = 0;
    }

    while (input->used < input->size) {
      // Buffer input data
      size_t available = input->size - input->used;
      size_t space = state->block_buffer_size - state->block_buffer_pos;
      size_t to_copy = (available < space) ? available : space;

      memcpy(
          state->block_buffer + state->prefix_len + state->block_buffer_pos,
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
            lz4_block_compress_linked(state->block_buffer, state->prefix_len,
                state->block_buffer_pos, state->compressed_buffer + 4,
                state->compressed_buffer_size - 4, &compressed_len,
                state->hash_table, state->hash_table_size);

        if (status != GCOMP_OK || compressed_len >= state->block_buffer_pos) {
          // Compression didn't help, store uncompressed
          uint32_t block_size =
              (uint32_t)state->block_buffer_pos | LZ4_BLOCK_UNCOMPRESSED_FLAG;
          gcomp_write_le32(state->compressed_buffer, block_size);
          memcpy(state->compressed_buffer + 4,
              state->block_buffer + state->prefix_len,
              state->block_buffer_pos);
          compressed_len = state->block_buffer_pos;
        }
        else {
          // Use compressed data
          gcomp_write_le32(state->compressed_buffer, (uint32_t)compressed_len);
        }

        // Add block checksum if enabled
        size_t total_block_len = 4 + compressed_len;
        if (state->header.block_checksum) {
          uint32_t checksum =
              gcomp_xxhash32(state->compressed_buffer + 4, compressed_len, 0);
          gcomp_write_le32(
              state->compressed_buffer + total_block_len, checksum);
          total_block_len += 4;
        }

        // Stage compressed block for incremental output
        state->compressed_buffer_len = total_block_len;
        state->compressed_buffer_pos = 0;

        // Retire the block: keep what the next one may match into, drop the
        // rest, and move the hash table with it.
        lz4_encoder_slide_window(state);

        // Emit as much of the staged block as fits
        while (state->compressed_buffer_pos < state->compressed_buffer_len &&
            output->used < output->size) {
          ((uint8_t *)output->data)[output->used++] =
              state->compressed_buffer[state->compressed_buffer_pos++];
        }

        if (state->compressed_buffer_pos < state->compressed_buffer_len) {
          // Output buffer filled mid-block; caller should resume later
          return GCOMP_OK;
        }

        // Finished emitting this block; clear staging for the next one
        state->compressed_buffer_len = 0;
        state->compressed_buffer_pos = 0;
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
      // Need more output space. GCOMP_ERR_LIMIT, not GCOMP_OK:
      // gcomp_encoder_finish() documents GCOMP_OK as meaning the stream is
      // complete, so returning it here made a truncated stream
      // indistinguishable from a finished one.
      return GCOMP_ERR_LIMIT;
    }
  }

  // Flush any remaining buffered data as a final block
  if (state->stage == LZ4_ENC_STAGE_BLOCKS && !state->blocks_finished) {
    // Check if we have compressed data still being output
    if (state->compressed_buffer_len > 0) {
      // Continue outputting previously compressed block
      while (state->compressed_buffer_pos < state->compressed_buffer_len &&
          output->used < output->size) {
        ((uint8_t *)output->data)[output->used++] =
            state->compressed_buffer[state->compressed_buffer_pos++];
      }
      if (state->compressed_buffer_pos < state->compressed_buffer_len) {
        return GCOMP_ERR_LIMIT; // Need more output space; call finish again.
      }
      // Done with this block
      state->compressed_buffer_len = 0;
      state->compressed_buffer_pos = 0;
      state->block_buffer_pos = 0;
    }
    else if (state->block_buffer_pos > 0) {
      // Compress final block
      size_t compressed_len;
      gcomp_status_t status =
          lz4_block_compress_linked(state->block_buffer, state->prefix_len,
              state->block_buffer_pos, state->compressed_buffer + 4,
              state->compressed_buffer_size - 4, &compressed_len,
              state->hash_table, state->hash_table_size);

      if (status != GCOMP_OK || compressed_len >= state->block_buffer_pos) {
        // Store uncompressed
        uint32_t block_size =
            (uint32_t)state->block_buffer_pos | LZ4_BLOCK_UNCOMPRESSED_FLAG;
        gcomp_write_le32(state->compressed_buffer, block_size);
        memcpy(state->compressed_buffer + 4,
            state->block_buffer + state->prefix_len, state->block_buffer_pos);
        compressed_len = state->block_buffer_pos;
      }
      else {
        gcomp_write_le32(state->compressed_buffer, (uint32_t)compressed_len);
      }

      // Add block checksum if enabled
      size_t total_block_len = 4 + compressed_len;
      if (state->header.block_checksum) {
        uint32_t checksum =
            gcomp_xxhash32(state->compressed_buffer + 4, compressed_len, 0);
        gcomp_write_le32(state->compressed_buffer + total_block_len, checksum);
        total_block_len += 4;
      }

      // Output final block
      state->compressed_buffer_len = total_block_len;
      state->compressed_buffer_pos = 0;
      while (state->compressed_buffer_pos < state->compressed_buffer_len &&
          output->used < output->size) {
        ((uint8_t *)output->data)[output->used++] =
            state->compressed_buffer[state->compressed_buffer_pos++];
      }

      if (state->compressed_buffer_pos < state->compressed_buffer_len) {
        return GCOMP_ERR_LIMIT; // Need more output space; call finish again.
      }

      state->compressed_buffer_len = 0;
      state->compressed_buffer_pos = 0;
      state->block_buffer_pos = 0;
    }
    state->blocks_finished = true;
    state->stage = LZ4_ENC_STAGE_END_MARK;

    // Prepare end mark
    gcomp_write_le32(state->end_mark_buf, LZ4_END_MARK);
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
        gcomp_write_le32(state->trailer_buf, checksum);
        state->trailer_len = 4;
      }
    }
    if (output->used >= output->size &&
        state->stage == LZ4_ENC_STAGE_END_MARK) {
      return GCOMP_ERR_LIMIT; // Need more output space; call finish again.
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
      return GCOMP_ERR_LIMIT; // Need more output space; call finish again.
    }
  }

  if (state->stage == LZ4_ENC_STAGE_DONE) {
    return GCOMP_OK;
  }

  // Not finished: something above still has bytes to emit.
  return GCOMP_ERR_LIMIT;
}

gcomp_status_t lz4_encoder_reset(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }

  lz4_encoder_state_t * state = (lz4_encoder_state_t *)encoder->method_state;

  // Reset state
  state->stage = LZ4_ENC_STAGE_HEADER;
  state->header_pos = 0;
  state->compressed_buffer_pos = 0;
  state->compressed_buffer_len = 0;
  state->end_mark_pos = 0;
  state->trailer_pos = 0;
  state->total_input_bytes = 0;
  state->finish_called = false;
  state->blocks_finished = false;

  // Clear the hash table and put the window back where a new frame starts:
  // at the dictionary, or at nothing.  The two must move together -- an entry
  // and the byte it names are only meaningful as a pair.
  lz4_encoder_seed_window(state);

  // Reset content checksum
  if (state->header.content_checksum) {
    gcomp_xxhash32_reset(&state->content_hash, 0);
  }

  return GCOMP_OK;
}
