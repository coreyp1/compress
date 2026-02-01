/**
 * @file thread_pool.h
 *
 * Thread pool abstraction for parallel compression in the Ghoti.io Compress
 * library.
 *
 * This thread pool provides a simple interface for submitting jobs to be
 * executed by a pool of worker threads. It uses the cutil library for
 * cross-platform threading primitives.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_THREAD_POOL_H
#define GHOTI_IO_GCOMP_THREAD_POOL_H

#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration
typedef struct gcomp_thread_pool_s gcomp_thread_pool_t;

/**
 * @brief Job function type.
 *
 * Jobs are functions that take a context pointer and return a status code.
 * The job function is executed by a worker thread.
 *
 * @param ctx User-provided context pointer.
 * @return Status code (GCOMP_OK on success, error code on failure).
 */
typedef gcomp_status_t (*gcomp_job_func_t)(void * ctx);

/**
 * @brief Job completion callback type.
 *
 * Called when a job completes (successfully or with error).
 *
 * @param ctx User-provided context pointer (same as passed to job function).
 * @param status The status returned by the job function.
 * @param user_data User-provided callback data.
 */
typedef void (*gcomp_job_complete_cb_t)(
    void * ctx, gcomp_status_t status, void * user_data);

/**
 * @brief Thread pool configuration.
 */
typedef struct {
  uint32_t num_threads; ///< Number of worker threads (0 = inline)
  const gcomp_allocator_t * allocator; ///< Allocator to use (NULL = default)
} gcomp_thread_pool_config_t;

/**
 * @brief Create a thread pool.
 *
 * Creates a thread pool with the specified number of worker threads.
 * If num_threads is 0 or 1, jobs are executed inline (no threads created).
 *
 * @param config Configuration for the thread pool.
 * @param pool_out Pointer to receive the created thread pool.
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_API gcomp_status_t gcomp_thread_pool_create(
    const gcomp_thread_pool_config_t * config, gcomp_thread_pool_t ** pool_out);

/**
 * @brief Destroy a thread pool.
 *
 * Waits for all pending jobs to complete, then shuts down worker threads
 * and frees resources.
 *
 * @param pool Thread pool to destroy.
 */
GCOMP_API void gcomp_thread_pool_destroy(gcomp_thread_pool_t * pool);

/**
 * @brief Submit a job to the thread pool.
 *
 * The job will be executed by an available worker thread. If the pool was
 * created with 0 or 1 threads, the job is executed inline (synchronously).
 *
 * @param pool Thread pool.
 * @param func Job function to execute.
 * @param ctx Context pointer to pass to the job function.
 * @param complete_cb Optional callback called when job completes (may be NULL).
 * @param user_data User data to pass to completion callback.
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_API gcomp_status_t gcomp_thread_pool_submit(gcomp_thread_pool_t * pool,
    gcomp_job_func_t func, void * ctx, gcomp_job_complete_cb_t complete_cb,
    void * user_data);

/**
 * @brief Wait for all pending jobs to complete.
 *
 * Blocks until all submitted jobs have finished executing.
 *
 * @param pool Thread pool.
 * @return GCOMP_OK on success, or the first error encountered by any job.
 */
GCOMP_API gcomp_status_t gcomp_thread_pool_wait(gcomp_thread_pool_t * pool);

/**
 * @brief Get the number of worker threads in the pool.
 *
 * @param pool Thread pool.
 * @return Number of worker threads (0 if inline mode).
 */
GCOMP_API uint32_t gcomp_thread_pool_get_num_threads(
    const gcomp_thread_pool_t * pool);

/**
 * @brief Check if the thread pool is in inline mode.
 *
 * In inline mode, jobs are executed synchronously in the calling thread.
 *
 * @param pool Thread pool.
 * @return true if inline mode, false if using worker threads.
 */
GCOMP_API bool gcomp_thread_pool_is_inline(const gcomp_thread_pool_t * pool);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_THREAD_POOL_H
