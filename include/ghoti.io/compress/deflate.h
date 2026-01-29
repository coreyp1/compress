/**
 * @file deflate.h
 *
 * DEFLATE (RFC 1951) compression method for the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_COMPRESS_DEFLATE_H
#define GHOTI_IO_COMPRESS_DEFLATE_H

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the deflate method with a registry
 *
 * Call this to make the "deflate" method available for encoding and decoding.
 * Typically used with gcomp_registry_default() or a custom registry.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return ::GCOMP_OK on success
 */
GCOMP_API gcomp_status_t gcomp_method_deflate_register(
    gcomp_registry_t * registry);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_COMPRESS_DEFLATE_H
