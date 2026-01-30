/**
 * @file deflate_internal.h
 *
 * Internal declarations for the DEFLATE (RFC 1951) method implementation.
 *
 * This header is intended for use only by the deflate method sources. It
 * exposes internal helpers used by the method registration vtable.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_DEFLATE_INTERNAL_H
#define GHOTI_IO_GCOMP_DEFLATE_INTERNAL_H

#include "../../core/stream_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create and attach deflate decoder state to a decoder.
 *
 * On success, sets @c decoder->method_state and @c decoder->update_fn /
 * @c decoder->finish_fn.
 */
gcomp_status_t gcomp_deflate_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder);

/**
 * @brief Destroy and detach deflate decoder state.
 */
void gcomp_deflate_decoder_destroy(gcomp_decoder_t * decoder);

/**
 * @brief Reset deflate decoder to initial state.
 *
 * Resets all internal state so the decoder can be reused for a new stream.
 * Allocated buffers (window, Huffman tables) are retained but cleared.
 */
gcomp_status_t gcomp_deflate_decoder_reset(gcomp_decoder_t * decoder);

/**
 * @brief Deflate decoder update implementation.
 */
gcomp_status_t gcomp_deflate_decoder_update(
    gcomp_decoder_t * decoder, gcomp_buffer_t * input, gcomp_buffer_t * output);

/**
 * @brief Deflate decoder finish implementation.
 */
gcomp_status_t gcomp_deflate_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output);

/**
 * @brief Create and attach deflate encoder state to an encoder.
 *
 * On success, sets @c encoder->method_state and @c encoder->update_fn /
 * @c encoder->finish_fn.
 */
gcomp_status_t gcomp_deflate_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder);

/**
 * @brief Destroy and detach deflate encoder state.
 */
void gcomp_deflate_encoder_destroy(gcomp_encoder_t * encoder);

/**
 * @brief Reset deflate encoder to initial state.
 *
 * Resets all internal state so the encoder can be reused for a new stream.
 * Allocated buffers are retained (not freed and reallocated).
 */
gcomp_status_t gcomp_deflate_encoder_reset(gcomp_encoder_t * encoder);

/**
 * @brief Deflate encoder update implementation.
 */
gcomp_status_t gcomp_deflate_encoder_update(
    gcomp_encoder_t * encoder, gcomp_buffer_t * input, gcomp_buffer_t * output);

/**
 * @brief Deflate encoder finish implementation.
 */
gcomp_status_t gcomp_deflate_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_DEFLATE_INTERNAL_H
