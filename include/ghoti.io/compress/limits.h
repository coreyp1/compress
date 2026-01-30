/**
 * @file limits.h
 *
 * Safety limits and memory accounting for the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_LIMITS_H
#define GHOTI_IO_GCOMP_LIMITS_H

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/options.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Memory tracker structure
 *
 * Methods can use this structure to track memory usage and enforce
 * limits. Initialize with zeros before use.
 */
typedef struct {
  size_t current_bytes; ///< Current memory usage in bytes
} gcomp_memory_tracker_t;

/**
 * @brief Default maximum output size (512 MiB)
 */
#define GCOMP_DEFAULT_MAX_OUTPUT_BYTES (512ULL * 1024 * 1024)

/**
 * @brief Default maximum memory usage (256 MiB)
 */
#define GCOMP_DEFAULT_MAX_MEMORY_BYTES (256ULL * 1024 * 1024)

/**
 * @brief Default maximum expansion ratio (1000x)
 *
 * This means 1 KB of compressed data can expand to at most 1 MB of output.
 * This protects against "decompression bombs" - maliciously crafted inputs
 * that decompress to massive outputs (e.g., 1 MB → 1 TB).
 *
 * Set to 0 for unlimited (not recommended for untrusted input).
 */
#define GCOMP_DEFAULT_MAX_EXPANSION_RATIO 1000ULL

/**
 * @brief Read the maximum output bytes limit from options
 *
 * Reads the `limits.max_output_bytes` option from the options object.
 * If not set, returns the default value.
 *
 * @param opts Options object (can be NULL for default)
 * @param default_val Default value to use if not set in options
 * @return The limit value (0 means unlimited)
 */
GCOMP_API uint64_t gcomp_limits_read_output_max(
    const gcomp_options_t * opts, uint64_t default_val);

/**
 * @brief Read the maximum memory bytes limit from options
 *
 * Reads the `limits.max_memory_bytes` option from the options object.
 * If not set, returns the default value.
 *
 * @param opts Options object (can be NULL for default)
 * @param default_val Default value to use if not set in options
 * @return The limit value (0 means unlimited)
 */
GCOMP_API uint64_t gcomp_limits_read_memory_max(
    const gcomp_options_t * opts, uint64_t default_val);

/**
 * @brief Read the maximum window bytes limit from options
 *
 * Reads the `limits.max_window_bytes` option from the options object.
 * If not set, returns the default value.
 *
 * @param opts Options object (can be NULL for default)
 * @param default_val Default value to use if not set in options
 * @return The limit value (0 means unlimited)
 */
GCOMP_API uint64_t gcomp_limits_read_window_max(
    const gcomp_options_t * opts, uint64_t default_val);

/**
 * @brief Read the maximum expansion ratio from options
 *
 * Reads the `limits.max_expansion_ratio` option from the options object.
 * If not set, returns the default value.
 *
 * The expansion ratio is output_bytes / input_bytes. A ratio of 1000 means
 * 1 KB of compressed input can expand to at most 1 MB of output.
 *
 * @param opts Options object (can be NULL for default)
 * @param default_val Default value to use if not set in options
 * @return The limit value (0 means unlimited)
 */
GCOMP_API uint64_t gcomp_limits_read_expansion_ratio_max(
    const gcomp_options_t * opts, uint64_t default_val);

/**
 * @brief Check if output size exceeds limit
 *
 * Checks if the current output size exceeds the limit. If it does,
 * returns GCOMP_ERR_LIMIT. Otherwise returns GCOMP_OK.
 *
 * @param current Current output size in bytes
 * @param limit Maximum allowed output size (0 means unlimited)
 * @return GCOMP_OK if within limit, GCOMP_ERR_LIMIT if exceeded
 */
GCOMP_API gcomp_status_t gcomp_limits_check_output(
    size_t current, uint64_t limit);

/**
 * @brief Check if memory usage exceeds limit
 *
 * Checks if the current memory usage exceeds the limit. If it does,
 * returns GCOMP_ERR_LIMIT. Otherwise returns GCOMP_OK.
 *
 * @param current Current memory usage in bytes
 * @param limit Maximum allowed memory usage (0 means unlimited)
 * @return GCOMP_OK if within limit, GCOMP_ERR_LIMIT if exceeded
 */
GCOMP_API gcomp_status_t gcomp_limits_check_memory(
    size_t current, uint64_t limit);

/**
 * @brief Check if expansion ratio exceeds limit
 *
 * Checks if the output/input ratio exceeds the maximum allowed expansion.
 * This protects against decompression bombs where a small compressed input
 * expands to a massive output.
 *
 * The ratio is calculated as output_bytes / input_bytes. If input_bytes is 0,
 * the function returns GCOMP_OK (no ratio can be computed yet).
 *
 * @param input_bytes Total compressed input bytes consumed
 * @param output_bytes Total decompressed output bytes produced
 * @param ratio_limit Maximum allowed ratio (0 means unlimited)
 * @return GCOMP_OK if within limit, GCOMP_ERR_LIMIT if exceeded
 */
GCOMP_API gcomp_status_t gcomp_limits_check_expansion_ratio(
    uint64_t input_bytes, uint64_t output_bytes, uint64_t ratio_limit);

/**
 * @brief Track a memory allocation
 *
 * Adds the allocation size to the memory tracker. This should be called
 * whenever memory is allocated for the compression operation.
 *
 * @param tracker Memory tracker (must not be NULL)
 * @param size Size of the allocation in bytes
 */
GCOMP_API void gcomp_memory_track_alloc(
    gcomp_memory_tracker_t * tracker, size_t size);

/**
 * @brief Track a memory deallocation
 *
 * Subtracts the deallocation size from the memory tracker. This should
 * be called whenever memory is freed for the compression operation.
 *
 * @param tracker Memory tracker (must not be NULL)
 * @param size Size of the deallocation in bytes
 */
GCOMP_API void gcomp_memory_track_free(
    gcomp_memory_tracker_t * tracker, size_t size);

/**
 * @brief Check if memory usage exceeds limit
 *
 * Checks if the current tracked memory usage exceeds the limit. If it
 * does, returns GCOMP_ERR_LIMIT. Otherwise returns GCOMP_OK.
 *
 * @param tracker Memory tracker (must not be NULL)
 * @param limit Maximum allowed memory usage (0 means unlimited)
 * @return GCOMP_OK if within limit, GCOMP_ERR_LIMIT if exceeded
 */
GCOMP_API gcomp_status_t gcomp_memory_check_limit(
    const gcomp_memory_tracker_t * tracker, uint64_t limit);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_LIMITS_H
