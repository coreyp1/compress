/**
 * @file rle.h
 *
 * RLE (Run-Length Encoding) method for the Ghoti.io Compress library.
 *
 * Stream interpretation is profile-driven. Reference profiles: packbits
 * (TIFF / Apple MacPaint) and tga (Truevision Targa). Use option
 * rle.format to select; default is "packbits". Shared limit options
 * apply (limits.max_output_bytes, limits.max_memory_bytes,
 * limits.max_expansion_ratio).
 *
 * See documentation/modules/rle.md for full API and options.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_COMPRESS_RLE_H
#define GHOTI_IO_COMPRESS_RLE_H

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the RLE method with a registry.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG if registry is NULL
 */
GCOMP_API gcomp_status_t gcomp_method_rle_register(gcomp_registry_t * registry);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_COMPRESS_RLE_H
