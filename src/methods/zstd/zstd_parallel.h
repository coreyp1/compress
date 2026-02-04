/**
 * @file zstd_parallel.h
 *
 * Zstd-specific parallel block compression interface.
 *
 * This module provides parallel block compression for Zstd encoding when
 * `threads.count > 1`. It integrates the generic thread pool and job queue
 * with Zstd block compression.
 *
 * ## Architecture
 *
 * When parallel compression is enabled:
 * 1. Input blocks are submitted to the parallel context
 * 2. Worker threads compress blocks in parallel using Zstd block compression
 * 3. Results are retrieved in submission order
 * 4. Output is valid concatenation of frames (each job produces one frame)
 *
 * When disabled (threads.count <= 1):
 * - Compression is performed inline, avoiding threading overhead
 *
 * ## Memory Management
 *
 * Each parallel block requires:
 * - Input buffer (job_size bytes)
 * - Output buffer (worst-case: job_size + frame overhead)
 * - Match finder hash/chain tables
 *
 * Total memory for N in-flight jobs:
 *   N * (job_size * 2 + hash_table_size + chain_table_size)
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_ZSTD_PARALLEL_H
#define GHOTI_IO_GCOMP_ZSTD_PARALLEL_H

#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/job_queue.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/thread_pool.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct zstd_parallel_ctx_s zstd_parallel_ctx_t;
typedef struct zstd_parallel_job_s zstd_parallel_job_t;

/**
 * @brief Zstd parallel block job.
 *
 * Extends gcomp_block_job_t with Zstd-specific fields.
 */
struct zstd_parallel_job_s {
  gcomp_block_job_t base;          ///< Base job structure
  void * match_finder;             ///< Per-job match finder context
  uint8_t * seq_buffer;            ///< Sequence buffer for encoding
  size_t seq_buffer_capacity;      ///< Sequence buffer capacity
  uint8_t * literals_buffer;       ///< Literals buffer for encoding
  size_t literals_buffer_capacity; ///< Literals buffer capacity
  uint32_t content_checksum; ///< Content checksum (low 32 bits of xxHash64)
  bool checksum_enabled;     ///< Whether checksum was requested
  int compression_level;     ///< Compression level for this job
  uint8_t window_log;        ///< Window log for this job
  zstd_parallel_job_t * next_inline; ///< Next job in inline result queue
};

/**
 * @brief Zstd parallel compression configuration.
 */
typedef struct {
  uint32_t num_threads;                ///< Number of worker threads
  uint32_t max_in_flight;              ///< Max in-flight jobs (0 = default)
  uint64_t job_size;                   ///< Job size in bytes (0 = auto)
  bool checksum_enabled;               ///< Compute content checksums
  int compression_level;               ///< Compression level (1-22)
  uint8_t window_log;                  ///< Window log (10-31)
  uint64_t max_memory_bytes;           ///< Memory limit
  const gcomp_allocator_t * allocator; ///< Allocator (NULL = default)
  gcomp_memory_tracker_t *
      mem_tracker; ///< Optional: track allocations (NULL = do not track)
} zstd_parallel_config_t;

/**
 * @brief Create a Zstd parallel compression context.
 *
 * If num_threads <= 1, creates an inline context (no actual threading).
 *
 * @param config Configuration.
 * @param ctx_out Pointer to receive context.
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_INTERNAL_API gcomp_status_t zstd_parallel_create(
    const zstd_parallel_config_t * config, zstd_parallel_ctx_t ** ctx_out);

/**
 * @brief Destroy a Zstd parallel compression context.
 *
 * Waits for any pending jobs to complete before destroying.
 *
 * @param ctx Context to destroy.
 */
GCOMP_INTERNAL_API void zstd_parallel_destroy(zstd_parallel_ctx_t * ctx);

/**
 * @brief Allocate a job structure for parallel compression.
 *
 * The returned job has pre-allocated buffers for input, output, and
 * match finder tables. Call zstd_parallel_free_job() when done.
 *
 * @param ctx Parallel context.
 * @param job_out Pointer to receive allocated job.
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_INTERNAL_API gcomp_status_t zstd_parallel_alloc_job(
    zstd_parallel_ctx_t * ctx, zstd_parallel_job_t ** job_out);

/**
 * @brief Free a job structure.
 *
 * @param ctx Parallel context.
 * @param job Job to free.
 */
GCOMP_INTERNAL_API void zstd_parallel_free_job(
    zstd_parallel_ctx_t * ctx, zstd_parallel_job_t * job);

/**
 * @brief Submit a block for compression.
 *
 * The job's input buffer must be filled before calling this function.
 * The job remains owned by the caller but must not be modified until
 * retrieved via zstd_parallel_get_result().
 *
 * In parallel mode, each job produces a complete zstd frame. The output
 * of multiple jobs can be concatenated to form valid zstd stream.
 *
 * @param ctx Parallel context.
 * @param job Job to submit (input buffer must be filled).
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_INTERNAL_API gcomp_status_t zstd_parallel_submit(
    zstd_parallel_ctx_t * ctx, zstd_parallel_job_t * job);

/**
 * @brief Get the next compressed frame in order.
 *
 * Blocks until the next job in sequence order is complete.
 * The job's output buffer contains the compressed frame.
 *
 * @param ctx Parallel context.
 * @param job_out Pointer to receive completed job.
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_INTERNAL_API gcomp_status_t zstd_parallel_get_result(
    zstd_parallel_ctx_t * ctx, zstd_parallel_job_t ** job_out);

/**
 * @brief Check if a result is available without blocking.
 *
 * @param ctx Parallel context.
 * @return true if a result is ready, false otherwise.
 */
GCOMP_INTERNAL_API bool zstd_parallel_result_ready(
    const zstd_parallel_ctx_t * ctx);

/**
 * @brief Wait for all pending jobs to complete.
 *
 * @param ctx Parallel context.
 * @return GCOMP_OK on success, or first error from any job.
 */
GCOMP_INTERNAL_API gcomp_status_t zstd_parallel_wait(zstd_parallel_ctx_t * ctx);

/**
 * @brief Check if context is in inline mode.
 *
 * @param ctx Parallel context.
 * @return true if inline mode (no threading), false otherwise.
 */
GCOMP_INTERNAL_API bool zstd_parallel_is_inline(
    const zstd_parallel_ctx_t * ctx);

/**
 * @brief Get number of pending jobs.
 *
 * @param ctx Parallel context.
 * @return Number of jobs submitted but not yet retrieved.
 */
GCOMP_INTERNAL_API uint32_t zstd_parallel_pending_count(
    const zstd_parallel_ctx_t * ctx);

/**
 * @brief Reset context for reuse.
 *
 * All jobs must be retrieved before calling reset.
 *
 * @param ctx Parallel context.
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_INTERNAL_API gcomp_status_t zstd_parallel_reset(
    zstd_parallel_ctx_t * ctx);

/**
 * @brief Get the job size configured for this context.
 *
 * @param ctx Parallel context.
 * @return Job size in bytes.
 */
GCOMP_INTERNAL_API uint64_t zstd_parallel_get_job_size(
    const zstd_parallel_ctx_t * ctx);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_ZSTD_PARALLEL_H
