/**
 * @file errors.h
 *
 * Error codes and error handling for the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_ERRORS_H
#define GHOTI_IO_GCOMP_ERRORS_H

#include <ghoti.io/compress/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Status codes for compression operations
 */
typedef enum {
  GCOMP_OK = 0,              /**< Operation succeeded */
  GCOMP_ERR_INVALID_ARG,     /**< Invalid argument */
  GCOMP_ERR_MEMORY,          /**< Memory allocation failed */
  GCOMP_ERR_LIMIT,           /**< Resource limit exceeded */
  GCOMP_ERR_CORRUPT,         /**< Corrupted input data */
  GCOMP_ERR_UNSUPPORTED,      /**< Unsupported operation or format */
  GCOMP_ERR_INTERNAL,        /**< Internal library error */
  GCOMP_ERR_IO,              /**< I/O error */
} gcomp_status_t;

/**
 * @brief Convert a status code to a constant string.
 *
 * The returned pointer is to a statically allocated string and must not be
 * freed. This function is safe to call from multiple threads.
 *
 * @param status Status code.
 * @return Human-readable string for the status code.
 */
GCOMP_API const char * gcomp_status_to_string(gcomp_status_t status);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_ERRORS_H
