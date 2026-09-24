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
 * @file seekable.h
 *
 * Reading part of a compressed file without decoding the rest of it.
 *
 * ## The problem
 *
 * A compressed stream is read from the beginning. To get the byte at offset
 * 900 MB you decode the 900 MB in front of it, because every back-reference
 * may point anywhere behind. That is what makes a compressed log, archive or
 * column store awkward to sample.
 *
 * The way out is to write the file as several independent frames and record
 * where they are, so a read of one range touches only the frames that overlap
 * it.
 *
 * ## The format
 *
 * The Zstandard seekable format, which is a convention on top of RFC 8878
 * rather than a new container: independent frames, then a skippable frame
 * (section 3.1.2, magic `0x184D2A5E`) holding a table of per-frame compressed
 * and decompressed sizes, and a nine-byte footer ending in the magic number
 * `0x8F92EAB1`.
 *
 * The point of that choice is what it does *not* cost. A skippable frame is
 * something every Zstandard decoder already steps over, so a seekable file is
 * an ordinary multi-frame stream to anything that does not know about the
 * table - `zstd -d` decompresses it, and so does gcomp_decode_buffer().
 * Nothing has to understand seeking to read the file.
 *
 * ## Files without a table
 *
 * A file that has no seek table can still be opened, as long as every frame
 * declares its decompressed size in its own header (RFC 8878 section
 * 3.1.1.1.4). The index is then built by walking the compressed bytes - frame
 * headers only, nothing decoded - which is nearly free and makes any
 * concatenated multi-frame file random-access.
 *
 * A frame that declares no size cannot be placed without decoding everything
 * before it, so a file containing one is refused with ::GCOMP_ERR_UNSUPPORTED
 * rather than opened into something that would silently decode the whole file
 * on every read.
 */

#ifndef GHOTI_IO_GCOMP_SEEKABLE_H
#define GHOTI_IO_GCOMP_SEEKABLE_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief An opened seekable stream.
 *
 * Holds the index and one decoded frame. Not thread-safe: two threads reading
 * one of these share that cached frame. Open one per thread, which costs the
 * index and nothing else.
 */
typedef struct gcomp_seekable_s gcomp_seekable_t;

/**
 * @brief Open a seekable stream that is already in memory.
 *
 * The buffer must outlive the returned object: the index points into it and
 * reads decode straight out of it, so nothing is copied.
 *
 * @param registry Registry to find the method in (NULL for the default)
 * @param method_name Method name; only `"zstd"` supports this today
 * @param options Configuration options for the decodes (may be NULL)
 * @param data Whole compressed file
 * @param size How many bytes
 * @param out Receives the opened stream
 * @return ::GCOMP_OK; ::GCOMP_ERR_UNSUPPORTED when the file cannot be indexed
 *         - a method without seek support, or a frame that declares no
 *         decompressed size; ::GCOMP_ERR_CORRUPT when a seek table is present
 *         but does not describe this file
 */
GCOMP_API gcomp_status_t gcomp_seekable_open_buffer(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options, const void * data,
    size_t size, gcomp_seekable_t ** out);

/**
 * @brief Hands over bytes at an offset, for gcomp_seekable_open_cb().
 *
 * The one thing a seekable reader needs from a byte source: give me @p len
 * bytes starting at @p offset. A file, an object store, an mmap of something
 * larger than memory, a decrypting layer - anything that can answer that.
 *
 * Reads are not sequential and will jump backwards: opening reads the footer
 * at the very end before anything else, and every subsequent read goes
 * straight to a frame. A source that can only move forwards cannot back this.
 *
 * @param ctx The pointer handed to gcomp_seekable_open_cb()
 * @param offset Byte offset in the compressed file to start at
 * @param dst Where to put them
 * @param len How many are wanted
 * @param read_out Receives how many were supplied; fewer than @p len means
 *        end of file, and the caller treats a short read where it needed a
 *        whole structure as ::GCOMP_ERR_CORRUPT
 * @return ::GCOMP_OK, or any error, which is returned to the caller unchanged
 *         - ::GCOMP_ERR_IO is the one to use for a failed read
 */
typedef gcomp_status_t (*gcomp_seek_cb)(
    void * ctx, uint64_t offset, void * dst, size_t len, size_t * read_out);

/**
 * @brief Open a seekable stream whose bytes come from a callback.
 *
 * The counterpart to gcomp_seekable_open_buffer() for a file that is not in
 * memory, and need not fit: nothing larger than one frame plus the seek table
 * is ever held at once. The format and the index are identical - only where
 * the bytes come from differs - so a file written by
 * gcomp_seekable_write_buffer() is read either way, and the answers from
 * gcomp_seekable_size(), gcomp_seekable_frame_count() and
 * gcomp_seekable_read() are the same to the byte.
 *
 * ## What it costs
 *
 * Opening a file that carries a seek table costs two reads and memory for the
 * table: nine bytes for the footer, then the table itself. The frames are
 * never touched.
 *
 * Opening one without a table costs a walk of the frame headers, which reads
 * the whole file through a fixed-size window - no block payload is decoded,
 * but every byte is fetched, so this is the expensive case on a remote source.
 * gcomp_seekable_has_table() says afterwards which happened.
 *
 * A read then costs one fetch of the frame's compressed bytes and one decode
 * of it. The decoded frame is cached, as with a buffer source, so reads
 * within one frame fetch nothing further.
 *
 * ## Lifetime
 *
 * @p ctx must stay valid until gcomp_seekable_close(), which does not touch
 * it - there is no close callback, because a source this borrows is one the
 * caller already knows how to shut down. Close the stream first, then the
 * source.
 *
 * @param registry Registry to find the method in (NULL for the default)
 * @param method_name Method name; only `"zstd"` supports this today
 * @param options Configuration options for the decodes (may be NULL)
 * @param read The source; may not be NULL
 * @param ctx Passed to @p read unchanged, and may be NULL if it needs none
 * @param total_size The compressed file's length in bytes, which the caller
 *        knows and this cannot ask for; a wrong value is reported as
 *        ::GCOMP_ERR_CORRUPT rather than read past
 * @param out Receives the opened stream
 * @return ::GCOMP_OK; ::GCOMP_ERR_INVALID_ARG for a NULL @p read or a zero
 *         @p total_size; ::GCOMP_ERR_UNSUPPORTED when the file cannot be
 *         indexed - a method without seek support, or a frame that declares no
 *         decompressed size; ::GCOMP_ERR_CORRUPT when a seek table is present
 *         but does not describe this file; or whatever @p read returned
 */
GCOMP_API gcomp_status_t gcomp_seekable_open_cb(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options, gcomp_seek_cb read,
    void * ctx, uint64_t total_size, gcomp_seekable_t ** out);

/**
 * @brief Total decompressed size of the stream.
 */
GCOMP_API uint64_t gcomp_seekable_size(const gcomp_seekable_t * s);

/**
 * @brief How many frames the index holds.
 *
 * The granularity of a seek: a read touches only the frames that overlap it,
 * and the smallest amount of work any read can cost is one frame.
 */
GCOMP_API size_t gcomp_seekable_frame_count(const gcomp_seekable_t * s);

/**
 * @brief Non-zero when the file carried a seek table of its own.
 *
 * Zero means the index was built by walking the frames. Both give the same
 * answers; this says which, because "this file is seekable because somebody
 * wrote it that way" and "this file turned out to be indexable" are different
 * facts about it.
 */
GCOMP_API int gcomp_seekable_has_table(const gcomp_seekable_t * s);

/**
 * @brief Read @p len bytes starting at decompressed offset @p offset.
 *
 * Reads past the end return what there is, so a short read means the end of
 * the stream rather than an error.
 *
 * @param s Opened stream
 * @param offset Decompressed byte offset to start at
 * @param dst Where the bytes go
 * @param len How many are wanted
 * @param read_out Receives how many were produced
 * @return ::GCOMP_OK, or the error the underlying decode reported
 */
GCOMP_API gcomp_status_t gcomp_seekable_read(gcomp_seekable_t * s,
    uint64_t offset, void * dst, size_t len, size_t * read_out);

/**
 * @brief Close a stream opened by gcomp_seekable_open_buffer() or
 *        gcomp_seekable_open_cb().
 *
 * Safe on NULL. A stream opened from a callback does not have its @p ctx
 * touched here; close that afterwards.
 */
GCOMP_API void gcomp_seekable_close(gcomp_seekable_t * s);

/**
 * @brief Largest seekable file gcomp_seekable_write_buffer() can produce.
 *
 * The frames, plus the seek table: eight bytes of skippable-frame header,
 * twelve per frame with checksums or eight without, and a nine-byte footer.
 *
 * @param registry Registry to find the method in (NULL for the default)
 * @param method_name Method name; only `"zstd"` supports this today
 * @param options Configuration options (may be NULL)
 * @param input_size Bytes that will be written
 * @param bound_out Receives the bound
 * @return ::GCOMP_OK; ::GCOMP_ERR_UNSUPPORTED for a method without seek
 *         support; ::GCOMP_ERR_LIMIT when the bound does not fit a `size_t`
 */
GCOMP_API gcomp_status_t gcomp_seekable_write_bound(
    gcomp_registry_t * registry, const char * method_name,
    gcomp_options_t * options, uint64_t input_size, size_t * bound_out);

/**
 * @brief Write a seekable file: independent frames, then a seek table.
 *
 * Each frame holds `zstd.seekable_frame_size` decompressed bytes (the last
 * holds what is left) and declares that size in its own header, so the file is
 * indexable even by a reader that ignores the table.
 *
 * `zstd.seekable_checksum` adds the low 32 bits of the XXH64 of each frame's
 * decompressed content to its table entry. It is on by default: the table is
 * what a reader trusts to place a frame, and a table that has been edited
 * while the frames have not is otherwise undetectable.
 *
 * The result is an ordinary Zstandard stream to anything that does not know
 * about seek tables - the table is a skippable frame (RFC 8878 section 3.1.2),
 * which every decoder steps over.
 *
 * @param registry Registry to find the method in (NULL for the default)
 * @param method_name Method name; only `"zstd"` supports this today
 * @param options Configuration options (may be NULL)
 * @param input_data Bytes to compress
 * @param input_size How many
 * @param output Where the file goes
 * @param output_capacity How much room there is; see
 *        gcomp_seekable_write_bound()
 * @param output_size_out Receives how much was written
 * @return ::GCOMP_OK; ::GCOMP_ERR_UNSUPPORTED for a method without seek
 *         support; ::GCOMP_ERR_LIMIT when @p output_capacity is too small
 */
GCOMP_API gcomp_status_t gcomp_seekable_write_buffer(
    gcomp_registry_t * registry, const char * method_name,
    gcomp_options_t * options, const void * input_data, size_t input_size,
    void * output, size_t output_capacity, size_t * output_size_out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SEEKABLE_H
