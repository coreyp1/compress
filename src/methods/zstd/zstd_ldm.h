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
 * @file zstd_ldm.h
 *
 * Long-distance matching for the Zstandard encoder.
 *
 * The ordinary match finder reaches at most ::MF_MAX_DISTANCE, about 8 MB
 * (zstd_matchfinder_private.h), and its tables are sized for that.  A file
 * with two copies of the same megabyte thirty megabytes apart therefore
 * compresses as though the second copy were new.  Long-distance matching is
 * the standard answer: a second, much coarser index that covers the whole
 * declared window and looks only for long matches, so that the cost of
 * covering a gigabyte stays proportional to the input rather than to the
 * window.
 *
 * Nothing about it reaches the format.  RFC 8878 constrains an offset only by
 * the window the frame header declares (section 3.1.1.1.2), so a stream with
 * long matches in it is an ordinary Zstandard stream and any decoder reads
 * it - ours already did before this file existed, which was measured against
 * `zstd --long=27` output before any of this was written.  LDM is purely a
 * choice the encoder makes.
 *
 * The shape, following libzstd:
 *
 *   - A rolling hash over @ref zstd_ldm_t::min_match bytes (64 by default,
 *     far above the ordinary finder's 3), so that only matches worth a long
 *     offset are considered at all.
 *   - Positions are *inserted* only every `1 << hash_rate_log` bytes, which
 *     is what keeps the table small enough to cover a whole window; they are
 *     *queried* at every position, so a match is found within
 *     `1 << hash_rate_log` bytes of where it truly begins.
 *   - A hit is confirmed by comparing the bytes.  With a 64-byte minimum a
 *     false hit costs a comparison that nearly always fails in its first
 *     eight bytes, which is why no separate tag is stored.
 *
 * Positions in the table are absolute - counted from the start of the stream
 * rather than from the front of the window - for the same reason the binary
 * tree counts that way: sliding the window is then one addition instead of a
 * pass over a table with millions of entries in it.  See zstd_ldm_slide().
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_ZSTD_ZSTD_LDM_H
#define GHOTI_IO_GCOMP_SRC_METHODS_ZSTD_ZSTD_LDM_H

#include <ghoti.io/compress/macros.h>

#include "zstd_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZSTD_LDM_MIN_MATCH_MIN 16u   ///< Shortest match LDM will look for.
#define ZSTD_LDM_MIN_MATCH_MAX 4096u ///< Longest minimum it will accept.
#define ZSTD_LDM_HASH_LOG_MIN 6u     ///< Smallest table, 64 entries.
#define ZSTD_LDM_HASH_LOG_MAX 30u    ///< Largest table this will allocate.
#define ZSTD_LDM_HASH_RATE_LOG_MAX 12u ///< Coarsest sampling, one in 4096.

/// Defaults, chosen to match `zstd --long`'s behaviour on the same input.
#define ZSTD_LDM_MIN_MATCH_DEFAULT 64u
#define ZSTD_LDM_HASH_RATE_LOG_DEFAULT 6u

/**
 * @brief One long match the scan found, in window coordinates.
 *
 * Window coordinates rather than absolute because the list is produced and
 * consumed inside one call, before the window can move; only the table
 * outlives a block.
 */
typedef struct {
  size_t pos;      ///< Where the match starts, as an index into `data`.
  uint32_t length; ///< Bytes that match.
  uint32_t offset; ///< Distance back to the source.
} zstd_ldm_match_t;

/**
 * @brief The long-distance index, and the matches its last scan found.
 */
typedef struct zstd_ldm_s {
  uint64_t * table; ///< Absolute position plus one; 0 means empty.
  size_t table_size; ///< Entries; a power of two.
  size_t table_mask; ///< table_size - 1.
  unsigned hash_log; ///< Log2 of table_size.

  unsigned min_match;     ///< Bytes the rolling hash covers.
  unsigned hash_rate_log; ///< Insert one position in `1 << this`.
  size_t rate_mask;       ///< (1 << hash_rate_log) - 1.
  uint64_t power;         ///< P^min_match, for removing the oldest byte.

  size_t base_pos;  ///< Absolute position of data[0].
  size_t next_scan; ///< Absolute position the scan has reached.
  size_t max_offset; ///< The declared window; an offset may not exceed it.

  /// Where the match list is allocated from, and what accounts for it.  Kept
  /// here because zstd_mf_generate_sequences() has neither to hand, and the
  /// list grows during a scan; zstd_dict_parsed_t carries its allocator the
  /// same way.
  const gcomp_allocator_t * allocator;
  gcomp_memory_tracker_t * mem_tracker;

  zstd_ldm_match_t * matches; ///< Matches from the last scan, in order.
  size_t match_count;         ///< How many are in @ref matches.
  size_t match_capacity;      ///< How many @ref matches can hold.
} zstd_ldm_t;

/**
 * @brief Allocate the index.
 *
 * @param ldm Zeroed by this call before anything is allocated, so that
 *   zstd_ldm_destroy() is safe on it whether or not this succeeds.
 * @param window_size The declared window; sets both the table size and the
 *   furthest an offset may reach.
 * @param min_match Bytes a match must have to be considered.
 * @param hash_log Log2 of the table size, or 0 to size it from the window.
 * @param hash_rate_log Insert one position in `1 << this`.
 * @return ::GCOMP_OK, ::GCOMP_ERR_INVALID_ARG or ::GCOMP_ERR_MEMORY.
 */
gcomp_status_t zstd_ldm_init(zstd_ldm_t * ldm, const gcomp_allocator_t * alloc,
    size_t window_size, unsigned min_match, unsigned hash_log,
    unsigned hash_rate_log, gcomp_memory_tracker_t * mem_tracker);

/**
 * @brief What zstd_ldm_init() will allocate, without allocating it.
 *
 * Same reason as zstd_mf_memory_estimate(): a window log the caller chose can
 * ask for gigabytes, and a limit checked after those gigabytes are allocated
 * has not limited anything.
 */
size_t zstd_ldm_memory_estimate(
    size_t window_size, unsigned hash_log, unsigned hash_rate_log);

/// Release the index.  A zeroed or already-destroyed @p ldm is accepted.
void zstd_ldm_destroy(zstd_ldm_t * ldm, const gcomp_allocator_t * alloc,
    gcomp_memory_tracker_t * mem_tracker);

/**
 * @brief Move the window on by @p shift bytes.
 *
 * One addition: the table names absolute positions, so an entry that has
 * fallen out of the buffer is simply behind the new base and is rejected
 * when it is read.
 */
void zstd_ldm_slide(zstd_ldm_t * ldm, size_t shift);

/**
 * @brief Scan `[from, to)` for long matches, and index what it passes.
 *
 * Both inserting and querying happen in this one sweep, so a match is always
 * to something strictly earlier.  The matches found replace whatever the
 * previous scan left behind.
 *
 * @param data The window.
 * @param from Where to start scanning, an index into @p data.
 * @param to One past the last position to scan.
 * @param data_size Bytes readable in @p data; a match may extend to here.
 * @return ::GCOMP_OK or ::GCOMP_ERR_MEMORY.
 */
gcomp_status_t zstd_ldm_scan(zstd_ldm_t * ldm, const uint8_t * data,
    size_t from, size_t to, size_t data_size);

/**
 * @brief The recorded match beginning at @p pos, or NULL.
 *
 * Found by binary search, so the answer does not depend on the order the
 * caller asks in - which matters because the shortest-path parse re-parses a
 * segment at the levels that ask for two passes.
 */
const zstd_ldm_match_t * zstd_ldm_at(zstd_ldm_t * ldm, size_t pos);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_METHODS_ZSTD_ZSTD_LDM_H
