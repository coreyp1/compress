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
 *
 * Copyright 2026 by Corey Pennycuff
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
 * @brief Close a stream opened by gcomp_seekable_open_buffer().
 *
 * Safe on NULL.
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
