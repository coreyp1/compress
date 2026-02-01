/**
 * @file lz4_parallel.h
 *
 * LZ4-specific parallel block compression interface.
 *
 * This module provides parallel block compression for LZ4 encoding when
 * `threads.count > 1` and `lz4.independent_blocks=true`. It integrates
 * the generic thread pool and job queue with LZ4 block compression.
 *
 * ## Architecture
 *
 * When parallel compression is enabled:
 * 1. Input blocks are submitted to the parallel context
 * 2. Worker threads compress blocks in parallel using `lz4_block_compress()`
 * 3. Results are retrieved in submission order
 *
 * When disabled (threads.count <= 1 or dependent blocks):
 * - Compression is performed inline, avoiding threading overhead
 *
 * ## Memory Management
 *
 * Each parallel block requires:
 * - Input buffer (block_max_size bytes)
 * - Output buffer (LZ4 worst-case: block_max_size + header overhead)
 * - Hash table for match finding
 *
 * Total memory for N in-flight blocks:
 *   N * (block_max_size * 2 + hash_table_size * 4)
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_LZ4_PARALLEL_H
#define GHOTI_IO_GCOMP_LZ4_PARALLEL_H

#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/job_queue.h>
#include <ghoti.io/compress/thread_pool.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration
typedef struct lz4_parallel_ctx_s lz4_parallel_ctx_t;

// Forward declaration for linked list
typedef struct lz4_parallel_job_s lz4_parallel_job_t;

/**
 * @brief LZ4 parallel block job.
 *
 * Extends gcomp_block_job_t with LZ4-specific fields.
 */
struct lz4_parallel_job_s {
  gcomp_block_job_t base;           ///< Base job structure
  uint32_t * hash_table;            ///< Per-job hash table for compression
  size_t hash_table_size;           ///< Hash table size in entries
  bool store_uncompressed;          ///< Output uncompressed block
  uint32_t block_checksum;          ///< Block checksum (if enabled)
  lz4_parallel_job_t * next_inline; ///< Next job in inline result queue
};

/**
 * @brief LZ4 parallel compression configuration.
 */
typedef struct {
  uint32_t num_threads;                ///< Number of worker threads
  uint32_t max_in_flight;              ///< Max in-flight blocks (0 = default)
  uint32_t block_max_size;             ///< Block max size
  bool block_checksum;                 ///< Compute block checksums
  uint64_t max_memory_bytes;           ///< Memory limit
  const gcomp_allocator_t * allocator; ///< Allocator (NULL = default)
} lz4_parallel_config_t;

/**
 * @brief Create an LZ4 parallel compression context.
 *
 * If num_threads <= 1, creates an inline context (no actual threading).
 *
 * @param config Configuration.
 * @param ctx_out Pointer to receive context.
 * @return GCOMP_OK on success, error code on failure.
 */
gcomp_status_t lz4_parallel_create(
    const lz4_parallel_config_t * config, lz4_parallel_ctx_t ** ctx_out);

/**
 * @brief Destroy an LZ4 parallel compression context.
 *
 * Waits for any pending jobs to complete before destroying.
 *
 * @param ctx Context to destroy.
 */
void lz4_parallel_destroy(lz4_parallel_ctx_t * ctx);

/**
 * @brief Allocate a job structure for parallel compression.
 *
 * The returned job has pre-allocated buffers for input, output, and hash table.
 * Call lz4_parallel_free_job() when done.
 *
 * @param ctx Parallel context.
 * @param job_out Pointer to receive allocated job.
 * @return GCOMP_OK on success, error code on failure.
 */
gcomp_status_t lz4_parallel_alloc_job(
    lz4_parallel_ctx_t * ctx, lz4_parallel_job_t ** job_out);

/**
 * @brief Free a job structure.
 *
 * @param ctx Parallel context.
 * @param job Job to free.
 */
void lz4_parallel_free_job(lz4_parallel_ctx_t * ctx, lz4_parallel_job_t * job);

/**
 * @brief Submit a block for compression.
 *
 * The job's input buffer must be filled before calling this function.
 * The job remains owned by the caller but must not be modified until
 * retrieved via lz4_parallel_get_result().
 *
 * @param ctx Parallel context.
 * @param job Job to submit (input buffer must be filled).
 * @return GCOMP_OK on success, error code on failure.
 */
gcomp_status_t lz4_parallel_submit(
    lz4_parallel_ctx_t * ctx, lz4_parallel_job_t * job);

/**
 * @brief Get the next compressed block in order.
 *
 * Blocks until the next job in sequence order is complete.
 * The job's output buffer contains the compressed data.
 *
 * @param ctx Parallel context.
 * @param job_out Pointer to receive completed job.
 * @return GCOMP_OK on success, error code on failure.
 */
gcomp_status_t lz4_parallel_get_result(
    lz4_parallel_ctx_t * ctx, lz4_parallel_job_t ** job_out);

/**
 * @brief Check if a result is available without blocking.
 *
 * @param ctx Parallel context.
 * @return true if a result is ready, false otherwise.
 */
bool lz4_parallel_result_ready(const lz4_parallel_ctx_t * ctx);

/**
 * @brief Wait for all pending jobs to complete.
 *
 * @param ctx Parallel context.
 * @return GCOMP_OK on success, or first error from any job.
 */
gcomp_status_t lz4_parallel_wait(lz4_parallel_ctx_t * ctx);

/**
 * @brief Check if context is in inline mode.
 *
 * @param ctx Parallel context.
 * @return true if inline mode (no threading), false otherwise.
 */
bool lz4_parallel_is_inline(const lz4_parallel_ctx_t * ctx);

/**
 * @brief Get number of pending jobs.
 *
 * @param ctx Parallel context.
 * @return Number of jobs submitted but not yet retrieved.
 */
uint32_t lz4_parallel_pending_count(const lz4_parallel_ctx_t * ctx);

/**
 * @brief Reset context for reuse.
 *
 * All jobs must be retrieved before calling reset.
 *
 * @param ctx Parallel context.
 * @return GCOMP_OK on success, error code on failure.
 */
gcomp_status_t lz4_parallel_reset(lz4_parallel_ctx_t * ctx);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_LZ4_PARALLEL_H
