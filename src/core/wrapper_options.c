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
 * @file wrapper_options.c
 *
 * See wrapper_options.h.
 */

#include <ghoti.io/compress/macros.h>

#include "wrapper_options.h"

#include <ghoti.io/compress/method.h>

gcomp_status_t gcomp_clone_options_for_method(gcomp_registry_t * registry,
    const char * inner_method, const gcomp_options_t * src,
    gcomp_options_t ** dst_out) {
  if (!dst_out || !inner_method) {
    return GCOMP_ERR_INVALID_ARG;
  }
  *dst_out = NULL;
  if (!src) {
    return GCOMP_OK;
  }

  // This used to clone the whole options object, on the stated grounds that
  // "the inner method will ignore unknown keys with its schema".  Deflate's
  // policy is GCOMP_UNKNOWN_KEY_ERROR, so that was never true; it went
  // unnoticed only because nothing validated options at create time.  Handing
  // the inner encoder every gzip.* or zlib.* key the caller set would now
  // fail the call outright.
  //
  // What passes through is what the inner method declares it accepts, taken
  // from its own schema so this cannot drift as that schema changes.
  const gcomp_method_t * inner = gcomp_registry_find(registry, inner_method);
  if (!inner) {
    return GCOMP_ERR_UNSUPPORTED;
  }
  const gcomp_method_schema_t * schema = NULL;
  gcomp_status_t status = gcomp_method_get_all_schemas(inner, &schema);
  if (status != GCOMP_OK) {
    return status;
  }
  if (!schema) {
    return GCOMP_ERR_INTERNAL;
  }

  gcomp_options_t * dst = NULL;
  status = gcomp_options_create(&dst);
  if (status != GCOMP_OK) {
    return status;
  }

  for (size_t i = 0; i < schema->num_options; i++) {
    const gcomp_option_schema_t * opt = &schema->options[i];
    if (!opt || !opt->key) {
      continue;
    }
    switch (opt->type) {
    case GCOMP_OPT_INT64: {
      int64_t v = 0;
      if (gcomp_options_get_int64(src, opt->key, &v) == GCOMP_OK) {
        status = gcomp_options_set_int64(dst, opt->key, v);
      }
      break;
    }
    case GCOMP_OPT_UINT64: {
      uint64_t v = 0;
      if (gcomp_options_get_uint64(src, opt->key, &v) == GCOMP_OK) {
        status = gcomp_options_set_uint64(dst, opt->key, v);
      }
      break;
    }
    case GCOMP_OPT_BOOL: {
      int v = 0;
      if (gcomp_options_get_bool(src, opt->key, &v) == GCOMP_OK) {
        status = gcomp_options_set_bool(dst, opt->key, v);
      }
      break;
    }
    case GCOMP_OPT_STRING: {
      const char * v = NULL;
      if (gcomp_options_get_string(src, opt->key, &v) == GCOMP_OK && v) {
        status = gcomp_options_set_string(dst, opt->key, v);
      }
      break;
    }
    case GCOMP_OPT_BYTES: {
      const void * d = NULL;
      size_t n = 0;
      if (gcomp_options_get_bytes(src, opt->key, &d, &n) == GCOMP_OK && d) {
        status = gcomp_options_set_bytes(dst, opt->key, d, n);
      }
      break;
    }
    default:
      break;
    }
    if (status != GCOMP_OK) {
      gcomp_options_destroy(dst);
      return status;
    }
  }

  *dst_out = dst;
  return GCOMP_OK;
}
