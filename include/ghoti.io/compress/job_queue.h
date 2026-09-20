/**
 * @file job_queue.h
 *
 * Ordered job queue for parallel block compression.
 *
 * This queue manages compression jobs that may complete out of order but must
 * be returned to the caller in submission order. It provides bounded capacity
 * to limit memory usage during parallel compression.
 *
 * ## Usage Pattern
 *
 * ```c
 * gcomp_job_queue_t * queue;
 * gcomp_job_queue_create(&config, &queue);
 *
 * // Submit jobs (blocks if queue is full)
 * for (size_t i = 0; i < num_blocks; i++) {
 *   gcomp_job_queue_submit(queue, &jobs[i]);
 * }
 *
 * // Get results in order (blocks until next result is ready)
 * for (size_t i = 0; i < num_blocks; i++) {
 *   gcomp_block_job_t * result;
 *   gcomp_job_queue_get_next_result(queue, &result);
 *   // process result...
 * }
 * ```
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_JOB_QUEUE_H
#define GHOTI_IO_GCOMP_JOB_QUEUE_H

#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct gcomp_job_queue_s gcomp_job_queue_t;

/**
 * @brief Block job status.
 */
typedef enum {
  GCOMP_JOB_PENDING,  ///< Job is waiting to be processed
  GCOMP_JOB_RUNNING,  ///< Job is currently being processed
  GCOMP_JOB_COMPLETE, ///< Job completed successfully
  GCOMP_JOB_ERROR,    ///< Job completed with error
} gcomp_job_status_t;

/**
 * @brief Block compression job.
 *
 * Represents a single block to be compressed or decompressed.
 * The job structure is owned by the caller; the queue only manages
 * the job lifecycle.
 */
typedef struct gcomp_block_job_s {
  // Job identification
  uint64_t sequence_num;     ///< Sequence number for ordering
  gcomp_job_status_t status; ///< Current job status
  gcomp_status_t result; ///< Result code (valid when status==COMPLETE/ERROR)

  // Input data (owned by caller)
  const uint8_t * input; ///< Input buffer
  size_t input_size;     ///< Input size in bytes

  // Output data (owned by caller)
  uint8_t * output;       ///< Output buffer
  size_t output_capacity; ///< Output buffer capacity
  size_t output_size;     ///< Actual output size after processing

  // User data
  void * user_data; ///< User-provided context
} gcomp_block_job_t;

/**
 * @brief Job processing function type.
 *
 * Called by the queue to process a job. Should fill in the output
 * buffer and set output_size.
 *
 * @param job Job to process.
 * @param ctx User-provided context.
 * @return GCOMP_OK on success, error code on failure.
 */
typedef gcomp_status_t (*gcomp_job_process_func_t)(
    gcomp_block_job_t * job, void * ctx);

/**
 * @brief Job queue configuration.
 */
typedef struct {
  uint32_t capacity;                   ///< Max in-flight jobs (0 = unlimited)
  const gcomp_allocator_t * allocator; ///< Allocator (NULL = default)
} gcomp_job_queue_config_t;

/**
 * @brief Create a job queue.
 *
 * @param config Queue configuration.
 * @param queue_out Pointer to receive the created queue.
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_API gcomp_status_t gcomp_job_queue_create(
    const gcomp_job_queue_config_t * config, gcomp_job_queue_t ** queue_out);

/**
 * @brief Destroy a job queue.
 *
 * Note: All jobs must be retrieved or cancelled before destroying the queue.
 *
 * Threads blocked in gcomp_job_queue_submit() or
 * gcomp_job_queue_get_next_result() are released, return
 * GCOMP_ERR_INVALID_ARG, and are waited for before any memory is freed.
 * Calls that arrive after teardown begins are refused the same way.
 *
 * That is the only concurrency this call tolerates.  A thread part-way
 * through any other queue function when this is called reads memory that is
 * being freed; stop the callers first.
 *
 * @param queue Queue to destroy.
 */
GCOMP_API void gcomp_job_queue_destroy(gcomp_job_queue_t * queue);

/**
 * @brief Submit a job to the queue, waiting for space if the queue is full.
 *
 * The job is assigned a sequence number for ordering.
 *
 * WARNING: space in a bounded queue is freed by
 * gcomp_job_queue_get_next_result() and by nothing else.  Blocking here is
 * therefore only safe when a DIFFERENT thread collects results.  A caller
 * that both submits and collects -- which is the usual shape, since results
 * must be consumed in order -- will deadlock the moment the queue fills: it
 * ends up waiting for space that only it could free.  Such a caller wants
 * gcomp_job_queue_try_submit() and should collect a result when told the
 * queue is full.
 *
 * The job structure is owned by the caller but must remain valid until
 * retrieved via gcomp_job_queue_get_next_result().
 *
 * @param queue Job queue.
 * @param job Job to submit (must remain valid until retrieved).
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_API gcomp_status_t gcomp_job_queue_submit(
    gcomp_job_queue_t * queue, gcomp_block_job_t * job);

/**
 * @brief Submit a job to the queue, refusing rather than waiting.
 *
 * As gcomp_job_queue_submit(), except that a full queue is reported instead
 * of waited on.  A caller that collects its own results uses this, collects
 * a result when it is told the queue is full, and tries again.
 *
 * @param queue Job queue.
 * @param job Job to submit (must remain valid until retrieved).
 * @return GCOMP_OK on success, GCOMP_ERR_LIMIT if the queue is at capacity,
 *         other error codes on failure.
 */
GCOMP_API gcomp_status_t gcomp_job_queue_try_submit(
    gcomp_job_queue_t * queue, gcomp_block_job_t * job);

/**
 * @brief Mark a job as complete.
 *
 * Called after processing a job to signal completion.
 *
 * @param queue Job queue.
 * @param job Job that completed.
 * @param status Result status of the job.
 */
GCOMP_API void gcomp_job_queue_complete(
    gcomp_job_queue_t * queue, gcomp_block_job_t * job, gcomp_status_t status);

/**
 * @brief Get the next completed result in order.
 *
 * Blocks until the next job in sequence order is complete, then returns it.
 * Jobs are returned in the same order they were submitted, regardless of
 * the order they completed.
 *
 * @param queue Job queue.
 * @param job_out Pointer to receive the completed job.
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_API gcomp_status_t gcomp_job_queue_get_next_result(
    gcomp_job_queue_t * queue, gcomp_block_job_t ** job_out);

/**
 * @brief Check if a result is available without blocking.
 *
 * @param queue Job queue.
 * @return true if a result is ready, false otherwise.
 */
GCOMP_API bool gcomp_job_queue_result_ready(const gcomp_job_queue_t * queue);

/**
 * @brief Get the number of pending jobs (submitted but not retrieved).
 *
 * @param queue Job queue.
 * @return Number of pending jobs.
 */
GCOMP_API uint32_t gcomp_job_queue_pending_count(
    const gcomp_job_queue_t * queue);

/**
 * @brief Get the queue capacity.
 *
 * @param queue Job queue.
 * @return Queue capacity (0 = unlimited).
 */
GCOMP_API uint32_t gcomp_job_queue_capacity(const gcomp_job_queue_t * queue);

/**
 * @brief Reset the queue for reuse.
 *
 * Clears all pending jobs and resets sequence numbers.
 * All jobs must be retrieved or cancelled before calling reset.
 *
 * @param queue Job queue.
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_API gcomp_status_t gcomp_job_queue_reset(gcomp_job_queue_t * queue);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_JOB_QUEUE_H
