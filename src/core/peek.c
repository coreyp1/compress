/**
 * @file peek.c
 *
 * gcomp_peek(): what a stream's header says, before anything is decoded.
 *
 * The work is the method's, because only the method knows its own framing.
 * What this adds is the refusals - a method that cannot decode has no header
 * to describe, and one that has not implemented the hook says so rather than
 * having an empty answer passed off as a real one - and the guarantee that
 * @ref gcomp_stream_info_t is cleared before a method touches it, so that a
 * field a format does not carry reads as zero rather than as whatever was on
 * the caller's stack.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>
#include <string.h>

gcomp_status_t gcomp_peek(gcomp_registry_t * registry, const char * method_name,
    gcomp_options_t * options, const void * input, size_t input_size,
    gcomp_stream_info_t * info_out, size_t * needed_out) {
  if (!method_name || !info_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  memset(info_out, 0, sizeof(*info_out));
  if (needed_out) {
    *needed_out = 0;
  }
  if (!input && input_size > 0) {
    return GCOMP_ERR_INVALID_ARG;
  }

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
  if ((method->capabilities & GCOMP_CAP_DECODE) == 0) {
    return GCOMP_ERR_UNSUPPORTED;
  }
  // A method registered against an older ABI has no such field to read; its
  // `size` says where its descriptor stops.
  if (method->size < sizeof(gcomp_method_t) || !method->peek) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  return method->peek(options, input, input_size, info_out, needed_out);
}
