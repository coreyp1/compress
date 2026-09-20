/**
 * @file lz4_parallel.h
 *
 * LZ4-specific parallel block compression interface.
 *
 * ## What parallel mode produces
 *
 * One ordinary LZ4 frame.  Not a concatenation of frames -- that is what the
 * zstd encoder does, because a zstd frame carries the window and cannot be
 * split, but the LZ4 Frame Format already has the seam this needs.  With the
 * Block Independence flag set, "each block is compressed independently of the
 * others" (LZ4 Frame Format, "Blocks"), so block N may be compressed without
 * knowing anything about block N-1.  The frame header, the block order, the
 * end mark and the content checksum are all unchanged; only *where* each
 * block was compressed differs.
 *
 * The consequence worth stating plainly: **parallel output is byte-identical
 * to single-threaded output.**  A worker is handed exactly the window the
 * serial encoder would have handed itself -- the dictionary, if any, then the
 * block -- with a hash table in exactly the state the serial encoder would
 * have left it, and it applies the same stored-versus-compressed rule.  Same
 * bytes in, same bytes out, whatever the thread count.  Tests assert this
 * directly; see TheSameBytesWhicheverThreadCountProducedThem.
 *
 * ## When it is used
 *
 * Parallel mode requires `threads.count > 1` **and** independent blocks.
 * Linked blocks are the format saying that block N may reference block N-1,
 * which is precisely the dependency that makes parallelism impossible; the
 * encoder falls back to compressing in the calling thread, which is correct,
 * just not faster.  `lz4_encoder_worker_count()` reports what actually
 * happened, so a caller need not guess and a test can check.
 *
 * ## Work division
 *
 * One job is one block.  `lz4.block_size` is therefore also the parallel
 * granularity, which is why LZ4 needs no equivalent of `zstd.job_size`: the
 * frame format already names the unit.  At the default 4 MB that is a
 * substantial piece of work per job; at 64 KB it is small enough that the
 * queue handoff is a visible share of it, which is a reason to raise the
 * block size, not a reason for the encoder to regroup blocks behind the
 * caller's back.
 *
 * ## Memory
 *
 * Each in-flight job holds its own window, output buffer and hash table:
 *
 *   dictionary_size + block_max_size          (window)
 * + block_max_size + LZ4_PARALLEL_BLOCK_OVERHEAD  (framed output)
 * + hash_table_size * 4                       (match finder)
 *
 * `max_in_flight` bounds how many exist at once, and is itself lowered to fit
 * `limits.max_memory_bytes` -- so raising the block size costs memory rather
 * than breaching the limit.  Every allocation is reported to the encoder's
 * memory tracker, so the limit covers parallel mode as it covers everything
 * else.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_LZ4_LZ4_PARALLEL_H
#define GHOTI_IO_GCOMP_SRC_METHODS_LZ4_LZ4_PARALLEL_H

#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../core/block_job.h"
#include "../../core/stepdown.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bytes a framed block may need beyond the block itself.
 *
 * The 4-byte block size field, the optional 4-byte block checksum, and the
 * same 16 bytes of slack the serial encoder allows its own staging buffer --
 * the two must agree, because the point at which the block compressor runs
 * out of room is the point at which a block is stored instead of compressed,
 * and a different point would mean different output.
 */
#define LZ4_PARALLEL_BLOCK_OVERHEAD 24u

// Forward declaration
typedef struct lz4_parallel_ctx_s lz4_parallel_ctx_t;

/**
 * @brief One block's worth of parallel work.
 *
 * `base.input` points into `window`, past the dictionary prefix, so filling a
 * job means writing block bytes at `base.input` and setting
 * `base.input_size`; the prefix is already there and is not the caller's to
 * touch.
 *
 * `base.output` receives the block *as it appears in the frame*: the 4-byte
 * size field (with the uncompressed flag set when the block was stored), the
 * block body, and the block checksum when one is enabled.  The encoder copies
 * those bytes out verbatim, which is the whole reason the worker builds them
 * -- framing on the main thread would be work that did not have to be there.
 */
typedef struct lz4_parallel_job_s {
  gcomp_block_job_t base;    ///< Base job structure
  lz4_parallel_ctx_t * ctx;  ///< Context the worker reads its rules from
  uint8_t * window;          ///< Dictionary prefix followed by the block
  uint32_t * hash_table;     ///< This job's match finder table
  size_t hash_table_size;    ///< Hash table size in entries
  bool store_uncompressed;   ///< The block was stored, not compressed
  uint32_t block_checksum;   ///< Block checksum, when enabled
  bool stepped_down;         ///< The worker settled for a weaker encoding
  gcomp_stepdown_t stepdown; ///< Why, when it did
} lz4_parallel_job_t;

/**
 * @brief LZ4 parallel compression configuration.
 */
typedef struct {
  uint32_t num_threads;    ///< Worker threads (<= 1 => inline)
  uint32_t max_in_flight;  ///< Max in-flight blocks (0 = default)
  uint32_t block_max_size; ///< Block max size
  bool block_checksum;     ///< Append a block checksum to each block
  /**
   * Bytes every independent block is compressed against, or NULL.  Copied
   * into each job's window, so the caller need not keep it alive.
   */
  const uint8_t * dictionary;
  size_t dictionary_size; ///< Length of @ref dictionary
  /**
   * The hash table those dictionary bytes leave behind, or NULL.  Copied into
   * each job rather than recomputed: indexing up to 64 KB of dictionary once
   * per block is the cost the serial encoder already refused to pay.
   */
  const uint32_t * dict_hash_table;
  size_t hash_table_size;              ///< Entries per job table (0 = default)
  uint64_t max_memory_bytes;           ///< Memory limit (0 = unlimited)
  gcomp_memory_tracker_t * mem_tracker; ///< Where allocations are reported
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
GCOMP_INTERNAL_API gcomp_status_t lz4_parallel_create(
    const lz4_parallel_config_t * config, lz4_parallel_ctx_t ** ctx_out);

/**
 * @brief Destroy an LZ4 parallel compression context.
 *
 * Waits for any pending jobs to complete before destroying.
 *
 * @param ctx Context to destroy.
 */
GCOMP_INTERNAL_API void lz4_parallel_destroy(lz4_parallel_ctx_t * ctx);

/**
 * @brief Allocate a job, with its window, output buffer and hash table.
 *
 * The window already holds the dictionary prefix and the hash table already
 * holds what indexing that prefix produced, so the job is ready to be filled
 * with block bytes at `base.input`.  Call lz4_parallel_free_job() when done.
 *
 * @param ctx Parallel context.
 * @param job_out Pointer to receive allocated job.
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_INTERNAL_API gcomp_status_t lz4_parallel_alloc_job(
    lz4_parallel_ctx_t * ctx, lz4_parallel_job_t ** job_out);

/**
 * @brief Free a job structure.
 *
 * @param ctx Parallel context.
 * @param job Job to free.
 */
GCOMP_INTERNAL_API void lz4_parallel_free_job(
    lz4_parallel_ctx_t * ctx, lz4_parallel_job_t * job);

/**
 * @brief Submit a block for compression, waiting for room if there is none.
 *
 * WARNING: a caller that collects its own results must use
 * lz4_parallel_try_submit() instead -- see the note on
 * gcomp_parallel_block_submit().  Waiting here would be waiting for space
 * that only the calling thread can free.
 *
 * @param ctx Parallel context.
 * @param job Job to submit (input buffer must be filled).
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_INTERNAL_API gcomp_status_t lz4_parallel_submit(
    lz4_parallel_ctx_t * ctx, lz4_parallel_job_t * job);

/**
 * @brief Submit a block, reporting a full context rather than waiting for it.
 *
 * @param ctx Parallel context.
 * @param job Job to submit (input buffer must be filled).
 * @return GCOMP_OK on success, GCOMP_ERR_LIMIT when max_in_flight jobs are
 *         already outstanding and a result must be collected first.
 */
GCOMP_INTERNAL_API gcomp_status_t lz4_parallel_try_submit(
    lz4_parallel_ctx_t * ctx, lz4_parallel_job_t * job);

/**
 * @brief Get the next compressed block in order.
 *
 * Blocks until the next job in sequence order is complete.
 * The job's output buffer contains the framed block.
 *
 * @param ctx Parallel context.
 * @param job_out Pointer to receive completed job.
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_INTERNAL_API gcomp_status_t lz4_parallel_get_result(
    lz4_parallel_ctx_t * ctx, lz4_parallel_job_t ** job_out);

/**
 * @brief Check if a result is available without blocking.
 *
 * @param ctx Parallel context.
 * @return true if a result is ready, false otherwise.
 */
GCOMP_INTERNAL_API bool lz4_parallel_result_ready(
    const lz4_parallel_ctx_t * ctx);

/**
 * @brief Wait for all pending jobs to complete.
 *
 * @param ctx Parallel context.
 * @return GCOMP_OK on success, or first error from any job.
 */
GCOMP_INTERNAL_API gcomp_status_t lz4_parallel_wait(lz4_parallel_ctx_t * ctx);

/**
 * @brief Check if context is in inline mode.
 *
 * @param ctx Parallel context.
 * @return true if inline mode (no threading), false otherwise.
 */
GCOMP_INTERNAL_API bool lz4_parallel_is_inline(const lz4_parallel_ctx_t * ctx);

/**
 * @brief How many threads are actually compressing blocks.
 *
 * 1 in inline mode, whatever was configured otherwise.
 *
 * @param ctx Parallel context; NULL reads as 1.
 * @return Worker thread count.
 */
GCOMP_INTERNAL_API uint32_t lz4_parallel_worker_count(
    const lz4_parallel_ctx_t * ctx);

/**
 * @brief How many blocks may be in flight before a result must be collected.
 *
 * @param ctx Parallel context; NULL reads as 0.
 * @return In-flight bound.
 */
GCOMP_INTERNAL_API uint32_t lz4_parallel_max_in_flight(
    const lz4_parallel_ctx_t * ctx);

/**
 * @brief Get number of pending jobs.
 *
 * @param ctx Parallel context.
 * @return Number of jobs submitted but not yet retrieved.
 */
GCOMP_INTERNAL_API uint32_t lz4_parallel_pending_count(
    const lz4_parallel_ctx_t * ctx);

/**
 * @brief Reset context for reuse.
 *
 * All jobs must be retrieved before calling reset.
 *
 * @param ctx Parallel context.
 * @return GCOMP_OK on success, error code on failure.
 */
GCOMP_INTERNAL_API gcomp_status_t lz4_parallel_reset(lz4_parallel_ctx_t * ctx);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_METHODS_LZ4_LZ4_PARALLEL_H
