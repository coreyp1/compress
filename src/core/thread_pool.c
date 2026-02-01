/**
 * @file thread_pool.c
 *
 * Thread pool implementation for parallel compression.
 *
 * This implementation uses the cutil library for cross-platform threading
 * primitives. The thread pool supports both threaded and inline (synchronous)
 * execution modes.
 *
 * ## Architecture
 *
 * The thread pool maintains a queue of pending jobs and a pool of worker
 * threads. Workers wait on a semaphore for jobs to become available, then
 * pop jobs from the queue and execute them.
 *
 * For single-threaded mode (num_threads <= 1), jobs are executed inline
 * in the calling thread, avoiding threading overhead.
 *
 * ## Synchronization
 *
 * - Job queue access is protected by a mutex
 * - Workers wait on a semaphore (signaled when jobs are added)
 * - A separate semaphore tracks completion (for wait functionality)
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/compress/thread_pool.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cutil/mutex.h>
#include <cutil/semaphore.h>
#include <cutil/thread.h>

#include "alloc_internal.h"

//
// Internal Types
//

/**
 * @brief Internal job structure.
 */
typedef struct gcomp_job_s {
  gcomp_job_func_t func;               ///< Job function
  void * ctx;                          ///< Job context
  gcomp_job_complete_cb_t complete_cb; ///< Completion callback
  void * user_data;                    ///< Callback user data
  struct gcomp_job_s * next;           ///< Next job in queue
} gcomp_job_t;

/**
 * @brief Thread pool internal structure.
 */
struct gcomp_thread_pool_s {
  const gcomp_allocator_t * allocator; ///< Allocator for memory
  uint32_t num_threads;                ///< Number of worker threads
  GCU_Thread * threads;                ///< Array of thread handles
  bool shutdown;                       ///< Shutdown flag
  bool is_inline;                      ///< Inline mode (no threads)

  // Job queue
  gcomp_job_t * queue_head; ///< Head of job queue
  gcomp_job_t * queue_tail; ///< Tail of job queue
  uint32_t pending_count;   ///< Number of pending jobs

  // Error tracking
  gcomp_status_t first_error; ///< First error encountered

  // Synchronization
  GCU_MUTEX_T queue_mutex;     ///< Protects job queue
  GCU_Semaphore job_available; ///< Signaled when jobs available
  GCU_Semaphore jobs_complete; ///< Signaled when all jobs done
  GCU_MUTEX_T wait_mutex;      ///< Protects wait logic
  uint32_t active_jobs;        ///< Jobs currently being executed
  bool waiting;                ///< True if someone is waiting
};

//
// Helper Functions
//

/**
 * @brief Pop a job from the queue.
 *
 * Must be called with queue_mutex held.
 *
 * @param pool Thread pool.
 * @return Job pointer, or NULL if queue is empty.
 */
static gcomp_job_t * gcomp_thread_pool_pop_job(gcomp_thread_pool_t * pool) {
  if (!pool->queue_head) {
    return NULL;
  }

  gcomp_job_t * job = pool->queue_head;
  pool->queue_head = job->next;
  if (!pool->queue_head) {
    pool->queue_tail = NULL;
  }
  job->next = NULL;
  pool->pending_count--;
  pool->active_jobs++;

  return job;
}

/**
 * @brief Mark a job as complete.
 *
 * @param pool Thread pool.
 * @param status Status returned by job.
 */
static void gcomp_thread_pool_job_done(
    gcomp_thread_pool_t * pool, gcomp_status_t status) {
  GCU_MUTEX_LOCK(pool->wait_mutex);

  // Track first error
  if (status != GCOMP_OK && pool->first_error == GCOMP_OK) {
    pool->first_error = status;
  }

  pool->active_jobs--;

  // Check if all jobs are done and someone is waiting
  if (pool->waiting && pool->active_jobs == 0 && pool->pending_count == 0) {
    gcu_semaphore_signal(&pool->jobs_complete);
  }

  GCU_MUTEX_UNLOCK(pool->wait_mutex);
}

/**
 * @brief Worker thread function.
 *
 * Workers loop, waiting for jobs and executing them until shutdown.
 *
 * @param arg Pointer to the thread pool.
 * @return NULL.
 */
static GCU_THREAD_FUNC_RETURN_T GCU_THREAD_FUNC_CALLING_CONVENTION
gcomp_thread_pool_worker(GCU_THREAD_FUNC_ARG_T arg) {
  gcomp_thread_pool_t * pool = (gcomp_thread_pool_t *)arg;

  while (true) {
    // Wait for a job to be available
    gcu_semaphore_wait(&pool->job_available);

    // Check for shutdown
    if (pool->shutdown) {
      break;
    }

    // Get a job from the queue
    GCU_MUTEX_LOCK(pool->queue_mutex);
    gcomp_job_t * job = gcomp_thread_pool_pop_job(pool);
    GCU_MUTEX_UNLOCK(pool->queue_mutex);

    if (!job) {
      // Spurious wakeup or shutdown, continue
      continue;
    }

    // Execute the job
    gcomp_status_t status = job->func(job->ctx);

    // Call completion callback if provided
    if (job->complete_cb) {
      job->complete_cb(job->ctx, status, job->user_data);
    }

    // Mark job as done
    gcomp_thread_pool_job_done(pool, status);

    // Free the job structure
    gcomp_free(pool->allocator, job);
  }

  return (GCU_THREAD_FUNC_RETURN_T)0;
}

//
// Public API
//

gcomp_status_t gcomp_thread_pool_create(
    const gcomp_thread_pool_config_t * config,
    gcomp_thread_pool_t ** pool_out) {
  if (!pool_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *pool_out = NULL;

  // Use default allocator if not provided
  const gcomp_allocator_t * allocator = config && config->allocator
      ? config->allocator
      : gcomp_allocator_default();

  uint32_t num_threads = config ? config->num_threads : 0;

  // Allocate pool structure
  gcomp_thread_pool_t * pool =
      gcomp_calloc(allocator, 1, sizeof(gcomp_thread_pool_t));
  if (!pool) {
    return GCOMP_ERR_MEMORY;
  }

  pool->allocator = allocator;
  pool->num_threads = num_threads;
  pool->shutdown = false;
  pool->queue_head = NULL;
  pool->queue_tail = NULL;
  pool->pending_count = 0;
  pool->active_jobs = 0;
  pool->first_error = GCOMP_OK;
  pool->waiting = false;

  // Inline mode: no threads needed
  if (num_threads <= 1) {
    pool->is_inline = true;
    pool->threads = NULL;
    *pool_out = pool;
    return GCOMP_OK;
  }

  pool->is_inline = false;

  // Initialize synchronization primitives
  if (GCU_MUTEX_CREATE(pool->queue_mutex) != 0) {
    gcomp_free(allocator, pool);
    return GCOMP_ERR_INTERNAL;
  }

  if (GCU_MUTEX_CREATE(pool->wait_mutex) != 0) {
    GCU_MUTEX_DESTROY(pool->queue_mutex);
    gcomp_free(allocator, pool);
    return GCOMP_ERR_INTERNAL;
  }

  if (gcu_semaphore_create(&pool->job_available, 0) != 0) {
    GCU_MUTEX_DESTROY(pool->wait_mutex);
    GCU_MUTEX_DESTROY(pool->queue_mutex);
    gcomp_free(allocator, pool);
    return GCOMP_ERR_INTERNAL;
  }

  if (gcu_semaphore_create(&pool->jobs_complete, 0) != 0) {
    gcu_semaphore_destroy(&pool->job_available);
    GCU_MUTEX_DESTROY(pool->wait_mutex);
    GCU_MUTEX_DESTROY(pool->queue_mutex);
    gcomp_free(allocator, pool);
    return GCOMP_ERR_INTERNAL;
  }

  // Allocate thread array
  pool->threads = gcomp_calloc(allocator, num_threads, sizeof(GCU_Thread));
  if (!pool->threads) {
    gcu_semaphore_destroy(&pool->jobs_complete);
    gcu_semaphore_destroy(&pool->job_available);
    GCU_MUTEX_DESTROY(pool->wait_mutex);
    GCU_MUTEX_DESTROY(pool->queue_mutex);
    gcomp_free(allocator, pool);
    return GCOMP_ERR_MEMORY;
  }

  // Create worker threads
  for (uint32_t i = 0; i < num_threads; i++) {
    if (gcu_thread_create(&pool->threads[i], gcomp_thread_pool_worker, pool) !=
        0) {
      // Shutdown already created threads
      pool->shutdown = true;
      for (uint32_t j = 0; j < i; j++) {
        gcu_semaphore_signal(&pool->job_available);
      }
      for (uint32_t j = 0; j < i; j++) {
        gcu_thread_join(pool->threads[j]);
      }
      gcomp_free(allocator, pool->threads);
      gcu_semaphore_destroy(&pool->jobs_complete);
      gcu_semaphore_destroy(&pool->job_available);
      GCU_MUTEX_DESTROY(pool->wait_mutex);
      GCU_MUTEX_DESTROY(pool->queue_mutex);
      gcomp_free(allocator, pool);
      return GCOMP_ERR_INTERNAL;
    }
  }

  *pool_out = pool;
  return GCOMP_OK;
}

void gcomp_thread_pool_destroy(gcomp_thread_pool_t * pool) {
  if (!pool) {
    return;
  }

  if (!pool->is_inline) {
    // Signal shutdown to all workers
    pool->shutdown = true;
    for (uint32_t i = 0; i < pool->num_threads; i++) {
      gcu_semaphore_signal(&pool->job_available);
    }

    // Wait for all workers to exit
    for (uint32_t i = 0; i < pool->num_threads; i++) {
      gcu_thread_join(pool->threads[i]);
    }

    // Clean up any remaining jobs in the queue
    GCU_MUTEX_LOCK(pool->queue_mutex);
    while (pool->queue_head) {
      gcomp_job_t * job = pool->queue_head;
      pool->queue_head = job->next;
      gcomp_free(pool->allocator, job);
    }
    GCU_MUTEX_UNLOCK(pool->queue_mutex);

    // Destroy synchronization primitives
    gcu_semaphore_destroy(&pool->jobs_complete);
    gcu_semaphore_destroy(&pool->job_available);
    GCU_MUTEX_DESTROY(pool->wait_mutex);
    GCU_MUTEX_DESTROY(pool->queue_mutex);

    gcomp_free(pool->allocator, pool->threads);
  }

  gcomp_free(pool->allocator, pool);
}

gcomp_status_t gcomp_thread_pool_submit(gcomp_thread_pool_t * pool,
    gcomp_job_func_t func, void * ctx, gcomp_job_complete_cb_t complete_cb,
    void * user_data) {
  if (!pool || !func) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Inline mode: execute immediately
  if (pool->is_inline) {
    gcomp_status_t status = func(ctx);
    if (complete_cb) {
      complete_cb(ctx, status, user_data);
    }
    // Track first error for inline mode too
    if (status != GCOMP_OK && pool->first_error == GCOMP_OK) {
      pool->first_error = status;
    }
    return GCOMP_OK;
  }

  // Allocate job structure
  gcomp_job_t * job = gcomp_malloc(pool->allocator, sizeof(gcomp_job_t));
  if (!job) {
    return GCOMP_ERR_MEMORY;
  }

  job->func = func;
  job->ctx = ctx;
  job->complete_cb = complete_cb;
  job->user_data = user_data;
  job->next = NULL;

  // Add to queue
  GCU_MUTEX_LOCK(pool->queue_mutex);
  if (pool->queue_tail) {
    pool->queue_tail->next = job;
  }
  else {
    pool->queue_head = job;
  }
  pool->queue_tail = job;
  pool->pending_count++;
  GCU_MUTEX_UNLOCK(pool->queue_mutex);

  // Signal a worker
  gcu_semaphore_signal(&pool->job_available);

  return GCOMP_OK;
}

gcomp_status_t gcomp_thread_pool_wait(gcomp_thread_pool_t * pool) {
  if (!pool) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Inline mode: nothing to wait for
  if (pool->is_inline) {
    gcomp_status_t result = pool->first_error;
    pool->first_error = GCOMP_OK;
    return result;
  }

  // Check if already done
  GCU_MUTEX_LOCK(pool->wait_mutex);
  if (pool->active_jobs == 0 && pool->pending_count == 0) {
    gcomp_status_t result = pool->first_error;
    pool->first_error = GCOMP_OK;
    GCU_MUTEX_UNLOCK(pool->wait_mutex);
    return result;
  }

  // Mark that we're waiting
  pool->waiting = true;
  GCU_MUTEX_UNLOCK(pool->wait_mutex);

  // Wait for completion signal
  gcu_semaphore_wait(&pool->jobs_complete);

  // Get result and reset
  GCU_MUTEX_LOCK(pool->wait_mutex);
  pool->waiting = false;
  gcomp_status_t result = pool->first_error;
  pool->first_error = GCOMP_OK;
  GCU_MUTEX_UNLOCK(pool->wait_mutex);

  return result;
}

uint32_t gcomp_thread_pool_get_num_threads(const gcomp_thread_pool_t * pool) {
  if (!pool || pool->is_inline) {
    return 0;
  }
  return pool->num_threads;
}

bool gcomp_thread_pool_is_inline(const gcomp_thread_pool_t * pool) {
  return !pool || pool->is_inline;
}
