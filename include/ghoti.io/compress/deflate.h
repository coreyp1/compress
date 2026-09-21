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
 * @file deflate.h
 *
 * DEFLATE (RFC 1951) compression method for the Ghoti.io Compress library.
 *
 * The deflate method implements the raw DEFLATE compressed data format as
 * specified in RFC 1951. This is the core compression algorithm used by gzip,
 * zlib, and PNG. It combines LZ77 compression with Huffman coding.
 *
 * ## When to Use Deflate vs Gzip
 *
 * - Use **"deflate"** when another format provides framing (HTTP, PNG, ZIP)
 * - Use **"gzip"** for file storage/interchange (adds checksums and metadata)
 *
 * The gzip method internally uses deflate for compression, adding RFC 1952
 * header and trailer with CRC32 checksum.
 *
 * ## Options
 *
 * ### Compression options (deflate.* prefix):
 *
 * - `deflate.level` (int64): Compression level 0-9 (default: 6)
 *   - Level 0: No compression (stored blocks only)
 *   - Levels 1-3: Fast compression with fixed Huffman codes
 *   - Levels 4-5: Balanced compression with dynamic Huffman codes
 *   - Level 6: Default, good balance of speed and compression
 *   - Levels 7-9: Best compression, slowest (longer match searches)
 *
 * - `deflate.window_bits` (uint64): LZ77 window size as log2(bytes)
 *   - Range: 8-15 (256 bytes to 32 KiB)
 *   - Default: 15 (32 KiB, maximum per RFC 1951)
 *   - Smaller windows reduce memory but may hurt compression ratio
 *
 * - `deflate.strategy` (string): Compression strategy for specialized data
 *   - "default": Standard LZ77 + Huffman (recommended for most data)
 *   - "lazy": Defers matches at every level, not only from level 4 up;
 *     good for PNG filter output and other data where a match one byte later
 *     is often longer
 *   - "huffman_only": Huffman coding only, no LZ77 (fast, poor compression)
 *   - "rle": Run-length encoding mode (useful for PNG images)
 *
 * ### Core limit options (handled by infrastructure):
 *
 * - `limits.max_output_bytes` (uint64): Max decompressed output size
 * - `limits.max_memory_bytes` (uint64): Max memory usage during operation
 * - `limits.max_expansion_ratio` (uint64): Decompression bomb protection
 *
 * ## Example Usage
 *
 * @code
 * // Compress data using deflate with maximum compression
 * gcomp_options_t *opts = NULL;
 * gcomp_options_create(&opts);
 * gcomp_options_set_int64(opts, "deflate.level", 9);
 *
 * gcomp_encoder_t *enc = NULL;
 * gcomp_encoder_create(registry, "deflate", opts, &enc);
 * gcomp_encoder_update(enc, &input, &output);
 * gcomp_encoder_finish(enc, &output);
 * gcomp_encoder_destroy(enc);
 * gcomp_options_destroy(opts);
 *
 * // Decompress deflate data with output limit
 * gcomp_options_create(&opts);
 * gcomp_options_set_uint64(opts, "limits.max_output_bytes", 10 * 1024 * 1024);
 *
 * gcomp_decoder_t *dec = NULL;
 * gcomp_decoder_create(registry, "deflate", opts, &dec);
 * gcomp_decoder_update(dec, &input, &output);
 * gcomp_decoder_finish(dec, &output);
 * gcomp_decoder_destroy(dec);
 * gcomp_options_destroy(opts);
 * @endcode
 *
 * Or using the buffer convenience functions:
 *
 * @code
 * gcomp_encode_buffer(NULL, "deflate", NULL, input, in_len, output, out_cap,
 *     &out_len);
 * gcomp_decode_buffer(NULL, "deflate", NULL, compressed, comp_len, output,
 *     out_cap, &out_len);
 * @endcode
 *
 * @note For file storage, consider using "gzip" which adds checksums and
 * metadata. Raw deflate is typically used when another format (like PNG or
 * HTTP Content-Encoding) provides its own framing and integrity checks.
 */

#ifndef GHOTI_IO_GCOMP_DEFLATE_H
#define GHOTI_IO_GCOMP_DEFLATE_H

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The largest expansion RFC 1951 permits, as an output/input ratio.
 *
 * RFC 1951 section 3.2.5 bounds a match at 258 bytes, and a dynamic-Huffman
 * block (section 3.2.7) may give the length code and the distance code one bit
 * each, so two bits of input can carry 258 bytes of output:
 * 258 * 8 / 2 = 1032.  No stream this decoder should accept can do better, so
 * a `limits.max_expansion_ratio` at this value cannot refuse a legitimate
 * stream - it fires only on output the format could not have produced.
 *
 * Measured: 32 MiB of zeros compresses to 993:1 at level 9, just under it.
 *
 * A caller who wants a policy cap rather than an impossibility check sets a
 * smaller number; 0 turns the check off entirely.  Either way
 * `limits.max_output_bytes` remains the hard bound on what a decode may
 * produce.
 */
#define GCOMP_DEFLATE_MAX_EXPANSION_RATIO 1032ULL

/**
 * @brief Register the deflate method with a registry.
 *
 * Call this to make the "deflate" method available for encoding and decoding.
 * Typically used with gcomp_registry_default() or a custom registry.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return ::GCOMP_OK on success
 * @return ::GCOMP_ERR_INVALID_ARG if registry is NULL
 *
 * @note This function is called automatically during library initialization
 *       for the default registry via the auto-registration mechanism.
 *       Manual registration is only needed for custom registries.
 */
GCOMP_API gcomp_status_t gcomp_method_deflate_register(
    gcomp_registry_t * registry);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_DEFLATE_H
