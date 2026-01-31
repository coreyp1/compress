/**
 * @file passthru_method.h
 *
 * Shared pass-thru (no-op) compression method for testing. Copies input to
 * output without compression. Used by test_passthru.cpp,
 * test_buffer_wrappers.cpp, and test_callback_api.cpp to validate the stream
 * API and infrastructure.
 *
 * ## Purpose
 *
 * The passthru method is a minimal, correct implementation of the library's
 * method vtable. It does not compress or decompress; it only copies bytes from
 * the input buffer to the output buffer. This allows tests to exercise the
 * full pipeline (registry, stream creation, update/finish, buffer wrappers,
 * callback API) without depending on a real codec.
 *
 * ## How it works
 *
 * - **Encoder and decoder are identical.** Both update functions copy
 *   `min(available input, available output)` bytes and advance `input->used`
 *   and `output->used` accordingly.
 *
 * - **Buffer semantics:** Input and output use the standard gcomp_buffer_t
 *   convention: `data` points to the buffer, `size` is its capacity, and
 *   `used` is the number of bytes consumed (input) or produced (output).
 *   "Available" input is `size - used`; the method never reads past that.
 *
 * - **Partial I/O:** If the output buffer is smaller than the remaining
 *   input, only as many bytes as fit are copied. The caller can call update
 *   again with more output space or more input. No internal buffering is
 *   required.
 *
 * - **Finish:** Both encoder_finish and decoder_finish are no-ops and return
 *   GCOMP_OK. There is no trailing data to flush.
 *
 * - **Lifecycle:** create_encoder/create_decoder receive a pre-allocated
 *   gcomp_encoder_t/gcomp_decoder_t and only assign update_fn and finish_fn.
 *   destroy_encoder/destroy_decoder do nothing (no method-specific state).
 *
 * ## Using as a reference
 *
 * When implementing a new compression method (e.g. deflate), use this file
 * as a reference for:
 * - Filling the method descriptor (create_passthru_method) with the correct
 *   vtable hooks and capabilities.
 * - Implementing update to respect buffer boundaries and advance ->used.
 * - Implementing finish when there is no final payload.
 * - Handling NULL and invalid arguments defensively.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_COMPRESS_TESTS_PASSTHRU_METHOD_H
#define GHOTI_IO_COMPRESS_TESTS_PASSTHRU_METHOD_H

#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/stream.h>

#include "../src/core/stream_internal.h"

namespace gcomp_test {

// --- Encoder: update and finish ---

inline static gcomp_status_t passthru_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  (void)encoder;

  if (!input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  size_t to_copy = input->size - input->used;
  size_t available = output->size - output->used;

  if (to_copy > available) {
    to_copy = available;
  }

  if (to_copy > 0 && input->data != NULL && output->data != NULL) {
    memcpy((uint8_t *)output->data + output->used,
        (const uint8_t *)input->data + input->used, to_copy);
    input->used += to_copy;
    output->used += to_copy;
  }

  return GCOMP_OK;
}

inline static gcomp_status_t passthru_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  (void)encoder;
  if (!output) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return GCOMP_OK;
}

// --- Decoder: update and finish (same logic as encoder) ---

inline static gcomp_status_t passthru_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  (void)decoder;

  if (!input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  size_t to_copy = input->size - input->used;
  size_t available = output->size - output->used;

  if (to_copy > available) {
    to_copy = available;
  }

  if (to_copy > 0 && input->data != NULL && output->data != NULL) {
    memcpy((uint8_t *)output->data + output->used,
        (const uint8_t *)input->data + input->used, to_copy);
    input->used += to_copy;
    output->used += to_copy;
  }

  return GCOMP_OK;
}

inline static gcomp_status_t passthru_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  (void)decoder;
  if (!output) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return GCOMP_OK;
}

// --- Lifecycle: create and destroy ---

inline static gcomp_status_t passthru_create_encoder(
    gcomp_registry_t * registry, gcomp_options_t * options,
    gcomp_encoder_t ** encoder_out) {
  (void)registry;
  (void)options;

  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_encoder_t * encoder = *encoder_out;
  encoder->update_fn = passthru_encoder_update;
  encoder->finish_fn = passthru_encoder_finish;
  return GCOMP_OK;
}

inline static gcomp_status_t passthru_create_decoder(
    gcomp_registry_t * registry, gcomp_options_t * options,
    gcomp_decoder_t ** decoder_out) {
  (void)registry;
  (void)options;

  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_decoder_t * decoder = *decoder_out;
  decoder->update_fn = passthru_decoder_update;
  decoder->finish_fn = passthru_decoder_finish;
  return GCOMP_OK;
}

inline static void passthru_destroy_encoder(gcomp_encoder_t * encoder) {
  (void)encoder;
}

inline static void passthru_destroy_decoder(gcomp_decoder_t * decoder) {
  (void)decoder;
}

// --- Method descriptor factory ---

inline static gcomp_method_t create_passthru_method(const char * name) {
  gcomp_method_t method = {};
  method.abi_version = 1;
  method.size = sizeof(gcomp_method_t);
  method.name = name;
  method.capabilities =
      static_cast<gcomp_capabilities_t>(GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE);
  method.create_encoder = passthru_create_encoder;
  method.create_decoder = passthru_create_decoder;
  method.destroy_encoder = passthru_destroy_encoder;
  method.destroy_decoder = passthru_destroy_decoder;
  return method;
}

} // namespace gcomp_test

#endif // GHOTI_IO_COMPRESS_TESTS_PASSTHRU_METHOD_H
