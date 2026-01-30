/**
 * @file stream_internal.h
 *
 * Internal definitions for stream structures.
 * This header is only included by method implementations, not by users.
 *
 * OVERVIEW
 * ========
 *
 * This header defines the internal structure of encoders and decoders. Method
 * implementations (e.g., deflate) include this header to:
 *
 * 1. Access the base encoder/decoder structures
 * 2. Set update/finish function pointers
 * 3. Store method-specific state in the method_state pointer
 * 4. Set error details when errors occur
 *
 * ERROR DETAIL MECHANISM
 * ======================
 *
 * Each encoder/decoder contains:
 * - last_error: The most recent error status (GCOMP_OK if no error)
 * - error_detail: A human-readable string describing the error context
 *
 * Method implementations should call gcomp_encoder_set_error() or
 * gcomp_decoder_set_error() when detecting errors. The set_error functions:
 *
 * 1. Store the status code in last_error
 * 2. Format the error message into error_detail using vsnprintf
 * 3. Return the status code for convenient chaining:
 *
 *    return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
 *        "invalid block type %d at offset %zu", block_type, offset);
 *
 * Users query error details via the public API:
 * - gcomp_decoder_get_error() - returns last_error
 * - gcomp_decoder_get_error_detail() - returns error_detail string
 *
 * THREAD SAFETY
 * =============
 *
 * Encoders and decoders are NOT thread-safe. Each instance should only be
 * used by one thread at a time.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_STREAM_INTERNAL_H
#define GHOTI_IO_GCOMP_STREAM_INTERNAL_H

#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/stream.h>
#include <stddef.h>

#include <ghoti.io/compress/errors.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
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
 * @brief Maximum length for error detail strings
 *
 * This is a fixed-size buffer to avoid dynamic allocation in error paths.
 * 256 bytes is sufficient for typical error messages like:
 * "corrupt deflate stream at stage 'huffman_data' (output: 12345 bytes)"
 */
#define GCOMP_ERROR_DETAIL_MAX 256

/**
 * @brief Base encoder structure
 *
 * This structure contains the common fields for all encoder types. Method
 * implementations allocate their own extended structure with method_state
 * pointing to method-specific data.
 *
 * Lifecycle:
 * 1. gcomp_encoder_create() allocates this struct via the registry's allocator
 * 2. Method's create_encoder() sets up method_state, update_fn, finish_fn
 * 3. User calls update/finish via the public API
 * 4. gcomp_encoder_destroy() calls method's destroy_encoder, then frees struct
 *
 * Error handling:
 * - last_error is set by gcomp_encoder_set_error() when errors occur
 * - error_detail contains a human-readable description
 * - Both are initialized to zero/empty on creation
 */
struct gcomp_encoder_s {
  const gcomp_method_t * method;       ///< Method descriptor (immutable)
  gcomp_registry_t * registry;         ///< Registry for allocator access
  gcomp_options_t * options;           ///< User-provided options (may be NULL)
  void * method_state;                 ///< Method-specific encoder state
  gcomp_encoder_update_fn_t update_fn; ///< Method's update implementation
  gcomp_encoder_finish_fn_t finish_fn; ///< Method's finish implementation
  gcomp_status_t last_error;           ///< Last error status (GCOMP_OK if none)
  char error_detail[GCOMP_ERROR_DETAIL_MAX]; ///< Human-readable error context
};

/**
 * @brief Base decoder structure
 *
 * This structure contains the common fields for all decoder types. Method
 * implementations allocate their own extended structure with method_state
 * pointing to method-specific data.
 *
 * Lifecycle:
 * 1. gcomp_decoder_create() allocates this struct via the registry's allocator
 * 2. Method's create_decoder() sets up method_state, update_fn, finish_fn
 * 3. User calls update/finish via the public API
 * 4. gcomp_decoder_destroy() calls method's destroy_decoder, then frees struct
 *
 * Error handling:
 * - last_error is set by gcomp_decoder_set_error() when errors occur
 * - error_detail contains a human-readable description with context like
 *   the decoder stage, bytes processed, and specific validation failures
 * - Both are initialized to zero/empty on creation
 */
struct gcomp_decoder_s {
  const gcomp_method_t * method;       ///< Method descriptor (immutable)
  gcomp_registry_t * registry;         ///< Registry for allocator access
  gcomp_options_t * options;           ///< User-provided options (may be NULL)
  void * method_state;                 ///< Method-specific decoder state
  gcomp_decoder_update_fn_t update_fn; ///< Method's update implementation
  gcomp_decoder_finish_fn_t finish_fn; ///< Method's finish implementation
  gcomp_status_t last_error;           ///< Last error status (GCOMP_OK if none)
  char error_detail[GCOMP_ERROR_DETAIL_MAX]; ///< Human-readable error context
};

/**
 * @brief Set error detail on encoder (internal use only)
 *
 * This function is called by method implementations to set error details.
 * It uses printf-style formatting.
 *
 * @param encoder The encoder
 * @param status The error status
 * @param fmt Printf-style format string
 * @param ... Format arguments
 * @return The status code (for convenient return chaining)
 */
GCOMP_INTERNAL_API gcomp_status_t gcomp_encoder_set_error(
    gcomp_encoder_t * encoder, gcomp_status_t status, const char * fmt, ...);

/**
 * @brief Set error detail on decoder (internal use only)
 *
 * This function is called by method implementations to set error details.
 * It uses printf-style formatting.
 *
 * @param decoder The decoder
 * @param status The error status
 * @param fmt Printf-style format string
 * @param ... Format arguments
 * @return The status code (for convenient return chaining)
 */
GCOMP_INTERNAL_API gcomp_status_t gcomp_decoder_set_error(
    gcomp_decoder_t * decoder, gcomp_status_t status, const char * fmt, ...);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_STREAM_INTERNAL_H
