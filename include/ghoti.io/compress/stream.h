/**
 * @file stream.h
 *
 * Streaming compression and decompression API for the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_STREAM_H
#define GHOTI_IO_GCOMP_STREAM_H

#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/options.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Forward declarations
 */
typedef struct gcomp_encoder_s gcomp_encoder_t;
typedef struct gcomp_decoder_s gcomp_decoder_t;

/**
 * @brief Buffer structure for input/output operations
 */
typedef struct {
  const void *data;  /**< Pointer to data */
  size_t size;       /**< Size of data in bytes */
  size_t used;       /**< Number of bytes consumed/produced */
} gcomp_buffer_t;

/**
 * @brief Create an encoder
 *
 * Creates a streaming encoder for the specified compression method.
 *
 * @param registry The registry to use
 * @param method_name The name of the compression method (e.g., "zstd")
 * @param options Configuration options (can be NULL for defaults)
 * @param encoder_out Output parameter for the created encoder
 * @return Status code
 */
GCOMP_API gcomp_status_t
gcomp_encoder_create(gcomp_registry_t *registry,
                          const char *method_name,
                          gcomp_options_t *options,
                          gcomp_encoder_t **encoder_out);

/**
 * @brief Create a decoder
 *
 * Creates a streaming decoder for the specified compression method.
 *
 * @param registry The registry to use
 * @param method_name The name of the compression method (e.g., "gzip")
 * @param options Configuration options (can be NULL for defaults)
 * @param decoder_out Output parameter for the created decoder
 * @return Status code
 */
GCOMP_API gcomp_status_t
gcomp_decoder_create(gcomp_registry_t *registry,
                          const char *method_name,
                          gcomp_options_t *options,
                          gcomp_decoder_t **decoder_out);

/**
 * @brief Update encoder with input data
 *
 * Processes input data and produces compressed output. This function
 * may be called multiple times with partial input. It may produce zero
 * output on some calls (buffering internally).
 *
 * @param encoder The encoder
 * @param input Input buffer
 * @param output Output buffer
 * @return Status code
 */
GCOMP_API gcomp_status_t
gcomp_encoder_update(gcomp_encoder_t *encoder,
                         gcomp_buffer_t *input,
                         gcomp_buffer_t *output);

/**
 * @brief Finish encoding
 *
 * Finalizes the compression stream, flushes any pending output, and
 * emits trailers (if applicable). After calling this, the encoder
 * should not be used for further updates.
 *
 * @param encoder The encoder
 * @param output Output buffer
 * @return Status code
 */
GCOMP_API gcomp_status_t
gcomp_encoder_finish(gcomp_encoder_t *encoder,
                          gcomp_buffer_t *output);

/**
 * @brief Update decoder with input data
 *
 * Processes compressed input data and produces decompressed output.
 * This function may be called multiple times with partial input.
 *
 * @param decoder The decoder
 * @param input Input buffer
 * @param output Output buffer
 * @return Status code
 */
GCOMP_API gcomp_status_t
gcomp_decoder_update(gcomp_decoder_t *decoder,
                          gcomp_buffer_t *input,
                          gcomp_buffer_t *output);

/**
 * @brief Finish decoding
 *
 * Finalizes the decompression stream and validates trailers (if applicable).
 * After calling this, the decoder should not be used for further updates.
 *
 * @param decoder The decoder
 * @param output Output buffer
 * @return Status code
 */
GCOMP_API gcomp_status_t
gcomp_decoder_finish(gcomp_decoder_t *decoder,
                          gcomp_buffer_t *output);

/**
 * @brief Destroy an encoder
 *
 * @param encoder The encoder to destroy
 */
GCOMP_API void
gcomp_encoder_destroy(gcomp_encoder_t *encoder);

/**
 * @brief Destroy a decoder
 *
 * @param decoder The decoder to destroy
 */
GCOMP_API void
gcomp_decoder_destroy(gcomp_decoder_t *decoder);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GCOMP_STREAM_H */
