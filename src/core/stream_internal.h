/**
 * @file stream_internal.h
 *
 * Internal definitions for stream structures.
 * This header is only included by method implementations, not by users.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_STREAM_INTERNAL_H
#define GHOTI_IO_GCOMP_STREAM_INTERNAL_H

#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/stream.h>
#include <stddef.h>

#include <ghoti.io/compress/errors.h>

/* Forward declarations */
struct gcomp_buffer_s;

/**
 * @brief Encoder update function type
 */
typedef gcomp_status_t (*gcomp_encoder_update_fn_t)(
    gcomp_encoder_t * encoder, gcomp_buffer_t * input, gcomp_buffer_t * output);

/**
 * @brief Encoder finish function type
 */
typedef gcomp_status_t (*gcomp_encoder_finish_fn_t)(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output);

/**
 * @brief Decoder update function type
 */
typedef gcomp_status_t (*gcomp_decoder_update_fn_t)(
    gcomp_decoder_t * decoder, gcomp_buffer_t * input, gcomp_buffer_t * output);

/**
 * @brief Decoder finish function type
 */
typedef gcomp_status_t (*gcomp_decoder_finish_fn_t)(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output);

/**
 * @brief Base encoder structure
 *
 * Methods can extend this by creating a structure where the first member
 * is gcomp_encoder_t, followed by method-specific fields.
 */
struct gcomp_encoder_s {
  const gcomp_method_t * method;
  gcomp_registry_t * registry;
  gcomp_options_t * options;
  void * method_state; /* Method-specific encoder state */
  gcomp_encoder_update_fn_t update_fn;
  gcomp_encoder_finish_fn_t finish_fn;
};

/**
 * @brief Base decoder structure
 *
 * Methods can extend this by creating a structure where the first member
 * is gcomp_decoder_t, followed by method-specific fields.
 */
struct gcomp_decoder_s {
  const gcomp_method_t * method;
  gcomp_registry_t * registry;
  gcomp_options_t * options;
  void * method_state; /* Method-specific decoder state */
  gcomp_decoder_update_fn_t update_fn;
  gcomp_decoder_finish_fn_t finish_fn;
};

#endif /* GHOTI_IO_GCOMP_STREAM_INTERNAL_H */
