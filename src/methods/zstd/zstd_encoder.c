/**
 * @file zstd_encoder.c
 *
 * Zstandard encoder implementation for the Ghoti.io Compress library.
 *
 * This file implements the streaming Zstd encoder, which produces
 * Zstandard frame format output. The encoder supports both single-threaded
 * and parallel compression modes.
 *
 * ## State Machine (Single-Threaded Mode)
 *
 * The encoder progresses through these stages:
 * 1. HEADER: Write frame header (magic + header descriptor)
 * 2. BLOCKS: Write compressed data blocks
 * 3. CHECKSUM: Write content checksum (if enabled)
 * 4. DONE: Frame complete
 *
 * ## Parallel Mode (threads.count > 1)
 *
 * When parallel mode is enabled via `threads.count > 1`:
 *
 * ```
 * ┌────────────────────────────────────────────────────────────────────┐
 * │                    Parallel Encoder Flow                           │
 * │                                                                    │
 * │  update() called with input:                                       │
 * │  ┌───────────────────────────────────────────────────────────────┐ │
 * │  │ 1. Drain any buffered parallel output                         │ │
 * │  │ 2. Collect any ready parallel results                         │ │
 * │  │ 3. Accumulate input into current job's buffer                 │ │
 * │  │ 4. When job buffer full (job_size), submit to parallel ctx    │ │
 * │  │ 5. Allocate new job for next input                            │ │
 * │  └───────────────────────────────────────────────────────────────┘ │
 * │                                                                    │
 * │  finish() called:                                                  │
 * │  ┌───────────────────────────────────────────────────────────────┐ │
 * │  │ 1. Submit any partial job with remaining input                │ │
 * │  │ 2. Wait for all parallel jobs to complete                     │ │
 * │  │ 3. Collect all remaining results in order                     │ │
 * │  │ 4. Set stage to DONE                                          │ │
 * │  └───────────────────────────────────────────────────────────────┘ │
 * │                                                                    │
 * │  Output: Concatenated independent zstd frames (one per job)        │
 * └────────────────────────────────────────────────────────────────────┘
 * ```
 *
 * Key design decisions:
 * - Each parallel job produces a complete, independent zstd frame
 * - Frames are collected in submission order (guaranteed ordering)
 * - Output is valid concatenated zstd stream (decode with zstd.concat=true)
 * - Single-threaded mode (threads.count <= 1) produces a single frame
 *
 * ## Memory Management
 *
 * In parallel mode, the encoder maintains:
 * - `parallel_ctx`: The parallel compression context
 * - `parallel_job`: Current job being filled with input
 * - `parallel_output_buf`: Buffer for collecting compressed output
 *
 * Memory is tracked via `gcomp_memory_tracker_t` and respects
 * `limits.max_memory_bytes`.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include "zstd_internal.h"
#include "zstd_parallel.h"
#include <string.h>

//
// Helper Functions
//

/**
 * @brief Get window log from compression level.
 */
uint8_t zstd_level_to_window_log(int level) {
  // Simple mapping - higher levels get larger windows
  if (level <= 3) {
    return 17; // 128 KB
  }
  else if (level <= 6) {
    return 19; // 512 KB
  }
  else if (level <= 10) {
    return 21; // 2 MB
  }
  else if (level <= 16) {
    return 23; // 8 MB
  }
  else {
    return 25; // 32 MB
  }
}

/**
 * @brief Compute window size from window log.
 */
uint32_t zstd_window_log_to_size(uint8_t window_log) {
  if (window_log < ZSTD_WINDOW_LOG_MIN || window_log > ZSTD_WINDOW_LOG_MAX) {
    return 0;
  }
  return 1U << window_log;
}

//
// Parallel Mode Helpers
//

/**
 * @brief Drain pending parallel output to the output buffer.
 *
 * Copies data from the internal parallel output buffer to the caller's output.
 *
 * @param state Encoder state
 * @param output Output buffer
 * @return true if all pending output was drained, false if more space needed
 */
static bool zstd_encoder_drain_parallel_output(
    zstd_encoder_state_t * state, gcomp_buffer_t * output) {
  uint8_t * out_ptr = (uint8_t *)output->data;
  while (state->parallel_output_buf_pos < state->parallel_output_buf_len &&
      output->used < output->size) {
    out_ptr[output->used++] =
        state->parallel_output_buf[state->parallel_output_buf_pos++];
  }
  return state->parallel_output_buf_pos >= state->parallel_output_buf_len;
}

/**
 * @brief Collect completed parallel jobs and copy to output buffer.
 *
 * Checks for completed jobs and copies their output to either the parallel
 * output buffer (for buffering) or directly to the output.
 *
 * @param state Encoder state
 * @param output Output buffer
 * @return GCOMP_OK on success, error code on failure
 */
static gcomp_status_t zstd_encoder_collect_parallel_results(
    zstd_encoder_state_t * state, gcomp_buffer_t * output) {
  uint8_t * out_ptr = (uint8_t *)output->data;

  // Check if there are completed results
  while (zstd_parallel_result_ready(state->parallel_ctx)) {
    zstd_parallel_job_t * completed = NULL;
    gcomp_status_t status =
        zstd_parallel_get_result(state->parallel_ctx, &completed);
    if (status != GCOMP_OK) {
      return status;
    }

    // Check if job had an error
    if (completed->base.status == GCOMP_JOB_ERROR ||
        completed->base.result != GCOMP_OK) {
      status = completed->base.result;
      zstd_parallel_free_job(state->parallel_ctx, completed);
      return status;
    }

    // Copy job output to our buffer or directly to output
    size_t output_len = completed->base.output_size;
    const uint8_t * output_data = completed->base.output;

    // First, try to copy directly to output
    size_t direct_copy = output->size - output->used;
    if (direct_copy > output_len) {
      direct_copy = output_len;
    }
    if (direct_copy > 0) {
      memcpy(out_ptr + output->used, output_data, direct_copy);
      output->used += direct_copy;
    }

    // If there's remaining data, buffer it
    size_t remaining = output_len - direct_copy;
    if (remaining > 0) {
      // Ensure buffer has space
      if (remaining > state->parallel_output_buf_cap) {
        // Buffer too small - this shouldn't happen with proper sizing
        zstd_parallel_free_job(state->parallel_ctx, completed);
        return GCOMP_ERR_INTERNAL;
      }
      memcpy(state->parallel_output_buf, output_data + direct_copy, remaining);
      state->parallel_output_buf_pos = 0;
      state->parallel_output_buf_len = remaining;
    }

    zstd_parallel_free_job(state->parallel_ctx, completed);

    // If we buffered data, stop collecting more results
    if (remaining > 0) {
      break;
    }
  }

  return GCOMP_OK;
}

/**
 * @brief Submit the current parallel job and allocate a new one.
 *
 * @param state Encoder state
 * @return GCOMP_OK on success, error code on failure
 */
static gcomp_status_t zstd_encoder_submit_parallel_job(
    zstd_encoder_state_t * state) {
  if (!state->parallel_job) {
    return GCOMP_ERR_INTERNAL;
  }

  // Submit the job
  gcomp_status_t status =
      zstd_parallel_submit(state->parallel_ctx, state->parallel_job);
  if (status != GCOMP_OK) {
    return status;
  }

  state->parallel_job = NULL; // Job is now owned by parallel context

  // Allocate a new job for the next chunk
  status = zstd_parallel_alloc_job(state->parallel_ctx, &state->parallel_job);
  return status;
}

/**
 * @brief Process input in parallel mode.
 *
 * Accumulates input until job_size, then submits jobs for compression.
 *
 * @param encoder Encoder
 * @param state Encoder state
 * @param input Input buffer
 * @param output Output buffer
 * @return GCOMP_OK on success, error code on failure
 */
static gcomp_status_t zstd_encoder_update_parallel(gcomp_encoder_t * encoder,
    zstd_encoder_state_t * state, gcomp_buffer_t * input,
    gcomp_buffer_t * output) {
  (void)encoder;
  gcomp_status_t status;

  // First, drain any pending output from previous results
  if (!zstd_encoder_drain_parallel_output(state, output)) {
    return GCOMP_OK; // Need more output space
  }

  // Collect any ready results
  status = zstd_encoder_collect_parallel_results(state, output);
  if (status != GCOMP_OK) {
    gcomp_encoder_set_error(encoder, status, "parallel compression failed");
    return status;
  }

  // Check if we need to drain again after collecting
  if (!zstd_encoder_drain_parallel_output(state, output)) {
    return GCOMP_OK; // Need more output space
  }

  // Get job size from parallel context
  uint64_t job_size = zstd_parallel_get_job_size(state->parallel_ctx);

  // Accumulate input into current job
  while (input->used < input->size) {
    if (!state->parallel_job) {
      gcomp_encoder_set_error(
          encoder, GCOMP_ERR_INTERNAL, "internal error: no parallel job");
      return GCOMP_ERR_INTERNAL;
    }

    // How much space in current job?
    size_t job_space = job_size - state->parallel_job->base.input_size;
    size_t input_avail = input->size - input->used;
    size_t to_copy = (input_avail < job_space) ? input_avail : job_space;

    if (to_copy > 0) {
      const uint8_t * in_ptr = (const uint8_t *)input->data;
      // Cast away const - the input buffer was allocated by alloc_job and is
      // mutable during filling. It's const in the job structure because during
      // compression it's read-only.
      uint8_t * job_input = (uint8_t *)state->parallel_job->base.input;
      memcpy(job_input + state->parallel_job->base.input_size,
          in_ptr + input->used, to_copy);
      state->parallel_job->base.input_size += to_copy;
      input->used += to_copy;
      state->total_input_bytes += to_copy;
    }

    // If job is full, submit it
    if (state->parallel_job->base.input_size >= job_size) {
      status = zstd_encoder_submit_parallel_job(state);
      if (status != GCOMP_OK) {
        gcomp_encoder_set_error(encoder, status, "parallel job submit failed");
        return status;
      }

      // Try to collect results and drain output
      status = zstd_encoder_collect_parallel_results(state, output);
      if (status != GCOMP_OK) {
        gcomp_encoder_set_error(encoder, status, "parallel compression failed");
        return status;
      }

      if (!zstd_encoder_drain_parallel_output(state, output)) {
        return GCOMP_OK; // Need more output space
      }
    }
  }

  return GCOMP_OK;
}

/**
 * @brief Finish parallel encoding.
 *
 * Submits final partial job and collects all remaining results.
 *
 * @param encoder Encoder
 * @param state Encoder state
 * @param output Output buffer
 * @return GCOMP_OK when complete, error code on failure
 */
static gcomp_status_t zstd_encoder_finish_parallel(gcomp_encoder_t * encoder,
    zstd_encoder_state_t * state, gcomp_buffer_t * output) {
  gcomp_status_t status;

  // First, drain any pending output
  if (!zstd_encoder_drain_parallel_output(state, output)) {
    return GCOMP_OK; // Need more output space
  }

  // Submit final partial job if not already done
  if (!state->blocks_finished && state->parallel_job) {
    // Only submit if there's data in the job
    if (state->parallel_job->base.input_size > 0) {
      status = zstd_parallel_submit(state->parallel_ctx, state->parallel_job);
      if (status != GCOMP_OK) {
        gcomp_encoder_set_error(encoder, status, "parallel job submit failed");
        return status;
      }
      state->parallel_job = NULL; // Job is now owned by parallel context
    }
    else {
      // No data in job, just free it
      zstd_parallel_free_job(state->parallel_ctx, state->parallel_job);
      state->parallel_job = NULL;
    }
    state->blocks_finished = true;
  }

  // Wait for all pending jobs and collect results
  status = zstd_parallel_wait(state->parallel_ctx);
  if (status != GCOMP_OK) {
    gcomp_encoder_set_error(encoder, status, "parallel wait failed");
    return status;
  }

  // Collect all remaining results
  status = zstd_encoder_collect_parallel_results(state, output);
  if (status != GCOMP_OK) {
    gcomp_encoder_set_error(encoder, status, "parallel compression failed");
    return status;
  }

  // Drain any buffered output
  if (!zstd_encoder_drain_parallel_output(state, output)) {
    return GCOMP_OK; // Need more output space
  }

  // Check if there are still pending results
  if (zstd_parallel_pending_count(state->parallel_ctx) > 0) {
    // Shouldn't happen after wait, but be defensive
    return GCOMP_OK;
  }

  // All done
  state->stage = ZSTD_ENC_STAGE_DONE;
  return GCOMP_OK;
}

//
// Initialization
//

gcomp_status_t zstd_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  (void)registry;

  if (!encoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Get allocator from registry
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);
  gcomp_status_t status = GCOMP_OK;

  // Allocate state structure
  zstd_encoder_state_t * state = gcomp_calloc(alloc, 1, sizeof(*state));
  if (!state) {
    gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "failed to allocate state");
    return GCOMP_ERR_MEMORY;
  }
  state->allocator = alloc;

  // Initialize memory tracker
  gcomp_memory_track_alloc(&state->mem_tracker, sizeof(*state));

  // Read options
  int64_t level = ZSTD_LEVEL_DEFAULT;
  int checksum = 0;
  uint64_t window_log = 0;
  uint64_t content_size = 0;
  bool content_size_present = false;
  uint64_t max_memory = ZSTD_DEFAULT_MAX_MEMORY_BYTES;
  uint64_t num_threads = 1;
  uint64_t job_size = 0;
  const void * dict_opt = NULL;
  size_t dict_opt_size = 0;
  uint64_t dict_id_opt = 0;
  bool dict_id_opt_set = false;

  if (options) {
    gcomp_options_get_int64(options, "zstd.level", &level);
    gcomp_options_get_bool(options, "zstd.checksum", &checksum);
    gcomp_options_get_uint64(options, "zstd.window_log", &window_log);
    if (gcomp_options_get_uint64(options, "zstd.content_size", &content_size) ==
        GCOMP_OK) {
      content_size_present = true;
    }
    gcomp_options_get_uint64(options, "limits.max_memory_bytes", &max_memory);
    gcomp_options_get_uint64(options, "threads.count", &num_threads);
    gcomp_options_get_uint64(options, "zstd.job_size", &job_size);
    if (gcomp_options_get_bytes(options, "zstd.dictionary", &dict_opt,
            &dict_opt_size) == GCOMP_OK &&
        dict_opt != NULL && dict_opt_size > 0) {
      if (gcomp_options_get_uint64(
              options, "zstd.dictionary_id", &dict_id_opt) == GCOMP_OK) {
        dict_id_opt_set = true;
      }
    }
  }

  // Store threading options
  state->num_threads = (uint32_t)num_threads;
  state->job_size = job_size;

  // Validate and set compression level
  if (level < ZSTD_LEVEL_MIN || level > ZSTD_LEVEL_MAX) {
    status = GCOMP_ERR_INVALID_ARG;
    gcomp_encoder_set_error(encoder, status,
        "zstd.level must be between %d and %d", ZSTD_LEVEL_MIN, ZSTD_LEVEL_MAX);
    goto cleanup;
  }
  state->compression_level = (int)level;

  // Determine window log
  uint8_t effective_window_log;
  if (window_log == 0) {
    effective_window_log = zstd_level_to_window_log(state->compression_level);
  }
  else {
    if (window_log < ZSTD_WINDOW_LOG_MIN || window_log > ZSTD_WINDOW_LOG_MAX) {
      status = GCOMP_ERR_INVALID_ARG;
      gcomp_encoder_set_error(encoder, status,
          "zstd.window_log must be 0 (auto) or between %d and %d",
          ZSTD_WINDOW_LOG_MIN, ZSTD_WINDOW_LOG_MAX);
      goto cleanup;
    }
    effective_window_log = (uint8_t)window_log;
  }

  // Set up frame header
  state->header.window_log = effective_window_log;
  state->header.window_size = zstd_window_log_to_size(effective_window_log);
  state->header.content_checksum = (checksum != 0);
  state->header.content_size_present = content_size_present;
  state->header.content_size = content_size;
  state->header.single_segment =
      content_size_present && (content_size <= state->header.window_size);
  state->checksum_enabled = (checksum != 0);
  state->max_memory_bytes = max_memory;

  if (dict_opt != NULL && dict_opt_size > 0) {
    state->dict_bytes = gcomp_malloc(alloc, dict_opt_size);
    if (!state->dict_bytes) {
      status = GCOMP_ERR_MEMORY;
      gcomp_encoder_set_error(encoder, status,
          "failed to allocate %zu bytes for dictionary", dict_opt_size);
      goto cleanup;
    }
    memcpy(state->dict_bytes, dict_opt, dict_opt_size);
    state->dict_size = dict_opt_size;
    gcomp_memory_track_alloc(&state->mem_tracker, dict_opt_size);
    status = zstd_dict_parse(
        state->dict_bytes, state->dict_size, alloc, &state->dict_parsed);
    if (status != GCOMP_OK) {
      gcomp_encoder_set_error(encoder, status, "dictionary parse failed");
      gcomp_free(alloc, state->dict_bytes);
      state->dict_bytes = NULL;
      state->dict_size = 0;
      goto cleanup;
    }
    state->header.dict_id =
        dict_id_opt_set ? (uint32_t)dict_id_opt : state->dict_parsed.dict_id;
    if (state->dict_parsed.has_entropy_tables) {
      state->rep_offset_1 = state->dict_parsed.rep_offset_1;
      state->rep_offset_2 = state->dict_parsed.rep_offset_2;
      state->rep_offset_3 = state->dict_parsed.rep_offset_3;
    }
    else {
      state->rep_offset_1 = ZSTD_REP_OFFSET_1_INIT;
      state->rep_offset_2 = ZSTD_REP_OFFSET_2_INIT;
      state->rep_offset_3 = ZSTD_REP_OFFSET_3_INIT;
    }
    state->dict_block_buffer_capacity =
        state->dict_parsed.content_size + ZSTD_BLOCK_SIZE_MAX;
    state->dict_block_buffer =
        gcomp_malloc(alloc, state->dict_block_buffer_capacity);
    if (!state->dict_block_buffer) {
      status = GCOMP_ERR_MEMORY;
      gcomp_encoder_set_error(encoder, status,
          "failed to allocate %zu bytes for dictionary block buffer",
          (size_t)state->dict_block_buffer_capacity);
      goto cleanup;
    }
    gcomp_memory_track_alloc(
        &state->mem_tracker, state->dict_block_buffer_capacity);
  }
  else {
    state->rep_offset_1 = ZSTD_REP_OFFSET_1_INIT;
    state->rep_offset_2 = ZSTD_REP_OFFSET_2_INIT;
    state->rep_offset_3 = ZSTD_REP_OFFSET_3_INIT;
  }

  // Initialize content hash if checksum enabled
  if (checksum) {
    gcomp_xxhash64_reset(&state->content_hash, 0);
  }

  // Allocate block buffer (use calloc to satisfy valgrind - hash function
  // may read bytes before they're fully populated during streaming)
  size_t block_buffer_size = ZSTD_BLOCK_SIZE_MAX;
  state->block_buffer = gcomp_calloc(alloc, 1, block_buffer_size);
  if (!state->block_buffer) {
    status = GCOMP_ERR_MEMORY;
    gcomp_encoder_set_error(encoder, status,
        "failed to allocate %zu bytes for block buffer", block_buffer_size);
    goto cleanup;
  }
  state->block_buffer_capacity = block_buffer_size;
  gcomp_memory_track_alloc(&state->mem_tracker, block_buffer_size);

  // Allocate compressed buffer (may be larger than input for incompressible)
  size_t compressed_buffer_size =
      block_buffer_size + ZSTD_BLOCK_HEADER_SIZE + 256; // Some overhead
  state->compressed_buffer = gcomp_malloc(alloc, compressed_buffer_size);
  if (!state->compressed_buffer) {
    status = GCOMP_ERR_MEMORY;
    gcomp_encoder_set_error(encoder, status,
        "failed to allocate %zu bytes for compressed buffer",
        compressed_buffer_size);
    goto cleanup;
  }
  state->compressed_buffer_capacity = compressed_buffer_size;
  gcomp_memory_track_alloc(&state->mem_tracker, compressed_buffer_size);

  // Allocate match finder
  state->match_finder = gcomp_calloc(alloc, 1, sizeof(zstd_match_finder_t));
  if (!state->match_finder) {
    status = GCOMP_ERR_MEMORY;
    gcomp_encoder_set_error(encoder, status, "failed to allocate match finder");
    goto cleanup;
  }
  gcomp_memory_track_alloc(&state->mem_tracker, sizeof(zstd_match_finder_t));

  status = zstd_mf_init(state->match_finder, alloc, state->compression_level,
      state->header.window_size, &state->mem_tracker);
  if (status != GCOMP_OK) {
    gcomp_encoder_set_error(encoder, status, "match finder init failed");
    goto cleanup;
  }

  // Allocate sequence buffer
  // Maximum sequences = block_size / min_match_length
  state->seq_buffer_capacity = block_buffer_size / 3;
  state->seq_buffer =
      gcomp_malloc(alloc, state->seq_buffer_capacity * sizeof(zstd_sequence_t));
  if (!state->seq_buffer) {
    status = GCOMP_ERR_MEMORY;
    gcomp_encoder_set_error(encoder, status,
        "failed to allocate %zu bytes for sequence buffer",
        state->seq_buffer_capacity * sizeof(zstd_sequence_t));
    goto cleanup;
  }
  gcomp_memory_track_alloc(&state->mem_tracker,
      state->seq_buffer_capacity * sizeof(zstd_sequence_t));

  // Allocate literals buffer
  state->literals_buffer_capacity = block_buffer_size;
  state->literals_buffer = gcomp_malloc(alloc, state->literals_buffer_capacity);
  if (!state->literals_buffer) {
    status = GCOMP_ERR_MEMORY;
    gcomp_encoder_set_error(encoder, status,
        "failed to allocate %zu bytes for literals buffer",
        state->literals_buffer_capacity);
    goto cleanup;
  }
  gcomp_memory_track_alloc(
      &state->mem_tracker, state->literals_buffer_capacity);

  // Check memory limits
  if (state->mem_tracker.current_bytes > max_memory) {
    status = GCOMP_ERR_LIMIT;
    gcomp_encoder_set_error(encoder, status,
        "memory limit exceeded during encoder init (%zu bytes, limit %zu)",
        (size_t)state->mem_tracker.current_bytes, (size_t)max_memory);
    goto cleanup;
  }

  // Initialize parallel context if threads > 1
  if (state->num_threads > 1) {
    zstd_parallel_config_t pconfig = {
        .num_threads = state->num_threads,
        .max_in_flight = 0, // Auto
        .job_size = state->job_size,
        .checksum_enabled = state->checksum_enabled,
        .compression_level = state->compression_level,
        .window_log = effective_window_log,
        .max_memory_bytes = max_memory,
        .allocator = alloc,
        .mem_tracker = &state->mem_tracker,
    };
    status = zstd_parallel_create(&pconfig, &state->parallel_ctx);
    if (status != GCOMP_OK) {
      gcomp_encoder_set_error(
          encoder, status, "parallel context creation failed");
      goto cleanup;
    }

    // Allocate first job for parallel mode
    status = zstd_parallel_alloc_job(state->parallel_ctx, &state->parallel_job);
    if (status != GCOMP_OK) {
      gcomp_encoder_set_error(
          encoder, status, "failed to allocate parallel job");
      goto cleanup;
    }

    // Allocate parallel output buffer for collecting results
    // Size to hold at least one job's output with some margin
    uint64_t actual_job_size = zstd_parallel_get_job_size(state->parallel_ctx);
    state->parallel_output_buf_cap =
        actual_job_size + 4096; // Extra for frame overhead
    state->parallel_output_buf =
        gcomp_malloc(alloc, state->parallel_output_buf_cap);
    if (!state->parallel_output_buf) {
      status = GCOMP_ERR_MEMORY;
      gcomp_encoder_set_error(encoder, status,
          "failed to allocate %zu bytes for parallel output buffer",
          (size_t)state->parallel_output_buf_cap);
      goto cleanup;
    }
    gcomp_memory_track_alloc(
        &state->mem_tracker, state->parallel_output_buf_cap);

    // Set initial stage - parallel mode doesn't use HEADER stage
    state->stage = ZSTD_ENC_STAGE_BLOCKS;
  }
  else {
    // Single-threaded mode: Set initial stage and build header
    state->stage = ZSTD_ENC_STAGE_HEADER;

    // Build frame header
    status = zstd_write_frame_header(&state->header, state->header_buf,
        sizeof(state->header_buf), &state->header_len);
    if (status != GCOMP_OK) {
      gcomp_encoder_set_error(encoder, status, "frame header write failed");
      goto cleanup;
    }
  }

  encoder->method_state = state;
  return GCOMP_OK;

cleanup:
  if (state) {
    zstd_dict_destroy(&state->dict_parsed);
    if (state->dict_bytes) {
      gcomp_memory_track_free(&state->mem_tracker, state->dict_size);
      gcomp_free(alloc, state->dict_bytes);
    }
    if (state->dict_block_buffer) {
      gcomp_memory_track_free(
          &state->mem_tracker, state->dict_block_buffer_capacity);
      gcomp_free(alloc, state->dict_block_buffer);
    }
    if (state->block_buffer) {
      gcomp_free(alloc, state->block_buffer);
    }
    if (state->compressed_buffer) {
      gcomp_free(alloc, state->compressed_buffer);
    }
    if (state->match_finder) {
      zstd_mf_destroy(state->match_finder, alloc, NULL);
      gcomp_free(alloc, state->match_finder);
    }
    if (state->seq_buffer) {
      gcomp_free(alloc, state->seq_buffer);
    }
    if (state->literals_buffer) {
      gcomp_free(alloc, state->literals_buffer);
    }
    if (state->parallel_output_buf) {
      gcomp_free(alloc, state->parallel_output_buf);
    }
    if (state->parallel_job) {
      zstd_parallel_free_job(state->parallel_ctx, state->parallel_job);
    }
    if (state->parallel_ctx) {
      zstd_parallel_destroy(state->parallel_ctx);
    }
    gcomp_free(alloc, state);
  }
  return status;
}

//
// Destroy
//

void zstd_encoder_destroy(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return;
  }

  zstd_encoder_state_t * state = encoder->method_state;
  const gcomp_allocator_t * alloc = state->allocator;

  if (state->block_buffer) {
    gcomp_memory_track_free(&state->mem_tracker, state->block_buffer_capacity);
    gcomp_free(alloc, state->block_buffer);
  }
  if (state->compressed_buffer) {
    gcomp_memory_track_free(
        &state->mem_tracker, state->compressed_buffer_capacity);
    gcomp_free(alloc, state->compressed_buffer);
  }
  if (state->match_finder) {
    zstd_mf_destroy(state->match_finder, alloc, &state->mem_tracker);
    gcomp_memory_track_free(&state->mem_tracker, sizeof(zstd_match_finder_t));
    gcomp_free(alloc, state->match_finder);
  }
  if (state->dict_block_buffer) {
    gcomp_memory_track_free(
        &state->mem_tracker, state->dict_block_buffer_capacity);
    gcomp_free(alloc, state->dict_block_buffer);
  }
  zstd_dict_destroy(&state->dict_parsed);
  if (state->dict_bytes) {
    gcomp_memory_track_free(&state->mem_tracker, state->dict_size);
    gcomp_free(alloc, state->dict_bytes);
  }
  if (state->seq_buffer) {
    gcomp_memory_track_free(&state->mem_tracker,
        state->seq_buffer_capacity * sizeof(zstd_sequence_t));
    gcomp_free(alloc, state->seq_buffer);
  }
  if (state->literals_buffer) {
    gcomp_memory_track_free(
        &state->mem_tracker, state->literals_buffer_capacity);
    gcomp_free(alloc, state->literals_buffer);
  }
  if (state->parallel_output_buf) {
    gcomp_memory_track_free(
        &state->mem_tracker, state->parallel_output_buf_cap);
    gcomp_free(alloc, state->parallel_output_buf);
  }
  if (state->parallel_job) {
    zstd_parallel_free_job(state->parallel_ctx, state->parallel_job);
  }
  if (state->parallel_ctx) {
    zstd_parallel_destroy(state->parallel_ctx);
  }
  gcomp_memory_track_free(&state->mem_tracker, sizeof(*state));
  gcomp_free(alloc, state);
  encoder->method_state = NULL;
}

//
// Update
//

gcomp_status_t zstd_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!encoder || !encoder->method_state || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Buffer validation
  if (input->size > 0 && !input->data) {
    gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "input data is NULL with size > 0");
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output->size > 0 && !output->data) {
    gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "output data is NULL with size > 0");
    return GCOMP_ERR_INVALID_ARG;
  }

  zstd_encoder_state_t * state = encoder->method_state;
  const uint8_t * in_ptr = (const uint8_t *)input->data;
  uint8_t * out_ptr = (uint8_t *)output->data;

  if (state->stage == ZSTD_ENC_STAGE_ERROR) {
    gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INTERNAL, "encoder in error state");
    return GCOMP_ERR_INTERNAL;
  }
  if (state->stage == ZSTD_ENC_STAGE_DONE) {
    return GCOMP_OK;
  }

  // Parallel mode: use dedicated parallel update path
  if (state->parallel_ctx) {
    return zstd_encoder_update_parallel(encoder, state, input, output);
  }

  // Single-threaded mode: write header if needed
  if (state->stage == ZSTD_ENC_STAGE_HEADER) {
    while (
        state->header_pos < state->header_len && output->used < output->size) {
      out_ptr[output->used++] = state->header_buf[state->header_pos++];
    }
    if (state->header_pos >= state->header_len) {
      state->stage = ZSTD_ENC_STAGE_BLOCKS;
    }
    else {
      return GCOMP_OK; // Need more output space
    }
  }

  // Process input in BLOCKS stage
  if (state->stage == ZSTD_ENC_STAGE_BLOCKS) {
    // First, drain any pending compressed output
    while (state->compressed_buffer_pos < state->compressed_buffer_len &&
        output->used < output->size) {
      out_ptr[output->used++] =
          state->compressed_buffer[state->compressed_buffer_pos++];
    }
    if (state->compressed_buffer_pos < state->compressed_buffer_len) {
      return GCOMP_OK; // Need more output space
    }

    // Buffer input data
    while (input->used < input->size) {
      size_t to_copy = input->size - input->used;
      size_t space = state->block_buffer_capacity - state->block_buffer_pos;
      if (to_copy > space) {
        to_copy = space;
      }
      memcpy(state->block_buffer + state->block_buffer_pos,
          in_ptr + input->used, to_copy);
      state->block_buffer_pos += to_copy;
      input->used += to_copy;
      state->total_input_bytes += to_copy;

      // Update content hash if enabled
      if (state->checksum_enabled) {
        gcomp_xxhash64_update(&state->content_hash,
            state->block_buffer + state->block_buffer_pos - to_copy, to_copy);
      }

      // If block buffer is full, compress and output
      if (state->block_buffer_pos >= state->block_buffer_capacity) {
        uint8_t block_type;
        size_t compressed_len;
        size_t input_len = state->block_buffer_pos;
        gcomp_status_t status = zstd_block_compress(state, state->block_buffer,
            input_len, state->compressed_buffer + 3,
            state->compressed_buffer_capacity - 3, &compressed_len,
            &block_type);
        if (status != GCOMP_OK) {
          state->stage = ZSTD_ENC_STAGE_ERROR;
          gcomp_encoder_set_error(encoder, status, "block compression failed");
          return status;
        }

        // Write block header
        // For RLE blocks, the header size field is the regenerated size
        uint32_t header_size = (block_type == ZSTD_BLOCK_TYPE_RLE)
            ? (uint32_t)input_len
            : (uint32_t)compressed_len;
        zstd_write_block_header(
            state->compressed_buffer, false, block_type, header_size);
        state->compressed_buffer_len = 3 + compressed_len;
        state->compressed_buffer_pos = 0;

        // Output compressed block
        while (state->compressed_buffer_pos < state->compressed_buffer_len &&
            output->used < output->size) {
          out_ptr[output->used++] =
              state->compressed_buffer[state->compressed_buffer_pos++];
        }

        state->block_buffer_pos = 0;

        // If we couldn't output everything, return and continue later
        if (state->compressed_buffer_pos < state->compressed_buffer_len) {
          return GCOMP_OK;
        }
      }
    }
  }

  return GCOMP_OK;
}

//
// Finish
//

gcomp_status_t zstd_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  if (!encoder || !encoder->method_state || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Buffer validation
  if (output->size > 0 && !output->data) {
    gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "output data is NULL with size > 0");
    return GCOMP_ERR_INVALID_ARG;
  }

  zstd_encoder_state_t * state = encoder->method_state;
  uint8_t * out_ptr = (uint8_t *)output->data;

  if (state->stage == ZSTD_ENC_STAGE_ERROR) {
    gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INTERNAL, "encoder in error state");
    return GCOMP_ERR_INTERNAL;
  }
  if (state->stage == ZSTD_ENC_STAGE_DONE) {
    return GCOMP_OK;
  }

  state->finish_called = true;

  // Parallel mode: use dedicated parallel finish path
  if (state->parallel_ctx) {
    return zstd_encoder_finish_parallel(encoder, state, output);
  }

  // Single-threaded mode: drain any remaining header
  if (state->stage == ZSTD_ENC_STAGE_HEADER) {
    while (
        state->header_pos < state->header_len && output->used < output->size) {
      out_ptr[output->used++] = state->header_buf[state->header_pos++];
    }
    if (state->header_pos >= state->header_len) {
      state->stage = ZSTD_ENC_STAGE_BLOCKS;
    }
    else {
      return GCOMP_OK;
    }
  }

  // Flush remaining buffered data as final block
  if (state->stage == ZSTD_ENC_STAGE_BLOCKS) {
    // Drain pending compressed output
    while (state->compressed_buffer_pos < state->compressed_buffer_len &&
        output->used < output->size) {
      out_ptr[output->used++] =
          state->compressed_buffer[state->compressed_buffer_pos++];
    }
    if (state->compressed_buffer_pos < state->compressed_buffer_len) {
      return GCOMP_OK;
    }

    if (!state->blocks_finished) {
      // Compress remaining data as final block
      if (state->block_buffer_pos > 0) {
        uint8_t block_type;
        size_t compressed_len;
        size_t input_len = state->block_buffer_pos;
        gcomp_status_t status = zstd_block_compress(state, state->block_buffer,
            input_len, state->compressed_buffer + 3,
            state->compressed_buffer_capacity - 3, &compressed_len,
            &block_type);
        if (status != GCOMP_OK) {
          state->stage = ZSTD_ENC_STAGE_ERROR;
          gcomp_encoder_set_error(
              encoder, status, "final block compression failed");
          return status;
        }

        // Write block header (last block)
        // For RLE blocks, the header size field is the regenerated size
        // (input_len) For RAW blocks, it's the compressed size (same as
        // input_len)
        uint32_t header_size = (block_type == ZSTD_BLOCK_TYPE_RLE)
            ? (uint32_t)input_len
            : (uint32_t)compressed_len;
        zstd_write_block_header(
            state->compressed_buffer, true, block_type, header_size);
        state->compressed_buffer_len = 3 + compressed_len;
        state->compressed_buffer_pos = 0;
        state->block_buffer_pos = 0;
      }
      else {
        // No remaining data - write empty last block
        zstd_write_block_header(
            state->compressed_buffer, true, ZSTD_BLOCK_TYPE_RAW, 0);
        state->compressed_buffer_len = 3;
        state->compressed_buffer_pos = 0;
      }

      state->blocks_finished = true;
    }

    // Output final block
    while (state->compressed_buffer_pos < state->compressed_buffer_len &&
        output->used < output->size) {
      out_ptr[output->used++] =
          state->compressed_buffer[state->compressed_buffer_pos++];
    }
    if (state->compressed_buffer_pos < state->compressed_buffer_len) {
      return GCOMP_OK;
    }

    // Prepare checksum if enabled
    if (state->checksum_enabled) {
      uint64_t hash = gcomp_xxhash64_finalize(&state->content_hash);
      gcomp_write_le32(state->checksum_buf, (uint32_t)(hash & 0xFFFFFFFF));
      state->checksum_pos = 0;
      state->stage = ZSTD_ENC_STAGE_CHECKSUM;
    }
    else {
      state->stage = ZSTD_ENC_STAGE_DONE;
      return GCOMP_OK;
    }
  }

  // Output checksum
  if (state->stage == ZSTD_ENC_STAGE_CHECKSUM) {
    while (state->checksum_pos < ZSTD_CONTENT_CHECKSUM_SIZE &&
        output->used < output->size) {
      out_ptr[output->used++] = state->checksum_buf[state->checksum_pos++];
    }
    if (state->checksum_pos >= ZSTD_CONTENT_CHECKSUM_SIZE) {
      state->stage = ZSTD_ENC_STAGE_DONE;
      return GCOMP_OK;
    }
  }

  return GCOMP_OK;
}

//
// Reset
//

gcomp_status_t zstd_encoder_reset(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }

  zstd_encoder_state_t * state = encoder->method_state;
  gcomp_status_t status = GCOMP_OK;

  // Reset counters
  state->total_input_bytes = 0;
  state->finish_called = false;
  state->blocks_finished = false;

  // Reset repeat offsets
  state->rep_offset_1 = ZSTD_REP_OFFSET_1_INIT;
  state->rep_offset_2 = ZSTD_REP_OFFSET_2_INIT;
  state->rep_offset_3 = ZSTD_REP_OFFSET_3_INIT;

  // Reset content hash if enabled
  if (state->checksum_enabled) {
    gcomp_xxhash64_reset(&state->content_hash, 0);
  }

  // Parallel mode reset
  if (state->parallel_ctx) {
    // Reset parallel context
    status = zstd_parallel_reset(state->parallel_ctx);
    if (status != GCOMP_OK) {
      return status;
    }

    // Free and reallocate current job if needed
    if (state->parallel_job) {
      zstd_parallel_free_job(state->parallel_ctx, state->parallel_job);
      state->parallel_job = NULL;
    }
    status = zstd_parallel_alloc_job(state->parallel_ctx, &state->parallel_job);
    if (status != GCOMP_OK) {
      return status;
    }

    // Reset parallel output buffer
    state->parallel_output_buf_pos = 0;
    state->parallel_output_buf_len = 0;

    // Set stage to BLOCKS (parallel mode doesn't use HEADER stage)
    state->stage = ZSTD_ENC_STAGE_BLOCKS;
  }
  else {
    // Single-threaded mode reset
    state->stage = ZSTD_ENC_STAGE_HEADER;

    // Reset positions (retain buffers)
    state->header_pos = 0;
    state->checksum_pos = 0;
    state->block_buffer_pos = 0;
    state->compressed_buffer_pos = 0;
    state->compressed_buffer_len = 0;

    // Reset match finder state (clear hash tables, not free)
    if (state->match_finder) {
      zstd_mf_reset(state->match_finder);
    }

    // Rebuild frame header
    status = zstd_write_frame_header(&state->header, state->header_buf,
        sizeof(state->header_buf), &state->header_len);
  }

  return status;
}
