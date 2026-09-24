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
 * @file seek_table.h
 *
 * Accumulating a Zstandard seek table, and writing it out.
 *
 * Internal. The seek table is the skippable frame a seekable file ends with:
 * a magic number, one entry per frame, and a nine-byte footer (the format is
 * described in seekable.h).
 *
 * This exists because there are now two writers. gcomp_seekable_write_buffer()
 * has the whole input and knows the frame count before it starts; the
 * streaming encoder's `zstd.seekable` mode discovers frames as it goes and
 * cannot know how many there will be. Both have to emit the identical
 * structure, and a format written out in two places is one that drifts - so
 * the entries are accumulated here, the bytes are laid down here, and neither
 * caller spells out a field.
 *
 * Reading stays in seekable.c: a reader has the whole table in front of it and
 * shares none of this code path.
 */

#ifndef GHOTI_IO_GCOMP_SEEK_TABLE_H
#define GHOTI_IO_GCOMP_SEEK_TABLE_H

#include <ghoti.io/compress/macros.h>

#include "alloc_internal.h"
#include <ghoti.io/compress/errors.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Magic of the skippable frame holding the table (RFC 8878 section 3.1.2).
#define GCOMP_SEEK_TABLE_MAGIC 0x184D2A5Eu
/// Magic the nine-byte footer ends with.
#define GCOMP_SEEK_FOOTER_MAGIC 0x8F92EAB1u
/// Number_Of_Frames (4) + Seek_Table_Descriptor (1) + the magic (4).
#define GCOMP_SEEK_FOOTER_SIZE 9u
/// Bit 7 of Seek_Table_Descriptor: entries carry a checksum.
#define GCOMP_SEEK_DESC_CHECKSUM 0x80u
/// Bits 0-6 are reserved and a reader must refuse a table that sets them.
#define GCOMP_SEEK_DESC_RESERVED 0x7Fu

/// Number_Of_Frames is four bytes, so this is the most a table can describe.
#define GCOMP_SEEK_MAX_FRAMES 0xFFFFFFFFu

/**
 * @brief A seek table being built.
 *
 * Three 32-bit words per frame whether or not checksums are on, because the
 * cost of carrying an unused word is a word and the cost of two layouts is two
 * layouts. The third is simply not written when `checksum` is zero.
 */
typedef struct {
  const gcomp_allocator_t * alloc;
  uint32_t * entries; ///< 3 words per frame: compressed, decompressed, checksum
  size_t count;       ///< Frames recorded.
  size_t cap;         ///< Frames the allocation holds.
  int checksum;       ///< Record a checksum per frame.
} gcomp_seek_table_t;

/**
 * @brief Start a table.
 *
 * @param t Zeroed by this; free with gcomp_seek_table_free()
 * @param alloc Allocator to use
 * @param checksum Non-zero to record a checksum per frame
 * @param hint Frames to make room for up front; 0 for a default. A caller that
 *        knows the count exactly avoids every reallocation by passing it
 * @return ::GCOMP_OK or ::GCOMP_ERR_MEMORY
 */
gcomp_status_t gcomp_seek_table_init(gcomp_seek_table_t * t,
    const gcomp_allocator_t * alloc, int checksum, size_t hint);

/**
 * @brief Record one frame.
 *
 * @param t The table
 * @param compressed The frame's length in the file
 * @param decompressed What it expands to
 * @param content The decompressed bytes, for the checksum; may be NULL when
 *        checksums are off, and is not retained
 * @param content_len How many, which must equal @p decompressed when a
 *        checksum is being computed
 * @return ::GCOMP_OK; ::GCOMP_ERR_LIMIT when either size does not fit 32 bits
 *         or the frame count would exceed ::GCOMP_SEEK_MAX_FRAMES;
 *         ::GCOMP_ERR_MEMORY; ::GCOMP_ERR_INVALID_ARG when a checksum was
 *         asked for and @p content does not match @p decompressed
 */
gcomp_status_t gcomp_seek_table_append(gcomp_seek_table_t * t,
    uint64_t compressed, uint64_t decompressed, const void * content,
    size_t content_len);

/**
 * @brief How many bytes gcomp_seek_table_write() will lay down.
 *
 * The skippable frame's eight-byte header, the entries, and the footer.
 * Callers size an output buffer with this before writing, so it is exact
 * rather than an upper bound.
 */
uint64_t gcomp_seek_table_bytes(const gcomp_seek_table_t * t);

/**
 * @brief Write the table.
 *
 * @param t The table
 * @param out Where it goes
 * @param cap How much room there is
 * @param written_out Receives how much was used
 * @return ::GCOMP_OK; ::GCOMP_ERR_LIMIT when @p cap is smaller than
 *         gcomp_seek_table_bytes()
 */
gcomp_status_t gcomp_seek_table_write(const gcomp_seek_table_t * t,
    uint8_t * out, size_t cap, size_t * written_out);

/// Release the table. Safe on a zeroed or already-freed one.
void gcomp_seek_table_free(gcomp_seek_table_t * t);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SEEK_TABLE_H
