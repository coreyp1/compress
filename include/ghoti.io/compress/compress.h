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

#ifndef GHOTI_IO_GCOMP_H
#define GHOTI_IO_GCOMP_H

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Library version information
 */
#define GCOMP_VERSION_MAJOR 0
#define GCOMP_VERSION_MINOR 0
#define GCOMP_VERSION_PATCH 0

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
 * @param input_data Pointer to input data
 * @param input_size Size of input data in bytes
 * @param output_data Pointer to output buffer
 * @param output_capacity Capacity of output buffer in bytes
 * @param output_size_out Output parameter for actual number of bytes written
 * @return Status code. Returns GCOMP_ERR_LIMIT if output buffer is too small.
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
 * @param input_data Pointer to compressed input data
 * @param input_size Size of input data in bytes
 * @param output_data Pointer to output buffer
 * @param output_capacity Capacity of output buffer in bytes
 * @param output_size_out Output parameter for actual number of bytes written
 * @return Status code. Returns GCOMP_ERR_LIMIT if output buffer is too small.
 */
GCOMP_API gcomp_status_t gcomp_decode_buffer(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options,
    const void * input_data, size_t input_size, void * output_data,
    size_t output_capacity, size_t * output_size_out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_H
