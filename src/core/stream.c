/**
 * @file stream.c
 *
 * Implementation of the streaming compression API for the Ghoti.io Compress
 * library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "alloc_internal.h"
#include "registry_internal.h"
#include "stream_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/stream.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

gcomp_status_t gcomp_encoder_create(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options,
    gcomp_encoder_t ** encoder_out) {
  if (!registry || !method_name || !encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Find the method
  const gcomp_method_t * method = gcomp_registry_find(registry, method_name);
  if (!method) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  // Check if method supports encoding
  if (!(method->capabilities & GCOMP_CAP_ENCODE)) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  // Check if create_encoder function exists
  if (!method->create_encoder) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);

  // Create encoder structure
  gcomp_encoder_t * encoder = gcomp_calloc(alloc, 1, sizeof(gcomp_encoder_t));
  if (!encoder) {
    return GCOMP_ERR_MEMORY;
  }

  encoder->method = method;
  encoder->registry = registry;
  encoder->options = options;
  encoder->method_state = NULL;
  encoder->update_fn = NULL;
  encoder->finish_fn = NULL;

  // Call method's create_encoder - method may replace encoder with its own
  gcomp_encoder_t * method_encoder = encoder;
  gcomp_status_t status =
      method->create_encoder(registry, options, &method_encoder);
  if (status != GCOMP_OK) {
    gcomp_free(alloc, encoder);
    return status;
  }

  // If method created its own encoder, free the one we created
  if (method_encoder != encoder) {
    gcomp_free(alloc, encoder);
  }

  *encoder_out = method_encoder;
  return GCOMP_OK;
}

gcomp_status_t gcomp_decoder_create(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options,
    gcomp_decoder_t ** decoder_out) {
  if (!registry || !method_name || !decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Find the method
  const gcomp_method_t * method = gcomp_registry_find(registry, method_name);
  if (!method) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  // Check if method supports decoding
  if (!(method->capabilities & GCOMP_CAP_DECODE)) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  // Check if create_decoder function exists
  if (!method->create_decoder) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);

  // Create decoder structure
  gcomp_decoder_t * decoder = gcomp_calloc(alloc, 1, sizeof(gcomp_decoder_t));
  if (!decoder) {
    return GCOMP_ERR_MEMORY;
  }

  decoder->method = method;
  decoder->registry = registry;
  decoder->options = options;
  decoder->method_state = NULL;
  decoder->update_fn = NULL;
  decoder->finish_fn = NULL;

  // Call method's create_decoder - method may replace decoder with its own
  gcomp_decoder_t * method_decoder = decoder;
  gcomp_status_t status =
      method->create_decoder(registry, options, &method_decoder);
  if (status != GCOMP_OK) {
    gcomp_free(alloc, decoder);
    return status;
  }

  // If method created its own decoder, free the one we created
  if (method_decoder != decoder) {
    gcomp_free(alloc, decoder);
  }

  *decoder_out = method_decoder;
  return GCOMP_OK;
}

gcomp_status_t gcomp_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!encoder || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (!encoder->update_fn) {
    return GCOMP_ERR_INTERNAL;
  }

  return encoder->update_fn(encoder, input, output);
}

gcomp_status_t gcomp_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  if (!encoder || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (!encoder->finish_fn) {
    return GCOMP_ERR_INTERNAL;
  }

  return encoder->finish_fn(encoder, output);
}

gcomp_status_t gcomp_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!decoder || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (!decoder->update_fn) {
    return GCOMP_ERR_INTERNAL;
  }

  return decoder->update_fn(decoder, input, output);
}

gcomp_status_t gcomp_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  if (!decoder || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (!decoder->finish_fn) {
    return GCOMP_ERR_INTERNAL;
  }

  return decoder->finish_fn(decoder, output);
}

gcomp_status_t gcomp_encoder_reset(gcomp_encoder_t * encoder) {
  if (!encoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Clear error state
  encoder->last_error = GCOMP_OK;
  encoder->error_detail[0] = '\0';

  // If method doesn't support reset, return unsupported
  if (!encoder->reset_fn) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  return encoder->reset_fn(encoder);
}

gcomp_status_t gcomp_decoder_reset(gcomp_decoder_t * decoder) {
  if (!decoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Clear error state
  decoder->last_error = GCOMP_OK;
  decoder->error_detail[0] = '\0';

  // If method doesn't support reset, return unsupported
  if (!decoder->reset_fn) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  return decoder->reset_fn(decoder);
}

void gcomp_encoder_destroy(gcomp_encoder_t * encoder) {
  if (!encoder) {
    return;
  }

  const gcomp_allocator_t * alloc =
      gcomp_registry_get_allocator(encoder->registry);

  // Call method's destroy_encoder if it exists
  if (encoder->method && encoder->method->destroy_encoder) {
    encoder->method->destroy_encoder(encoder);
  }

  gcomp_free(alloc, encoder);
}

void gcomp_decoder_destroy(gcomp_decoder_t * decoder) {
  if (!decoder) {
    return;
  }

  const gcomp_allocator_t * alloc =
      gcomp_registry_get_allocator(decoder->registry);

  // Call method's destroy_decoder if it exists
  if (decoder->method && decoder->method->destroy_decoder) {
    decoder->method->destroy_decoder(decoder);
  }

  gcomp_free(alloc, decoder);
}

//
// Error Detail API
//
// Design rationale:
// -----------------
// Error detail strings provide human-readable context for debugging when
// compression/decompression fails. This is particularly useful for:
//
// 1. Debugging truncated or corrupt streams - the stage name and byte count
//    help identify where in the stream the problem occurred.
//
// 2. Distinguishing between different types of corruption - knowing whether
//    the error was in a block header vs. Huffman data helps narrow down issues.
//
// 3. Diagnosing limit violations - showing current/max values helps users
//    adjust limits appropriately.
//
// Implementation notes:
// - Error details are stored in a fixed-size buffer (GCOMP_ERROR_DETAIL_MAX)
//   to avoid dynamic allocation during error paths.
// - The set_error functions return the status code for convenient chaining:
//     return gcomp_decoder_set_error(dec, GCOMP_ERR_CORRUPT, "msg...");
// - Method implementations (e.g., deflate decoder) call set_error when they
//   detect errors, providing stage-specific context.
// - The get_error_detail function returns "" (not NULL) when no error has
//   occurred, making it safe to pass directly to printf/logging functions.
//

gcomp_status_t gcomp_encoder_get_error(const gcomp_encoder_t * encoder) {
  if (!encoder) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return encoder->last_error;
}

const char * gcomp_encoder_get_error_detail(const gcomp_encoder_t * encoder) {
  if (!encoder) {
    return "";
  }
  return encoder->error_detail;
}

gcomp_status_t gcomp_decoder_get_error(const gcomp_decoder_t * decoder) {
  if (!decoder) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return decoder->last_error;
}

const char * gcomp_decoder_get_error_detail(const gcomp_decoder_t * decoder) {
  if (!decoder) {
    return "";
  }
  return decoder->error_detail;
}

gcomp_status_t gcomp_encoder_set_error(
    gcomp_encoder_t * encoder, gcomp_status_t status, const char * fmt, ...) {
  if (!encoder) {
    return status;
  }

  encoder->last_error = status;

  if (fmt) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(encoder->error_detail, GCOMP_ERROR_DETAIL_MAX, fmt, args);
    va_end(args);
  }
  else {
    encoder->error_detail[0] = '\0';
  }

  return status;
}

gcomp_status_t gcomp_decoder_set_error(
    gcomp_decoder_t * decoder, gcomp_status_t status, const char * fmt, ...) {
  if (!decoder) {
    return status;
  }

  decoder->last_error = status;

  if (fmt) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(decoder->error_detail, GCOMP_ERROR_DETAIL_MAX, fmt, args);
    va_end(args);
  }
  else {
    decoder->error_detail[0] = '\0';
  }

  return status;
}
