/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Compress.
 *
 * Ghoti.io Compress is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Compress is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file parallel_block.h
 *
 * Generic parallel block job helper for method-specific parallel compression.
 *
 * This module encapsulates the common pattern used by LZ4 and Zstd parallel
 * encoders: inline vs threaded mode, cutil's GCU_Sequencer for ordering,
 * cutil's GCU_Pool for execution. Method-specific code provides the process
 * callback and job allocation; this helper handles create/destroy, submit,
 * and get_result ordering.
 *
 * The two cutil pieces do not overlap: the pool decides when a block is
 * compressed, the sequencer decides what order the finished blocks are
 * handed back in. Blocks finish out of order and must be written in file
 * order, which is the whole reason the sequencer is here.
 *
 * Job struct contract: the method job must have gcomp_block_job_t as its
 * first member, so that (gcomp_block_job_t *)job is valid. The sequencer
 * ticket is stored in that base's sequence_num, which is how the pool's
 * completion callback names the job it just finished.
 *
 * Internal only — not part of the public API.
 */

#ifndef GHOTI_IO_GCOMP_SRC_CORE_PARALLEL_BLOCK_H
#define GHOTI_IO_GCOMP_SRC_CORE_PARALLEL_BLOCK_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/cutil/pool.h>
#include <ghoti.io/cutil/sequencer.h>

#include "block_job.h"
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
 * Returns `int` rather than gcomp_status_t so that it is exactly cutil's
 * GCU_Pool_Task and can be handed to the pool without a cast.  Casting a
 * function pointer to a different signature and calling through it is
 * undefined behaviour, and the call sites here used to do precisely that.
 * The values are still gcomp_status_t values; GCOMP_OK is 0, which is what
 * the pool treats as success.
 *
 * @param job_ctx Opaque job pointer (struct with gcomp_block_job_t as first
 * member).
 * @return GCOMP_OK on success, a gcomp_status_t error code on failure.
 */
typedef int (*gcomp_parallel_block_process_fn_t)(void * job_ctx);

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
 * WARNING: in threaded mode this waits when the context already has
 * max_in_flight jobs outstanding, and the only thing that reduces that count
 * is gcomp_parallel_block_get_result().  A caller that collects its own
 * results must therefore use gcomp_parallel_block_try_submit() instead:
 * blocking here would be waiting for space that only this thread can free.
 *
 * @param ctx Context.
 * @param job_ctx Job (must have gcomp_block_job_t as first member).
 * @param process_fn Method-specific process function.
 * @return GCOMP_OK on success.
 */
gcomp_status_t gcomp_parallel_block_submit(gcomp_parallel_block_ctx_t * ctx,
    void * job_ctx, gcomp_parallel_block_process_fn_t process_fn);

/**
 * @brief Submit a job, reporting a full context rather than waiting for it.
 *
 * @param ctx Context.
 * @param job_ctx Job (must have gcomp_block_job_t as first member).
 * @param process_fn Method-specific process function.
 * @return GCOMP_OK on success, GCOMP_ERR_LIMIT when max_in_flight jobs are
 *         already outstanding and a result must be collected first.
 */
gcomp_status_t gcomp_parallel_block_try_submit(
    gcomp_parallel_block_ctx_t * ctx, void * job_ctx,
    gcomp_parallel_block_process_fn_t process_fn);

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

#endif /* GHOTI_IO_GCOMP_SRC_CORE_PARALLEL_BLOCK_H */
