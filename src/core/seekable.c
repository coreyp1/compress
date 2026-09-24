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
 * @file seekable.c
 *
 * Random access into a multi-frame Zstandard stream.
 *
 * Two ways to build the index, and the difference between them is only where
 * the numbers come from:
 *
 * - **A seek table**, if the file has one. The Zstandard seekable format
 *   writes it as a skippable frame at the end with magic `0x184D2A5E`,
 *   followed by a nine-byte footer ending in `0x8F92EAB1`.
 * - **The walker**, otherwise. Frame headers carry `Frame_Content_Size` when
 *   the writer recorded it (RFC 8878 section 3.1.1.1.4), and that is all an
 *   index needs. Nothing is decoded either way.
 *
 * The table is checked against the file rather than believed: its frame sizes
 * must add up to exactly the bytes in front of it. A truncated file is then
 * refused at open, which is the point at which it can still be reported as a
 * truncated file rather than as a decode failure halfway through a read.
 */

#include <ghoti.io/compress/macros.h>

#include "alloc_internal.h"
#include "endian.h"
#include "registry_internal.h"
#include "seek_table.h"
#include "walk_internal.h"
#include "../methods/zstd/zstd_walk.h"
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/seekable.h>
#include <ghoti.io/compress/xxhash64.h>
#include <ghoti.io/cutil/safemath.h>
#include <string.h>

// The table's magic numbers, its sizes and its descriptor bits are defined in
// seek_table.h, which is also the only thing that writes them. Reading them
// stays here: a reader has the whole table in front of it and shares no code
// path with either writer.

/**
 * @brief One frame's place in the file and in the decompressed stream.
 */
typedef struct {
  uint64_t compressed_offset;
  uint64_t compressed_size;
  uint64_t decompressed_offset;
  uint64_t decompressed_size;
} gcomp_seek_entry_t;

/**
 * @brief Where a seekable stream's compressed bytes come from.
 *
 * Two cases behind one type, so the indexer and the frame loader are written
 * once. `data` non-NULL is a caller's buffer, addressable throughout and
 * copied from nowhere; `data` NULL is a gcomp_seek_cb that has to be asked.
 *
 * `total` is authoritative in both cases and is what every bound is checked
 * against. For the buffer case it is `size`; for the callback case the caller
 * supplied it, because a callback cannot be asked how long the file is.
 */
typedef struct {
  const uint8_t * data; ///< The whole file, or NULL for a callback source.
  size_t size;          ///< Its length, when `data` is set.
  gcomp_seek_cb read;   ///< The source, when `data` is NULL.
  void * ctx;           ///< Handed to `read` unchanged.
  uint64_t total;       ///< File length; set either way.
} gcomp_seek_source_t;

struct gcomp_seekable_s {
  const gcomp_allocator_t * allocator;
  gcomp_registry_t * registry;
  gcomp_options_t * options; ///< Cloned, so the caller may change theirs

  gcomp_seek_source_t source;

  /**
   * @brief One frame's compressed bytes, for a callback source only.
   *
   * A buffer source decodes straight out of the caller's memory and never
   * allocates this. A callback source has to have the frame in hand before it
   * can be decoded, so it is fetched here and the buffer is kept and regrown,
   * because consecutive reads are usually in the same or a neighbouring frame
   * and those are of similar size.
   */
  uint8_t * raw_frame;
  size_t raw_frame_size;

  gcomp_seek_entry_t * entries;
  size_t count;
  uint64_t total_decompressed;
  int has_table;

  /**
   * @brief The last frame decoded, kept for the next read.
   *
   * One entry rather than a cache: reads of a compressed file are usually
   * sequential or clustered, so the frame just decoded is the one most likely
   * to be wanted next, and anything larger starts needing an eviction policy
   * to justify itself.
   */
  size_t cached_frame; ///< Index, or count when nothing is cached
  uint8_t * cached;
  size_t cached_size;
};

/**
 * @brief Read exactly @p len bytes at @p offset, or fail.
 *
 * Every structure this file reads - the footer, the seek table, a frame - is
 * only meaningful whole, so a short read is a corrupt file rather than
 * something to carry on from with less. The one caller that does want a short
 * read (the walker, at end of file) asks through
 * gcomp_seek_source_read_upto() instead.
 *
 * @return ::GCOMP_OK; ::GCOMP_ERR_CORRUPT when the range runs past the end or
 *         the source supplied less than asked; or the source's own error
 */
static gcomp_status_t gcomp_seek_source_read_exact(
    const gcomp_seek_source_t * src, uint64_t offset, void * dst, size_t len) {
  if (len == 0) {
    return GCOMP_OK;
  }
  uint64_t end = 0;
  if (!gcu_safe_add_u64(offset, (uint64_t)len, &end) || end > src->total) {
    return GCOMP_ERR_CORRUPT;
  }
  if (src->data) {
    memcpy(dst, src->data + (size_t)offset, len);
    return GCOMP_OK;
  }
  // A source is allowed to answer in pieces, the way read(2) may, so this
  // loops rather than demanding the whole range in one call.
  uint8_t * out = (uint8_t *)dst;
  size_t done = 0;
  while (done < len) {
    size_t got = 0;
    const gcomp_status_t s =
        src->read(src->ctx, offset + (uint64_t)done, out + done, len - done,
            &got);
    if (s != GCOMP_OK) {
      return s;
    }
    if (got == 0) {
      return GCOMP_ERR_CORRUPT; // Short where a whole structure was needed.
    }
    if (got > len - done) {
      // A source that claims more than it was offered has scribbled past the
      // end of the buffer. Nothing can be trusted after that.
      return GCOMP_ERR_CORRUPT;
    }
    done += got;
  }
  return GCOMP_OK;
}

/**
 * @brief Read up to @p len bytes at @p offset, stopping short at end of file.
 *
 * For the walker, which asks for a window at a time and is entitled to get
 * less at the end.
 */
static gcomp_status_t gcomp_seek_source_read_upto(
    const gcomp_seek_source_t * src, uint64_t offset, void * dst, size_t len,
    size_t * got_out) {
  *got_out = 0;
  if (offset >= src->total) {
    return GCOMP_OK;
  }
  const uint64_t left = src->total - offset;
  if ((uint64_t)len > left) {
    len = (size_t)left;
  }
  const gcomp_status_t s = gcomp_seek_source_read_exact(src, offset, dst, len);
  if (s != GCOMP_OK) {
    return s;
  }
  *got_out = len;
  return GCOMP_OK;
}

/**
 * @brief Find and validate a seek table at the end of the file.
 *
 * @return ::GCOMP_OK with @p entries_out filled; ::GCOMP_ERR_UNSUPPORTED when
 *         there is no table (which is not an error); ::GCOMP_ERR_CORRUPT when
 *         there is one that does not describe this file
 */
static gcomp_status_t gcomp_seek_read_table(const gcomp_allocator_t * alloc,
    const gcomp_seek_source_t * src, gcomp_seek_entry_t ** entries_out,
    size_t * count_out, uint64_t * total_out) {
  const uint64_t size = src->total;
  if (size < (uint64_t)GCOMP_SEEK_FOOTER_SIZE + 8u) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  // The footer first, because it says how big the table is and therefore how
  // much to ask a callback source for. Nine bytes, at the very end.
  uint8_t footer[GCOMP_SEEK_FOOTER_SIZE];
  {
    const gcomp_status_t fs = gcomp_seek_source_read_exact(
        src, size - GCOMP_SEEK_FOOTER_SIZE, footer, sizeof(footer));
    if (fs != GCOMP_OK) {
      return fs;
    }
  }
  if (gcomp_read_le32(footer + GCOMP_SEEK_FOOTER_SIZE - 4u) !=
      GCOMP_SEEK_FOOTER_MAGIC) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  const uint8_t descriptor = footer[GCOMP_SEEK_FOOTER_SIZE - 5u];
  if (descriptor & GCOMP_SEEK_DESC_RESERVED) {
    // The spec reserves these and says a reader must reject them; a table
    // whose shape this build does not know is one whose entries it would
    // misread.
    return GCOMP_ERR_CORRUPT;
  }
  const int has_checksums = (descriptor & GCOMP_SEEK_DESC_CHECKSUM) != 0;
  const uint64_t entry_size = has_checksums ? 12u : 8u;

  const uint32_t frames = gcomp_read_le32(footer);
  uint64_t entries_bytes = 0;
  if (!gcu_safe_mul_u64((uint64_t)frames, entry_size, &entries_bytes)) {
    return GCOMP_ERR_CORRUPT;
  }
  // The skippable frame is 8 bytes of header, the entries, and the footer.
  uint64_t table_bytes = 0;
  if (!gcu_safe_add_u64(entries_bytes, 8u + GCOMP_SEEK_FOOTER_SIZE,
          &table_bytes) ||
      table_bytes > size) {
    return GCOMP_ERR_CORRUPT;
  }

  const uint64_t table_start = size - table_bytes;

  // The skippable frame's own eight-byte header.
  uint8_t skip_header[8];
  {
    const gcomp_status_t hs =
        gcomp_seek_source_read_exact(src, table_start, skip_header, 8u);
    if (hs != GCOMP_OK) {
      return hs;
    }
  }
  if (gcomp_read_le32(skip_header) != GCOMP_SEEK_TABLE_MAGIC) {
    // The footer magic can occur by chance in compressed bytes, so failing to
    // find the skippable frame it implies means there was no table, not that
    // the file is broken.
    return GCOMP_ERR_UNSUPPORTED;
  }
  if (gcomp_read_le32(skip_header + 4u) != (uint32_t)(table_bytes - 8u)) {
    return GCOMP_ERR_CORRUPT;
  }

  if (frames == 0) {
    *entries_out = NULL;
    *count_out = 0;
    *total_out = 0;
    return GCOMP_OK;
  }

  gcomp_seek_entry_t * entries =
      gcomp_calloc(alloc, frames, sizeof(*entries));
  if (!entries) {
    return GCOMP_ERR_MEMORY;
  }

  // The entries themselves. A buffer source could be read in place, but the
  // entries are at most twelve bytes each and reading them the same way for
  // both sources is what keeps one code path below rather than two.
  if (entries_bytes > (uint64_t)SIZE_MAX) {
    gcomp_free(alloc, entries);
    return GCOMP_ERR_LIMIT;
  }
  uint8_t * raw = gcomp_malloc(alloc, (size_t)entries_bytes);
  if (!raw) {
    gcomp_free(alloc, entries);
    return GCOMP_ERR_MEMORY;
  }
  {
    const gcomp_status_t es = gcomp_seek_source_read_exact(
        src, table_start + 8u, raw, (size_t)entries_bytes);
    if (es != GCOMP_OK) {
      gcomp_free(alloc, raw);
      gcomp_free(alloc, entries);
      return es;
    }
  }

  const uint8_t * p = raw;
  uint64_t c_off = 0;
  uint64_t d_off = 0;
  for (uint32_t i = 0; i < frames; i++) {
    const uint32_t c = gcomp_read_le32(p);
    const uint32_t d = gcomp_read_le32(p + 4u);
    p += entry_size;

    entries[i].compressed_offset = c_off;
    entries[i].compressed_size = c;
    entries[i].decompressed_offset = d_off;
    entries[i].decompressed_size = d;

    if (!gcu_safe_add_u64(c_off, c, &c_off) ||
        !gcu_safe_add_u64(d_off, d, &d_off)) {
      gcomp_free(alloc, raw);
      gcomp_free(alloc, entries);
      return GCOMP_ERR_CORRUPT;
    }
  }

  gcomp_free(alloc, raw);

  // The frames the table describes must be exactly the bytes in front of it.
  // Checking here is what turns a truncated file into an error at open rather
  // than a decode failure part way through somebody's read.
  if (c_off != table_start) {
    gcomp_free(alloc, entries);
    return GCOMP_ERR_CORRUPT;
  }

  *entries_out = entries;
  *count_out = frames;
  *total_out = d_off;
  return GCOMP_OK;
}

/**
 * @brief Build an index by walking the frames, for a file with no seek table.
 *
 * Costs the frame headers and nothing else. A frame that declares no
 * decompressed size cannot be placed - everything before it would have to be
 * decoded to know where it starts in the output - so the file is refused here
 * rather than opened into something that decodes it all on every read.
 */
static gcomp_status_t gcomp_seek_index_by_walking(
    const gcomp_allocator_t * alloc, const gcomp_seek_source_t * src,
    gcomp_seek_entry_t ** entries_out, size_t * count_out,
    uint64_t * total_out) {
  zstd_walk_t w;
  memset(&w, 0, sizeof(w));

  size_t cap = 64;
  size_t count = 0;
  gcomp_seek_entry_t * entries = gcomp_malloc(alloc, cap * sizeof(*entries));
  if (!entries) {
    return GCOMP_ERR_MEMORY;
  }

  // A buffer source is walked where it lies. A callback source is walked
  // through a window, which is what keeps this O(1) in memory for a file
  // larger than memory.
  //
  // The window has to be able to hold one whole header, because
  // zstd_walk_update() consumes nothing across a header that is not all
  // present and would otherwise make no progress. A zstd frame header is at
  // most 18 bytes and a block header 3, so 64 KiB is four orders of magnitude
  // of margin; the loop still checks for no progress rather than assuming it,
  // because a walker change is not a thing this function would otherwise
  // notice.
  const size_t window = 64u * 1024u;
  uint8_t * buf = NULL;
  size_t held = 0; ///< Bytes in `buf` not yet consumed by the walker.
  if (!src->data) {
    buf = gcomp_malloc(alloc, window);
    if (!buf) {
      gcomp_free(alloc, entries);
      return GCOMP_ERR_MEMORY;
    }
  }

  uint64_t d_off = 0;
  uint64_t used_total = 0; ///< File offset the walker has reached.
  for (;;) {
    gcomp_walk_event_t batch[64];
    size_t used = 0, n = 0;
    gcomp_status_t s;

    if (src->data) {
      s = zstd_walk_update(&w, src->data + (size_t)used_total,
          src->size - (size_t)used_total, &used, batch, 64, &n);
    }
    else {
      // Top the window up from where the walker has got to, keeping whatever
      // it declined to consume last time at the front.
      size_t got = 0;
      const gcomp_status_t rs = gcomp_seek_source_read_upto(src,
          used_total + (uint64_t)held, buf + held, window - held, &got);
      if (rs != GCOMP_OK) {
        gcomp_free(alloc, buf);
        gcomp_free(alloc, entries);
        return rs;
      }
      held += got;
      if (held == 0) {
        break; // End of file, and nothing left over.
      }
      s = zstd_walk_update(&w, buf, held, &used, batch, 64, &n);
      if (s == GCOMP_OK) {
        if (used == 0 && n == 0 && got == 0) {
          // The walker wants more and there is no more: a unit's header runs
          // past the end of the file.
          gcomp_free(alloc, buf);
          gcomp_free(alloc, entries);
          return GCOMP_ERR_UNSUPPORTED;
        }
        if (used == 0 && held == window) {
          // A full window it will not consume any of. Cannot happen with
          // headers this small, and would be an infinite loop if it did.
          gcomp_free(alloc, buf);
          gcomp_free(alloc, entries);
          return GCOMP_ERR_UNSUPPORTED;
        }
        memmove(buf, buf + used, held - used);
        held -= used;
      }
    }

    if (s != GCOMP_OK) {
      gcomp_free(alloc, buf);
      gcomp_free(alloc, entries);
      return GCOMP_ERR_UNSUPPORTED;
    }
    used_total += (uint64_t)used;

    for (size_t i = 0; i < n; i++) {
      if (batch[i].kind != GCOMP_WALK_FRAME) {
        continue; // Blocks are finer than a seek needs; skippable frames hold
                  // no data.
      }
      if (batch[i].content_size == 0) {
        gcomp_free(alloc, buf);
        gcomp_free(alloc, entries);
        return GCOMP_ERR_UNSUPPORTED;
      }
      if (count == cap) {
        size_t next = 0;
        if (!gcu_safe_mul_size(cap, 2u, &next)) {
          gcomp_free(alloc, buf);
          gcomp_free(alloc, entries);
          return GCOMP_ERR_LIMIT;
        }
        gcomp_seek_entry_t * grown =
            gcomp_realloc(alloc, entries, next * sizeof(*entries));
        if (!grown) {
          gcomp_free(alloc, buf);
          gcomp_free(alloc, entries);
          return GCOMP_ERR_MEMORY;
        }
        entries = grown;
        cap = next;
      }
      entries[count].compressed_offset = batch[i].offset;
      entries[count].compressed_size = batch[i].size;
      entries[count].decompressed_offset = d_off;
      entries[count].decompressed_size = batch[i].content_size;
      if (!gcu_safe_add_u64(d_off, batch[i].content_size, &d_off)) {
        gcomp_free(alloc, buf);
        gcomp_free(alloc, entries);
        return GCOMP_ERR_LIMIT;
      }
      count++;
    }
    if (used == 0 && n == 0) {
      break;
    }
  }

  gcomp_free(alloc, buf);

  if (used_total != src->total || count == 0) {
    gcomp_free(alloc, entries);
    return GCOMP_ERR_UNSUPPORTED;
  }

  *entries_out = entries;
  *count_out = count;
  *total_out = d_off;
  return GCOMP_OK;
}

/**
 * @brief Index a source and wrap it in a gcomp_seekable_t.
 *
 * The whole of both open functions except for validating their own arguments,
 * so a buffer source and a callback source cannot come to different
 * conclusions about one file - which is the property
 * test_seekable.cpp's cross-checks assert.
 */
static gcomp_status_t gcomp_seekable_open_source(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options,
    const gcomp_seek_source_t * src, gcomp_seekable_t ** out) {
  // Only Zstandard for now, and deliberately. LZ4 blocks that are independent
  // can be decoded alone, but a compressed block's header does not say what it
  // expands to, so an index for one cannot be built without decoding the file
  // - which is the thing this exists to avoid.
  if (strcmp(method_name, "zstd") != 0) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  if (!registry) {
    registry = gcomp_registry_default();
    if (!registry) {
      return GCOMP_ERR_INTERNAL;
    }
  }
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);

  gcomp_seek_entry_t * entries = NULL;
  size_t count = 0;
  uint64_t total = 0;
  int has_table = 0;

  gcomp_status_t s =
      gcomp_seek_read_table(alloc, src, &entries, &count, &total);
  if (s == GCOMP_OK) {
    has_table = 1;
  }
  else if (s == GCOMP_ERR_UNSUPPORTED) {
    s = gcomp_seek_index_by_walking(alloc, src, &entries, &count, &total);
    if (s != GCOMP_OK) {
      return s;
    }
  }
  else {
    return s; // A table that is present and wrong.
  }

  gcomp_seekable_t * sk = gcomp_calloc(alloc, 1, sizeof(*sk));
  if (!sk) {
    gcomp_free(alloc, entries);
    return GCOMP_ERR_MEMORY;
  }

  // The caller's options may be changed or destroyed while this is open, and
  // every read uses them.
  if (options) {
    if (gcomp_options_clone(options, &sk->options) != GCOMP_OK) {
      gcomp_free(alloc, entries);
      gcomp_free(alloc, sk);
      return GCOMP_ERR_MEMORY;
    }
  }

  sk->allocator = alloc;
  sk->registry = registry;
  sk->source = *src;
  sk->entries = entries;
  sk->count = count;
  sk->total_decompressed = total;
  sk->has_table = has_table;
  sk->cached_frame = count; // Nothing cached yet.

  *out = sk;
  return GCOMP_OK;
}

gcomp_status_t gcomp_seekable_open_buffer(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options, const void * data,
    size_t size, gcomp_seekable_t ** out) {
  if (!method_name || !data || size == 0 || !out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  *out = NULL;

  gcomp_seek_source_t src;
  memset(&src, 0, sizeof(src));
  src.data = (const uint8_t *)data;
  src.size = size;
  src.total = (uint64_t)size;

  return gcomp_seekable_open_source(registry, method_name, options, &src, out);
}

gcomp_status_t gcomp_seekable_open_cb(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options, gcomp_seek_cb read,
    void * ctx, uint64_t total_size, gcomp_seekable_t ** out) {
  if (!method_name || !read || total_size == 0 || !out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  *out = NULL;

  gcomp_seek_source_t src;
  memset(&src, 0, sizeof(src));
  src.read = read;
  src.ctx = ctx;
  src.total = total_size;

  return gcomp_seekable_open_source(registry, method_name, options, &src, out);
}

uint64_t gcomp_seekable_size(const gcomp_seekable_t * s) {
  return s ? s->total_decompressed : 0;
}

size_t gcomp_seekable_frame_count(const gcomp_seekable_t * s) {
  return s ? s->count : 0;
}

int gcomp_seekable_has_table(const gcomp_seekable_t * s) {
  return s ? s->has_table : 0;
}

void gcomp_seekable_close(gcomp_seekable_t * s) {
  if (!s) {
    return;
  }
  const gcomp_allocator_t * alloc = s->allocator;
  gcomp_free(alloc, s->cached);
  gcomp_free(alloc, s->raw_frame);
  gcomp_free(alloc, s->entries);
  if (s->options) {
    gcomp_options_destroy(s->options);
  }
  gcomp_free(alloc, s);
}

/**
 * @brief Which frame holds decompressed offset @p offset?
 *
 * Binary search, because the whole point is that a read does not walk the file
 * - walking the index instead would put the cost back, just smaller.
 */
static size_t gcomp_seek_find_frame(
    const gcomp_seekable_t * s, uint64_t offset) {
  size_t lo = 0;
  size_t hi = s->count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2u;
    if (offset < s->entries[mid].decompressed_offset) {
      hi = mid;
    }
    else if (offset >= s->entries[mid].decompressed_offset +
            s->entries[mid].decompressed_size) {
      lo = mid + 1u;
    }
    else {
      return mid;
    }
  }
  return s->count;
}

/**
 * @brief Decode one frame into the cache, unless it is already there.
 */
static gcomp_status_t gcomp_seek_load_frame(
    gcomp_seekable_t * s, size_t index) {
  if (s->cached_frame == index && s->cached) {
    return GCOMP_OK;
  }
  const gcomp_seek_entry_t * e = &s->entries[index];
  const size_t want = (size_t)e->decompressed_size;

  if (s->cached_size < want) {
    uint8_t * grown = gcomp_realloc(s->allocator, s->cached, want ? want : 1u);
    if (!grown) {
      return GCOMP_ERR_MEMORY;
    }
    s->cached = grown;
    s->cached_size = want;
  }

  // Where the frame's compressed bytes are. A buffer source has them already;
  // a callback source has to be asked, into a buffer kept across reads.
  const uint8_t * compressed = NULL;
  if (s->source.data) {
    compressed = s->source.data + (size_t)e->compressed_offset;
  }
  else {
    if (e->compressed_size > (uint64_t)SIZE_MAX) {
      return GCOMP_ERR_LIMIT;
    }
    const size_t need = (size_t)e->compressed_size;
    if (s->raw_frame_size < need) {
      uint8_t * grown =
          gcomp_realloc(s->allocator, s->raw_frame, need ? need : 1u);
      if (!grown) {
        return GCOMP_ERR_MEMORY;
      }
      s->raw_frame = grown;
      s->raw_frame_size = need;
    }
    const gcomp_status_t rs = gcomp_seek_source_read_exact(
        &s->source, e->compressed_offset, s->raw_frame, need);
    if (rs != GCOMP_OK) {
      s->cached_frame = s->count;
      return rs;
    }
    compressed = s->raw_frame;
  }

  size_t produced = 0;
  const gcomp_status_t st = gcomp_decode_buffer(s->registry, "zstd",
      s->options, compressed, (size_t)e->compressed_size,
      s->cached, s->cached_size ? s->cached_size : 1u, &produced);
  if (st != GCOMP_OK) {
    s->cached_frame = s->count;
    return st;
  }
  // The index said how big this frame is. A frame that decodes to something
  // else means the index and the file disagree, and every offset after it is
  // then wrong - so this is corrupt rather than merely surprising.
  if (produced != want) {
    s->cached_frame = s->count;
    return GCOMP_ERR_CORRUPT;
  }
  s->cached_frame = index;
  return GCOMP_OK;
}

gcomp_status_t gcomp_seekable_read(gcomp_seekable_t * s, uint64_t offset,
    void * dst, size_t len, size_t * read_out) {
  if (!s || (!dst && len > 0) || !read_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  *read_out = 0;
  if (len == 0 || offset >= s->total_decompressed) {
    return GCOMP_OK; // Past the end is a short read, not a failure.
  }

  uint8_t * o = (uint8_t *)dst;
  size_t done = 0;

  while (done < len && offset + done < s->total_decompressed) {
    const size_t idx = gcomp_seek_find_frame(s, offset + done);
    if (idx >= s->count) {
      return GCOMP_ERR_CORRUPT; // The index does not cover its own total.
    }
    const gcomp_status_t st = gcomp_seek_load_frame(s, idx);
    if (st != GCOMP_OK) {
      *read_out = done;
      return st;
    }
    const gcomp_seek_entry_t * e = &s->entries[idx];
    const uint64_t within = (offset + done) - e->decompressed_offset;
    size_t take = (size_t)(e->decompressed_size - within);
    if (take > len - done) {
      take = len - done;
    }
    memcpy(o + done, s->cached + within, take);
    done += take;
  }

  *read_out = done;
  return GCOMP_OK;
}

//
// Writing
//

/// Default decompressed bytes per frame; `zstd.seekable_frame_size`.
#define GCOMP_SEEK_DEFAULT_FRAME_SIZE (1024u * 1024u)

/**
 * @brief Read the two options that shape a seekable file.
 */
static void gcomp_seek_write_settings(
    gcomp_options_t * options, uint64_t * frame_size_out, int * checksum_out) {
  uint64_t frame_size = GCOMP_SEEK_DEFAULT_FRAME_SIZE;
  int checksum = 1;
  if (options) {
    uint64_t v = 0;
    if (gcomp_options_get_uint64(options, "zstd.seekable_frame_size", &v) ==
            GCOMP_OK &&
        v > 0) {
      frame_size = v;
    }
    int b = 0;
    if (gcomp_options_get_bool(options, "zstd.seekable_checksum", &b) ==
        GCOMP_OK) {
      checksum = b ? 1 : 0;
    }
  }
  *frame_size_out = frame_size;
  *checksum_out = checksum;
}

/// Frames a file of @p input_size bytes will be cut into.
static uint64_t gcomp_seek_frame_count_for(
    uint64_t input_size, uint64_t frame_size) {
  if (input_size == 0) {
    return 0;
  }
  return (input_size + frame_size - 1u) / frame_size;
}

gcomp_status_t gcomp_seekable_write_bound(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options, uint64_t input_size,
    size_t * bound_out) {
  if (!method_name || !bound_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (strcmp(method_name, "zstd") != 0) {
    return GCOMP_ERR_UNSUPPORTED;
  }
  if (!registry) {
    registry = gcomp_registry_default();
    if (!registry) {
      return GCOMP_ERR_INTERNAL;
    }
  }

  uint64_t frame_size = 0;
  int checksum = 0;
  gcomp_seek_write_settings(options, &frame_size, &checksum);
  const uint64_t frames = gcomp_seek_frame_count_for(input_size, frame_size);

  // Each frame is bounded on its own, because that is what the encoder will be
  // asked for: a bound taken over the whole input would be smaller than the
  // sum of the per-frame bounds, and smaller is the wrong direction.
  uint64_t total = 0;
  if (frames > 0) {
    size_t full_bound = 0;
    gcomp_status_t s = gcomp_encode_bound(
        registry, "zstd", options, (size_t)frame_size, &full_bound);
    if (s != GCOMP_OK) {
      return s;
    }
    const uint64_t last_len = input_size - (frames - 1u) * frame_size;
    size_t last_bound = 0;
    s = gcomp_encode_bound(
        registry, "zstd", options, (size_t)last_len, &last_bound);
    if (s != GCOMP_OK) {
      return s;
    }
    if (!gcu_safe_mul_u64(frames - 1u, (uint64_t)full_bound, &total) ||
        !gcu_safe_add_u64(total, (uint64_t)last_bound, &total)) {
      return GCOMP_ERR_LIMIT;
    }
  }

  const uint64_t entry_size = checksum ? 12u : 8u;
  uint64_t table = 0;
  if (!gcu_safe_mul_u64(frames, entry_size, &table) ||
      !gcu_safe_add_u64(table, 8u + GCOMP_SEEK_FOOTER_SIZE, &table) ||
      !gcu_safe_add_u64(total, table, &total)) {
    return GCOMP_ERR_LIMIT;
  }
  if (total > (uint64_t)(size_t)-1) {
    return GCOMP_ERR_LIMIT;
  }
  *bound_out = (size_t)total;
  return GCOMP_OK;
}

gcomp_status_t gcomp_seekable_write_buffer(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options,
    const void * input_data, size_t input_size, void * output,
    size_t output_capacity, size_t * output_size_out) {
  if (!method_name || !output || !output_size_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (!input_data && input_size > 0) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (strcmp(method_name, "zstd") != 0) {
    return GCOMP_ERR_UNSUPPORTED;
  }
  *output_size_out = 0;

  if (!registry) {
    registry = gcomp_registry_default();
    if (!registry) {
      return GCOMP_ERR_INTERNAL;
    }
  }
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);

  uint64_t frame_size = 0;
  int checksum = 0;
  gcomp_seek_write_settings(options, &frame_size, &checksum);
  const uint64_t frames =
      gcomp_seek_frame_count_for((uint64_t)input_size, frame_size);
  if (frames > 0xFFFFFFFFu) {
    return GCOMP_ERR_LIMIT; // Number_Of_Frames is four bytes.
  }

  // The frame count is known here, so the table is sized once and never grown.
  gcomp_seek_table_t table;
  {
    const gcomp_status_t ts = gcomp_seek_table_init(
        &table, alloc, checksum, frames ? (size_t)frames : 1u);
    if (ts != GCOMP_OK) {
      return ts;
    }
  }

  // Each frame declares its own decompressed size, so the file is indexable by
  // walking even if the table is lost or ignored. That redundancy is the
  // point: the table makes opening cheap, the headers make it possible.
  gcomp_options_t * frame_options = NULL;
  if (options) {
    if (gcomp_options_clone(options, &frame_options) != GCOMP_OK) {
      gcomp_seek_table_free(&table);
      return GCOMP_ERR_MEMORY;
    }
  }
  else if (gcomp_options_create(&frame_options) != GCOMP_OK) {
    gcomp_seek_table_free(&table);
    return GCOMP_ERR_MEMORY;
  }

  const uint8_t * in = (const uint8_t *)input_data;
  uint8_t * out = (uint8_t *)output;
  size_t written = 0;
  gcomp_status_t status = GCOMP_OK;

  for (uint64_t i = 0; i < frames; i++) {
    const uint64_t at = i * frame_size;
    uint64_t len = frame_size;
    if (at + len > (uint64_t)input_size) {
      len = (uint64_t)input_size - at;
    }

    if (gcomp_options_set_uint64(frame_options, "zstd.content_size", len) !=
        GCOMP_OK) {
      status = GCOMP_ERR_INTERNAL;
      break;
    }

    size_t produced = 0;
    status = gcomp_encode_buffer(registry, "zstd", frame_options, in + at,
        (size_t)len, out + written, output_capacity - written, &produced);
    if (status != GCOMP_OK) {
      break;
    }
    // The 32-bit field checks and the checksum both live in the table now.
    status = gcomp_seek_table_append(
        &table, (uint64_t)produced, len, in + at, (size_t)len);
    if (status != GCOMP_OK) {
      break;
    }
    written += produced;
  }

  if (status == GCOMP_OK) {
    size_t table_written = 0;
    status = gcomp_seek_table_write(
        &table, out + written, output_capacity - written, &table_written);
    if (status == GCOMP_OK) {
      written += table_written;
    }
  }

  gcomp_options_destroy(frame_options);
  gcomp_seek_table_free(&table);

  if (status != GCOMP_OK) {
    *output_size_out = 0;
    return status;
  }
  *output_size_out = written;
  return GCOMP_OK;
}
