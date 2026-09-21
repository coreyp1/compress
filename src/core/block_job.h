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
 * @file block_job.h
 *
 * The block job structure shared by the parallel encoders.
 *
 * A method-specific job (gcomp_lz4_parallel_job_t, gcomp_zstd_parallel_job_t)
 * carries one of these as its first member, so that a pointer to the job is
 * also a valid pointer to this structure.
 *
 * These types used to live in the public job_queue.h, alongside an ordered
 * queue that has since been replaced by cutil's GCU_Sequencer.  The queue is
 * gone; the job structure it used to carry is not, because the encoders
 * embed it.  Nothing outside this library ever needed either, so it is
 * private now.
 *
 * Internal only -- not part of the public API.
 */

#ifndef GHOTI_IO_GCOMP_SRC_CORE_BLOCK_JOB_H
#define GHOTI_IO_GCOMP_SRC_CORE_BLOCK_JOB_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/errors.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
 * Represents a single block to be compressed or decompressed.  The structure
 * is owned by the caller throughout; the parallel helper only orders it.
 */
typedef struct gcomp_block_job_s {
  // Job identification
  uint64_t sequence_num;     ///< Sequencer ticket, assigned on submission
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

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_CORE_BLOCK_JOB_H
