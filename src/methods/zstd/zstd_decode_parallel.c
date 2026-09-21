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
 * @file zstd_decode_parallel.c
 *
 * Decoding a Zstandard stream with several threads, when it has several frames.
 *
 * ## The one unit that can be split
 *
 * RFC 8878 section 3.1: "a Zstandard stream is composed of one or more frames".
 * Blocks inside a frame share a window (section 3.1.1.1.2), so they cannot be
 * decoded apart - but frames can, because a frame carries everything needed to
 * read it.
 *
 * That makes this narrower than the LZ4 path, and deliberately so. Our own
 * encoder emits a single frame, which is the right trade for ratio and the
 * same one libzstd makes, so our own output declines here. What benefits is a
 * stream that has frames because somebody wanted them: concatenated files,
 * output from tools that write one frame per thread, and the seekable files
 * Phase D will produce.
 *
 * ## Why a declared content size is required
 *
 * A job needs somewhere to put its frame's output before the frames ahead of
 * it have finished, so it needs to know how big that is first. Frame_Content_Size
 * (section 3.1.1.1.4) says, when the writer chose to record it, and the
 * reference records it whenever it knows the size up front. Without it a job
 * would have to grow a buffer by guessing, so this declines instead: the
 * ordinary decoder handles those streams perfectly well.
 *
 * The declared size is still not trusted. It bounds an allocation that the
 * caller's own limits have already been checked against, and the decode that
 * follows enforces everything it normally would - a frame that lies about its
 * size fails exactly as it does single-threaded.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/alloc_internal.h"
#include "../../core/block_job.h"
#include "../../core/parallel_block.h"
#include "../../core/registry_internal.h"
#include "../../core/walk_internal.h"
#include "zstd_internal.h"
#include "zstd_walk.h"
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/cutil/safemath.h>
#include <string.h>

/// Frames beyond this and the bookkeeping costs more than the threads save.
#define ZSTD_DECODE_PARALLEL_MAX_FRAMES (1u << 20)

/**
 * @brief One frame, decoded by a worker.
 *
 * @ref base must be first; the parallel helper stores its sequencer ticket
 * there.
 */
typedef struct {
  gcomp_block_job_t base;

  gcomp_registry_t * registry;
  gcomp_options_t * options; ///< Shared, read-only, threads.count forced to 1
  const uint8_t * frame;
  size_t frame_size;

  uint8_t * out;
  size_t out_capacity;
  size_t out_size;

  gcomp_status_t status;
} zstd_decode_job_t;

/**
 * @brief Decode one frame. Runs on a worker thread.
 *
 * Goes through gcomp_decode_buffer() rather than reaching into the decoder, so
 * a frame decoded here takes exactly the path it takes single-threaded -
 * including every limit and the content checksum. Sharing one options object
 * across workers is safe because nothing writes to it, and the default
 * registry is documented as safe for concurrent reads.
 */
static int zstd_decode_job_run(void * job_ctx) {
  zstd_decode_job_t * job = (zstd_decode_job_t *)job_ctx;
  size_t produced = 0;
  const gcomp_status_t s = gcomp_decode_buffer(job->registry, "zstd",
      job->options, job->frame, job->frame_size, job->out, job->out_capacity,
      &produced);
  job->out_size = (s == GCOMP_OK) ? produced : 0;
  job->status = s;
  return (int)s;
}

/**
 * @brief Collect every walk event for a stream.
 */
static gcomp_status_t zstd_collect_events(const gcomp_allocator_t * alloc,
    const uint8_t * data, size_t size, gcomp_walk_event_t ** events_out,
    size_t * count_out) {
  zstd_walk_t w;
  memset(&w, 0, sizeof(w));

  size_t cap = 256;
  size_t count = 0;
  gcomp_walk_event_t * all = gcomp_malloc(alloc, cap * sizeof(*all));
  if (!all) {
    return GCOMP_ERR_MEMORY;
  }

  size_t used_total = 0;
  for (;;) {
    gcomp_walk_event_t batch[64];
    size_t used = 0, n = 0;
    const gcomp_status_t s = zstd_walk_update(&w, data + used_total,
        size - used_total, &used, batch, 64, &n);
    if (s != GCOMP_OK) {
      gcomp_free(alloc, all);
      return s;
    }
    used_total += used;
    if (n > 0) {
      if (count + n > cap) {
        size_t next = cap;
        while (count + n > next) {
          if (!gcu_safe_mul_size(next, 2u, &next)) {
            gcomp_free(alloc, all);
            return GCOMP_ERR_LIMIT;
          }
        }
        gcomp_walk_event_t * grown =
            gcomp_realloc(alloc, all, next * sizeof(*all));
        if (!grown) {
          gcomp_free(alloc, all);
          return GCOMP_ERR_MEMORY;
        }
        all = grown;
        cap = next;
      }
      memcpy(all + count, batch, n * sizeof(*all));
      count += n;
    }
    if (used == 0 && n == 0) {
      break;
    }
  }

  if (used_total != size) {
    gcomp_free(alloc, all);
    return GCOMP_ERR_UNSUPPORTED;
  }

  *events_out = all;
  *count_out = count;
  return GCOMP_OK;
}

gcomp_status_t zstd_decode_parallel(gcomp_registry_t * registry,
    gcomp_options_t * options, const void * input, size_t input_size,
    void * output, size_t output_capacity, size_t * output_size_out) {
  if (!input || input_size == 0 || !output || !output_size_out || !registry) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);

  uint64_t threads = 1;
  if (options &&
      gcomp_options_get_uint64(options, "threads.count", &threads) != GCOMP_OK) {
    threads = 1;
  }
  if (threads <= 1) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  const uint64_t max_output =
      gcomp_limits_read_output_max(options, GCOMP_DEFAULT_MAX_OUTPUT_BYTES);

  const uint8_t * in = (const uint8_t *)input;
  gcomp_walk_event_t * ev = NULL;
  size_t n_ev = 0;
  if (zstd_collect_events(alloc, in, input_size, &ev, &n_ev) != GCOMP_OK) {
    // A stream this cannot walk is the ordinary decoder's to report, with its
    // own message and its own stage.
    return GCOMP_ERR_UNSUPPORTED;
  }

  // Every frame must declare its size, and there must be more than one of
  // them. Both are checked before anything is decoded, so declining never
  // leaves bytes in the caller's buffer for the fallback to write over.
  size_t frames = 0;
  uint64_t total_output = 0;
  uint64_t largest_frame_output = 0;
  for (size_t i = 0; i < n_ev; i++) {
    if (ev[i].kind != GCOMP_WALK_FRAME) {
      continue;
    }
    frames++;
    if (ev[i].content_size == 0) {
      gcomp_free(alloc, ev);
      return GCOMP_ERR_UNSUPPORTED;
    }
    if (!gcu_safe_add_u64(total_output, ev[i].content_size, &total_output)) {
      gcomp_free(alloc, ev);
      return GCOMP_ERR_UNSUPPORTED;
    }
    if (ev[i].content_size > largest_frame_output) {
      largest_frame_output = ev[i].content_size;
    }
  }
  if (frames < 2 || frames > ZSTD_DECODE_PARALLEL_MAX_FRAMES) {
    gcomp_free(alloc, ev);
    return GCOMP_ERR_UNSUPPORTED;
  }
  // What the frames say they hold has to fit what the caller offered and what
  // the limits allow, before a byte is allocated for it.
  if (total_output > (uint64_t)output_capacity ||
      (max_output != 0 && total_output > max_output)) {
    gcomp_free(alloc, ev);
    return GCOMP_ERR_UNSUPPORTED;
  }

  // One options object, shared read-only by the workers, with threads.count
  // forced down so that a job cannot find its way back in here.
  gcomp_options_t * job_options = NULL;
  if (options) {
    if (gcomp_options_clone(options, &job_options) != GCOMP_OK) {
      gcomp_free(alloc, ev);
      return GCOMP_ERR_UNSUPPORTED;
    }
  }
  else if (gcomp_options_create(&job_options) != GCOMP_OK) {
    gcomp_free(alloc, ev);
    return GCOMP_ERR_UNSUPPORTED;
  }
  if (gcomp_options_set_uint64(job_options, "threads.count", 1) != GCOMP_OK) {
    gcomp_options_destroy(job_options);
    gcomp_free(alloc, ev);
    return GCOMP_ERR_UNSUPPORTED;
  }

  gcomp_parallel_block_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.allocator = alloc;
  cfg.num_threads = (uint32_t)(threads > 64 ? 64 : threads);
  cfg.max_in_flight = cfg.num_threads * 2u;

  // A job buffer holds a whole frame's output, which is far larger than an LZ4
  // block, so the memory limit binds much sooner here.
  const uint64_t max_memory =
      gcomp_limits_read_memory_max(options, GCOMP_DEFAULT_MAX_MEMORY_BYTES);
  if (max_memory != 0 && largest_frame_output > 0) {
    uint64_t affordable = max_memory / largest_frame_output;
    if (affordable < 1) {
      affordable = 1;
    }
    if (affordable < cfg.max_in_flight) {
      cfg.max_in_flight = (uint32_t)affordable;
    }
  }

  gcomp_parallel_block_ctx_t * ctx = NULL;
  if (gcomp_parallel_block_create(&cfg, &ctx) != GCOMP_OK) {
    gcomp_options_destroy(job_options);
    gcomp_free(alloc, ev);
    return GCOMP_ERR_UNSUPPORTED;
  }

  uint8_t * out = (uint8_t *)output;
  size_t produced = 0;
  gcomp_status_t result = GCOMP_OK;
  size_t in_flight = 0;
  size_t next_event = 0;
  size_t submitted = 0;
  size_t collected = 0;

  while (collected < frames && result == GCOMP_OK) {
    while (submitted < frames) {
      // Find the next frame event; skippable frames carry nothing to decode
      // and are stepped over here, which keeps the data frames in order.
      while (next_event < n_ev && ev[next_event].kind != GCOMP_WALK_FRAME) {
        next_event++;
      }
      if (next_event >= n_ev) {
        break;
      }
      const gcomp_walk_event_t * f = &ev[next_event];

      zstd_decode_job_t * job = gcomp_calloc(alloc, 1, sizeof(*job));
      if (!job) {
        result = GCOMP_ERR_MEMORY;
        break;
      }
      job->out_capacity = (size_t)f->content_size;
      job->out = gcomp_malloc(alloc, job->out_capacity);
      if (!job->out) {
        gcomp_free(alloc, job);
        result = GCOMP_ERR_MEMORY;
        break;
      }
      job->registry = registry;
      job->options = job_options;
      job->frame = in + f->offset;
      job->frame_size = (size_t)f->size;

      const gcomp_status_t sub =
          gcomp_parallel_block_try_submit(ctx, job, zstd_decode_job_run);
      if (sub == GCOMP_ERR_LIMIT) {
        gcomp_free(alloc, job->out);
        gcomp_free(alloc, job);
        break; // Full: collect one first.
      }
      if (sub != GCOMP_OK) {
        gcomp_free(alloc, job->out);
        gcomp_free(alloc, job);
        result = sub;
        break;
      }
      submitted++;
      in_flight++;
      next_event++;
    }
    if (result != GCOMP_OK) {
      break;
    }

    gcomp_block_job_t * base = NULL;
    const gcomp_status_t got = gcomp_parallel_block_get_result(ctx, &base);
    if (!base) {
      result = (got == GCOMP_OK) ? GCOMP_ERR_INTERNAL : got;
      break;
    }
    zstd_decode_job_t * job = (zstd_decode_job_t *)base;
    collected++;
    in_flight--;

    if (job->status != GCOMP_OK) {
      result = job->status;
    }
    else if (job->out_size > output_capacity - produced) {
      result = GCOMP_ERR_LIMIT;
    }
    else {
      memcpy(out + produced, job->out, job->out_size);
      produced += job->out_size;
    }
    gcomp_free(alloc, job->out);
    gcomp_free(alloc, job);
  }

  // A job still in flight owns a buffer this function allocated, and a failed
  // one comes back with an error status and a valid pointer - so the status is
  // ignored here and only a NULL job ends the drain.
  while (in_flight > 0) {
    gcomp_block_job_t * base = NULL;
    (void)gcomp_parallel_block_get_result(ctx, &base);
    if (!base) {
      break;
    }
    zstd_decode_job_t * job = (zstd_decode_job_t *)base;
    gcomp_free(alloc, job->out);
    gcomp_free(alloc, job);
    in_flight--;
  }

  gcomp_parallel_block_wait(ctx);
  gcomp_parallel_block_destroy(ctx);
  gcomp_options_destroy(job_options);
  gcomp_free(alloc, ev);

  if (result != GCOMP_OK) {
    *output_size_out = 0;
    return result;
  }
  *output_size_out = produced;
  return GCOMP_OK;
}
