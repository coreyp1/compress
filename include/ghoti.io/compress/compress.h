/**
 * @file compress.h
 *
 * Main header for the Ghoti.io Compress library.
 *
 * Cross-platform C library implementing streaming compression with no
 * external dependencies.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_COMPRESS_H
#define GHOTI_IO_GCOMP_COMPRESS_H

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>
#include <stdint.h>


/*
 * Library version information comes from libver.h, which takes it from the
 * generated libver_gen.h: GCOMP_VERSION_MAJOR / _MINOR / _PATCH, plus
 * GCOMP_VERSION_STRING and the packed GCOMP_VERSION_NUMBER. It used to be
 * written out here as three zeros that no build step ever updated.
 */

/**
 * @brief Get the major version number
 * @return The major version number
 */
GCOMP_API uint32_t gcomp_version_major(void);

/**
 * @brief Get the minor version number
 * @return The minor version number
 */
GCOMP_API uint32_t gcomp_version_minor(void);

/**
 * @brief Get the patch version number
 * @return The patch version number
 */
GCOMP_API uint32_t gcomp_version_patch(void);

/**
 * @brief Get the version string
 * @return A string representation of the version (e.g., "0.0.0")
 */
GCOMP_API const char * gcomp_version_string(void);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Encode data from a buffer to a buffer
 *
 * Convenience function that encodes input data using the specified compression
 * method. This function handles encoder creation, multiple update calls, and
 * finish internally.
 *
 * @param registry The registry to use (can be NULL to use default registry)
 * @param method_name The name of the compression method (e.g., "deflate")
 * @param options Configuration options (can be NULL for defaults)
 * @param input_data Pointer to input data; may be NULL only when input_size is
 * 0
 * @param input_size Size of input data in bytes
 * @param output_data Pointer to output buffer (must be non-NULL)
 * @param output_capacity Capacity of output buffer in bytes; must be > 0
 * @param output_size_out Output parameter for actual number of bytes written
 * @return Status code. GCOMP_ERR_INVALID_ARG if output_capacity is 0, or
 *         input_data is NULL with input_size > 0; GCOMP_ERR_LIMIT if output
 *         buffer is too small. On error when output_capacity is 0,
 *         *output_size_out is set to 0.
 */
GCOMP_API gcomp_status_t gcomp_encode_buffer(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options,
    const void * input_data, size_t input_size, void * output_data,
    size_t output_capacity, size_t * output_size_out);

/**
 * @brief Decode data from a buffer to a buffer
 *
 * Convenience function that decodes compressed data using the specified
 * compression method. This function handles decoder creation, multiple update
 * calls, and finish internally.
 *
 * @param registry The registry to use (can be NULL to use default registry)
 * @param method_name The name of the compression method (e.g., "deflate")
 * @param options Configuration options (can be NULL for defaults)
 * @param input_data Pointer to compressed input data; may be NULL only when
 *        input_size is 0
 * @param input_size Size of input data in bytes
 * @param output_data Pointer to output buffer (must be non-NULL)
 * @param output_capacity Capacity of output buffer in bytes; must be > 0
 * @param output_size_out Output parameter for actual number of bytes written
 * @return Status code. GCOMP_ERR_INVALID_ARG if output_capacity is 0, or
 *         input_data is NULL with input_size > 0; GCOMP_ERR_LIMIT if output
 *         buffer is too small. On error when output_capacity is 0,
 *         *output_size_out is set to 0.
 */
GCOMP_API gcomp_status_t gcomp_decode_buffer(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options,
    const void * input_data, size_t input_size, void * output_data,
    size_t output_capacity, size_t * output_size_out);

/**
 * @brief Largest output gcomp_encode_buffer() can produce for this input size
 *
 * An output buffer of at least this size is always enough: encoding
 * @p input_size bytes with @p method_name and @p options into a buffer this
 * large cannot fail for want of room, whatever those bytes turn out to be.
 * Data that does not compress is the case that matters - every method here
 * falls back to storing such data verbatim, and the bound is what that costs,
 * which is the input plus the framing around it.
 *
 * Use it to size a buffer before a one-shot encode:
 *
 * @code
 * size_t cap = 0;
 * if (gcomp_encode_bound(NULL, "zstd", opts, len, &cap) != GCOMP_OK) { ... }
 * void * buf = malloc(cap);
 * size_t written = 0;
 * gcomp_encode_buffer(NULL, "zstd", opts, data, len, buf, cap, &written);
 * @endcode
 *
 * ## What it does not cover
 *
 * One whole stream: create, update as often as you like, finish. It does
 * **not** account for gcomp_encoder_flush(). A flush ends a block early and
 * pads to a byte boundary, so it adds output every time it is called and the
 * total depends on how often a caller chooses to call it - which is not
 * knowable from the input size. A caller that flushes must size its own
 * buffer, or stream into one it can refill.
 *
 * The options matter and are read: a gzip name and comment, an LZ4 block size
 * and its checksums, a zstd window that lowers the maximum block size all move
 * the answer. Pass the same options you will pass to the encoder.
 *
 * @param registry The registry to use (can be NULL to use default registry)
 * @param method_name The name of the compression method (e.g., "deflate")
 * @param options Configuration options (can be NULL for defaults)
 * @param input_size Number of input bytes that will be encoded
 * @param bound_out Receives the worst-case encoded size
 * @return GCOMP_OK; GCOMP_ERR_INVALID_ARG for a NULL name or output pointer;
 *         GCOMP_ERR_UNSUPPORTED if the method cannot encode or does not
 *         implement a bound; GCOMP_ERR_LIMIT if the bound is too large to
 *         represent in a size_t
 */
GCOMP_API gcomp_status_t gcomp_encode_bound(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options, size_t input_size,
    size_t * bound_out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_COMPRESS_H
