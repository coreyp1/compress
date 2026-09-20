/**
 * @file lzw.h
 *
 * LZW (Lempel–Ziv–Welch) method for the Ghoti.io Compress library.
 *
 * Stream interpretation is profile-driven. Reference profiles: gif (GIF 89a,
 * LSB) and tiff (TIFF 6.0, MSB). Use option lzw.format to select; default
 * is "gif". Options lzw.lit_width and lzw.max_code_bits control code widths.
 * Option lzw.encoder_lookup selects encoder dictionary lookup: "linear" (O(n)
 * scan) or "hash" (O(1), default). Shared limit options apply
 * (limits.max_output_bytes, limits.max_memory_bytes,
 * limits.max_expansion_ratio).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_LZW_H
#define GHOTI_IO_GCOMP_LZW_H

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The largest expansion TIFF 6.0 LZW permits, as an output/input ratio.
 *
 * Section 13 caps a code at twelve bits and the table at 4096 entries, of
 * which 3838 are added by the decoder (258 through 4095).  Each added entry is
 * one byte longer than the entry it extends, so the longest string the table
 * can hold is 3839 bytes, and emitting it costs twelve bits:
 * 3839 * 8 / 12 = 2559.3, rounded up.
 *
 * A stream that never sends Clear can repeat that code indefinitely, so the
 * bound is sustained rather than momentary.  A smaller `lzw.max_code_bits`
 * lowers it sharply - at nine bits it is 227:1 - so this is the bound across
 * every setting, not the bound at the default.
 *
 * Measured: 32 MiB of zeros compresses to 1313:1.
 */
#define GCOMP_LZW_MAX_EXPANSION_RATIO 2560ULL

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

#endif // GHOTI_IO_GCOMP_LZW_H
