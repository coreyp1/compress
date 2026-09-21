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
 * @file lz4_parallel.c
 *
 * LZ4-specific parallel block compression implementation.
 *
 * The interface, and the reasoning behind producing one frame rather than
 * many, are in lz4_parallel.h.  This file is the machinery: a context that
 * owns the pool and the ordering queue, jobs that own the buffers a worker
 * needs, and one worker function.
 *
 * ## The rule this file exists to preserve
 *
 * A worker must produce, for its block, exactly the bytes the serial encoder
 * would have produced for that block.  Everything below follows from that:
 *
 *   - The window is `[dictionary][block]` and the compressor is told the
 *     prefix length, because that is what lz4_encoder_update() passes.
 *   - The hash table starts as the dictionary left it -- copied, not rebuilt
 *     -- because that is what lz4_encoder_seed_window() restores.
 *   - The table has the same number of entries as the serial encoder's.  A
 *     different size is a different hash, and a different hash is a different
 *     set of matches: still valid LZ4, but not the same bytes.
 *   - The destination capacity handed to the block compressor is computed the
 *     same way, because running out of room in it is how a block is decided to
 *     be stored rather than compressed.
 *   - The stored-versus-compressed test is the same comparison.
 *
 * Break any one of them and the library still round-trips; it just quietly
 * stops being one encoder.  TheSameBytesWhicheverThreadCountProducedThem is
 * the test that notices.
 *
 * ## Layers
 *
 * ```
 * ┌─────────────────────────────────────────────────────────────┐
 * │                    lz4_parallel_ctx_t                       │
 * │      (block framing rules + per-job buffer management)      │
 * └───────────────────────────┬─────────────────────────────────┘
 *                             │
 *                             v
 * ┌─────────────────────────────────────────────────────────────┐
 * │                gcomp_parallel_block_ctx_t                   │
 * │   (inline vs threaded, thread pool, in-order job queue)     │
 * └─────────────────────────────────────────────────────────────┘
 * ```
 *
 * Ordering, the inline/threaded split and the in-flight bound all belong to
 * the generic helper, which zstd uses too; what is LZ4-specific is only what
 * a block is and how one is framed.
 *
 * ## Thread safety
 *
 * `create()`, `destroy()`, `alloc_job()` and `free_job()` are called from the
 * encoder's thread only.  `submit()` and `get_result()` are safe against the
 * workers because the job queue is; they are not meant to be called from two
 * threads at once, and the encoder never does.
 *
 * A worker touches only its own job: its window, its table, its output
 * buffer.  The context is read for framing rules that do not change after
 * create().  Nothing is written through it.
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/compress/macros.h>
#include "lz4_parallel.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lz4_internal.h"

#include "../../core/alloc_internal.h"
#include "../../core/parallel_block.h"

//
// Constants
//

/**
 * @brief In-flight blocks per worker when the caller does not say.
 *
 * One block per worker would leave a worker idle for as long as it takes the
 * encoder to refill the job it just handed back; two gives it something to
 * start on.  More than that buys little and costs a block buffer each.
 */
#define LZ4_PARALLEL_DEFAULT_MAX_IN_FLIGHT_MULTIPLIER 2

/// Table size used when the caller does not name one.
#define LZ4_PARALLEL_DEFAULT_HASH_TABLE_SIZE (1U << 16)

//
// Internal Types
//

struct lz4_parallel_ctx_s {
  const gcomp_allocator_t * allocator;
  gcomp_parallel_block_ctx_t * block_ctx;
  gcomp_memory_tracker_t * mem_tracker;

  uint32_t block_max_size;
  bool block_checksum;
  uint32_t num_threads;
  uint32_t max_in_flight;

  /// Dictionary bytes, owned here and copied into every job's window.
  uint8_t * dictionary;
  size_t dictionary_size;
  /// The table those bytes produce, owned here and copied into every job.
  uint32_t * dict_hash_table;
  size_t hash_table_size;

  uint64_t max_memory_bytes;
};

//
// Memory tracking helpers
//
// The tracker belongs to the encoder and is only ever touched from its
// thread: jobs are allocated and freed there, never in a worker.

static void lz4_parallel_track_alloc(lz4_parallel_ctx_t * ctx, size_t size) {
  if (ctx->mem_tracker) {
    gcomp_memory_track_alloc(ctx->mem_tracker, size);
  }
}

static void lz4_parallel_track_free(lz4_parallel_ctx_t * ctx, size_t size) {
  if (ctx->mem_tracker) {
    gcomp_memory_track_free(ctx->mem_tracker, size);
  }
}

/// Bytes one job holds, for the in-flight budget and for the tracker.
static size_t lz4_parallel_job_bytes(const lz4_parallel_ctx_t * ctx) {
  return sizeof(lz4_parallel_job_t) + ctx->dictionary_size +
      ctx->block_max_size + ctx->block_max_size +
      LZ4_PARALLEL_BLOCK_OVERHEAD + ctx->hash_table_size * sizeof(uint32_t);
}

//
// Job Processing Function
//

/**
 * @brief Compress one block and frame it, exactly as the serial encoder does.
 *
 * Runs on a worker thread.  See the file comment for why every step here is a
 * deliberate copy of lz4_encoder_update()'s block path rather than a tidier
 * equivalent.
 */
static int lz4_parallel_process_job(void * job_ctx) {
  lz4_parallel_job_t * job = (lz4_parallel_job_t *)job_ctx;
  lz4_parallel_ctx_t * ctx = job->ctx;

  const size_t block_len = job->base.input_size;
  const size_t checksum_len =
      ctx->block_checksum ? (size_t)LZ4_BLOCK_CHECKSUM_SIZE : 0u;
  uint8_t * out = job->base.output;

  if (job->base.output_capacity <= (size_t)LZ4_BLOCK_HEADER_SIZE) {
    return GCOMP_ERR_LIMIT;
  }

  // The same destination the serial encoder gives the block compressor:
  // everything after the 4-byte size field.  Its capacity is what decides
  // whether a block ends up stored, so it is not a free choice.
  size_t body_cap = job->base.output_capacity - LZ4_BLOCK_HEADER_SIZE;
  size_t compressed_len = 0;
  gcomp_status_t status = lz4_block_compress_linked(job->window,
      ctx->dictionary_size, block_len, out + LZ4_BLOCK_HEADER_SIZE, body_cap,
      &compressed_len, job->hash_table, job->hash_table_size);

  job->stepped_down = false;
  if (status != GCOMP_OK || compressed_len >= block_len) {
    // Compression did not pay, so store the block.  GCOMP_ERR_LIMIT here is
    // not a failure -- see the same note in lz4_encoder_update(): the block
    // compressor was given a destination the size of the block, so filling it
    // *is* the discovery that the compressed form is larger.
    job->stepped_down = true;
    job->stepdown = (status == GCOMP_OK || status == GCOMP_ERR_LIMIT)
        ? GCOMP_STEPDOWN_STORED_IS_SMALLER
        : GCOMP_STEPDOWN_ENCODE_FAILED;

    if (job->base.output_capacity <
        (size_t)LZ4_BLOCK_HEADER_SIZE + block_len + checksum_len) {
      // Nowhere to put even the raw block.  Unlike the case above this one is
      // a real failure: there is no weaker encoding left to fall back to.
      return GCOMP_ERR_LIMIT;
    }
    gcomp_write_le32(
        out, (uint32_t)block_len | LZ4_BLOCK_UNCOMPRESSED_FLAG);
    memcpy(out + LZ4_BLOCK_HEADER_SIZE, job->base.input, block_len);
    compressed_len = block_len;
    job->store_uncompressed = true;
  }
  else {
    gcomp_write_le32(out, (uint32_t)compressed_len);
    job->store_uncompressed = false;
  }

  size_t framed_len = (size_t)LZ4_BLOCK_HEADER_SIZE + compressed_len;
  if (checksum_len > 0) {
    if (job->base.output_capacity < framed_len + checksum_len) {
      return GCOMP_ERR_LIMIT;
    }
    // The checksum covers the block as written, compressed or stored: LZ4
    // Frame Format, "Block checksum".  Computing it here rather than when the
    // encoder collects the result keeps it on the worker, where it is free.
    job->block_checksum =
        gcomp_xxhash32(out + LZ4_BLOCK_HEADER_SIZE, compressed_len, 0);
    gcomp_write_le32(out + framed_len, job->block_checksum);
    framed_len += checksum_len;
  }

  job->base.output_size = framed_len;
  return GCOMP_OK;
}

//
// Public API
//

gcomp_status_t lz4_parallel_create(
    const lz4_parallel_config_t * config, lz4_parallel_ctx_t ** ctx_out) {
  if (!ctx_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *ctx_out = NULL;

  const gcomp_allocator_t * allocator = config && config->allocator
      ? config->allocator
      : gcomp_allocator_default();

  uint32_t num_threads = config ? config->num_threads : 0;
  uint32_t block_max_size =
      config ? config->block_max_size : LZ4_DEFAULT_BLOCK_SIZE;
  if (block_max_size == 0) {
    block_max_size = LZ4_DEFAULT_BLOCK_SIZE;
  }

  lz4_parallel_ctx_t * ctx =
      gcomp_calloc(allocator, 1, sizeof(lz4_parallel_ctx_t));
  if (!ctx) {
    return GCOMP_ERR_MEMORY;
  }

  ctx->allocator = allocator;
  ctx->mem_tracker = config ? config->mem_tracker : NULL;
  ctx->block_max_size = block_max_size;
  ctx->block_checksum = config ? config->block_checksum : false;
  ctx->max_memory_bytes = config ? config->max_memory_bytes : 0;
  ctx->hash_table_size = config && config->hash_table_size > 0
      ? config->hash_table_size
      : LZ4_PARALLEL_DEFAULT_HASH_TABLE_SIZE;
  ctx->num_threads = num_threads > 1 ? num_threads : 1;

  lz4_parallel_track_alloc(ctx, sizeof(lz4_parallel_ctx_t));

  // Take a copy of the dictionary and of the table it produces.  The caller's
  // copies belong to the encoder, which may be reset or destroyed while jobs
  // are still in flight; a worker reading through a dangling pointer is not a
  // failure mode worth saving two allocations for.
  if (config && config->dictionary && config->dictionary_size > 0) {
    ctx->dictionary_size = config->dictionary_size;
    ctx->dictionary = gcomp_malloc(allocator, ctx->dictionary_size);
    if (!ctx->dictionary) {
      lz4_parallel_track_free(ctx, sizeof(lz4_parallel_ctx_t));
      gcomp_free(allocator, ctx);
      return GCOMP_ERR_MEMORY;
    }
    memcpy(ctx->dictionary, config->dictionary, ctx->dictionary_size);
    lz4_parallel_track_alloc(ctx, ctx->dictionary_size);

    size_t table_bytes = ctx->hash_table_size * sizeof(uint32_t);
    ctx->dict_hash_table = gcomp_malloc(allocator, table_bytes);
    if (!ctx->dict_hash_table) {
      lz4_parallel_track_free(ctx, ctx->dictionary_size);
      lz4_parallel_track_free(ctx, sizeof(lz4_parallel_ctx_t));
      gcomp_free(allocator, ctx->dictionary);
      gcomp_free(allocator, ctx);
      return GCOMP_ERR_MEMORY;
    }
    if (config->dict_hash_table) {
      memcpy(ctx->dict_hash_table, config->dict_hash_table, table_bytes);
    }
    else {
      memset(ctx->dict_hash_table, 0, table_bytes);
      lz4_block_index_window(ctx->dictionary, ctx->dictionary_size,
          ctx->dict_hash_table, ctx->hash_table_size);
    }
    lz4_parallel_track_alloc(ctx, table_bytes);
  }

  uint32_t max_in_flight = 1;
  if (num_threads > 1) {
    max_in_flight = config && config->max_in_flight > 0
        ? config->max_in_flight
        : num_threads * LZ4_PARALLEL_DEFAULT_MAX_IN_FLIGHT_MULTIPLIER;
    if (ctx->max_memory_bytes > 0) {
      // Fit the in-flight blocks into what is left of the budget rather than
      // overrunning it.  One is the floor: below that there is no stream.
      size_t per_job_memory = lz4_parallel_job_bytes(ctx);
      uint64_t already_used =
          ctx->mem_tracker ? ctx->mem_tracker->current_bytes : 0;
      uint64_t budget = ctx->max_memory_bytes > already_used
          ? ctx->max_memory_bytes - already_used
          : 0;
      uint32_t mem_limited_jobs = (uint32_t)(budget / per_job_memory);
      if (mem_limited_jobs < max_in_flight) {
        max_in_flight = mem_limited_jobs;
      }
      if (max_in_flight < 1) {
        max_in_flight = 1;
      }
    }
  }
  ctx->max_in_flight = max_in_flight;

  gcomp_parallel_block_config_t block_config = {
      .allocator = allocator,
      .num_threads = num_threads,
      .max_in_flight = max_in_flight,
  };
  gcomp_status_t status =
      gcomp_parallel_block_create(&block_config, &ctx->block_ctx);
  if (status != GCOMP_OK) {
    lz4_parallel_destroy(ctx);
    return status;
  }

  *ctx_out = ctx;
  return GCOMP_OK;
}

void lz4_parallel_destroy(lz4_parallel_ctx_t * ctx) {
  if (!ctx) {
    return;
  }
  gcomp_parallel_block_destroy(ctx->block_ctx);
  if (ctx->dict_hash_table) {
    lz4_parallel_track_free(ctx, ctx->hash_table_size * sizeof(uint32_t));
    gcomp_free(ctx->allocator, ctx->dict_hash_table);
  }
  if (ctx->dictionary) {
    lz4_parallel_track_free(ctx, ctx->dictionary_size);
    gcomp_free(ctx->allocator, ctx->dictionary);
  }
  lz4_parallel_track_free(ctx, sizeof(lz4_parallel_ctx_t));
  gcomp_free(ctx->allocator, ctx);
}

gcomp_status_t lz4_parallel_alloc_job(
    lz4_parallel_ctx_t * ctx, lz4_parallel_job_t ** job_out) {
  if (!ctx || !job_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *job_out = NULL;

  const size_t window_bytes = ctx->dictionary_size + ctx->block_max_size;
  const size_t output_cap = ctx->block_max_size + LZ4_PARALLEL_BLOCK_OVERHEAD;
  const size_t table_bytes = ctx->hash_table_size * sizeof(uint32_t);

  lz4_parallel_job_t * job =
      gcomp_calloc(ctx->allocator, 1, sizeof(lz4_parallel_job_t));
  if (!job) {
    return GCOMP_ERR_MEMORY;
  }

  uint8_t * window = gcomp_malloc(ctx->allocator, window_bytes);
  uint8_t * output_buf = window ? gcomp_malloc(ctx->allocator, output_cap) : NULL;
  uint32_t * hash_table =
      output_buf ? gcomp_malloc(ctx->allocator, table_bytes) : NULL;
  if (!hash_table) {
    gcomp_free(ctx->allocator, output_buf);
    gcomp_free(ctx->allocator, window);
    gcomp_free(ctx->allocator, job);
    return GCOMP_ERR_MEMORY;
  }

  // Start the job where lz4_encoder_seed_window() starts a block: the
  // dictionary at the front of the window, and the table it leaves behind.
  if (ctx->dictionary_size > 0) {
    memcpy(window, ctx->dictionary, ctx->dictionary_size);
    memcpy(hash_table, ctx->dict_hash_table, table_bytes);
  }
  else {
    memset(hash_table, 0, table_bytes);
  }

  job->ctx = ctx;
  job->window = window;
  job->base.input = window + ctx->dictionary_size;
  job->base.input_size = 0;
  job->base.output = output_buf;
  job->base.output_capacity = output_cap;
  job->base.output_size = 0;
  job->hash_table = hash_table;
  job->hash_table_size = ctx->hash_table_size;
  job->store_uncompressed = false;
  job->block_checksum = 0;
  job->stepped_down = false;

  lz4_parallel_track_alloc(ctx, lz4_parallel_job_bytes(ctx));

  *job_out = job;
  return GCOMP_OK;
}

void lz4_parallel_free_job(lz4_parallel_ctx_t * ctx, lz4_parallel_job_t * job) {
  if (!ctx || !job) {
    return;
  }

  gcomp_free(ctx->allocator, job->window);
  gcomp_free(ctx->allocator, job->base.output);
  gcomp_free(ctx->allocator, job->hash_table);
  gcomp_free(ctx->allocator, job);
  lz4_parallel_track_free(ctx, lz4_parallel_job_bytes(ctx));
}

gcomp_status_t lz4_parallel_submit(
    lz4_parallel_ctx_t * ctx, lz4_parallel_job_t * job) {
  if (!ctx || !job) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_parallel_block_submit(
      ctx->block_ctx, job, lz4_parallel_process_job);
}

gcomp_status_t lz4_parallel_try_submit(
    lz4_parallel_ctx_t * ctx, lz4_parallel_job_t * job) {
  if (!ctx || !job) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_parallel_block_try_submit(
      ctx->block_ctx, job, lz4_parallel_process_job);
}

gcomp_status_t lz4_parallel_get_result(
    lz4_parallel_ctx_t * ctx, lz4_parallel_job_t ** job_out) {
  if (!ctx || !job_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  *job_out = NULL;
  gcomp_block_job_t * base = NULL;
  gcomp_status_t status =
      gcomp_parallel_block_get_result(ctx->block_ctx, &base);
  if (!base) {
    return status;
  }
  *job_out = (lz4_parallel_job_t *)base;
  return base->result;
}

bool lz4_parallel_result_ready(const lz4_parallel_ctx_t * ctx) {
  return ctx && gcomp_parallel_block_result_ready(ctx->block_ctx);
}

gcomp_status_t lz4_parallel_wait(lz4_parallel_ctx_t * ctx) {
  if (!ctx) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_parallel_block_wait(ctx->block_ctx);
}

bool lz4_parallel_is_inline(const lz4_parallel_ctx_t * ctx) {
  return !ctx || gcomp_parallel_block_is_inline(ctx->block_ctx);
}

uint32_t lz4_parallel_worker_count(const lz4_parallel_ctx_t * ctx) {
  if (!ctx || gcomp_parallel_block_is_inline(ctx->block_ctx)) {
    return 1;
  }
  return ctx->num_threads;
}

uint32_t lz4_parallel_max_in_flight(const lz4_parallel_ctx_t * ctx) {
  return ctx ? ctx->max_in_flight : 0;
}

uint32_t lz4_parallel_pending_count(const lz4_parallel_ctx_t * ctx) {
  return ctx ? gcomp_parallel_block_pending_count(ctx->block_ctx) : 0;
}

gcomp_status_t lz4_parallel_reset(lz4_parallel_ctx_t * ctx) {
  if (!ctx) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_parallel_block_reset(ctx->block_ctx);
}
