/**
 * @file parallel_block.c
 *
 * Generic parallel block job helper implementation.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/compress/macros.h>
#include "parallel_block.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alloc_internal.h"

//
// Inline mode: list of completed jobs (FIFO)
//

typedef struct inline_node_s {
  void * job; ///< Method job (first member is gcomp_block_job_t)
  struct inline_node_s * next;
} inline_node_t;

//
// Context
//

struct gcomp_parallel_block_ctx_s {
  const gcomp_allocator_t * allocator;
  uint32_t num_threads;
  uint32_t max_in_flight;
  bool is_inline;

  GCU_Pool * pool;
  GCU_Sequencer * sequencer;

  inline_node_t * inline_head;
  inline_node_t * inline_tail;
  uint32_t inline_count;
};

//
// Completion callback for threaded mode
//

/**
 * @brief Translate a sequencer outcome into this library's status codes.
 *
 * GCU_SEQUENCER_EMPTY is a distinct outcome rather than an error, but the
 * inline path reports "nothing to collect" as GCOMP_ERR_INVALID_ARG and the
 * two modes must not diverge, so it is folded in here.
 */
static gcomp_status_t parallel_block_status(GCU_Sequencer_Result result) {
  switch (result) {
    case GCU_SEQUENCER_OK:
      return GCOMP_OK;
    case GCU_SEQUENCER_FULL:
      return GCOMP_ERR_LIMIT;
    default:
      return GCOMP_ERR_INVALID_ARG;
  }
}

static void parallel_block_job_complete(
    void * job_ctx, int status, void * user_data) {
  GCU_Sequencer * sequencer = (GCU_Sequencer *)user_data;
  gcomp_block_job_t * base = (gcomp_block_job_t *)job_ctx;

  // The job structure is the caller's, so recording the outcome in it is
  // this module's job and not the sequencer's; the sequencer carries the
  // same status separately, for callers whose payload has nowhere to put it.
  base->status = (status == GCOMP_OK) ? GCOMP_JOB_COMPLETE : GCOMP_JOB_ERROR;
  base->result = (gcomp_status_t)status;

  gcu_sequencer_complete(sequencer, base->sequence_num, status);
}

//
// Public API
//

gcomp_status_t gcomp_parallel_block_create(
    const gcomp_parallel_block_config_t * config,
    gcomp_parallel_block_ctx_t ** ctx_out) {
  if (!ctx_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  *ctx_out = NULL;

  const gcomp_allocator_t * allocator = (config && config->allocator)
      ? config->allocator
      : gcomp_allocator_default();
  uint32_t num_threads = config ? config->num_threads : 0;
  uint32_t max_in_flight = config ? config->max_in_flight : 0;

  gcomp_parallel_block_ctx_t * ctx =
      gcomp_calloc(allocator, 1, sizeof(gcomp_parallel_block_ctx_t));
  if (!ctx) {
    return GCOMP_ERR_MEMORY;
  }
  ctx->allocator = allocator;
  ctx->num_threads = num_threads;
  ctx->max_in_flight = max_in_flight > 0 ? max_in_flight : 1;

  if (num_threads <= 1) {
    ctx->is_inline = true;
    ctx->pool = NULL;
    ctx->sequencer = NULL;
    ctx->inline_head = NULL;
    ctx->inline_tail = NULL;
    ctx->inline_count = 0;
    *ctx_out = ctx;
    return GCOMP_OK;
  }

  ctx->is_inline = false;
  ctx->inline_head = NULL;
  ctx->inline_tail = NULL;
  ctx->inline_count = 0;

  // num_threads is at least 2 here:  a request for one worker or none took
  // the inline path above, so the pool is never asked for an inline one.
  GCU_Pool_Config pool_config = {
      .thread_count = num_threads,
      .max_queued = 0,
      .name_prefix = "gcomp-blk",
      .allocator = allocator,
  };
  ctx->pool = gcu_pool_create(&pool_config);
  if (!ctx->pool) {
    gcomp_free(allocator, ctx);
    return GCOMP_ERR_INTERNAL;
  }

  // The tracked allocator, not the default:  gcomp_allocator_t *is* cutil's
  // GCU_Allocator, and the ring has to be counted against this library's
  // memory limit exactly as the queue it replaces was.
  GCU_Sequencer_Config sequencer_config = {
      .capacity = ctx->max_in_flight,
      .allocator = allocator,
  };
  ctx->sequencer = gcu_sequencer_create(&sequencer_config);
  if (!ctx->sequencer) {
    gcu_pool_destroy(ctx->pool);
    gcomp_free(allocator, ctx);
    return GCOMP_ERR_MEMORY;
  }

  *ctx_out = ctx;
  return GCOMP_OK;
}

void gcomp_parallel_block_destroy(gcomp_parallel_block_ctx_t * ctx) {
  if (!ctx) {
    return;
  }
  if (!ctx->is_inline) {
    // Drain before the queue goes away:  a running job's completion callback
    // writes to that queue.  gcu_pool_destroy() drains too, but waiting here
    // keeps the ordering explicit rather than resting on it.
    gcu_pool_wait(ctx->pool);
    gcu_sequencer_destroy(ctx->sequencer);
    gcu_pool_destroy(ctx->pool);
  }
  /* Inline mode: free any remaining nodes (should be empty at destroy) */
  while (ctx->inline_head) {
    inline_node_t * n = ctx->inline_head;
    ctx->inline_head = n->next;
    gcomp_free(ctx->allocator, n);
  }
  ctx->inline_tail = NULL;
  ctx->inline_count = 0;
  gcomp_free(ctx->allocator, ctx);
}

static gcomp_status_t gcomp_parallel_block_submit_internal(
    gcomp_parallel_block_ctx_t * ctx, void * job_ctx,
    gcomp_parallel_block_process_fn_t process_fn, bool blocking) {
  if (!ctx || !job_ctx || !process_fn) {
    return GCOMP_ERR_INVALID_ARG;
  }
  gcomp_block_job_t * base = (gcomp_block_job_t *)job_ctx;

  if (ctx->is_inline) {
    gcomp_status_t status = process_fn(job_ctx);
    base->status = (status == GCOMP_OK) ? GCOMP_JOB_COMPLETE : GCOMP_JOB_ERROR;
    base->result = status;

    inline_node_t * n = gcomp_malloc(ctx->allocator, sizeof(inline_node_t));
    if (!n) {
      return GCOMP_ERR_MEMORY;
    }
    n->job = job_ctx;
    n->next = NULL;
    if (ctx->inline_tail) {
      ctx->inline_tail->next = n;
    }
    else {
      ctx->inline_head = n;
    }
    ctx->inline_tail = n;
    ctx->inline_count++;
    return GCOMP_OK;
  }

  uint64_t ticket = 0;
  GCU_Sequencer_Result submitted = blocking
      ? gcu_sequencer_submit_wait(ctx->sequencer, base, &ticket)
      : gcu_sequencer_submit(ctx->sequencer, base, &ticket);
  if (submitted != GCU_SEQUENCER_OK) {
    return parallel_block_status(submitted);
  }

  // Recorded before the pool is told about the job, because the completion
  // callback reads it back to name the ticket and a worker may run the job
  // the instant it is enqueued.
  base->sequence_num = ticket;
  base->status = GCOMP_JOB_PENDING;
  base->result = GCOMP_OK;

  if (!gcu_pool_enqueue_cb(ctx->pool, process_fn, job_ctx,
          parallel_block_job_complete, ctx->sequencer)) {
    parallel_block_job_complete(job_ctx, GCOMP_ERR_INTERNAL, ctx->sequencer);
  }
  return GCOMP_OK;
}

gcomp_status_t gcomp_parallel_block_submit(gcomp_parallel_block_ctx_t * ctx,
    void * job_ctx, gcomp_parallel_block_process_fn_t process_fn) {
  return gcomp_parallel_block_submit_internal(ctx, job_ctx, process_fn, true);
}

gcomp_status_t gcomp_parallel_block_try_submit(
    gcomp_parallel_block_ctx_t * ctx, void * job_ctx,
    gcomp_parallel_block_process_fn_t process_fn) {
  return gcomp_parallel_block_submit_internal(ctx, job_ctx, process_fn, false);
}

gcomp_status_t gcomp_parallel_block_get_result(
    gcomp_parallel_block_ctx_t * ctx, gcomp_block_job_t ** base_out) {
  if (!ctx || !base_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  *base_out = NULL;

  if (ctx->is_inline) {
    if (!ctx->inline_head) {
      return GCOMP_ERR_INVALID_ARG;
    }
    inline_node_t * n = ctx->inline_head;
    ctx->inline_head = n->next;
    if (!ctx->inline_head) {
      ctx->inline_tail = NULL;
    }
    ctx->inline_count--;
    *base_out = (gcomp_block_job_t *)n->job;
    gcomp_free(ctx->allocator, n);
    return (*base_out)->result;
  }

  void * payload = NULL;
  GCU_Sequencer_Result collected =
      gcu_sequencer_next(ctx->sequencer, &payload, NULL);
  if (collected != GCU_SEQUENCER_OK) {
    return parallel_block_status(collected);
  }
  *base_out = (gcomp_block_job_t *)payload;
  return (*base_out)->result;
}

bool gcomp_parallel_block_result_ready(const gcomp_parallel_block_ctx_t * ctx) {
  if (!ctx) {
    return false;
  }
  if (ctx->is_inline) {
    return ctx->inline_head != NULL;
  }
  return gcu_sequencer_is_ready(ctx->sequencer);
}

gcomp_status_t gcomp_parallel_block_wait(gcomp_parallel_block_ctx_t * ctx) {
  if (!ctx) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (ctx->is_inline) {
    return GCOMP_OK;
  }
  // The pool keeps the first error until it is told to forget it, so that
  // several waiters see the same answer.  This caller wants each wait to
  // report only what happened since the last one, which is what the pool this
  // replaced did as a side effect of being read.
  int status = gcu_pool_wait(ctx->pool);
  gcu_pool_clear_error(ctx->pool);
  return (gcomp_status_t)status;
}

bool gcomp_parallel_block_is_inline(const gcomp_parallel_block_ctx_t * ctx) {
  return ctx && ctx->is_inline;
}

uint32_t gcomp_parallel_block_pending_count(
    const gcomp_parallel_block_ctx_t * ctx) {
  if (!ctx) {
    return 0;
  }
  if (ctx->is_inline) {
    return ctx->inline_count;
  }
  return (uint32_t)gcu_sequencer_count_outstanding(ctx->sequencer);
}

gcomp_status_t gcomp_parallel_block_reset(gcomp_parallel_block_ctx_t * ctx) {
  if (!ctx) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (ctx->is_inline) {
    if (ctx->inline_count > 0) {
      return GCOMP_ERR_INVALID_ARG;
    }
    ctx->inline_head = NULL;
    ctx->inline_tail = NULL;
    return GCOMP_OK;
  }
  if (gcu_sequencer_count_outstanding(ctx->sequencer) > 0) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return parallel_block_status(gcu_sequencer_reset(ctx->sequencer));
}
