/**
 * @file lz4_parallel.c
 *
 * LZ4-specific parallel block compression implementation.
 *
 * This module provides parallel block compression for LZ4 by integrating
 * the generic thread pool and job queue infrastructure with LZ4-specific
 * compression logic.
 *
 * ## Architecture
 *
 * The parallel compression system has three layers:
 *
 * ```
 * ┌─────────────────────────────────────────────────────────────┐
 * │                    lz4_parallel_ctx_t                       │
 * │         (LZ4-specific parallel compression context)         │
 * └───────────────────────────┬─────────────────────────────────┘
 *                             │
 *           ┌─────────────────┼─────────────────┐
 *           │                 │                 │
 *           v                 v                 v
 * ┌─────────────────┐ ┌───────────────┐ ┌────────────────────┐
 * │ gcomp_thread_   │ │ gcomp_job_    │ │ Per-Job Resources  │
 * │ pool_t          │ │ queue_t       │ │ (hash tables,      │
 * │ (worker threads)│ │ (ordering)    │ │  output buffers)   │
 * └─────────────────┘ └───────────────┘ └────────────────────┘
 * ```
 *
 * ## Modes of Operation
 *
 * ### Inline Mode (is_inline = true)
 *
 * When `threads.count <= 1` or `lz4.independent_blocks = false`, compression
 * runs in the calling thread without any threading overhead:
 *
 * - `submit()` compresses the block immediately and returns
 * - `get_result()` returns the already-completed job
 * - No thread pool or job queue is created
 * - Single hash table and output buffer are reused
 *
 * ### Threaded Mode (is_inline = false)
 *
 * When `threads.count > 1` AND `lz4.independent_blocks = true`:
 *
 * - Worker threads compress blocks in parallel
 * - Job queue ensures results are returned in submission order
 * - Each in-flight job has its own hash table and output buffer
 * - Bounded memory: max_in_flight limits concurrent jobs
 *
 * ## Memory Management
 *
 * Memory is tracked carefully to enforce `limits.max_memory_bytes`:
 *
 * | Resource | Per-Job | Total |
 * |----------|---------|-------|
 * | Context struct | - | 1 |
 * | Thread pool | - | 1 |
 * | Job queue | - | 1 |
 * | Hash tables | max_in_flight | 16K × 4 bytes = 64KB each |
 * | Output buffers | max_in_flight | block_max_size + overhead |
 *
 * In inline mode, only one hash table and output buffer are allocated.
 *
 * ## Job Lifecycle
 *
 * ```
 *   ┌────────────────┐
 *   │  submit(job)   │
 *   └───────┬────────┘
 *           │ Allocate resources, enqueue
 *           v
 *   ┌────────────────┐
 *   │   PENDING      │──────────────────┐
 *   └───────┬────────┘                  │
 *           │ Worker picks up           │ (inline: immediate)
 *           v                           │
 *   ┌────────────────┐                  │
 *   │  COMPRESSING   │◄─────────────────┘
 *   └───────┬────────┘
 *           │ Compression complete
 *           v
 *   ┌────────────────┐
 *   │   COMPLETE     │
 *   └───────┬────────┘
 *           │ get_result() retrieves in order
 *           v
 *   ┌────────────────┐
 *   │   RETURNED     │
 *   └────────────────┘
 * ```
 *
 * ## Block Checksums
 *
 * When `lz4.block_checksum = true`, the xxHash32 of each compressed block
 * is computed after compression. The checksum is stored in the job result
 * for the encoder to append to the output stream.
 *
 * ## Error Handling
 *
 * - Memory allocation failures return GCOMP_ERR_MEMORY
 * - Compression errors are stored in job->status for retrieval
 * - If a worker thread encounters an error, it's propagated to get_result()
 *
 * ## Thread Safety
 *
 * - `lz4_parallel_create()` / `destroy()`: NOT thread-safe (call from main)
 * - `lz4_parallel_submit()`: Thread-safe (uses job queue mutex)
 * - `lz4_parallel_get_result()`: Thread-safe (uses job queue mutex)
 *
 * Typically, a single thread (the encoder's main thread) calls submit()
 * and get_result() sequentially, so thread safety is primarily relevant
 * for internal coordination with worker threads.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include "lz4_parallel.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lz4_internal.h"

#include "../../core/alloc_internal.h"

//
// Constants
//

// Default max in-flight blocks based on thread count
#define LZ4_PARALLEL_DEFAULT_MAX_IN_FLIGHT_MULTIPLIER 2

// Hash table size (must be power of 2)
#define LZ4_HASH_TABLE_SIZE (1U << 14) // 16K entries

// LZ4 worst-case expansion overhead per block
#define LZ4_BLOCK_OVERHEAD 16

//
// Internal Types
//

/**
 * @brief LZ4 parallel context structure.
 */
struct lz4_parallel_ctx_s {
  const gcomp_allocator_t * allocator;
  gcomp_thread_pool_t * pool;
  gcomp_job_queue_t * queue;

  // Configuration
  uint32_t num_threads;
  uint32_t max_in_flight;
  uint32_t block_max_size;
  bool block_checksum;
  uint64_t max_memory_bytes;

  // Mode
  bool is_inline;

  // Inline mode: completed job queue (simple linked list)
  lz4_parallel_job_t * inline_head;
  lz4_parallel_job_t * inline_tail;
  uint32_t inline_count;
};

//
// Job Processing Function
//

/**
 * @brief Process a block compression job.
 *
 * Called by thread pool workers to compress a single block.
 */
static gcomp_status_t lz4_parallel_process_job(void * ctx) {
  lz4_parallel_job_t * job = (lz4_parallel_job_t *)ctx;

  size_t output_len = 0;
  gcomp_status_t status = lz4_block_compress(job->base.input,
      job->base.input_size, job->base.output, job->base.output_capacity,
      &output_len, job->hash_table, job->hash_table_size);

  if (status == GCOMP_ERR_LIMIT) {
    // Compression expanded data, store uncompressed
    if (job->base.output_capacity < job->base.input_size) {
      return GCOMP_ERR_LIMIT;
    }
    memcpy(job->base.output, job->base.input, job->base.input_size);
    job->base.output_size = job->base.input_size;
    job->store_uncompressed = true;
    status = GCOMP_OK;
  }
  else if (status == GCOMP_OK) {
    job->base.output_size = output_len;
    job->store_uncompressed = false;
  }
  else {
    return status;
  }

  // Compute block checksum if needed
  // Note: We checksum the compressed data (or uncompressed if stored raw)
  // This will be filled in by the caller who knows whether checksums are
  // enabled

  return GCOMP_OK;
}

/**
 * @brief Job completion callback.
 *
 * Marks the job as complete in the job queue.
 */
static void lz4_parallel_job_complete(
    void * ctx, gcomp_status_t status, void * user_data) {
  lz4_parallel_job_t * job = (lz4_parallel_job_t *)ctx;
  gcomp_job_queue_t * queue = (gcomp_job_queue_t *)user_data;

  gcomp_job_queue_complete(queue, &job->base, status);
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

  // Allocate context
  lz4_parallel_ctx_t * ctx =
      gcomp_calloc(allocator, 1, sizeof(lz4_parallel_ctx_t));
  if (!ctx) {
    return GCOMP_ERR_MEMORY;
  }

  ctx->allocator = allocator;
  ctx->num_threads = num_threads;
  ctx->block_max_size = block_max_size;
  ctx->block_checksum = config ? config->block_checksum : false;
  ctx->max_memory_bytes = config ? config->max_memory_bytes : 0;

  // Inline mode if no threading
  if (num_threads <= 1) {
    ctx->is_inline = true;
    ctx->pool = NULL;
    ctx->queue = NULL;
    ctx->max_in_flight = 1;
    ctx->inline_head = NULL;
    ctx->inline_tail = NULL;
    ctx->inline_count = 0;
    *ctx_out = ctx;
    return GCOMP_OK;
  }

  ctx->is_inline = false;

  // Calculate max in-flight blocks
  uint32_t max_in_flight = config && config->max_in_flight > 0
      ? config->max_in_flight
      : num_threads * LZ4_PARALLEL_DEFAULT_MAX_IN_FLIGHT_MULTIPLIER;

  // If memory limit is set, calculate based on per-job memory
  if (ctx->max_memory_bytes > 0) {
    size_t per_job_memory = block_max_size +      // Input buffer
        (block_max_size + LZ4_BLOCK_OVERHEAD) +   // Output buffer
        (LZ4_HASH_TABLE_SIZE * sizeof(uint32_t)); // Hash table
    uint32_t mem_limited_jobs =
        (uint32_t)(ctx->max_memory_bytes / per_job_memory);
    if (mem_limited_jobs < max_in_flight) {
      max_in_flight = mem_limited_jobs;
    }
    if (max_in_flight < 1) {
      max_in_flight = 1;
    }
  }
  ctx->max_in_flight = max_in_flight;

  // Create thread pool
  gcomp_thread_pool_config_t pool_config = {
      .num_threads = num_threads,
      .allocator = allocator,
  };
  gcomp_status_t status = gcomp_thread_pool_create(&pool_config, &ctx->pool);
  if (status != GCOMP_OK) {
    gcomp_free(allocator, ctx);
    return status;
  }

  // Create job queue
  gcomp_job_queue_config_t queue_config = {
      .capacity = max_in_flight,
      .allocator = allocator,
  };
  status = gcomp_job_queue_create(&queue_config, &ctx->queue);
  if (status != GCOMP_OK) {
    gcomp_thread_pool_destroy(ctx->pool);
    gcomp_free(allocator, ctx);
    return status;
  }

  *ctx_out = ctx;
  return GCOMP_OK;
}

void lz4_parallel_destroy(lz4_parallel_ctx_t * ctx) {
  if (!ctx) {
    return;
  }

  if (!ctx->is_inline) {
    // Wait for pending jobs
    gcomp_thread_pool_wait(ctx->pool);

    // Destroy in reverse order
    gcomp_job_queue_destroy(ctx->queue);
    gcomp_thread_pool_destroy(ctx->pool);
  }

  gcomp_free(ctx->allocator, ctx);
}

gcomp_status_t lz4_parallel_alloc_job(
    lz4_parallel_ctx_t * ctx, lz4_parallel_job_t ** job_out) {
  if (!ctx || !job_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *job_out = NULL;

  // Allocate job structure
  lz4_parallel_job_t * job =
      gcomp_calloc(ctx->allocator, 1, sizeof(lz4_parallel_job_t));
  if (!job) {
    return GCOMP_ERR_MEMORY;
  }

  // Allocate input buffer
  uint8_t * input_buf = gcomp_malloc(ctx->allocator, ctx->block_max_size);
  if (!input_buf) {
    gcomp_free(ctx->allocator, job);
    return GCOMP_ERR_MEMORY;
  }

  // Allocate output buffer (worst case: input size + overhead)
  size_t output_cap = ctx->block_max_size + LZ4_BLOCK_OVERHEAD;
  uint8_t * output_buf = gcomp_malloc(ctx->allocator, output_cap);
  if (!output_buf) {
    gcomp_free(ctx->allocator, input_buf);
    gcomp_free(ctx->allocator, job);
    return GCOMP_ERR_MEMORY;
  }

  // Allocate hash table
  uint32_t * hash_table =
      gcomp_calloc(ctx->allocator, LZ4_HASH_TABLE_SIZE, sizeof(uint32_t));
  if (!hash_table) {
    gcomp_free(ctx->allocator, output_buf);
    gcomp_free(ctx->allocator, input_buf);
    gcomp_free(ctx->allocator, job);
    return GCOMP_ERR_MEMORY;
  }

  // Initialize job
  // Note: We use a trick here - the input buffer pointer is stored in
  // base.user_data so we can retrieve it later. The actual input pointer
  // will be set by the caller after copying data.
  job->base.input = input_buf;
  job->base.input_size = 0;
  job->base.output = output_buf;
  job->base.output_capacity = output_cap;
  job->base.output_size = 0;
  job->base.user_data = input_buf; // Store for freeing
  job->hash_table = hash_table;
  job->hash_table_size = LZ4_HASH_TABLE_SIZE;
  job->store_uncompressed = false;
  job->block_checksum = 0;

  *job_out = job;
  return GCOMP_OK;
}

void lz4_parallel_free_job(lz4_parallel_ctx_t * ctx, lz4_parallel_job_t * job) {
  if (!ctx || !job) {
    return;
  }

  // Free buffers (input pointer stored in user_data)
  if (job->base.user_data) {
    gcomp_free(ctx->allocator, job->base.user_data);
  }
  if (job->base.output) {
    gcomp_free(ctx->allocator, job->base.output);
  }
  if (job->hash_table) {
    gcomp_free(ctx->allocator, job->hash_table);
  }
  gcomp_free(ctx->allocator, job);
}

gcomp_status_t lz4_parallel_submit(
    lz4_parallel_ctx_t * ctx, lz4_parallel_job_t * job) {
  if (!ctx || !job) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Inline mode: compress immediately and add to result queue
  if (ctx->is_inline) {
    gcomp_status_t status = lz4_parallel_process_job(job);
    job->base.status =
        (status == GCOMP_OK) ? GCOMP_JOB_COMPLETE : GCOMP_JOB_ERROR;
    job->base.result = status;

    // Compute block checksum if enabled
    if (ctx->block_checksum && status == GCOMP_OK) {
      job->block_checksum =
          gcomp_xxhash32(job->base.output, job->base.output_size, 0);
    }

    // Add to inline result queue
    job->next_inline = NULL;
    if (ctx->inline_tail) {
      ctx->inline_tail->next_inline = job;
    }
    else {
      ctx->inline_head = job;
    }
    ctx->inline_tail = job;
    ctx->inline_count++;

    return GCOMP_OK;
  }

  // Submit to job queue (assigns sequence number)
  gcomp_status_t status = gcomp_job_queue_submit(ctx->queue, &job->base);
  if (status != GCOMP_OK) {
    return status;
  }

  // Submit to thread pool for execution
  status = gcomp_thread_pool_submit(ctx->pool, lz4_parallel_process_job, job,
      lz4_parallel_job_complete, ctx->queue);
  if (status != GCOMP_OK) {
    // Job is already in queue, but won't be processed
    // This is a partial failure state - mark as error
    gcomp_job_queue_complete(ctx->queue, &job->base, status);
  }

  return GCOMP_OK;
}

gcomp_status_t lz4_parallel_get_result(
    lz4_parallel_ctx_t * ctx, lz4_parallel_job_t ** job_out) {
  if (!ctx || !job_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *job_out = NULL;

  // Inline mode: return from inline result queue
  if (ctx->is_inline) {
    if (!ctx->inline_head) {
      return GCOMP_ERR_INVALID_ARG; // No pending jobs
    }

    // Pop from head
    lz4_parallel_job_t * job = ctx->inline_head;
    ctx->inline_head = job->next_inline;
    if (!ctx->inline_head) {
      ctx->inline_tail = NULL;
    }
    job->next_inline = NULL;
    ctx->inline_count--;

    *job_out = job;
    // Propagate job error if the job failed
    return job->base.result;
  }

  // Get next result from queue
  gcomp_block_job_t * base_job = NULL;
  gcomp_status_t status =
      gcomp_job_queue_get_next_result(ctx->queue, &base_job);
  if (status != GCOMP_OK) {
    return status;
  }

  // Cast back to LZ4 job (base is first member)
  lz4_parallel_job_t * job = (lz4_parallel_job_t *)base_job;

  // Compute block checksum if enabled and job succeeded
  if (ctx->block_checksum && job->base.result == GCOMP_OK) {
    job->block_checksum =
        gcomp_xxhash32(job->base.output, job->base.output_size, 0);
  }

  *job_out = job;
  // Propagate job error if the job failed
  return job->base.result;
}

bool lz4_parallel_result_ready(const lz4_parallel_ctx_t * ctx) {
  if (!ctx) {
    return false;
  }
  if (ctx->is_inline) {
    return ctx->inline_head != NULL;
  }
  return gcomp_job_queue_result_ready(ctx->queue);
}

gcomp_status_t lz4_parallel_wait(lz4_parallel_ctx_t * ctx) {
  if (!ctx) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (ctx->is_inline) {
    return GCOMP_OK; // Nothing to wait for
  }

  return gcomp_thread_pool_wait(ctx->pool);
}

bool lz4_parallel_is_inline(const lz4_parallel_ctx_t * ctx) {
  return !ctx || ctx->is_inline;
}

uint32_t lz4_parallel_pending_count(const lz4_parallel_ctx_t * ctx) {
  if (!ctx) {
    return 0;
  }
  if (ctx->is_inline) {
    return ctx->inline_count;
  }
  return gcomp_job_queue_pending_count(ctx->queue);
}

gcomp_status_t lz4_parallel_reset(lz4_parallel_ctx_t * ctx) {
  if (!ctx) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (ctx->is_inline) {
    // Check that no jobs are pending
    if (ctx->inline_count > 0) {
      return GCOMP_ERR_INVALID_ARG;
    }
    ctx->inline_head = NULL;
    ctx->inline_tail = NULL;
    return GCOMP_OK;
  }

  return gcomp_job_queue_reset(ctx->queue);
}
