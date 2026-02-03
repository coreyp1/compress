/**
 * @file zstd_parallel.c
 *
 * Zstd-specific parallel block compression implementation.
 *
 * This module provides parallel block compression for Zstd by integrating
 * the generic thread pool and job queue infrastructure with Zstd-specific
 * compression logic.
 *
 * ## Architecture
 *
 * The parallel compression system has three layers:
 *
 * ```
 * ┌─────────────────────────────────────────────────────────────┐
 * │                    zstd_parallel_ctx_t                      │
 * │        (Zstd-specific parallel compression context)         │
 * └───────────────────────────┬─────────────────────────────────┘
 *                             │
 *           ┌─────────────────┼─────────────────┐
 *           │                 │                 │
 *           v                 v                 v
 * ┌─────────────────┐ ┌───────────────┐ ┌────────────────────┐
 * │ gcomp_thread_   │ │ gcomp_job_    │ │ Per-Job Resources  │
 * │ pool_t          │ │ queue_t       │ │ (match finders,    │
 * │ (worker threads)│ │ (ordering)    │ │  output buffers)   │
 * └─────────────────┘ └───────────────┘ └────────────────────┘
 * ```
 *
 * ## Modes of Operation
 *
 * ### Inline Mode (is_inline = true)
 *
 * When `threads.count <= 1`, compression runs in the calling thread without
 * any threading overhead:
 *
 * - `submit()` compresses the block immediately and returns
 * - `get_result()` returns the already-completed job
 * - No thread pool or job queue is created
 * - Single match finder and output buffer are reused
 *
 * ### Threaded Mode (is_inline = false)
 *
 * When `threads.count > 1`:
 *
 * - Worker threads compress blocks in parallel
 * - Each job produces a complete zstd frame
 * - Job queue ensures results are returned in submission order
 * - Each in-flight job has its own match finder and output buffer
 * - Output is valid concatenation of frames
 *
 * ## Memory Management
 *
 * Memory is tracked to enforce `limits.max_memory_bytes`:
 *
 * | Resource | Per-Job | Total |
 * |----------|---------|-------|
 * | Context struct | - | 1 |
 * | Thread pool | - | 1 |
 * | Job queue | - | 1 |
 * | Match finders | max_in_flight | hash + chain tables |
 * | Output buffers | max_in_flight | job_size + frame overhead |
 *
 * In inline mode, only one match finder and output buffer are allocated.
 *
 * ## Frame Output
 *
 * Each job produces a complete zstd frame:
 * - Magic number (4 bytes)
 * - Frame header (variable)
 * - One or more blocks
 * - Content checksum (4 bytes, if enabled)
 *
 * The concatenation of all job outputs forms a valid multi-frame zstd stream.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include "zstd_parallel.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "zstd_internal.h"

#include "../../core/alloc_internal.h"

//
// Constants
//

// Default max in-flight jobs based on thread count
#define ZSTD_PARALLEL_DEFAULT_MAX_IN_FLIGHT_MULTIPLIER 2

// Frame overhead (magic + max header + checksum)
#define ZSTD_FRAME_OVERHEAD                                                    \
  (4 + ZSTD_HEADER_MAX_SIZE + 4 + ZSTD_BLOCK_HEADER_SIZE)

//
// Internal Types
//

/**
 * @brief Zstd parallel context structure.
 */
struct zstd_parallel_ctx_s {
  const gcomp_allocator_t * allocator;
  gcomp_thread_pool_t * pool;
  gcomp_job_queue_t * queue;

  // Configuration
  uint32_t num_threads;
  uint32_t max_in_flight;
  uint64_t job_size;
  bool checksum_enabled;
  int compression_level;
  uint8_t window_log;
  uint64_t max_memory_bytes;
  gcomp_memory_tracker_t * mem_tracker;

  // Mode
  bool is_inline;

  // Inline mode: completed job queue (simple linked list)
  zstd_parallel_job_t * inline_head;
  zstd_parallel_job_t * inline_tail;
  uint32_t inline_count;
};

//
// Helper: Compress a job to a complete zstd frame
//

/**
 * @brief Compress input data into a complete zstd frame.
 *
 * @param job The job containing input data and configuration.
 * @param allocator Allocator for temporary buffers.
 * @return GCOMP_OK on success, error code on failure.
 */
static gcomp_status_t zstd_parallel_compress_frame(
    zstd_parallel_job_t * job, const gcomp_allocator_t * allocator) {
  const uint8_t * input = job->base.input;
  size_t input_len = job->base.input_size;
  uint8_t * output = job->base.output;
  size_t output_cap = job->base.output_capacity;

  // Initialize content hash if checksum enabled
  gcomp_xxhash64_state_t content_hash;
  if (job->checksum_enabled) {
    gcomp_xxhash64_reset(&content_hash, 0);
    gcomp_xxhash64_update(&content_hash, input, input_len);
  }

  // Build frame header
  zstd_frame_header_t header = {
      .window_log = job->window_log,
      .window_size = zstd_window_log_to_size(job->window_log),
      .content_checksum = job->checksum_enabled,
      .content_size_present = true,
      .content_size = input_len,
      .single_segment = (input_len <= zstd_window_log_to_size(job->window_log)),
      .dict_id = 0,
      .dict_id_flag = 0,
  };

  // Write magic number
  if (output_cap < 4) {
    return GCOMP_ERR_LIMIT;
  }
  gcomp_write_le32(output, ZSTD_MAGIC);
  size_t pos = 4;

  // Write frame header (without magic - zstd_write_frame_header includes it)
  size_t header_len;
  gcomp_status_t status =
      zstd_write_frame_header(&header, output, output_cap, &header_len);
  if (status != GCOMP_OK) {
    return status;
  }
  pos = header_len; // zstd_write_frame_header includes magic

  // Create a temporary encoder state for block compression
  // We need match finder and sequence buffers from the job
  zstd_encoder_state_t temp_state = {
      .allocator = allocator,
      .match_finder = (zstd_match_finder_t *)job->match_finder,
      .seq_buffer = (zstd_sequence_t *)job->seq_buffer,
      .seq_buffer_capacity = job->seq_buffer_capacity / sizeof(zstd_sequence_t),
      .literals_buffer = job->literals_buffer,
      .literals_buffer_capacity = job->literals_buffer_capacity,
      .rep_offset_1 = ZSTD_REP_OFFSET_1_INIT,
      .rep_offset_2 = ZSTD_REP_OFFSET_2_INIT,
      .rep_offset_3 = ZSTD_REP_OFFSET_3_INIT,
  };

  // Compress input as a single block (or multiple blocks if large)
  size_t remaining = input_len;
  const uint8_t * inp = input;

  while (remaining > 0) {
    size_t block_input_len =
        (remaining > ZSTD_BLOCK_SIZE_MAX) ? ZSTD_BLOCK_SIZE_MAX : remaining;
    bool is_last = (remaining - block_input_len == 0);

    // Reset match finder before each block to avoid position confusion
    // (each block is compressed independently)
    if (temp_state.match_finder) {
      zstd_mf_reset(temp_state.match_finder);
    }

    // Reset repeat offsets for each block
    temp_state.rep_offset_1 = ZSTD_REP_OFFSET_1_INIT;
    temp_state.rep_offset_2 = ZSTD_REP_OFFSET_2_INIT;
    temp_state.rep_offset_3 = ZSTD_REP_OFFSET_3_INIT;

    // Compress block
    uint8_t block_type;
    size_t compressed_len;
    status = zstd_block_compress(&temp_state, inp, block_input_len,
        output + pos + ZSTD_BLOCK_HEADER_SIZE,
        output_cap - pos - ZSTD_BLOCK_HEADER_SIZE, &compressed_len,
        &block_type);
    if (status != GCOMP_OK) {
      return status;
    }

    // Write block header
    // For RLE blocks, the header size field is the regenerated size
    uint32_t header_size = (block_type == ZSTD_BLOCK_TYPE_RLE)
        ? (uint32_t)block_input_len
        : (uint32_t)compressed_len;
    zstd_write_block_header(output + pos, is_last, block_type, header_size);
    pos += ZSTD_BLOCK_HEADER_SIZE + compressed_len;

    inp += block_input_len;
    remaining -= block_input_len;
  }

  // Write content checksum if enabled
  if (job->checksum_enabled) {
    if (pos + 4 > output_cap) {
      return GCOMP_ERR_LIMIT;
    }
    uint64_t hash = gcomp_xxhash64_finalize(&content_hash);
    gcomp_write_le32(output + pos, (uint32_t)(hash & 0xFFFFFFFF));
    job->content_checksum = (uint32_t)(hash & 0xFFFFFFFF);
    pos += 4;
  }

  job->base.output_size = pos;
  return GCOMP_OK;
}

//
// Job Processing Function
//

/**
 * @brief Process a frame compression job.
 *
 * Called by thread pool workers to compress a single input into a frame.
 */
static gcomp_status_t zstd_parallel_process_job(void * ctx) {
  zstd_parallel_job_t * job = (zstd_parallel_job_t *)ctx;

  // Get allocator from match finder (stored in the context that created job)
  // For now, use default allocator in worker threads
  const gcomp_allocator_t * allocator = gcomp_allocator_default();

  return zstd_parallel_compress_frame(job, allocator);
}

/**
 * @brief Job completion callback.
 *
 * Marks the job as complete in the job queue.
 */
static void zstd_parallel_job_complete(
    void * ctx, gcomp_status_t status, void * user_data) {
  zstd_parallel_job_t * job = (zstd_parallel_job_t *)ctx;
  gcomp_job_queue_t * queue = (gcomp_job_queue_t *)user_data;

  gcomp_job_queue_complete(queue, &job->base, status);
}

//
// Public API
//

gcomp_status_t zstd_parallel_create(
    const zstd_parallel_config_t * config, zstd_parallel_ctx_t ** ctx_out) {
  if (!ctx_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *ctx_out = NULL;

  const gcomp_allocator_t * allocator = config && config->allocator
      ? config->allocator
      : gcomp_allocator_default();

  uint32_t num_threads = config ? config->num_threads : 0;

  // Calculate job size
  uint64_t job_size = ZSTD_DEFAULT_JOB_SIZE;
  if (config && config->job_size > 0) {
    job_size = config->job_size;
    if (job_size < ZSTD_MIN_JOB_SIZE) {
      job_size = ZSTD_MIN_JOB_SIZE;
    }
    if (job_size > ZSTD_MAX_JOB_SIZE) {
      job_size = ZSTD_MAX_JOB_SIZE;
    }
  }

  // Allocate context
  zstd_parallel_ctx_t * ctx =
      gcomp_calloc(allocator, 1, sizeof(zstd_parallel_ctx_t));
  if (!ctx) {
    return GCOMP_ERR_MEMORY;
  }

  ctx->allocator = allocator;
  ctx->num_threads = num_threads;
  ctx->job_size = job_size;
  ctx->checksum_enabled = config ? config->checksum_enabled : false;
  ctx->compression_level =
      config ? config->compression_level : ZSTD_LEVEL_DEFAULT;
  ctx->window_log = config ? config->window_log : 0;
  ctx->max_memory_bytes = config ? config->max_memory_bytes : 0;
  ctx->mem_tracker = config && config->mem_tracker ? config->mem_tracker : NULL;

  if (ctx->mem_tracker) {
    gcomp_memory_track_alloc(ctx->mem_tracker, sizeof(zstd_parallel_ctx_t));
  }

  // Calculate effective window log if auto
  if (ctx->window_log == 0) {
    ctx->window_log = zstd_level_to_window_log(ctx->compression_level);
  }

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

  // Calculate max in-flight jobs
  uint32_t max_in_flight = config && config->max_in_flight > 0
      ? config->max_in_flight
      : num_threads * ZSTD_PARALLEL_DEFAULT_MAX_IN_FLIGHT_MULTIPLIER;

  // If memory limit is set, calculate based on per-job memory
  if (ctx->max_memory_bytes > 0) {
    uint32_t window_size = zstd_window_log_to_size(ctx->window_log);
    // Per-job memory: input buffer + output buffer + match finder + seq buffer
    // + literals
    size_t per_job_memory = job_size +             // Input buffer
        (job_size + ZSTD_FRAME_OVERHEAD) +         // Output buffer
        (sizeof(uint32_t) * (1UL << 14)) +         // Hash table
        (sizeof(uint32_t) * window_size) +         // Chain table
        (job_size / 3 * sizeof(zstd_sequence_t)) + // Seq buffer
        job_size;                                  // Literals buffer
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

void zstd_parallel_destroy(zstd_parallel_ctx_t * ctx) {
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

  if (ctx->mem_tracker) {
    gcomp_memory_track_free(ctx->mem_tracker, sizeof(zstd_parallel_ctx_t));
  }
  gcomp_free(ctx->allocator, ctx);
}

gcomp_status_t zstd_parallel_alloc_job(
    zstd_parallel_ctx_t * ctx, zstd_parallel_job_t ** job_out) {
  if (!ctx || !job_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *job_out = NULL;
  gcomp_status_t status = GCOMP_OK;

  // Allocate job structure
  zstd_parallel_job_t * job =
      gcomp_calloc(ctx->allocator, 1, sizeof(zstd_parallel_job_t));
  if (!job) {
    return GCOMP_ERR_MEMORY;
  }
  if (ctx->mem_tracker) {
    gcomp_memory_track_alloc(ctx->mem_tracker, sizeof(zstd_parallel_job_t));
  }

  // Allocate input buffer
  uint8_t * input_buf = gcomp_malloc(ctx->allocator, ctx->job_size);
  if (!input_buf) {
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  if (ctx->mem_tracker) {
    gcomp_memory_track_alloc(ctx->mem_tracker, (size_t)ctx->job_size);
  }

  // Allocate output buffer (worst case: input size + frame overhead)
  size_t output_cap = ctx->job_size + ZSTD_FRAME_OVERHEAD +
      (ctx->job_size / ZSTD_BLOCK_SIZE_MAX + 1) * ZSTD_BLOCK_HEADER_SIZE;
  uint8_t * output_buf = gcomp_malloc(ctx->allocator, output_cap);
  if (!output_buf) {
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, (size_t)ctx->job_size);
    }
    gcomp_free(ctx->allocator, input_buf);
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  if (ctx->mem_tracker) {
    gcomp_memory_track_alloc(ctx->mem_tracker, output_cap);
  }

  // Allocate match finder
  zstd_match_finder_t * mf =
      gcomp_calloc(ctx->allocator, 1, sizeof(zstd_match_finder_t));
  if (!mf) {
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, output_cap);
      gcomp_memory_track_free(ctx->mem_tracker, (size_t)ctx->job_size);
    }
    gcomp_free(ctx->allocator, output_buf);
    gcomp_free(ctx->allocator, input_buf);
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }

  uint32_t window_size = zstd_window_log_to_size(ctx->window_log);
  status = zstd_mf_init(mf, ctx->allocator, ctx->compression_level, window_size,
      ctx->mem_tracker);
  if (status != GCOMP_OK) {
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, output_cap);
      gcomp_memory_track_free(ctx->mem_tracker, (size_t)ctx->job_size);
    }
    gcomp_free(ctx->allocator, mf);
    gcomp_free(ctx->allocator, output_buf);
    gcomp_free(ctx->allocator, input_buf);
    goto cleanup;
  }
  if (ctx->mem_tracker) {
    gcomp_memory_track_alloc(ctx->mem_tracker, sizeof(zstd_match_finder_t));
  }

  // Allocate sequence buffer
  size_t seq_cap = ctx->job_size / 3;
  size_t seq_alloc_size = seq_cap * sizeof(zstd_sequence_t);
  uint8_t * seq_buf = gcomp_malloc(ctx->allocator, seq_alloc_size);
  if (!seq_buf) {
    zstd_mf_destroy(mf, ctx->allocator, ctx->mem_tracker);
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, sizeof(zstd_match_finder_t));
      gcomp_memory_track_free(ctx->mem_tracker, output_cap);
      gcomp_memory_track_free(ctx->mem_tracker, (size_t)ctx->job_size);
    }
    gcomp_free(ctx->allocator, mf);
    gcomp_free(ctx->allocator, output_buf);
    gcomp_free(ctx->allocator, input_buf);
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  if (ctx->mem_tracker) {
    gcomp_memory_track_alloc(ctx->mem_tracker, seq_alloc_size);
  }

  // Allocate literals buffer
  uint8_t * lit_buf = gcomp_malloc(ctx->allocator, ctx->job_size);
  if (!lit_buf) {
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, seq_alloc_size);
    }
    gcomp_free(ctx->allocator, seq_buf);
    zstd_mf_destroy(mf, ctx->allocator, ctx->mem_tracker);
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, sizeof(zstd_match_finder_t));
      gcomp_memory_track_free(ctx->mem_tracker, output_cap);
      gcomp_memory_track_free(ctx->mem_tracker, (size_t)ctx->job_size);
    }
    gcomp_free(ctx->allocator, mf);
    gcomp_free(ctx->allocator, output_buf);
    gcomp_free(ctx->allocator, input_buf);
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  if (ctx->mem_tracker) {
    gcomp_memory_track_alloc(ctx->mem_tracker, (size_t)ctx->job_size);
  }

  // Initialize job
  job->base.input = input_buf;
  job->base.input_size = 0;
  job->base.output = output_buf;
  job->base.output_capacity = output_cap;
  job->base.output_size = 0;
  job->base.user_data = input_buf; // Store for freeing
  job->match_finder = mf;
  job->seq_buffer = seq_buf;
  job->seq_buffer_capacity = seq_cap * sizeof(zstd_sequence_t);
  job->literals_buffer = lit_buf;
  job->literals_buffer_capacity = ctx->job_size;
  job->checksum_enabled = ctx->checksum_enabled;
  job->compression_level = ctx->compression_level;
  job->window_log = ctx->window_log;
  job->content_checksum = 0;
  job->next_inline = NULL;

  *job_out = job;
  return GCOMP_OK;

cleanup:
  if (ctx->mem_tracker) {
    gcomp_memory_track_free(ctx->mem_tracker, sizeof(zstd_parallel_job_t));
  }
  gcomp_free(ctx->allocator, job);
  return status;
}

void zstd_parallel_free_job(
    zstd_parallel_ctx_t * ctx, zstd_parallel_job_t * job) {
  if (!ctx || !job) {
    return;
  }

  if (ctx->mem_tracker) {
    if (job->base.user_data) {
      gcomp_memory_track_free(ctx->mem_tracker, (size_t)ctx->job_size);
    }
    if (job->base.output) {
      gcomp_memory_track_free(ctx->mem_tracker, job->base.output_capacity);
    }
  }

  // Free buffers (input pointer stored in user_data)
  if (job->base.user_data) {
    gcomp_free(ctx->allocator, job->base.user_data);
  }
  if (job->base.output) {
    gcomp_free(ctx->allocator, job->base.output);
  }
  if (job->match_finder) {
    zstd_mf_destroy(job->match_finder, ctx->allocator, ctx->mem_tracker);
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, sizeof(zstd_match_finder_t));
    }
    gcomp_free(ctx->allocator, job->match_finder);
  }
  if (job->seq_buffer) {
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, job->seq_buffer_capacity);
    }
    gcomp_free(ctx->allocator, job->seq_buffer);
  }
  if (job->literals_buffer) {
    if (ctx->mem_tracker) {
      gcomp_memory_track_free(ctx->mem_tracker, job->literals_buffer_capacity);
    }
    gcomp_free(ctx->allocator, job->literals_buffer);
  }
  if (ctx->mem_tracker) {
    gcomp_memory_track_free(ctx->mem_tracker, sizeof(zstd_parallel_job_t));
  }
  gcomp_free(ctx->allocator, job);
}

gcomp_status_t zstd_parallel_submit(
    zstd_parallel_ctx_t * ctx, zstd_parallel_job_t * job) {
  if (!ctx || !job) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Reset match finder for this job
  zstd_mf_reset(job->match_finder);

  // Inline mode: compress immediately and add to result queue
  if (ctx->is_inline) {
    gcomp_status_t status = zstd_parallel_compress_frame(job, ctx->allocator);
    job->base.status =
        (status == GCOMP_OK) ? GCOMP_JOB_COMPLETE : GCOMP_JOB_ERROR;
    job->base.result = status;

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
  status = gcomp_thread_pool_submit(ctx->pool, zstd_parallel_process_job, job,
      zstd_parallel_job_complete, ctx->queue);
  if (status != GCOMP_OK) {
    // Job is already in queue, but won't be processed
    // This is a partial failure state - mark as error
    gcomp_job_queue_complete(ctx->queue, &job->base, status);
  }

  return GCOMP_OK;
}

gcomp_status_t zstd_parallel_get_result(
    zstd_parallel_ctx_t * ctx, zstd_parallel_job_t ** job_out) {
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
    zstd_parallel_job_t * job = ctx->inline_head;
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

  // Cast back to Zstd job (base is first member)
  zstd_parallel_job_t * job = (zstd_parallel_job_t *)base_job;

  *job_out = job;
  // Propagate job error if the job failed
  return job->base.result;
}

bool zstd_parallel_result_ready(const zstd_parallel_ctx_t * ctx) {
  if (!ctx) {
    return false;
  }
  if (ctx->is_inline) {
    return ctx->inline_head != NULL;
  }
  return gcomp_job_queue_result_ready(ctx->queue);
}

gcomp_status_t zstd_parallel_wait(zstd_parallel_ctx_t * ctx) {
  if (!ctx) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (ctx->is_inline) {
    return GCOMP_OK; // Nothing to wait for
  }

  return gcomp_thread_pool_wait(ctx->pool);
}

bool zstd_parallel_is_inline(const zstd_parallel_ctx_t * ctx) {
  return !ctx || ctx->is_inline;
}

uint32_t zstd_parallel_pending_count(const zstd_parallel_ctx_t * ctx) {
  if (!ctx) {
    return 0;
  }
  if (ctx->is_inline) {
    return ctx->inline_count;
  }
  return gcomp_job_queue_pending_count(ctx->queue);
}

gcomp_status_t zstd_parallel_reset(zstd_parallel_ctx_t * ctx) {
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

uint64_t zstd_parallel_get_job_size(const zstd_parallel_ctx_t * ctx) {
  if (!ctx) {
    return ZSTD_DEFAULT_JOB_SIZE;
  }
  return ctx->job_size;
}
