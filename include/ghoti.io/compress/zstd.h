/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Compress.
 *
 * Ghoti.io Compress is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Compress is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

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
 * - Multiple compression levels (0-22; 0 is the fast strategy)
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
 * - `zstd.level` (int64, default 3): Compression level (0-22). 0 is the fast
 *   strategy: one hash probe and no chain, for throughput over ratio.
 * - `zstd.checksum` (bool, default false): Enable content checksum (xxHash64)
 * - `zstd.window_log` (uint64, default 0/auto): Window log (10-31, 0=auto)
 * - `zstd.dictionary` (bytes, optional): Raw or formatted dictionary (RFC 8878
 * §5)
 * - `zstd.dictionary_id` (uint64, optional): Dictionary ID to write (encoder)
 * or validate (decoder); the frame field itself is 32 bits
 * - `zstd.content_size` (uint64, optional): Content size for header
 * - `zstd.concat` (bool, default false): Decoder: support concatenated frames
 * - `zstd.job_size` (uint64, default 0): Encoder: bytes per parallel job
 * (64KB-16MB; 0 means 512 KB, whatever the level)
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
 * @brief The largest expansion RFC 8878 permits, as an output/input ratio.
 *
 * The cheapest output in the format is an RLE_Block (section 3.1.1.2.2): a
 * three-byte Block_Header and a single byte, four bytes in all, standing for
 * Block_Size repetitions of that byte.  Section 3.1.1.2.4 bounds Block_Size by
 * Block_Maximum_Size, which is min(Window_Size, 128 KB), so four bytes carry
 * at most 131072: 131072 / 4 = 32768.
 *
 * A Compressed_Block cannot beat that - its literals and sequences sections
 * cost more than one byte between them - and a frame header is input that
 * produces nothing, so it only lowers the ratio.
 *
 * Measured: 32 MiB of zeros compresses to 32482:1, just under it.
 */
#define GCOMP_ZSTD_MAX_EXPANSION_RATIO 32768ULL

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
