/**
 * @file job_queue.c
 *
 * Ordered job queue implementation for parallel block compression.
 *
 * ## Architecture
 *
 * The queue maintains:
 * - An array of job slots (for bounded capacity)
 * - Sequence numbers for ordering
 * - Synchronization for thread-safe access
 *
 * Jobs are submitted with incrementing sequence numbers. When jobs complete
 * (possibly out of order), they are marked as complete. Results are returned
 * in sequence order, blocking if the next-in-sequence job isn't ready yet.
 *
 * ## Synchronization
 *
 * - Mutex protects all queue state
 * - Semaphore signals when space becomes available (for bounded queue)
 * - Semaphore signals when next-in-sequence result is ready
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/compress/job_queue.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cutil/mutex.h"
#include "cutil/semaphore.h"

#include "alloc_internal.h"

//
// Internal Types
//

/**
 * @brief Job slot in the queue.
 */
typedef struct gcomp_job_slot_s {
  gcomp_block_job_t * job;        ///< Job pointer (NULL if empty)
  bool complete;                  ///< True if job has completed
  struct gcomp_job_slot_s * next; ///< Next slot in linked list
} gcomp_job_slot_t;

/**
 * @brief Job queue internal structure.
 */
struct gcomp_job_queue_s {
  const gcomp_allocator_t * allocator; ///< Allocator

  // Configuration
  uint32_t capacity; ///< Max in-flight jobs (0 = unlimited)

  // Sequence tracking
  uint64_t next_submit_seq; ///< Next sequence number to assign
  uint64_t next_result_seq; ///< Next sequence number to return

  // Job tracking (linked list for flexibility)
  gcomp_job_slot_t * slots_head; ///< Head of active slots list
  gcomp_job_slot_t * slots_tail; ///< Tail of active slots list
  uint32_t active_count;         ///< Number of active (submitted) jobs

  // Synchronization
  GCU_MUTEX_T mutex;             ///< Protects all state
  GCU_Semaphore space_available; ///< Signaled when slot becomes free
  GCU_Semaphore result_ready;    ///< Signaled when next result ready
  bool waiting_for_space;        ///< True if submit is waiting for space
  bool waiting_for_result;       ///< True if get_next is waiting
};

//
// Helper Functions
//

/**
 * @brief Find a slot by sequence number.
 *
 * Must be called with mutex held.
 */
static gcomp_job_slot_t * gcomp_job_queue_find_slot(
    gcomp_job_queue_t * queue, uint64_t seq) {
  for (gcomp_job_slot_t * slot = queue->slots_head; slot; slot = slot->next) {
    if (slot->job && slot->job->sequence_num == seq) {
      return slot;
    }
  }
  return NULL;
}

/**
 * @brief Check if the next result in sequence is ready.
 *
 * Must be called with mutex held.
 */
static bool gcomp_job_queue_next_ready_locked(gcomp_job_queue_t * queue) {
  gcomp_job_slot_t * slot =
      gcomp_job_queue_find_slot(queue, queue->next_result_seq);
  return slot && slot->complete;
}

/**
 * @brief Remove and return the first slot from the list.
 *
 * Must be called with mutex held.
 */
static gcomp_job_slot_t * gcomp_job_queue_remove_first(
    gcomp_job_queue_t * queue) {
  if (!queue->slots_head) {
    return NULL;
  }

  gcomp_job_slot_t * slot = queue->slots_head;
  queue->slots_head = slot->next;
  if (!queue->slots_head) {
    queue->slots_tail = NULL;
  }
  slot->next = NULL;
  queue->active_count--;

  return slot;
}

//
// Public API
//

gcomp_status_t gcomp_job_queue_create(
    const gcomp_job_queue_config_t * config, gcomp_job_queue_t ** queue_out) {
  if (!queue_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *queue_out = NULL;

  const gcomp_allocator_t * allocator = config && config->allocator
      ? config->allocator
      : gcomp_allocator_default();

  uint32_t capacity = config ? config->capacity : 0;

  // Allocate queue structure
  gcomp_job_queue_t * queue =
      gcomp_calloc(allocator, 1, sizeof(gcomp_job_queue_t));
  if (!queue) {
    return GCOMP_ERR_MEMORY;
  }

  queue->allocator = allocator;
  queue->capacity = capacity;
  queue->next_submit_seq = 0;
  queue->next_result_seq = 0;
  queue->slots_head = NULL;
  queue->slots_tail = NULL;
  queue->active_count = 0;
  queue->waiting_for_space = false;
  queue->waiting_for_result = false;

  // Initialize synchronization
  if (GCU_MUTEX_CREATE(queue->mutex) != 0) {
    gcomp_free(allocator, queue);
    return GCOMP_ERR_INTERNAL;
  }

  if (gcu_semaphore_create(&queue->space_available, 0) != 0) {
    GCU_MUTEX_DESTROY(queue->mutex);
    gcomp_free(allocator, queue);
    return GCOMP_ERR_INTERNAL;
  }

  if (gcu_semaphore_create(&queue->result_ready, 0) != 0) {
    gcu_semaphore_destroy(&queue->space_available);
    GCU_MUTEX_DESTROY(queue->mutex);
    gcomp_free(allocator, queue);
    return GCOMP_ERR_INTERNAL;
  }

  *queue_out = queue;
  return GCOMP_OK;
}

void gcomp_job_queue_destroy(gcomp_job_queue_t * queue) {
  if (!queue) {
    return;
  }

  // Free any remaining slots
  GCU_MUTEX_LOCK(queue->mutex);
  while (queue->slots_head) {
    gcomp_job_slot_t * slot = queue->slots_head;
    queue->slots_head = slot->next;
    gcomp_free(queue->allocator, slot);
  }
  GCU_MUTEX_UNLOCK(queue->mutex);

  // Destroy synchronization
  gcu_semaphore_destroy(&queue->result_ready);
  gcu_semaphore_destroy(&queue->space_available);
  GCU_MUTEX_DESTROY(queue->mutex);

  gcomp_free(queue->allocator, queue);
}

gcomp_status_t gcomp_job_queue_submit(
    gcomp_job_queue_t * queue, gcomp_block_job_t * job) {
  if (!queue || !job) {
    return GCOMP_ERR_INVALID_ARG;
  }

  GCU_MUTEX_LOCK(queue->mutex);

  // Check capacity (if bounded)
  while (queue->capacity > 0 && queue->active_count >= queue->capacity) {
    queue->waiting_for_space = true;
    GCU_MUTEX_UNLOCK(queue->mutex);
    gcu_semaphore_wait(&queue->space_available);
    GCU_MUTEX_LOCK(queue->mutex);
  }
  queue->waiting_for_space = false;

  // Allocate a new slot
  gcomp_job_slot_t * slot =
      gcomp_calloc(queue->allocator, 1, sizeof(gcomp_job_slot_t));
  if (!slot) {
    GCU_MUTEX_UNLOCK(queue->mutex);
    return GCOMP_ERR_MEMORY;
  }

  // Initialize job
  job->sequence_num = queue->next_submit_seq++;
  job->status = GCOMP_JOB_PENDING;
  job->result = GCOMP_OK;

  slot->job = job;
  slot->complete = false;
  slot->next = NULL;

  // Add to tail of list
  if (queue->slots_tail) {
    queue->slots_tail->next = slot;
  }
  else {
    queue->slots_head = slot;
  }
  queue->slots_tail = slot;
  queue->active_count++;

  GCU_MUTEX_UNLOCK(queue->mutex);

  return GCOMP_OK;
}

void gcomp_job_queue_complete(
    gcomp_job_queue_t * queue, gcomp_block_job_t * job, gcomp_status_t status) {
  if (!queue || !job) {
    return;
  }

  GCU_MUTEX_LOCK(queue->mutex);

  // Find the slot for this job
  gcomp_job_slot_t * slot = gcomp_job_queue_find_slot(queue, job->sequence_num);
  if (slot) {
    slot->complete = true;
    job->status = (status == GCOMP_OK) ? GCOMP_JOB_COMPLETE : GCOMP_JOB_ERROR;
    job->result = status;

    // If someone is waiting for a result and this is the next one, signal
    if (queue->waiting_for_result &&
        job->sequence_num == queue->next_result_seq) {
      gcu_semaphore_signal(&queue->result_ready);
    }
  }

  GCU_MUTEX_UNLOCK(queue->mutex);
}

gcomp_status_t gcomp_job_queue_get_next_result(
    gcomp_job_queue_t * queue, gcomp_block_job_t ** job_out) {
  if (!queue || !job_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  *job_out = NULL;

  GCU_MUTEX_LOCK(queue->mutex);

  // Wait until next-in-sequence result is ready
  while (!gcomp_job_queue_next_ready_locked(queue)) {
    // Check if there are any pending jobs
    if (queue->active_count == 0) {
      GCU_MUTEX_UNLOCK(queue->mutex);
      return GCOMP_ERR_INVALID_ARG; // No jobs pending
    }

    queue->waiting_for_result = true;
    GCU_MUTEX_UNLOCK(queue->mutex);
    gcu_semaphore_wait(&queue->result_ready);
    GCU_MUTEX_LOCK(queue->mutex);
  }
  queue->waiting_for_result = false;

  // Find and remove the slot
  // The next result should be at the head of the list (since we submit in
  // order)
  gcomp_job_slot_t * slot = queue->slots_head;
  if (slot && slot->job && slot->job->sequence_num == queue->next_result_seq) {
    *job_out = slot->job;
    queue->next_result_seq++;

    // Remove from list
    queue->slots_head = slot->next;
    if (!queue->slots_head) {
      queue->slots_tail = NULL;
    }
    queue->active_count--;

    // Signal if someone is waiting for space
    if (queue->waiting_for_space) {
      gcu_semaphore_signal(&queue->space_available);
    }

    gcomp_free(queue->allocator, slot);
  }

  GCU_MUTEX_UNLOCK(queue->mutex);

  return GCOMP_OK;
}

bool gcomp_job_queue_result_ready(const gcomp_job_queue_t * queue) {
  if (!queue) {
    return false;
  }

  // Cast away const for mutex (internal detail, doesn't modify logical state)
  gcomp_job_queue_t * mutable_queue = (gcomp_job_queue_t *)queue;

  GCU_MUTEX_LOCK(mutable_queue->mutex);
  bool ready = gcomp_job_queue_next_ready_locked(mutable_queue);
  GCU_MUTEX_UNLOCK(mutable_queue->mutex);

  return ready;
}

uint32_t gcomp_job_queue_pending_count(const gcomp_job_queue_t * queue) {
  if (!queue) {
    return 0;
  }

  gcomp_job_queue_t * mutable_queue = (gcomp_job_queue_t *)queue;

  GCU_MUTEX_LOCK(mutable_queue->mutex);
  uint32_t count = mutable_queue->active_count;
  GCU_MUTEX_UNLOCK(mutable_queue->mutex);

  return count;
}

uint32_t gcomp_job_queue_capacity(const gcomp_job_queue_t * queue) {
  return queue ? queue->capacity : 0;
}

gcomp_status_t gcomp_job_queue_reset(gcomp_job_queue_t * queue) {
  if (!queue) {
    return GCOMP_ERR_INVALID_ARG;
  }

  GCU_MUTEX_LOCK(queue->mutex);

  // Check that no jobs are pending
  if (queue->active_count > 0) {
    GCU_MUTEX_UNLOCK(queue->mutex);
    return GCOMP_ERR_INVALID_ARG;
  }

  // Reset sequence numbers
  queue->next_submit_seq = 0;
  queue->next_result_seq = 0;

  GCU_MUTEX_UNLOCK(queue->mutex);

  return GCOMP_OK;
}
