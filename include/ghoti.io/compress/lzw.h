/**
 * @file lzw.h
 *
 * LZW (Lempel–Ziv–Welch) method for the Ghoti.io Compress library.
 *
 * Stream interpretation is profile-driven. Reference profiles: gif (GIF 89a,
 * LSB) and tiff (TIFF 6.0, MSB). Use option lzw.format to select; default
 * is "gif". Options lzw.lit_width and lzw.max_code_bits control code widths.
 * Shared limit options apply (limits.max_output_bytes, limits.max_memory_bytes,
 * limits.max_expansion_ratio).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_COMPRESS_LZW_H
#define GHOTI_IO_COMPRESS_LZW_H

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the LZW method with a registry.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return ::GCOMP_OK on success, ::GCOMP_ERR_INVALID_ARG if registry is NULL
 */
GCOMP_API gcomp_status_t gcomp_method_lzw_register(gcomp_registry_t * registry);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_COMPRESS_LZW_H
