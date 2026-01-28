/**
 * @file stream.c
 *
 * Implementation of the streaming compression API for the Ghoti.io Compress
 * library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "stream_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/stream.h>
#include <stdlib.h>

gcomp_status_t gcomp_encoder_create(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options,
    gcomp_encoder_t ** encoder_out) {
  if (!registry || !method_name || !encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  /* Find the method */
  const gcomp_method_t * method = gcomp_registry_find(registry, method_name);
  if (!method) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  /* Check if method supports encoding */
  if (!(method->capabilities & GCOMP_CAP_ENCODE)) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  /* Check if create_encoder function exists */
  if (!method->create_encoder) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  /* Create encoder structure */
  gcomp_encoder_t * encoder = calloc(1, sizeof(gcomp_encoder_t));
  if (!encoder) {
    return GCOMP_ERR_MEMORY;
  }

  encoder->method = method;
  encoder->registry = registry;
  encoder->options = options;
  encoder->method_state = NULL;
  encoder->update_fn = NULL;
  encoder->finish_fn = NULL;

  /* Call method's create_encoder - method may replace encoder with its own */
  gcomp_encoder_t * method_encoder = encoder;
  gcomp_status_t status =
      method->create_encoder(registry, options, &method_encoder);
  if (status != GCOMP_OK) {
    free(encoder);
    return status;
  }

  /* If method created its own encoder, free the one we created */
  if (method_encoder != encoder) {
    free(encoder);
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

  /* Find the method */
  const gcomp_method_t * method = gcomp_registry_find(registry, method_name);
  if (!method) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  /* Check if method supports decoding */
  if (!(method->capabilities & GCOMP_CAP_DECODE)) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  /* Check if create_decoder function exists */
  if (!method->create_decoder) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  /* Create decoder structure */
  gcomp_decoder_t * decoder = calloc(1, sizeof(gcomp_decoder_t));
  if (!decoder) {
    return GCOMP_ERR_MEMORY;
  }

  decoder->method = method;
  decoder->registry = registry;
  decoder->options = options;
  decoder->method_state = NULL;
  decoder->update_fn = NULL;
  decoder->finish_fn = NULL;

  /* Call method's create_decoder - method may replace decoder with its own */
  gcomp_decoder_t * method_decoder = decoder;
  gcomp_status_t status =
      method->create_decoder(registry, options, &method_decoder);
  if (status != GCOMP_OK) {
    free(decoder);
    return status;
  }

  /* If method created its own decoder, free the one we created */
  if (method_decoder != decoder) {
    free(decoder);
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

void gcomp_encoder_destroy(gcomp_encoder_t * encoder) {
  if (!encoder) {
    return;
  }

  /* Call method's destroy_encoder if it exists */
  if (encoder->method && encoder->method->destroy_encoder) {
    encoder->method->destroy_encoder(encoder);
  }

  free(encoder);
}

void gcomp_decoder_destroy(gcomp_decoder_t * decoder) {
  if (!decoder) {
    return;
  }

  /* Call method's destroy_decoder if it exists */
  if (decoder->method && decoder->method->destroy_decoder) {
    decoder->method->destroy_decoder(decoder);
  }

  free(decoder);
}
