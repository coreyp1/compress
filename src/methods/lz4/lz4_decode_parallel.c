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
 * @file lz4_decode_parallel.c
 *
 * Decoding an LZ4 frame stream with several threads.
 *
 * ## Why this format and not the others
 *
 * A block can be decoded on its own only if it does not reference anything
 * before it. In the LZ4 frame format that is the B.Indep flag, which this
 * library sets by default and its parallel encoder always sets. Nothing else
 * here can be split: Zstandard blocks share a window (RFC 8878 section
 * 3.1.1.1.2), and deflate, gzip, zlib, LZW and RLE are single streams of
 * back-references from end to end. So "threads on decode" means LZ4, plus
 * Zstandard when the stream happens to hold several frames.
 *
 * ## Declining is the common case, and is not a failure
 *
 * The frame says whether its blocks are independent, so the decision is read
 * rather than guessed, and everything not certainly splittable is handed back
 * to the ordinary decoder with ::GCOMP_ERR_UNSUPPORTED. That covers linked
 * blocks, a frame with one block, and a frame compressed against a dictionary
 * - the last because every block then references content this path does not
 * carry, and getting that wrong produces output rather than an error.
 *
 * ## What it must not change
 *
 * The bytes, and every check the ordinary path makes: block checksums, the
 * content checksum, the declared content size, and the output limits. Threads
 * are a speed setting. An option that quietly relaxes a check when it is
 * turned up is not one anybody can use.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/alloc_internal.h"
#include "../../core/block_job.h"
#include "../../core/endian.h"
#include "../../core/parallel_block.h"
#include "../../core/registry_internal.h"
#include "../../core/walk_internal.h"
#include "lz4_internal.h"
#include "lz4_walk.h"
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/xxhash32.h>
#include <ghoti.io/cutil/safemath.h>
#include <string.h>

/// How many blocks one frame may hold before this gives up and declines.
#define LZ4_DECODE_PARALLEL_MAX_BLOCKS (1u << 22)

/**
 * @brief One block, decoded by a worker.
 *
 * @ref base must be first: the parallel helper casts a job pointer to
 * ::gcomp_block_job_t and stores its sequencer ticket there.
 */
typedef struct {
  gcomp_block_job_t base;

  const uint8_t * payload; ///< The block's bytes, header and checksum excluded
  size_t payload_size;
  int stored;              ///< Payload is the output verbatim
  int verify_checksum;     ///< Frame sets B.Checksum
  uint32_t expected_checksum;

  uint8_t * out;      ///< Where this block's output goes
  size_t out_capacity;
  size_t out_size;

  gcomp_status_t status;
} lz4_decode_job_t;

/**
 * @brief Decode one block. Runs on a worker thread.
 *
 * Returns int rather than gcomp_status_t so that it is exactly cutil's
 * GCU_Pool_Task; the values are still gcomp_status_t values.
 */
static int lz4_decode_job_run(void * job_ctx) {
  lz4_decode_job_t * job = (lz4_decode_job_t *)job_ctx;

  // The block checksum covers the block as it sits on the wire, so it is
  // checked before anything is decoded - a corrupt block should be reported as
  // corrupt, not decoded into whatever it happens to expand to.
  if (job->verify_checksum) {
    gcomp_xxhash32_state_t h;
    gcomp_xxhash32_reset(&h, 0);
    gcomp_xxhash32_update(&h, job->payload, job->payload_size);
    if (gcomp_xxhash32_finalize(&h) != job->expected_checksum) {
      job->status = GCOMP_ERR_CORRUPT;
      return (int)GCOMP_ERR_CORRUPT;
    }
  }

  if (job->stored) {
    if (job->payload_size > job->out_capacity) {
      job->status = GCOMP_ERR_LIMIT;
      return (int)GCOMP_ERR_LIMIT;
    }
    memcpy(job->out, job->payload, job->payload_size);
    job->out_size = job->payload_size;
    job->status = GCOMP_OK;
    return (int)GCOMP_OK;
  }

  // No history: that a block needs none is the property B.Indep promises, and
  // passing NULL here is what holds the encoder to it.
  size_t produced = 0;
  const gcomp_status_t s = lz4_block_decompress(job->payload, job->payload_size,
      job->out, job->out_capacity, &produced, NULL, 0);
  job->out_size = (s == GCOMP_OK) ? produced : 0;
  job->status = s;
  return (int)s;
}

/**
 * @brief Everything one frame needs before its blocks can be handed out.
 */
typedef struct {
  size_t first_block;  ///< Index into the collected block list
  size_t block_count;
  int content_checksum;
  uint32_t expected_content_checksum;
  int has_content_size;
  uint64_t content_size;
  uint32_t block_max_size;
} lz4_frame_plan_t;

/**
 * @brief Read a frame's descriptor again, for the parts the walk does not
 *        report.
 *
 * The walker reports where things are, not what a frame's flags were, and the
 * two decisions this path has to make - are the blocks independent, is there a
 * dictionary - are flags. Re-reading one header per frame is cheaper than
 * widening the walk event for two callers, only one of which cares.
 *
 * @return ::GCOMP_OK, ::GCOMP_ERR_UNSUPPORTED when this frame must not be
 *         split, or a parse error.
 */
static gcomp_status_t lz4_frame_plan_read(const uint8_t * frame, size_t size,
    lz4_frame_plan_t * plan_out, size_t * header_size_out) {
  lz4_frame_header_t header;
  size_t header_size = 0;
  const gcomp_status_t s =
      lz4_parse_frame_header(frame, size, &header, &header_size);
  if (s != GCOMP_OK) {
    return s;
  }

  // Linked blocks reference the blocks before them, which is exactly what a
  // job cannot see.
  if (!header.block_independence) {
    return GCOMP_ERR_UNSUPPORTED;
  }
  // A dictionary is history every block may reach into. Decoding a block
  // without it produces bytes rather than an error, which is the worst kind of
  // wrong, so this path does not attempt it.
  if (header.dict_id_present) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  plan_out->content_checksum = header.content_checksum ? 1 : 0;
  plan_out->has_content_size = header.content_size_present ? 1 : 0;
  plan_out->content_size = header.content_size;
  plan_out->block_max_size = header.block_max_size;
  *header_size_out = header_size;
  return GCOMP_OK;
}

/**
 * @brief Collect every walk event for a stream.
 *
 * Grown rather than counted first: a second walk to size the array costs the
 * same headers again, and the array is one pointer per block.
 */
static gcomp_status_t lz4_collect_events(const gcomp_allocator_t * alloc,
    const uint8_t * data, size_t size, gcomp_walk_event_t ** events_out,
    size_t * count_out) {
  lz4_walk_t w;
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
    const gcomp_status_t s = lz4_walk_update(&w, data + used_total,
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

  // A stream that does not account for all of its bytes is one this path must
  // not guess about: the ordinary decoder will report it properly.
  if (used_total != size) {
    gcomp_free(alloc, all);
    return GCOMP_ERR_UNSUPPORTED;
  }

  *events_out = all;
  *count_out = count;
  return GCOMP_OK;
}

gcomp_status_t lz4_decode_parallel(gcomp_registry_t * registry,
    gcomp_options_t * options, const void * input, size_t input_size,
    void * output, size_t output_capacity, size_t * output_size_out) {
  if (!input || input_size == 0 || !output || !output_size_out) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  const gcomp_allocator_t * alloc =
      registry ? gcomp_registry_get_allocator(registry) : NULL;

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
  const uint64_t max_ratio = gcomp_limits_read_expansion_ratio_max(
      options, GCOMP_LZ4_MAX_EXPANSION_RATIO);

  const uint8_t * in = (const uint8_t *)input;
  gcomp_walk_event_t * ev = NULL;
  size_t n_ev = 0;
  gcomp_status_t status = lz4_collect_events(alloc, in, input_size, &ev, &n_ev);
  if (status != GCOMP_OK) {
    // A corrupt stream is the ordinary decoder's to report, with its own
    // message and its own stage. Declining keeps one account of what went
    // wrong rather than two that can disagree.
    return GCOMP_ERR_UNSUPPORTED;
  }

  // Worth splitting at all?  One block is one job, and a stream of stored
  // blocks is a memcpy either way.
  size_t total_blocks = 0;
  for (size_t i = 0; i < n_ev; i++) {
    if (ev[i].kind == GCOMP_WALK_BLOCK) {
      total_blocks++;
    }
  }
  if (total_blocks < 2 || total_blocks > LZ4_DECODE_PARALLEL_MAX_BLOCKS) {
    gcomp_free(alloc, ev);
    return GCOMP_ERR_UNSUPPORTED;
  }

  // Every frame is examined before any of them is decoded. Declining half way
  // through would mean the caller's output buffer already held bytes from the
  // frames that did split, and gcomp_decode_buffer() would then decode the
  // whole stream again on top of them.
  //
  // The pass also finds the largest block any frame can produce, which is what
  // a job buffer has to hold and therefore what bounds how many may be in
  // flight.
  uint64_t max_block = 0;
  for (size_t k = 0; k < n_ev; k++) {
    if (ev[k].kind != GCOMP_WALK_FRAME) {
      continue;
    }
    lz4_frame_plan_t probe;
    memset(&probe, 0, sizeof(probe));
    size_t probe_header = 0;
    if (lz4_frame_plan_read(in + ev[k].offset, (size_t)ev[k].size, &probe,
            &probe_header) != GCOMP_OK) {
      gcomp_free(alloc, ev);
      return GCOMP_ERR_UNSUPPORTED;
    }
    const uint64_t b =
        probe.block_max_size ? probe.block_max_size : LZ4_DEFAULT_BLOCK_SIZE;
    if (b > max_block) {
      max_block = b;
    }
  }
  if (max_block == 0) {
    gcomp_free(alloc, ev);
    return GCOMP_ERR_UNSUPPORTED;
  }

  gcomp_parallel_block_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.allocator = alloc;
  cfg.num_threads = (uint32_t)(threads > 64 ? 64 : threads);
  cfg.max_in_flight = cfg.num_threads * 2u;

  // limits.max_memory_bytes caps how many jobs may be in flight rather than
  // failing the decode. Fewer threads is a slower answer; GCOMP_ERR_LIMIT on a
  // stream that decodes fine single-threaded would make threads.count a
  // correctness setting, which is the thing this must not become.
  const uint64_t max_memory =
      gcomp_limits_read_memory_max(options, GCOMP_DEFAULT_MAX_MEMORY_BYTES);
  if (max_memory != 0) {
    uint64_t affordable = max_memory / max_block;
    if (affordable < 1) {
      affordable = 1; // One job at a time is still correct, just not parallel.
    }
    if (affordable < cfg.max_in_flight) {
      cfg.max_in_flight = (uint32_t)affordable;
    }
  }

  gcomp_parallel_block_ctx_t * ctx = NULL;
  status = gcomp_parallel_block_create(&cfg, &ctx);
  if (status != GCOMP_OK) {
    gcomp_free(alloc, ev);
    return GCOMP_ERR_UNSUPPORTED;
  }

  uint8_t * out = (uint8_t *)output;
  size_t produced = 0;
  gcomp_status_t result = GCOMP_OK;
  size_t in_flight = 0;

  // Frames are walked in order; a frame's blocks are what run in parallel.
  // Keeping the frame loop sequential is what makes each frame's content
  // checksum and declared size checkable exactly where the ordinary decoder
  // checks them.
  size_t i = 0;
  while (i < n_ev && result == GCOMP_OK) {
    if (ev[i].kind != GCOMP_WALK_FRAME) {
      i++; // Skippable frames carry nothing to decode.
      continue;
    }

    const gcomp_walk_event_t frame = ev[i];
    lz4_frame_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    size_t header_size = 0;
    status = lz4_frame_plan_read(
        in + frame.offset, (size_t)frame.size, &plan, &header_size);
    if (status != GCOMP_OK) {
      result = GCOMP_ERR_UNSUPPORTED;
      break;
    }

    gcomp_xxhash32_state_t content_hash;
    gcomp_xxhash32_reset(&content_hash, 0);
    const size_t frame_output_start = produced;

    // The blocks of this frame are the events before it that lie inside it.
    // The walker emits a block when it completes, so they all precede the
    // frame event and follow the previous frame's.
    size_t first = 0;
    size_t count = 0;
    for (size_t j = 0; j < i; j++) {
      if (ev[j].kind == GCOMP_WALK_BLOCK &&
          ev[j].offset >= frame.offset &&
          ev[j].offset + ev[j].size <= frame.offset + frame.size) {
        if (count == 0) {
          first = j;
        }
        count++;
      }
    }
    if (count == 0) {
      result = GCOMP_ERR_UNSUPPORTED;
      break;
    }

    const size_t job_capacity =
        plan.block_max_size ? plan.block_max_size : LZ4_DEFAULT_BLOCK_SIZE;

    size_t submitted = 0;
    size_t collected = 0;
    while (collected < count && result == GCOMP_OK) {
      // Submit while there is room, then take one result.
      while (submitted < count) {
        lz4_decode_job_t * job = gcomp_calloc(alloc, 1, sizeof(*job));
        if (!job) {
          result = GCOMP_ERR_MEMORY;
          break;
        }
        job->out = gcomp_malloc(alloc, job_capacity);
        if (!job->out) {
          gcomp_free(alloc, job);
          result = GCOMP_ERR_MEMORY;
          break;
        }
        const gcomp_walk_event_t * b = &ev[first + submitted];
        job->payload = in + b->payload_offset;
        job->payload_size = (size_t)b->payload_size;
        job->stored = b->stored;
        job->out_capacity = job_capacity;
        job->verify_checksum =
            (b->size > 4u + b->payload_size) ? 1 : 0;
        if (job->verify_checksum) {
          job->expected_checksum =
              gcomp_read_le32(in + b->payload_offset + b->payload_size);
        }

        const gcomp_status_t sub =
            gcomp_parallel_block_try_submit(ctx, job, lz4_decode_job_run);
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
      }
      if (result != GCOMP_OK) {
        break;
      }

      // gcomp_parallel_block_get_result() returns the *job's* own result, so a
      // block that failed to decode comes back as an error status together
      // with a perfectly valid job pointer. Treating that as "no job" and
      // breaking is what leaked 24 allocations through this function until
      // AddressSanitizer said so: the job is ours either way, and the only
      // thing the status decides is whether its bytes are usable.
      gcomp_block_job_t * base = NULL;
      const gcomp_status_t got = gcomp_parallel_block_get_result(ctx, &base);
      if (!base) {
        // Nothing handed back at all: that is the helper failing, not a block.
        result = (got == GCOMP_OK) ? GCOMP_ERR_INTERNAL : got;
        break;
      }
      lz4_decode_job_t * job = (lz4_decode_job_t *)base;
      collected++;
      in_flight--;

      if (job->status != GCOMP_OK) {
        result = job->status;
      }
      else if (job->out_size > output_capacity - produced) {
        result = GCOMP_ERR_LIMIT;
      }
      else if (max_output != 0 &&
          (uint64_t)produced + job->out_size > max_output) {
        result = GCOMP_ERR_LIMIT;
      }
      else {
        memcpy(out + produced, job->out, job->out_size);
        if (plan.content_checksum) {
          gcomp_xxhash32_update(&content_hash, out + produced, job->out_size);
        }
        produced += job->out_size;
      }
      gcomp_free(alloc, job->out);
      gcomp_free(alloc, job);
    }

    if (result != GCOMP_OK) {
      break;
    }

    const uint64_t frame_output = (uint64_t)(produced - frame_output_start);

    // The same checks the ordinary decoder makes, in the same place.
    if (plan.has_content_size && frame_output != plan.content_size) {
      result = GCOMP_ERR_CORRUPT;
      break;
    }
    if (plan.content_checksum) {
      // Four bytes after the EndMark, which is the last four of the frame.
      const uint32_t expected =
          gcomp_read_le32(in + frame.offset + frame.size - 4u);
      if (gcomp_xxhash32_finalize(&content_hash) != expected) {
        result = GCOMP_ERR_CORRUPT;
        break;
      }
    }
    if (max_ratio != 0 && frame.size > 0) {
      if (gcomp_limits_check_expansion_ratio(
              frame.size, frame_output, max_ratio) != GCOMP_OK) {
        result = GCOMP_ERR_LIMIT;
        break;
      }
    }
    i++;
  }

  // Anything still in flight when an error broke the loop above owns a buffer
  // this function allocated, and destroying the context does not free it. The
  // sequencer hands jobs back in submission order whether or not anyone still
  // wants them, so draining is just collecting the rest and throwing them
  // away.
  while (in_flight > 0) {
    gcomp_block_job_t * base = NULL;
    // The status is deliberately ignored: these jobs are being thrown away,
    // and a failed one still owns the buffer this function gave it.
    (void)gcomp_parallel_block_get_result(ctx, &base);
    if (!base) {
      break;
    }
    lz4_decode_job_t * job = (lz4_decode_job_t *)base;
    gcomp_free(alloc, job->out);
    gcomp_free(alloc, job);
    in_flight--;
  }

  gcomp_parallel_block_wait(ctx);
  gcomp_parallel_block_destroy(ctx);
  gcomp_free(alloc, ev);

  if (result != GCOMP_OK) {
    *output_size_out = 0;
    return result;
  }
  *output_size_out = produced;
  return GCOMP_OK;
}
