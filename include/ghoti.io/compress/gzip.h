/**
 * @file gzip.h
 *
 * GZIP (RFC 1952) compression method for the Ghoti.io Compress library.
 *
 * The gzip method is a wrapper around the deflate method (RFC 1951), adding:
 * - RFC 1952 header with magic bytes, flags, MTIME, XFL, OS, and optional
 *   fields (FEXTRA, FNAME, FCOMMENT, FHCRC)
 * - CRC32 checksum of uncompressed data
 * - RFC 1952 trailer with CRC32 and ISIZE (uncompressed size mod 2^32)
 *
 * ## Options
 *
 * ### Gzip-specific options (gzip.* prefix):
 * - `gzip.mtime` (uint64): Modification time as Unix timestamp (default: 0)
 * - `gzip.os` (uint64): Operating system code 0-255 (default: 255 = unknown)
 * - `gzip.name` (string): Original filename (optional)
 * - `gzip.comment` (string): File comment (optional)
 * - `gzip.extra` (bytes): FEXTRA field data (optional)
 * - `gzip.header_crc` (bool): Include header CRC (default: false)
 * - `gzip.xfl` (uint64): Extra flags (auto-calculated based on level if not
 * set)
 * - `gzip.concat` (bool): Decoder: support concatenated members (default:
 * false)
 *
 * ### Header field size limits (decoder safety):
 * - `gzip.max_name_bytes` (uint64): Max FNAME length (default: 1 MiB)
 * - `gzip.max_comment_bytes` (uint64): Max FCOMMENT length (default: 1 MiB)
 * - `gzip.max_extra_bytes` (uint64): Max FEXTRA length (default: 64 KiB)
 *
 * ### Pass-through options (forwarded to deflate):
 * - `deflate.level` (int64): Compression level 0-9 (default: 6)
 * - `deflate.window_bits` (uint64): LZ77 window size 8-15 (default: 15)
 * - `deflate.strategy` (string): Compression strategy
 *
 * ### Core limit options (handled by infrastructure):
 * - `limits.max_output_bytes` (uint64): Max decompressed output
 * - `limits.max_memory_bytes` (uint64): Max memory usage
 * - `limits.max_expansion_ratio` (uint64): Decompression bomb protection
 *
 * ## Example Usage
 *
 * @code
 * // Compress data to gzip format
 * gcomp_options_t* opts = NULL;
 * gcomp_options_create(&opts);
 * gcomp_options_set_string(opts, "gzip.name", "myfile.txt");
 * gcomp_options_set_int64(opts, "deflate.level", 9);
 *
 * gcomp_encoder_t* enc = NULL;
 * gcomp_encoder_create(registry, "gzip", opts, &enc);
 * // ... use encoder ...
 * gcomp_encoder_destroy(enc);
 * gcomp_options_destroy(opts);
 *
 * // Decompress gzip data (supports concatenated members)
 * gcomp_options_create(&opts);
 * gcomp_options_set_bool(opts, "gzip.concat", 1);
 *
 * gcomp_decoder_t* dec = NULL;
 * gcomp_decoder_create(registry, "gzip", opts, &dec);
 * // ... use decoder ...
 * gcomp_decoder_destroy(dec);
 * gcomp_options_destroy(opts);
 * @endcode
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_COMPRESS_GZIP_H
#define GHOTI_IO_COMPRESS_GZIP_H

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the gzip method with a registry
 *
 * Call this to make the "gzip" method available for encoding and decoding.
 * Typically used with gcomp_registry_default() or a custom registry.
 *
 * @note The gzip method requires the "deflate" method to be registered first.
 *       If using auto-registration (the default), both methods are registered
 *       automatically. If using explicit registration, register deflate before
 *       gzip.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return ::GCOMP_OK on success
 * @return ::GCOMP_ERR_INVALID_ARG if registry is NULL
 */
GCOMP_API gcomp_status_t gcomp_method_gzip_register(
    gcomp_registry_t * registry);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_COMPRESS_GZIP_H
