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
 * @file bound.c
 *
 * gcomp_encode_bound(): how big an output buffer has to be.
 *
 * The work is the method's - each one knows its own framing - so this is
 * lookup, validation and dispatch.  What it adds is the two refusals: a method
 * that cannot encode has no bound to give, and a method that has not
 * implemented the hook says so rather than having something plausible guessed
 * on its behalf.  A wrong bound is worse than no bound: a caller allocates to
 * it and believes the buffer is big enough.
 */

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>

gcomp_status_t gcomp_encode_bound(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options, size_t input_size,
    size_t * bound_out) {
  if (!method_name || !bound_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  *bound_out = 0;

  if (!registry) {
    registry = gcomp_registry_default();
    if (!registry) {
      return GCOMP_ERR_INTERNAL;
    }
  }

  const gcomp_method_t * method = gcomp_registry_find(registry, method_name);
  if (!method) {
    // What gcomp_encoder_create() and gcomp_decoder_create() answer for a name
    // that is not registered (stream.c).  One convention, not two.
    return GCOMP_ERR_UNSUPPORTED;
  }
  if ((method->capabilities & GCOMP_CAP_ENCODE) == 0) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  // A method registered against an older ABI has no such field to read; its
  // `size` says where its descriptor stops.
  if (method->size < sizeof(gcomp_method_t) || !method->encode_bound) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  return method->encode_bound(options, input_size, bound_out);
}
