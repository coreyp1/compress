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
 * In parallel mode (see below) steps 1 and 2 move into per-block jobs: input
 * is collected into a job's window instead of `block_buffer` and a worker
 * compresses and frames it. Steps 3 and 4 are unchanged, and so is
 * everything outside the block loop -- the header, the end mark and the
 * trailer are written here either way.
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
 * Parallel mode allocates neither `block_buffer` nor `hash_table`: each
 * in-flight job carries its own, so the encoder's would never be read. It
 * keeps `compressed_buffer`, which becomes the place a collected block's
 * tail waits when the caller's output buffer fills part-way through it, and
 * adds a copy of the dictionary to seed jobs from. Job buffers are reported
 * to the same tracker, so the limit covers them too.
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
 * A single encoder instance is NOT thread-safe: one encoder belongs to one
 * thread, and two threads sharing one is a defect whatever the options say.
 *
 * `threads.count > 1` is about something else -- it lets *this* encoder hand
 * whole blocks to a pool of workers, which the LZ4 Frame Format permits
 * exactly when the Block Independence flag is set.  The caller still drives a
 * single encoder from a single thread and still gets one frame; only the
 * block compression happens elsewhere, and the bytes are the same either way.
 * See lz4_parallel.h.
 */

#include "lz4_internal.h"
#include "lz4_parallel.h"
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

/// Defined with the other parallel helpers, below; destroy() is above them.
static void lz4_encoder_discard_parallel_jobs(lz4_encoder_state_t * state);

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
  state->prefix_len = state->dictionary_size;
  if (state->dictionary_size > 0 && state->dict_hash_table) {
    // The dictionary is already at the front of the window and is never
    // overwritten, so only the table has to be restored - and the table the
    // dictionary alone produces was worked out once, at create time.  Copying
    // it back is one pass over the table; rebuilding it is a pass over the
    // table plus a scan of up to 64 KB of dictionary, for every independent
    // block.  liblz4 keeps a preloaded table for the same reason.
    memcpy(state->hash_table, state->dict_hash_table,
        state->hash_table_size * sizeof(uint32_t));
    return;
  }
  memset(state->hash_table, 0, state->hash_table_size * sizeof(uint32_t));
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
  state->num_threads = 1;

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

  // Read threads.count.  Whether it is honoured is decided later, in init:
  // it takes independent blocks as well, and that is a different option.
  if (gcomp_options_get_uint64(options, "threads.count", &u64_val) ==
      GCOMP_OK) {
    state->num_threads = u64_val > 1 ? (uint32_t)u64_val : 1u;
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

  // Whether the workers get used at all.
  //
  // `threads.count` asks for them.  The LZ4 Frame Format grants them only
  // when the Block Independence flag is set, because a linked block may
  // reference the block before it, and that dependency is exactly what makes
  // compressing the two at the same time impossible.  Linked blocks are not
  // an error and neither is asking for threads; the two together simply leave
  // nothing to parallelise, so the encoder compresses in the calling thread
  // and reports it through gcomp_lz4_encoder_worker_count() rather than
  // failing a call or quietly leaving the caller to assume.
  const bool use_parallel =
      state->num_threads > 1 && state->header.block_independence;

  state->dictionary_size = dict_size;
  state->block_buffer_size = state->header.block_max_size;
  // 64K entries.  In parallel mode this is also every job's table size: the
  // hash is a function of the table size, so a job with a different one would
  // find a different set of matches and emit different bytes.
  state->hash_table_size = 65536;

  if (use_parallel) {
    // Each in-flight block carries its own window and table, so the encoder's
    // own would never be read; they are not allocated.  What it does still
    // need is the dictionary -- to seed every job's window from -- and the
    // table indexing it produces, worked out once here instead of once per
    // block.
    state->prefix_capacity = 0;
    state->prefix_len = 0;
    if (dict_size > 0) {
      state->dictionary = (uint8_t *)gcomp_malloc(alloc, dict_size);
      if (!state->dictionary) {
        status = gcomp_encoder_set_error(encoder, GCOMP_ERR_MEMORY,
            "failed to allocate lz4 dictionary copy (%zu bytes)", dict_size);
        goto cleanup;
      }
      gcomp_memory_track_alloc(&state->mem_tracker, dict_size);
      memcpy(state->dictionary, dict_data, dict_size);

      size_t table_bytes = state->hash_table_size * sizeof(uint32_t);
      state->dict_hash_table = (uint32_t *)gcomp_malloc(alloc, table_bytes);
      if (!state->dict_hash_table) {
        status = gcomp_encoder_set_error(encoder, GCOMP_ERR_MEMORY,
            "failed to allocate lz4 dictionary hash table (%zu bytes)",
            table_bytes);
        goto cleanup;
      }
      gcomp_memory_track_alloc(&state->mem_tracker, table_bytes);
      memset(state->dict_hash_table, 0, table_bytes);
      lz4_block_index_window(state->dictionary, dict_size,
          state->dict_hash_table, state->hash_table_size);
    }
  }
  else {
    // Allocate block buffer.  Linked blocks keep LZ4_WINDOW_SIZE bytes of the
    // preceding frame in front of the block being filled, so a match can reach
    // back across the block boundary; independent blocks keep nothing and the
    // buffer is exactly one block, as before.

    // Independent blocks need a window too when there is a dictionary: such a
    // block may not reference the blocks before it, but it may reference the
    // dictionary, so the dictionary sits in front of every one of them.
    state->prefix_capacity =
        (state->header.block_independence && dict_size == 0) ? 0
                                                             : LZ4_WINDOW_SIZE;
    state->prefix_len = 0;
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
      // linked blocks the window slides and the frame's own output displaces
      // it, which is what should happen.
      memcpy(state->block_buffer, dict_data, dict_size);
      state->prefix_len = dict_size;
    }

    // Allocate hash table for compression (64KB entries)
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

      // Keep that table.  Every independent block starts from the dictionary
      // and nothing else, so this is the table each of them begins with:
      // lz4_encoder_seed_window() copies it back instead of scanning up to
      // 64 KB of dictionary again for every block.  liblz4 keeps a preloaded
      // table for the same reason.
      state->dict_hash_table = (uint32_t *)gcomp_malloc(
          alloc, state->hash_table_size * sizeof(uint32_t));
      if (!state->dict_hash_table) {
        status = gcomp_encoder_set_error(encoder, GCOMP_ERR_MEMORY,
            "failed to allocate lz4 dictionary hash table (%zu bytes)",
            state->hash_table_size * sizeof(uint32_t));
        goto cleanup;
      }
      gcomp_memory_track_alloc(
          &state->mem_tracker, state->hash_table_size * sizeof(uint32_t));
      memcpy(state->dict_hash_table, state->hash_table,
          state->hash_table_size * sizeof(uint32_t));
    }
  }

  // Allocate compressed buffer (block + header + checksum overhead).
  //
  // In parallel mode this is not where a block is built -- a worker builds it
  // whole, framed -- but it is still where the tail of one waits when the
  // caller's output buffer fills part-way through handing it over.  One
  // block's worth is enough because only one result is ever held back at a
  // time, and LZ4_PARALLEL_BLOCK_OVERHEAD is this same arithmetic so that a
  // framed block always fits.
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

  if (use_parallel) {
    // Created last of the encoder's allocations, so that the in-flight bound
    // it works out is against what is actually left of the memory budget.
    lz4_parallel_config_t pconfig = {
        .num_threads = state->num_threads,
        .max_in_flight = 0, // Auto
        .block_max_size = state->header.block_max_size,
        .block_checksum = state->header.block_checksum,
        .dictionary = state->dictionary,
        .dictionary_size = state->dictionary_size,
        .dict_hash_table = state->dict_hash_table,
        .hash_table_size = state->hash_table_size,
        .max_memory_bytes = state->max_memory_bytes,
        .mem_tracker = &state->mem_tracker,
        .allocator = alloc,
    };
    status = lz4_parallel_create(&pconfig, &state->parallel_ctx);
    if (status != GCOMP_OK) {
      gcomp_encoder_set_error(
          encoder, status, "lz4 parallel context creation failed");
      goto cleanup;
    }
    status = lz4_parallel_alloc_job(state->parallel_ctx, &state->parallel_job);
    if (status != GCOMP_OK) {
      gcomp_encoder_set_error(
          encoder, status, "failed to allocate lz4 parallel job");
      goto cleanup;
    }
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

  // A reset starts a new frame, so the record of what the previous one had to
  // settle for does not carry into it.
  memset(&state->stepdowns, 0, sizeof(state->stepdowns));

  // Success
  encoder->method_state = state;
  return GCOMP_OK;

cleanup:
  // Single cleanup path for all error cases.  The job goes back before the
  // context that allocated it: its buffers are freed through that context.
  if (state) {
    if (state->parallel_job) {
      lz4_parallel_free_job(state->parallel_ctx, state->parallel_job);
    }
    if (state->parallel_ctx) {
      lz4_parallel_destroy(state->parallel_ctx);
    }
    gcomp_free(alloc, state->dictionary);
    gcomp_free(alloc, state->dict_hash_table);
    gcomp_free(alloc, state->hash_table);
    gcomp_free(alloc, state->compressed_buffer);
    gcomp_free(alloc, state->block_buffer);
    gcomp_free(alloc, state);
  }
  return status;
}

uint32_t gcomp_lz4_encoder_worker_count(const gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return 1;
  }
  const lz4_encoder_state_t * state =
      (const lz4_encoder_state_t *)encoder->method_state;
  return lz4_parallel_worker_count(state->parallel_ctx);
}

const gcomp_stepdown_tally_t * gcomp_lz4_encoder_stepdowns(
    const gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return NULL;
  }
  const lz4_encoder_state_t * state = (const lz4_encoder_state_t *)encoder->method_state;
  return &state->stepdowns;
}

void lz4_encoder_destroy(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return;
  }

  lz4_encoder_state_t * state = (lz4_encoder_state_t *)encoder->method_state;
  const gcomp_allocator_t * alloc = state->allocator;

  // Parallel teardown first: destroying the context waits for the workers, so
  // nothing is still reading a job's buffers by the time they are freed, and
  // the job in hand is freed through the context that allocated it.
  if (state->parallel_job) {
    lz4_parallel_free_job(state->parallel_ctx, state->parallel_job);
    state->parallel_job = NULL;
  }
  lz4_encoder_discard_parallel_jobs(state);
  if (state->parallel_ctx) {
    lz4_parallel_destroy(state->parallel_ctx);
    state->parallel_ctx = NULL;
  }
  if (state->dictionary) {
    gcomp_memory_track_free(&state->mem_tracker, state->dictionary_size);
    gcomp_free(alloc, state->dictionary);
  }

  if (state->dict_hash_table) {
    gcomp_memory_track_free(
        &state->mem_tracker, state->hash_table_size * sizeof(uint32_t));
    gcomp_free(alloc, state->dict_hash_table);
  }
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

/**
 * @brief Copy as much of the staged block as the output buffer will take.
 *
 * A compressed block is built whole in `compressed_buffer` and then handed
 * out across however many calls it takes, so every one of these hand-offs
 * is a straight copy of a run of bytes.  Moving them one at a time, with a
 * bounds test per byte, was 28% of LZ4 encoding.
 *
 * @return true once the staged block has been emitted in full.
 */
static bool lz4_emit_staged(
    lz4_encoder_state_t * state, gcomp_buffer_t * output) {
  size_t pending = state->compressed_buffer_len - state->compressed_buffer_pos;
  if (pending > 0u) {
    size_t space = output->size - output->used;
    size_t n = (pending < space) ? pending : space;
    if (n > 0u) {
      memcpy((uint8_t *)output->data + output->used,
          state->compressed_buffer + state->compressed_buffer_pos, n);
      output->used += n;
      state->compressed_buffer_pos += n;
    }
  }
  return state->compressed_buffer_pos >= state->compressed_buffer_len;
}

//
// Parallel mode
//
// Everything below runs only when state->parallel_ctx is set, which is to say
// when threads.count asked for workers and the frame's blocks are
// independent.  The shape follows the zstd encoder's parallel path, because
// the problem is the same one; what differs is that a job here is a block
// inside the frame rather than a frame of its own, so the header, the end
// mark and the content checksum are the serial path's and are untouched.

/**
 * @brief Hand out whatever of a collected block is still waiting.
 *
 * Only one block's tail is ever held back -- see
 * lz4_encoder_take_parallel_result() -- so this is the whole of the encoder's
 * staged parallel output.
 *
 * @return true once nothing is staged.
 */
/**
 * @brief Is a collected block still part-way through being handed out?
 *
 * There is room for exactly one, so this is also the answer to "may another
 * result be collected" -- collecting a second would write over the first.
 */
static bool lz4_encoder_has_staged_output(const lz4_encoder_state_t * state) {
  return state->compressed_buffer_len > 0;
}

static bool lz4_encoder_drain_parallel_output(
    lz4_encoder_state_t * state, gcomp_buffer_t * output) {
  if (state->compressed_buffer_len == 0) {
    return true;
  }
  if (!lz4_emit_staged(state, output)) {
    return false;
  }
  state->compressed_buffer_len = 0;
  state->compressed_buffer_pos = 0;
  return true;
}

/**
 * @brief Take one finished block: its bytes to the caller, its job back.
 *
 * The job's output buffer already holds the block exactly as the frame wants
 * it, so this copies and does not encode.  What will not fit in the caller's
 * buffer is held in compressed_buffer until there is room; because only one
 * block can be held that way, collecting stops as soon as it happens.
 *
 * The job is freed on every path, error included -- it is this function's to
 * dispose of once it has been handed one.
 *
 * @param buffered_out Receives whether a tail was held back.
 */
static gcomp_status_t lz4_encoder_take_parallel_result(
    lz4_encoder_state_t * state, gcomp_buffer_t * output,
    lz4_parallel_job_t * completed, bool * buffered_out) {
  *buffered_out = false;

  if (completed->base.status == GCOMP_JOB_ERROR ||
      completed->base.result != GCOMP_OK) {
    gcomp_status_t status = completed->base.result;
    lz4_parallel_free_job(state->parallel_ctx, completed);
    return status;
  }

  // What the worker settled for belongs in this encoder's tally, or a block
  // stored by a worker would be a step-down nothing counted -- which is the
  // failure mode src/core/stepdown.h exists to prevent.
  if (completed->stepped_down) {
    gcomp_stepdown_note(&state->stepdowns, completed->stepdown);
  }

  const uint8_t * data = completed->base.output;
  size_t len = completed->base.output_size;

  size_t direct = output->size - output->used;
  if (direct > len) {
    direct = len;
  }
  if (direct > 0) {
    memcpy((uint8_t *)output->data + output->used, data, direct);
    output->used += direct;
  }

  size_t remaining = len - direct;
  if (remaining > 0) {
    if (remaining > state->compressed_buffer_size) {
      // Cannot happen: the staging buffer is sized from the same block size
      // and the same overhead the job's output buffer is.  Refuse rather than
      // overrun if it ever does.
      lz4_parallel_free_job(state->parallel_ctx, completed);
      return GCOMP_ERR_INTERNAL;
    }
    memcpy(state->compressed_buffer, data + direct, remaining);
    state->compressed_buffer_pos = 0;
    state->compressed_buffer_len = remaining;
    *buffered_out = true;
  }

  lz4_parallel_free_job(state->parallel_ctx, completed);
  return GCOMP_OK;
}

/**
 * @brief Collect every block that is already finished, waiting for none.
 */
static gcomp_status_t lz4_encoder_collect_parallel_results(
    lz4_encoder_state_t * state, gcomp_buffer_t * output) {
  // Nothing may be collected while a block is still being handed out: the one
  // staging buffer would be overwritten and those bytes would simply vanish
  // from the frame.  The loop below breaks when it stages something, but that
  // is not enough on its own -- a caller can reach here with a block already
  // staged, because lz4_encoder_submit_parallel_job() collects one to make
  // room and then returns to a caller that collects again.  That is exactly
  // how it happened: two threads, a 1 KB output buffer and 2 MB of input
  // produced a 713,861-byte frame where the serial encoder produced 840,163,
  // and the frame still decoded -- to the wrong content.  See
  // MoreBlocksThanSlotsDoesNotStall.
  while (!lz4_encoder_has_staged_output(state) &&
      lz4_parallel_result_ready(state->parallel_ctx)) {
    lz4_parallel_job_t * completed = NULL;
    gcomp_status_t status =
        lz4_parallel_get_result(state->parallel_ctx, &completed);
    if (status != GCOMP_OK) {
      if (completed) {
        lz4_parallel_free_job(state->parallel_ctx, completed);
      }
      return status;
    }
    bool buffered = false;
    status =
        lz4_encoder_take_parallel_result(state, output, completed, &buffered);
    if (status != GCOMP_OK) {
      return status;
    }
    // Only one block's tail can be held back at a time, so once something is
    // staged there is nowhere to put the next result.
    if (buffered) {
      break;
    }
  }
  return GCOMP_OK;
}

/**
 * @brief Collect one block, waiting for it if it is not finished yet.
 *
 * Used when the context has no room for another job: the only thing that
 * frees a slot is taking a result back.
 */
static gcomp_status_t lz4_encoder_collect_one_parallel_result(
    lz4_encoder_state_t * state, gcomp_buffer_t * output) {
  lz4_parallel_job_t * completed = NULL;
  gcomp_status_t status =
      lz4_parallel_get_result(state->parallel_ctx, &completed);
  if (status != GCOMP_OK) {
    if (completed) {
      lz4_parallel_free_job(state->parallel_ctx, completed);
    }
    return status;
  }
  bool buffered = false;
  return lz4_encoder_take_parallel_result(state, output, completed, &buffered);
}

/**
 * @brief Hand the block being filled to the workers.
 *
 * WAITING FOR YOURSELF
 * ====================
 *
 * The context holds at most max_in_flight blocks, and the only thing that
 * frees a slot is collecting a result.  The thread that collects is this one.
 * So when the context is full the thing to do is collect, never wait: there
 * is nobody else to wait for.  The zstd encoder learned this the hard way --
 * see the note of the same name in zstd_encoder.c, where waiting one level
 * down deadlocked every stream longer than max_in_flight jobs.
 *
 * Collecting needs somewhere to put the result, and only one block's tail can
 * be held back.  If the caller's buffer is full and a tail is already staged
 * there is nothing useful to do here: @p submitted_out is set false and the
 * job stays filled and unsubmitted, to be offered again once the caller has
 * drained.
 *
 * @param submitted_out Receives whether the job was handed over.
 */
static gcomp_status_t lz4_encoder_submit_parallel_job(
    lz4_encoder_state_t * state, gcomp_buffer_t * output,
    bool * submitted_out) {
  *submitted_out = false;
  if (!state->parallel_job) {
    return GCOMP_ERR_INTERNAL;
  }

  for (;;) {
    gcomp_status_t status =
        lz4_parallel_try_submit(state->parallel_ctx, state->parallel_job);
    if (status == GCOMP_OK) {
      break;
    }
    if (status != GCOMP_ERR_LIMIT) {
      return status;
    }
    if (lz4_encoder_has_staged_output(state)) {
      return GCOMP_OK; // Nowhere to put a result; the caller must drain.
    }
    status = lz4_encoder_collect_one_parallel_result(state, output);
    if (status != GCOMP_OK) {
      return status;
    }
  }

  state->parallel_job = NULL; // The context owns it now.
  *submitted_out = true;
  return GCOMP_OK;
}

/**
 * @brief Throw away every block still in flight, freeing its job.
 *
 * For destroy and reset, where the frame is being abandoned rather than
 * finished.  Waiting first is what makes it safe to free the buffers: a
 * worker may still be writing into one.  Without this, tearing an encoder
 * down mid-frame would leak every job the workers had not yet handed back.
 */
static void lz4_encoder_discard_parallel_jobs(lz4_encoder_state_t * state) {
  if (!state->parallel_ctx) {
    return;
  }
  lz4_parallel_wait(state->parallel_ctx);
  while (lz4_parallel_pending_count(state->parallel_ctx) > 0) {
    lz4_parallel_job_t * completed = NULL;
    lz4_parallel_get_result(state->parallel_ctx, &completed);
    if (!completed) {
      break;
    }
    lz4_parallel_free_job(state->parallel_ctx, completed);
  }
}

/**
 * @brief Fill and submit blocks from the caller's input.
 *
 * The content checksum is updated here, on this thread, as bytes are copied
 * into a job.  It covers the uncompressed content in order, and jobs are
 * filled in order, so the running hash is the same one the serial path
 * computes -- which it has to be, because it goes in the frame trailer and a
 * decoder will check it.
 */
static gcomp_status_t lz4_encoder_update_parallel(gcomp_encoder_t * encoder,
    lz4_encoder_state_t * state, gcomp_buffer_t * input,
    gcomp_buffer_t * output) {
  gcomp_status_t status;

  if (!lz4_encoder_drain_parallel_output(state, output)) {
    return GCOMP_OK; // Need more output space.
  }

  status = lz4_encoder_collect_parallel_results(state, output);
  if (status != GCOMP_OK) {
    return gcomp_encoder_set_error(
        encoder, status, "lz4 parallel compression failed");
  }

  if (!lz4_encoder_drain_parallel_output(state, output)) {
    return GCOMP_OK;
  }

  const size_t block_size = state->block_buffer_size;

  while (input->used < input->size) {
    if (!state->parallel_job) {
      return gcomp_encoder_set_error(
          encoder, GCOMP_ERR_INTERNAL, "lz4 parallel job missing");
    }

    size_t job_space = block_size - state->parallel_job->base.input_size;
    size_t available = input->size - input->used;
    size_t to_copy = (available < job_space) ? available : job_space;

    if (to_copy > 0) {
      const uint8_t * in_ptr = (const uint8_t *)input->data + input->used;
      // Cast away const: the window is the job's own and is mutable while it
      // is being filled.  It is const in the job because a worker only reads
      // it, and by then this encoder has let go of it.
      uint8_t * job_input = (uint8_t *)state->parallel_job->base.input;
      memcpy(job_input + state->parallel_job->base.input_size, in_ptr,
          to_copy);
      state->parallel_job->base.input_size += to_copy;
      input->used += to_copy;
      state->total_input_bytes += to_copy;

      if (state->header.content_checksum) {
        gcomp_xxhash32_update(&state->content_hash, in_ptr, to_copy);
      }
    }

    if (state->parallel_job->base.input_size < block_size) {
      continue; // Block not full yet; more input will finish it.
    }

    bool submitted = false;
    status = lz4_encoder_submit_parallel_job(state, output, &submitted);
    if (status != GCOMP_OK) {
      return gcomp_encoder_set_error(
          encoder, status, "lz4 parallel job submit failed");
    }
    if (!submitted) {
      // The workers are full and their results have nowhere to go.  The block
      // is still here, still full; it goes next time, once the caller has
      // taken what is already staged.
      return GCOMP_OK;
    }

    status = lz4_parallel_alloc_job(state->parallel_ctx, &state->parallel_job);
    if (status != GCOMP_OK) {
      return gcomp_encoder_set_error(
          encoder, status, "failed to allocate lz4 parallel job");
    }

    status = lz4_encoder_collect_parallel_results(state, output);
    if (status != GCOMP_OK) {
      return gcomp_encoder_set_error(
          encoder, status, "lz4 parallel compression failed");
    }
    if (!lz4_encoder_drain_parallel_output(state, output)) {
      return GCOMP_OK;
    }
  }

  return GCOMP_OK;
}

/**
 * @brief Get the last block out and every earlier one collected.
 *
 * Leaves the encoder at the end mark with blocks_finished set, which is where
 * the serial path leaves it too, so the rest of finish() is shared.
 *
 * @return GCOMP_OK when every block is out, GCOMP_ERR_LIMIT when the caller
 *         must make room and call finish() again.
 */
static gcomp_status_t lz4_encoder_finish_parallel_blocks(
    gcomp_encoder_t * encoder, lz4_encoder_state_t * state,
    gcomp_buffer_t * output) {
  gcomp_status_t status;

  if (!lz4_encoder_drain_parallel_output(state, output)) {
    return GCOMP_ERR_LIMIT;
  }

  if (state->parallel_job) {
    if (state->parallel_job->base.input_size > 0) {
      bool submitted = false;
      status = lz4_encoder_submit_parallel_job(state, output, &submitted);
      if (status != GCOMP_OK) {
        return gcomp_encoder_set_error(
            encoder, status, "lz4 parallel job submit failed");
      }
      if (!submitted) {
        // finish() reports completion with GCOMP_OK, and the last block has
        // not even been handed over, so this must not.
        return GCOMP_ERR_LIMIT;
      }
    }
    else {
      // A frame whose length is a multiple of the block size ends with an
      // empty job in hand.  An empty block is not written: the LZ4 Frame
      // Format's end mark is a zero block size, so one would end the frame
      // early.
      lz4_parallel_free_job(state->parallel_ctx, state->parallel_job);
      state->parallel_job = NULL;
    }
  }

  status = lz4_parallel_wait(state->parallel_ctx);
  if (status != GCOMP_OK) {
    return gcomp_encoder_set_error(encoder, status, "lz4 parallel wait failed");
  }

  status = lz4_encoder_collect_parallel_results(state, output);
  if (status != GCOMP_OK) {
    return gcomp_encoder_set_error(
        encoder, status, "lz4 parallel compression failed");
  }

  if (!lz4_encoder_drain_parallel_output(state, output)) {
    return GCOMP_ERR_LIMIT;
  }

  if (lz4_parallel_pending_count(state->parallel_ctx) > 0) {
    // Blocks remain, so the frame is not complete; another call will take
    // them once there is room.
    return GCOMP_ERR_LIMIT;
  }

  return GCOMP_OK;
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

  // Parallel mode fills and submits blocks instead of compressing them here.
  // Only the block loop differs: the header above and the end mark and
  // trailer in finish() are the same bytes in the same places.
  if (state->stage == LZ4_ENC_STAGE_BLOCKS && state->parallel_ctx) {
    return lz4_encoder_update_parallel(encoder, state, input, output);
  }

  // Process input data
  if (state->stage == LZ4_ENC_STAGE_BLOCKS) {
    // If we have a partially-emitted block from a previous call, flush it
    if (state->compressed_buffer_len > 0) {
      lz4_emit_staged(state, output);
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
          // GCOMP_ERR_LIMIT here is not a failure: the block compressor is
          // given a destination the size of the block, so running out of room
          // in it *is* the discovery that the compressed form is larger than
          // the raw one.  A bigger buffer would not produce a better block.
          gcomp_stepdown_note(&state->stepdowns,
              (status == GCOMP_OK || status == GCOMP_ERR_LIMIT)
                  ? GCOMP_STEPDOWN_STORED_IS_SMALLER
                  : GCOMP_STEPDOWN_ENCODE_FAILED);
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
        lz4_emit_staged(state, output);

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
    if (state->parallel_ctx) {
      // Get the last block submitted and every earlier one collected.  This
      // is the only part of finish() that differs; what follows is shared.
      gcomp_status_t parallel_status =
          lz4_encoder_finish_parallel_blocks(encoder, state, output);
      if (parallel_status != GCOMP_OK) {
        return parallel_status;
      }
    }
    // Check if we have compressed data still being output
    else if (state->compressed_buffer_len > 0) {
      // Continue outputting previously compressed block
      lz4_emit_staged(state, output);
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
        // See the note on the other call: a full destination means the
        // compressed form is larger, not that anything went wrong.
        gcomp_stepdown_note(&state->stepdowns,
            (status == GCOMP_OK || status == GCOMP_ERR_LIMIT)
                ? GCOMP_STEPDOWN_STORED_IS_SMALLER
                : GCOMP_STEPDOWN_ENCODE_FAILED);
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
      lz4_emit_staged(state, output);

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

/**
 * @brief Compress whatever is in the block buffer and stage it as a block.
 *
 * The same work lz4_encoder_update() does when a block fills up and
 * lz4_encoder_finish() does for the tail, for a block that is short of full.
 * Kept here rather than shared with those two because each of them threads it
 * through a different surrounding state machine; what matters is that the
 * three agree on the rules, and they do: the same compressor, the same
 * destination capacity, the same stored-versus-compressed test, the same
 * checksum.
 */
static void lz4_encoder_stage_partial_block(lz4_encoder_state_t * state) {
  size_t compressed_len;
  gcomp_status_t status = lz4_block_compress_linked(state->block_buffer,
      state->prefix_len, state->block_buffer_pos, state->compressed_buffer + 4,
      state->compressed_buffer_size - 4, &compressed_len, state->hash_table,
      state->hash_table_size);

  if (status != GCOMP_OK || compressed_len >= state->block_buffer_pos) {
    // See the note in lz4_encoder_update(): a full destination here is the
    // discovery that the compressed form is larger, not a failure.
    gcomp_stepdown_note(&state->stepdowns,
        (status == GCOMP_OK || status == GCOMP_ERR_LIMIT)
            ? GCOMP_STEPDOWN_STORED_IS_SMALLER
            : GCOMP_STEPDOWN_ENCODE_FAILED);
    uint32_t block_size =
        (uint32_t)state->block_buffer_pos | LZ4_BLOCK_UNCOMPRESSED_FLAG;
    gcomp_write_le32(state->compressed_buffer, block_size);
    memcpy(state->compressed_buffer + 4, state->block_buffer + state->prefix_len,
        state->block_buffer_pos);
    compressed_len = state->block_buffer_pos;
  }
  else {
    gcomp_write_le32(state->compressed_buffer, (uint32_t)compressed_len);
  }

  size_t total_block_len = 4 + compressed_len;
  if (state->header.block_checksum) {
    uint32_t checksum =
        gcomp_xxhash32(state->compressed_buffer + 4, compressed_len, 0);
    gcomp_write_le32(state->compressed_buffer + total_block_len, checksum);
    total_block_len += 4;
  }

  state->compressed_buffer_len = total_block_len;
  state->compressed_buffer_pos = 0;

  // Retire the block exactly as the streaming path does: keep what the next
  // one may match into, drop the rest, move the table with it.
  lz4_encoder_slide_window(state);
}

gcomp_status_t lz4_encoder_flush(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output, gcomp_flush_t mode) {
  if (!encoder || !encoder->method_state || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output->size > 0 && !output->data) {
    return GCOMP_ERR_INVALID_ARG;
  }

  lz4_encoder_state_t * state = (lz4_encoder_state_t *)encoder->method_state;

  if (state->stage == LZ4_ENC_STAGE_ERROR) {
    return GCOMP_ERR_INTERNAL;
  }
  if (state->finish_called) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "lz4 encoder cannot flush after finish");
  }

  // The frame header comes before any block, so a flush before the first byte
  // of input still has this much to hand over.
  if (state->stage == LZ4_ENC_STAGE_HEADER) {
    while (
        state->header_pos < state->header_len && output->used < output->size) {
      ((uint8_t *)output->data)[output->used++] =
          state->header_buf[state->header_pos++];
    }
    if (state->header_pos < state->header_len) {
      return GCOMP_ERR_LIMIT;
    }
    state->stage = LZ4_ENC_STAGE_BLOCKS;
  }

  if (state->parallel_ctx) {
    if (!lz4_encoder_drain_parallel_output(state, output)) {
      return GCOMP_ERR_LIMIT;
    }

    // Hand over the block being filled, then wait for every block still out
    // with the workers.  A flush has to wait: its whole point is that nothing
    // consumed is left in flight.
    if (state->parallel_job && state->parallel_job->base.input_size > 0) {
      bool submitted = false;
      gcomp_status_t status =
          lz4_encoder_submit_parallel_job(state, output, &submitted);
      if (status != GCOMP_OK) {
        return gcomp_encoder_set_error(
            encoder, status, "lz4 parallel job submit failed");
      }
      if (!submitted) {
        return GCOMP_ERR_LIMIT; // Output full with a result staged; drain.
      }
      status = lz4_parallel_alloc_job(state->parallel_ctx, &state->parallel_job);
      if (status != GCOMP_OK) {
        return gcomp_encoder_set_error(
            encoder, status, "failed to allocate lz4 parallel job");
      }
    }

    gcomp_status_t status = lz4_parallel_wait(state->parallel_ctx);
    if (status != GCOMP_OK) {
      return gcomp_encoder_set_error(
          encoder, status, "lz4 parallel wait failed");
    }
    status = lz4_encoder_collect_parallel_results(state, output);
    if (status != GCOMP_OK) {
      return gcomp_encoder_set_error(
          encoder, status, "lz4 parallel compression failed");
    }
    if (!lz4_encoder_drain_parallel_output(state, output)) {
      return GCOMP_ERR_LIMIT;
    }
    if (lz4_parallel_pending_count(state->parallel_ctx) > 0) {
      return GCOMP_ERR_LIMIT; // Blocks remain; call again once drained.
    }

    // Parallel encoding requires independent blocks, so there is no history
    // for GCOMP_FLUSH_FULL to drop -- every block already starts from the
    // dictionary alone.
    return GCOMP_OK;
  }

  // A block staged by an earlier call -- by update(), or by a flush that ran
  // out of output -- goes first.  block_buffer_pos was zeroed when it was
  // staged, so the branch below will not stage it a second time.
  if (state->compressed_buffer_len > 0) {
    if (!lz4_emit_staged(state, output)) {
      return GCOMP_ERR_LIMIT;
    }
    state->compressed_buffer_len = 0;
    state->compressed_buffer_pos = 0;
  }
  else if (state->block_buffer_pos > 0) {
    lz4_encoder_stage_partial_block(state);
    if (!lz4_emit_staged(state, output)) {
      return GCOMP_ERR_LIMIT;
    }
    state->compressed_buffer_len = 0;
    state->compressed_buffer_pos = 0;
  }

  if (mode == GCOMP_FLUSH_FULL && !state->header.block_independence) {
    // Linked blocks are the only case with history to drop.  Independent
    // blocks retired theirs when the block was staged, above.
    //
    // This resets to an empty window rather than back to the dictionary:
    // with linked blocks the window slides and the frame's own output
    // displaces the dictionary as it goes, so by now there is generally
    // nothing of it left to restore.  Subsequent blocks start from nothing,
    // which is what "nothing after this refers to anything before it" means.
    state->prefix_len = 0;
    state->block_buffer_pos = 0;
    memset(state->hash_table, 0, state->hash_table_size * sizeof(uint32_t));
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
  state->compressed_buffer_pos = 0;
  state->compressed_buffer_len = 0;
  state->end_mark_pos = 0;
  state->trailer_pos = 0;
  state->total_input_bytes = 0;
  state->finish_called = false;
  state->blocks_finished = false;

  if (state->parallel_ctx) {
    // Abandon whatever the workers still hold: reset starts a new frame, and
    // blocks of the old one have nowhere to go.  They have to be drained
    // before the context can be reset, and waited for before their buffers
    // can be freed.
    if (state->parallel_job) {
      lz4_parallel_free_job(state->parallel_ctx, state->parallel_job);
      state->parallel_job = NULL;
    }
    lz4_encoder_discard_parallel_jobs(state);

    gcomp_status_t status = lz4_parallel_reset(state->parallel_ctx);
    if (status != GCOMP_OK) {
      return status;
    }
    status = lz4_parallel_alloc_job(state->parallel_ctx, &state->parallel_job);
    if (status != GCOMP_OK) {
      return status;
    }
  }
  else {
    // Clear the hash table and put the window back where a new frame starts:
    // at the dictionary, or at nothing.  The two must move together -- an
    // entry and the byte it names are only meaningful as a pair.
    lz4_encoder_seed_window(state);
  }

  // Reset content checksum
  if (state->header.content_checksum) {
    gcomp_xxhash32_reset(&state->content_hash, 0);
  }

  return GCOMP_OK;
}
