/**
 * @file limits.c
 *
 * Implementation of safety limits and memory accounting for the Ghoti.io
 * Compress library.
 *
 * ## Overview
 *
 * This module provides safety limits to protect against resource exhaustion
 * from malicious or malformed compressed data. Three main categories of
 * limits are supported:
 *
 * 1. **Output limits** (`limits.max_output_bytes`) - Caps the total
 *    decompressed output size. Default: 512 MiB.
 *
 * 2. **Memory limits** (`limits.max_memory_bytes`) - Caps working memory
 *    used by decoders (state structs, buffers, Huffman tables). Default: 256 MiB.
 *
 * 3. **Expansion ratio limits** (`limits.max_expansion_ratio`) - Caps the
 *    ratio of output bytes to input bytes. Default: 1000x. This specifically
 *    targets "decompression bombs" where a tiny input expands to massive output.
 *
 * ## Expansion Ratio Algorithm
 *
 * The expansion ratio check prevents decompression bombs by tracking:
 * - `input_bytes`: Total compressed bytes consumed by the decoder
 * - `output_bytes`: Total decompressed bytes produced
 *
 * The check enforces: `output_bytes <= ratio_limit * input_bytes`
 *
 * This is computed using multiplication (not division) to avoid precision
 * loss and handle the edge case where `input_bytes == 0`.
 *
 * **Overflow handling**: If `ratio_limit * input_bytes` would overflow
 * `uint64_t`, the effective limit is treated as infinite (the check passes).
 * This is correct because it means the limit is larger than any possible
 * output size.
 *
 * ## Memory Tracking
 *
 * The `gcomp_memory_tracker_t` structure provides opt-in memory accounting
 * for methods that want to track and limit their memory usage. Methods call:
 * - `gcomp_memory_track_alloc()` after allocating memory
 * - `gcomp_memory_track_free()` before freeing memory
 * - `gcomp_memory_check_limit()` to verify usage is within bounds
 *
 * The tracker handles overflow (clamps to SIZE_MAX) and underflow (clamps
 * to 0) gracefully to avoid undefined behavior from arithmetic errors.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/options.h>
#include <stddef.h>
#include <stdint.h>

// Option key names for limits
#define GCOMP_LIMIT_KEY_OUTPUT_MAX "limits.max_output_bytes"
#define GCOMP_LIMIT_KEY_MEMORY_MAX "limits.max_memory_bytes"
#define GCOMP_LIMIT_KEY_WINDOW_MAX "limits.max_window_bytes"
#define GCOMP_LIMIT_KEY_EXPANSION_RATIO_MAX "limits.max_expansion_ratio"

uint64_t gcomp_limits_read_output_max(
    const gcomp_options_t * opts, uint64_t default_val) {
  if (!opts) {
    return default_val;
  }

  uint64_t value;
  gcomp_status_t status =
      gcomp_options_get_uint64(opts, GCOMP_LIMIT_KEY_OUTPUT_MAX, &value);
  if (status == GCOMP_OK) {
    return value;
  }

  return default_val;
}

uint64_t gcomp_limits_read_memory_max(
    const gcomp_options_t * opts, uint64_t default_val) {
  if (!opts) {
    return default_val;
  }

  uint64_t value;
  gcomp_status_t status =
      gcomp_options_get_uint64(opts, GCOMP_LIMIT_KEY_MEMORY_MAX, &value);
  if (status == GCOMP_OK) {
    return value;
  }

  return default_val;
}

uint64_t gcomp_limits_read_window_max(
    const gcomp_options_t * opts, uint64_t default_val) {
  if (!opts) {
    return default_val;
  }

  uint64_t value;
  gcomp_status_t status =
      gcomp_options_get_uint64(opts, GCOMP_LIMIT_KEY_WINDOW_MAX, &value);
  if (status == GCOMP_OK) {
    return value;
  }

  return default_val;
}

uint64_t gcomp_limits_read_expansion_ratio_max(
    const gcomp_options_t * opts, uint64_t default_val) {
  if (!opts) {
    return default_val;
  }

  uint64_t value;
  gcomp_status_t status =
      gcomp_options_get_uint64(opts, GCOMP_LIMIT_KEY_EXPANSION_RATIO_MAX, &value);
  if (status == GCOMP_OK) {
    return value;
  }

  return default_val;
}

gcomp_status_t gcomp_limits_check_output(size_t current, uint64_t limit) {
  // 0 means unlimited
  if (limit == 0) {
    return GCOMP_OK;
  }

  // Check for overflow: if current > limit, we've exceeded
  if (current > limit) {
    return GCOMP_ERR_LIMIT;
  }

  return GCOMP_OK;
}

gcomp_status_t gcomp_limits_check_memory(size_t current, uint64_t limit) {
  // 0 means unlimited
  if (limit == 0) {
    return GCOMP_OK;
  }

  // Check for overflow: if current > limit, we've exceeded
  if (current > limit) {
    return GCOMP_ERR_LIMIT;
  }

  return GCOMP_OK;
}

/**
 * @brief Check if the expansion ratio exceeds the limit.
 *
 * This function implements decompression bomb protection by checking whether
 * the ratio of decompressed output to compressed input exceeds a threshold.
 *
 * ## Algorithm
 *
 * We want to check: `output_bytes / input_bytes > ratio_limit`
 *
 * To avoid floating-point arithmetic and division-by-zero, we rewrite as:
 * `output_bytes > ratio_limit * input_bytes`
 *
 * ## Edge Cases
 *
 * - **ratio_limit == 0**: Unlimited mode, always returns OK.
 * - **input_bytes == 0**: No ratio can be computed yet, returns OK.
 *   This allows the first few bytes of output before any input is consumed
 *   (e.g., if the decoder produces output from the bit buffer before reading
 *   the next input byte).
 * - **Overflow**: If `ratio_limit * input_bytes` would overflow uint64_t,
 *   the effective limit is larger than any possible output, so we allow it.
 *
 * ## Typical Values
 *
 * - Default ratio_limit: 1000 (1 KB input → max 1 MB output)
 * - For highly compressible data (e.g., all zeros), the actual ratio can be
 *   very high (10000x or more). Users processing trusted data may want to
 *   increase or disable the limit.
 * - For security-sensitive contexts processing untrusted input, a lower
 *   limit (e.g., 100) provides stronger protection.
 *
 * @param input_bytes Total compressed bytes consumed
 * @param output_bytes Total decompressed bytes produced
 * @param ratio_limit Maximum allowed ratio (0 = unlimited)
 * @return GCOMP_OK if within limit, GCOMP_ERR_LIMIT if exceeded
 */
gcomp_status_t gcomp_limits_check_expansion_ratio(
    uint64_t input_bytes, uint64_t output_bytes, uint64_t ratio_limit) {
  // 0 means unlimited
  if (ratio_limit == 0) {
    return GCOMP_OK;
  }

  // If no input yet, we can't compute a ratio - allow it.
  // This handles the case where the decoder produces output from its
  // internal bit buffer before consuming the next input byte.
  if (input_bytes == 0) {
    return GCOMP_OK;
  }

  // Check: output_bytes / input_bytes > ratio_limit
  // Rewritten to avoid division: output_bytes > ratio_limit * input_bytes
  //
  // We must check for overflow before multiplying. If ratio_limit is larger
  // than UINT64_MAX / input_bytes, then the product would overflow, meaning
  // the effective limit is larger than any possible 64-bit value.
  if (ratio_limit > UINT64_MAX / input_bytes) {
    // Overflow would occur - the effective limit is infinite for this input
    // size, so any output is allowed.
    return GCOMP_OK;
  }

  uint64_t max_output = ratio_limit * input_bytes;
  if (output_bytes > max_output) {
    return GCOMP_ERR_LIMIT;
  }

  return GCOMP_OK;
}

void gcomp_memory_track_alloc(gcomp_memory_tracker_t * tracker, size_t size) {
  if (!tracker) {
    return;
  }

  // Check for overflow before adding
  if (tracker->current_bytes > SIZE_MAX - size) {
    // Overflow would occur - set to maximum
    tracker->current_bytes = SIZE_MAX;
  }
  else {
    tracker->current_bytes += size;
  }
}

void gcomp_memory_track_free(gcomp_memory_tracker_t * tracker, size_t size) {
  if (!tracker) {
    return;
  }

  // Check for underflow before subtracting
  if (tracker->current_bytes < size) {
    // Underflow would occur - set to zero
    tracker->current_bytes = 0;
  }
  else {
    tracker->current_bytes -= size;
  }
}

gcomp_status_t gcomp_memory_check_limit(
    const gcomp_memory_tracker_t * tracker, uint64_t limit) {
  if (!tracker) {
    return GCOMP_ERR_INVALID_ARG;
  }

  return gcomp_limits_check_memory(tracker->current_bytes, limit);
}
