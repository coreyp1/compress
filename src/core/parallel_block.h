/**
 * @file parallel_block.h
 *
 * Generic parallel block job helper for method-specific parallel compression.
 *
 * This module encapsulates the common pattern used by LZ4 and Zstd parallel
 * encoders: inline vs threaded mode, job queue for ordering, thread pool for
 * execution. Method-specific code provides the process callback and job
 * allocation; this helper handles create/destroy, submit, and get_result
 * ordering.
 *
 * Job struct contract: the method job must have gcomp_block_job_t as its first
 * member so that (gcomp_block_job_t *)job is valid for the queue.
 *
 * Internal only — not part of the public API.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_COMPRESS_PARALLEL_BLOCK_H
#define GHOTI_IO_COMPRESS_PARALLEL_BLOCK_H

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

typedef struct gcomp_parallel_block_ctx_s gcomp_parallel_block_ctx_t;

/**
 * @brief Process callback: run the method-specific work on one job.
 *
 * @param job_ctx Opaque job pointer (struct with gcomp_block_job_t as first
 * member).
 * @return GCOMP_OK on success, error code on failure.
 */
typedef gcomp_status_t (*gcomp_parallel_block_process_fn_t)(void * job_ctx);

/**
 * @brief Configuration for the generic parallel block context.
 */
typedef struct {
  const gcomp_allocator_t * allocator; ///< Allocator (NULL = default)
  uint32_t num_threads;                ///< Worker threads (<= 1 => inline)
  uint32_t max_in_flight;              ///< Max in-flight jobs (threaded mode)
} gcomp_parallel_block_config_t;

/**
 * @brief Create a parallel block context.
 *
 * If num_threads <= 1, inline mode is used (no pool/queue).
 *
 * @param config Configuration.
 * @param ctx_out Pointer to receive context.
 * @return GCOMP_OK on success.
 */
gcomp_status_t gcomp_parallel_block_create(
    const gcomp_parallel_block_config_t * config,
    gcomp_parallel_block_ctx_t ** ctx_out);

/**
 * @brief Destroy context. Waits for pending jobs first.
 */
void gcomp_parallel_block_destroy(gcomp_parallel_block_ctx_t * ctx);

/**
 * @brief Submit a job. In inline mode runs process_fn immediately and queues
 * result; in threaded mode enqueues and runs on worker.
 *
 * @param ctx Context.
 * @param job_ctx Job (must have gcomp_block_job_t as first member).
 * @param process_fn Method-specific process function.
 * @return GCOMP_OK on success.
 */
gcomp_status_t gcomp_parallel_block_submit(gcomp_parallel_block_ctx_t * ctx,
    void * job_ctx, gcomp_parallel_block_process_fn_t process_fn);

/**
 * @brief Get the next completed job in submission order (blocks until ready).
 *
 * @param ctx Context.
 * @param base_out Receives the job's base (gcomp_block_job_t *). Caller casts
 *                 to method job type.
 * @return GCOMP_OK on success; propagates job error via base_out->result.
 */
gcomp_status_t gcomp_parallel_block_get_result(
    gcomp_parallel_block_ctx_t * ctx, gcomp_block_job_t ** base_out);

/**
 * @brief Check if a result is available without blocking.
 */
bool gcomp_parallel_block_result_ready(const gcomp_parallel_block_ctx_t * ctx);

/**
 * @brief Wait for all pending jobs to complete.
 */
gcomp_status_t gcomp_parallel_block_wait(gcomp_parallel_block_ctx_t * ctx);

/**
 * @brief True if context is in inline mode.
 */
bool gcomp_parallel_block_is_inline(const gcomp_parallel_block_ctx_t * ctx);

/**
 * @brief Number of jobs submitted but not yet retrieved.
 */
uint32_t gcomp_parallel_block_pending_count(
    const gcomp_parallel_block_ctx_t * ctx);

/**
 * @brief Reset for reuse. All jobs must be retrieved first.
 */
gcomp_status_t gcomp_parallel_block_reset(gcomp_parallel_block_ctx_t * ctx);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_COMPRESS_PARALLEL_BLOCK_H */
