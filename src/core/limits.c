/**
 * @file limits.c
 *
 * Implementation of safety limits and memory accounting for the Ghoti.io
 * Compress library.
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
