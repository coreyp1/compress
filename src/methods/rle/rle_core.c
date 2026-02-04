/**
 * @file rle_core.c
 *
 * RLE core: literal and repeat span emission with output bounds.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "rle_core.h"
#include "../../core/safe_math.h"
#include <string.h>

gcomp_status_t rle_emit_literal(uint8_t * output_data, size_t output_size,
    size_t * output_used, const uint8_t * data, size_t len,
    uint64_t max_output_bytes) {
  if (!output_data || !output_used || !data) {
    return GCOMP_ERR_INVALID_ARG;
  }

  size_t used = *output_used;
  size_t new_used;
  if (!gcomp_safe_add_size(used, len, &new_used)) {
    return GCOMP_ERR_CORRUPT;
  }
  if (new_used > output_size) {
    return GCOMP_ERR_LIMIT;
  }
  if (max_output_bytes != 0 && new_used > max_output_bytes) {
    return GCOMP_ERR_LIMIT;
  }

  memcpy(output_data + used, data, len);
  *output_used = new_used;
  return GCOMP_OK;
}

gcomp_status_t rle_emit_repeat(uint8_t * output_data, size_t output_size,
    size_t * output_used, uint8_t byte, size_t count,
    uint64_t max_output_bytes) {
  if (!output_data || !output_used) {
    return GCOMP_ERR_INVALID_ARG;
  }

  size_t used = *output_used;
  size_t new_used;
  if (!gcomp_safe_add_size(used, count, &new_used)) {
    return GCOMP_ERR_CORRUPT;
  }
  if (new_used > output_size) {
    return GCOMP_ERR_LIMIT;
  }
  if (max_output_bytes != 0 && new_used > max_output_bytes) {
    return GCOMP_ERR_LIMIT;
  }

  memset(output_data + used, byte, count);
  *output_used = new_used;
  return GCOMP_OK;
}
