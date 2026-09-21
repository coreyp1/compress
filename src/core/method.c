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
 * @file method.c
 *
 * Implementation of method schema query helpers for the Ghoti.io Compress
 * library.
 *
 * These helpers provide a stable API for discovering the option schema
 * associated with a compression method.
 */

#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/method.h>
#include <string.h>

GCOMP_API gcomp_status_t gcomp_method_get_all_schemas(
    const gcomp_method_t * method, const gcomp_method_schema_t ** schema_out) {
  if (!method || !schema_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (!method->get_schema) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  const gcomp_method_schema_t * schema = method->get_schema();
  if (!schema) {
    return GCOMP_ERR_INTERNAL;
  }

  *schema_out = schema;
  return GCOMP_OK;
}

GCOMP_API gcomp_status_t gcomp_method_get_option_keys(
    const gcomp_method_t * method, const char * const ** keys_out,
    size_t * count_out) {
  if (!method || !keys_out || !count_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  const gcomp_method_schema_t * schema = NULL;
  gcomp_status_t status = gcomp_method_get_all_schemas(method, &schema);
  if (status != GCOMP_OK) {
    return status;
  }

  if (!schema) {
    return GCOMP_ERR_INTERNAL;
  }

  *count_out = schema->num_options;

  if (schema->num_options == 0) {
    *keys_out = NULL;
    return GCOMP_OK;
  }

  if (schema->keys) {
    *keys_out = schema->keys;
    return GCOMP_OK;
  }

  // If no explicit keys array is provided, we cannot safely synthesise one
  // without dynamic allocation and ownership rules. For now, treat this as
  // unsupported and let callers fall back to iterating @ref options directly.
  return GCOMP_ERR_UNSUPPORTED;
}

GCOMP_API gcomp_status_t gcomp_method_get_option_schema(
    const gcomp_method_t * method, const char * key,
    const gcomp_option_schema_t ** schema_out) {
  if (!method || !key || !schema_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  const gcomp_method_schema_t * schema = NULL;
  gcomp_status_t status = gcomp_method_get_all_schemas(method, &schema);
  if (status != GCOMP_OK) {
    return status;
  }

  if (!schema) {
    return GCOMP_ERR_INTERNAL;
  }

  for (size_t i = 0; i < schema->num_options; ++i) {
    const gcomp_option_schema_t * opt = &schema->options[i];
    if (opt && opt->key && strcmp(opt->key, key) == 0) {
      *schema_out = opt;
      return GCOMP_OK;
    }
  }

  // Key not found in schema.
  return GCOMP_ERR_INVALID_ARG;
}
