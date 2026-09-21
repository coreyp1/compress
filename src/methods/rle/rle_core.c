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
 * @file rle_core.c
 *
 * RLE core: literal and repeat span emission with output bounds.
 *
 * All size arithmetic uses gcu_safe_add_size to avoid overflow from
 * untrusted or option-derived lengths; overflow returns GCOMP_ERR_CORRUPT.
 * Writing past output_size or max_output_bytes returns GCOMP_ERR_LIMIT.
 */

#include <ghoti.io/compress/macros.h>
#include "rle_core.h"
#include <ghoti.io/cutil/safemath.h>
#include <string.h>

gcomp_status_t rle_emit_literal(uint8_t * output_data, size_t output_size,
    size_t * output_used, const uint8_t * data, size_t len,
    uint64_t max_output_bytes) {
  if (!output_data || !output_used || !data) {
    return GCOMP_ERR_INVALID_ARG;
  }

  size_t used = *output_used;
  size_t new_used;
  if (!gcu_safe_add_size(used, len, &new_used)) {
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
  if (!gcu_safe_add_size(used, count, &new_used)) {
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
