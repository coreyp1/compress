/**
 * @file zstd.h
 *
 * Zstandard compression for the Ghoti.io Compress library.
 *
 * This module provides Zstandard frame format compression and decompression.
 * The Zstd frame format is specified at:
 * https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md
 *
 * ## Features
 *
 * - Full Zstandard frame format support (header + blocks + trailer)
 * - Content checksum support (xxHash64, low 32 bits)
 * - Content size field support
 * - Multiple compression levels (1-22)
 * - Configurable window size
 * - Concatenated frame support
 * - Streaming with arbitrary input/output buffer sizes
 * - Memory tracking and limits
 *
 * ## Usage
 *
 * The Zstd method is registered with name "zstd" and can be used through the
 * standard compress API:
 *
 * ```c
 * // Encode
 * gcomp_encoder_t *encoder = NULL;
 * gcomp_encoder_create(registry, "zstd", options, &encoder);
 * gcomp_encoder_update(encoder, &input, &output);
 * gcomp_encoder_finish(encoder, &output);
 * gcomp_encoder_destroy(encoder);
 *
 * // Decode
 * gcomp_decoder_t *decoder = NULL;
 * gcomp_decoder_create(registry, "zstd", options, &decoder);
 * gcomp_decoder_update(decoder, &input, &output);
 * gcomp_decoder_finish(decoder, &output);
 * gcomp_decoder_destroy(decoder);
 * ```
 *
 * Or using the buffer convenience functions:
 *
 * ```c
 * gcomp_encode_buffer(NULL, "zstd", NULL, input, in_len, output, out_cap,
 *     &out_len);
 * gcomp_decode_buffer(NULL, "zstd", NULL, input, in_len, output, out_cap,
 *     &out_len);
 * ```
 *
 * ## Options
 *
 * Zstd-specific:
 * - `zstd.level` (int64, default 3): Compression level (1-22)
 * - `zstd.checksum` (bool, default false): Enable content checksum (xxHash64)
 * - `zstd.window_log` (uint64, default 0/auto): Window log (10-31, 0=auto)
 * - `zstd.dictionary` (bytes, optional): Raw or formatted dictionary (RFC 8878
 * §5)
 * - `zstd.dictionary_id` (uint32, optional): Dictionary ID to write (encoder)
 * or validate (decoder)
 * - `zstd.content_size` (uint64, optional): Content size for header
 * - `zstd.concat` (bool, default false): Decoder: support concatenated frames
 * - `zstd.job_size` (uint64, default 0/auto): Encoder: job size for parallel
 * compression (64KB–16MB)
 *
 * Threading (encoder):
 * - `threads.count` (uint64, default 1): Worker threads (0 or 1 =
 * single-threaded; >1 = parallel)
 *
 * Limits (shared):
 * - `limits.max_output_bytes` (uint64): Max decompressed output
 * - `limits.max_window_bytes` (uint64): Max window size
 * - `limits.max_memory_bytes` (uint64): Max memory usage
 * - `limits.max_expansion_ratio` (uint64): Decompression bomb protection
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_ZSTD_H
#define GHOTI_IO_GCOMP_ZSTD_H

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/registry.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the Zstd method with a registry.
 *
 * This function registers the "zstd" compression method with the given
 * registry. The method provides Zstandard frame format compression and
 * decompression.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return GCOMP_OK on success, error code on failure
 *
 * @note This function is called automatically during library initialization
 *       for the default registry via the auto-registration mechanism.
 *       Manual registration is only needed for custom registries.
 */
GCOMP_API gcomp_status_t gcomp_method_zstd_register(
    gcomp_registry_t * registry);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_ZSTD_H
