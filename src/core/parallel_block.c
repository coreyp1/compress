/**
 * @file parallel_block.c
 *
 * Generic parallel block job helper implementation.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

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

  gcomp_thread_pool_t * pool;
  gcomp_job_queue_t * queue;

  inline_node_t * inline_head;
  inline_node_t * inline_tail;
  uint32_t inline_count;
};

//
// Completion callback for threaded mode
//

static void parallel_block_job_complete(
    void * job_ctx, gcomp_status_t status, void * user_data) {
  gcomp_job_queue_t * queue = (gcomp_job_queue_t *)user_data;
  gcomp_block_job_t * base = (gcomp_block_job_t *)job_ctx;
  gcomp_job_queue_complete(queue, base, status);
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
    ctx->queue = NULL;
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

  gcomp_thread_pool_config_t pool_config = {
      .num_threads = num_threads,
      .allocator = allocator,
  };
  gcomp_status_t status = gcomp_thread_pool_create(&pool_config, &ctx->pool);
  if (status != GCOMP_OK) {
    gcomp_free(allocator, ctx);
    return status;
  }

  gcomp_job_queue_config_t queue_config = {
      .capacity = ctx->max_in_flight,
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

void gcomp_parallel_block_destroy(gcomp_parallel_block_ctx_t * ctx) {
  if (!ctx) {
    return;
  }
  if (!ctx->is_inline) {
    gcomp_thread_pool_wait(ctx->pool);
    gcomp_job_queue_destroy(ctx->queue);
    gcomp_thread_pool_destroy(ctx->pool);
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

gcomp_status_t gcomp_parallel_block_submit(gcomp_parallel_block_ctx_t * ctx,
    void * job_ctx, gcomp_parallel_block_process_fn_t process_fn) {
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

  gcomp_status_t status = gcomp_job_queue_submit(ctx->queue, base);
  if (status != GCOMP_OK) {
    return status;
  }
  status = gcomp_thread_pool_submit(
      ctx->pool, process_fn, job_ctx, parallel_block_job_complete, ctx->queue);
  if (status != GCOMP_OK) {
    gcomp_job_queue_complete(ctx->queue, base, status);
  }
  return GCOMP_OK;
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

  gcomp_status_t status = gcomp_job_queue_get_next_result(ctx->queue, base_out);
  if (status != GCOMP_OK) {
    return status;
  }
  return (*base_out)->result;
}

bool gcomp_parallel_block_result_ready(const gcomp_parallel_block_ctx_t * ctx) {
  if (!ctx) {
    return false;
  }
  if (ctx->is_inline) {
    return ctx->inline_head != NULL;
  }
  return gcomp_job_queue_result_ready(ctx->queue);
}

gcomp_status_t gcomp_parallel_block_wait(gcomp_parallel_block_ctx_t * ctx) {
  if (!ctx) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (ctx->is_inline) {
    return GCOMP_OK;
  }
  return gcomp_thread_pool_wait(ctx->pool);
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
  return gcomp_job_queue_pending_count(ctx->queue);
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
  return gcomp_job_queue_reset(ctx->queue);
}
