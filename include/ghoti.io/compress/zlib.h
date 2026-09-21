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
 * @file zlib.h
 *
 * zlib (RFC 1950) container compression for the Ghoti.io Compress library.
 *
 * This is the wrapper that sits between raw DEFLATE and the formats that
 * actually carry it: two header bytes, the deflate stream, and a four-byte
 * Adler-32 of the uncompressed data.
 *
 * ```
 *   +---+---+                 +---+---+---+---+
 *   |CMF|FLG|  DEFLATE data   |    ADLER32    |
 *   +---+---+                 +---+---+---+---+
 * ```
 *
 * Six bytes of overhead, and what PNG's IDAT stream, HTTP's `deflate`
 * content-coding, PDF's `/FlateDecode` and a great many embedded formats are
 * made of.  `deflate` alone is not interchangeable with any of them: it has
 * no header to identify it and no checksum to verify it.
 *
 * ## Choosing between deflate, zlib and gzip
 *
 * All three compress with the same algorithm and differ only in what is
 * wrapped around it:
 *
 * | Method | Overhead | Checksum | Use when |
 * |---|---|---|---|
 * | `deflate` | 0 bytes | none | Something else already frames and verifies the data |
 * | `zlib` | 6 bytes | Adler-32 of the input | A format asks for RFC 1950 -- PNG, PDF, HTTP `deflate` |
 * | `gzip` | 18+ bytes | CRC-32 of the input | Files on disk; carries a name, a timestamp and a stronger checksum |
 *
 * ## Options
 *
 * Compression is configured with the deflate keys, which pass straight
 * through: `deflate.level`, `deflate.window_bits`, `deflate.strategy`.  The
 * window given there is also what goes into the header's CINFO field, so a
 * decoder is told the truth about what it needs.
 *
 * - `zlib.dictionary` (bytes): a preset dictionary.  See below.
 * - `limits.max_output_bytes`, `limits.max_memory_bytes`,
 *   `limits.max_expansion_ratio`: as elsewhere.
 *
 * ## Preset dictionaries (FDICT)
 *
 * RFC 1950 allows a stream to be compressed against a preset dictionary,
 * flagged by FDICT and identified by a four-byte Adler-32 of the dictionary.
 * RFC 1951 has no notion of one, which is what makes it work: a dictionary is
 * simply history.  The bytes go into deflate's window ahead of the first block
 * and the first distances reach back into them.
 *
 * Set `zlib.dictionary` on an encoder and the header carries FDICT and DICTID;
 * set the same bytes on a decoder and the stream decodes.  Only the last
 * 32 KB of a dictionary is reachable, since a distance cannot exceed the
 * window, so a longer one is used from its tail - but DICTID covers the whole
 * of what was supplied, as RFC 1950 section 2.2 specifies.
 *
 * The identifier is checked, which is what it is for.  Decoding a stream whose
 * FDICT is set without supplying `zlib.dictionary` is `GCOMP_ERR_UNSUPPORTED`
 * and names the id wanted; supplying the wrong one is `GCOMP_ERR_CORRUPT` and
 * names both.  Neither produces plausible nonsense.
 *
 * gcomp_peek() reports the requirement before you commit to a decode:
 * `has_dictionary` and `dictionary_id`.
 *
 * A raw deflate stream has no FDICT bit to announce the need, so there the
 * caller says so with `deflate.dictionary`.
 *
 * FDICT is rare in practice: PNG forbids it outright (PNG §10.3), and HTTP
 * and PDF do not use it.  It earns its keep where many small, similar messages
 * are compressed independently.
 *
 * ## Usage
 *
 * ```c
 * gcomp_encoder_t *encoder = NULL;
 * gcomp_encoder_create(registry, "zlib", options, &encoder);
 * gcomp_encoder_update(encoder, &input, &output);
 * gcomp_encoder_finish(encoder, &output);
 * gcomp_encoder_destroy(encoder);
 * ```
 *
 * Or in one call:
 *
 * ```c
 * gcomp_encode_buffer(NULL, "zlib", NULL, in, in_len, out, out_cap, &out_len);
 * gcomp_decode_buffer(NULL, "zlib", NULL, in, in_len, out, out_cap, &out_len);
 * ```
 */

#ifndef GHOTI_IO_GCOMP_ZLIB_H
#define GHOTI_IO_GCOMP_ZLIB_H

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The largest expansion a zlib stream can carry, output over input.
 *
 * The payload is deflate, so RFC 1951's 1032:1 bound applies unchanged (see
 * ::GCOMP_DEFLATE_MAX_EXPANSION_RATIO for the derivation).  RFC 1950's own
 * framing - a two-byte header, a four-byte Adler-32, and four more when FDICT
 * is set - is input that produces no output, so it can only lower the ratio,
 * never raise it.
 *
 * Measured: 32 MiB of zeros compresses to 1030:1 at level 9.
 */
#define GCOMP_ZLIB_MAX_EXPANSION_RATIO 1032ULL

/**
 * @brief Register the zlib method with a registry.
 *
 * Requires the "deflate" method to be registered in the same registry, which
 * it is by default.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return GCOMP_OK on success, error code on failure
 *
 * @note Called automatically for the default registry via auto-registration.
 */
GCOMP_API gcomp_status_t gcomp_method_zlib_register(
    gcomp_registry_t * registry);

/**
 * @brief What the two header bytes of a zlib stream say.
 *
 * @see gcomp_zlib_peek_header
 */
typedef struct {
  /// Window size as log2, 8 to 15; CINFO + 8.
  uint8_t window_bits;
  /**
   * @brief The FLEVEL hint, 0 (fastest) to 3 (maximum).
   *
   * RFC 1950 section 2.2 says this "need not be set correctly" and that a
   * decompressor must not depend on it.  It is informational, and this
   * library sets it truthfully when encoding.
   */
  uint8_t level_hint;
  /// Non-zero when FDICT is set, meaning a preset dictionary is required.
  uint8_t has_dictionary;
  /// Adler-32 of that dictionary; 0 unless has_dictionary.
  uint32_t dictionary_id;
  /// Bytes of header: 2, or 6 when has_dictionary.
  size_t header_size;
} gcomp_zlib_header_info_t;

/**
 * @brief Read a zlib header without decoding anything.
 *
 * Useful to tell a zlib stream from a raw deflate one, to size a window
 * before decoding, or to see which dictionary a stream wants.
 *
 * Validates the header the same way the decoder does: CM must be 8, CINFO at
 * most 7, and the two bytes together a multiple of 31.
 *
 * @param input Start of the stream.
 * @param input_size Bytes available; 2 is enough unless FDICT is set, when 6
 *        are needed.
 * @param info_out Receives the parsed header.
 * @return GCOMP_OK on success, GCOMP_ERR_LIMIT when @p input_size is too
 *         small to decide, GCOMP_ERR_CORRUPT when this is not a zlib header,
 *         GCOMP_ERR_INVALID_ARG for NULL arguments.
 */
GCOMP_API gcomp_status_t gcomp_zlib_peek_header(const void * input,
    size_t input_size, gcomp_zlib_header_info_t * info_out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_ZLIB_H
