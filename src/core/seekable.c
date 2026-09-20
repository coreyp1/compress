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
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>

#include "alloc_internal.h"
#include "endian.h"
#include "registry_internal.h"
#include "walk_internal.h"
#include "../methods/zstd/zstd_walk.h"
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/seekable.h>
#include <ghoti.io/cutil/safemath.h>
#include <string.h>

/// Skippable frame carrying a seek table.
#define GCOMP_SEEK_TABLE_MAGIC 0x184D2A5EU
/// Last four bytes of the file, so the table can be found from the end.
#define GCOMP_SEEK_FOOTER_MAGIC 0x8F92EAB1U
/// Number_Of_Frames (4) + Seek_Table_Descriptor (1) + magic (4).
#define GCOMP_SEEK_FOOTER_SIZE 9u
/// Bit 7 of Seek_Table_Descriptor: entries carry a checksum.
#define GCOMP_SEEK_DESC_CHECKSUM 0x80u
/// Bits 0-6 are reserved and a reader must refuse a table that sets them.
#define GCOMP_SEEK_DESC_RESERVED 0x7Fu

/**
 * @brief One frame's place in the file and in the decompressed stream.
 */
typedef struct {
  uint64_t compressed_offset;
  uint64_t compressed_size;
  uint64_t decompressed_offset;
  uint64_t decompressed_size;
} gcomp_seek_entry_t;

struct gcomp_seekable_s {
  const gcomp_allocator_t * allocator;
  gcomp_registry_t * registry;
  gcomp_options_t * options; ///< Cloned, so the caller may change theirs

  const uint8_t * data;
  size_t size;

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
 * @brief Find and validate a seek table at the end of the file.
 *
 * @return ::GCOMP_OK with @p entries_out filled; ::GCOMP_ERR_UNSUPPORTED when
 *         there is no table (which is not an error); ::GCOMP_ERR_CORRUPT when
 *         there is one that does not describe this file
 */
static gcomp_status_t gcomp_seek_read_table(const gcomp_allocator_t * alloc,
    const uint8_t * data, size_t size, gcomp_seek_entry_t ** entries_out,
    size_t * count_out, uint64_t * total_out) {
  if (size < GCOMP_SEEK_FOOTER_SIZE + 8u) {
    return GCOMP_ERR_UNSUPPORTED;
  }
  if (gcomp_read_le32(data + size - 4u) != GCOMP_SEEK_FOOTER_MAGIC) {
    return GCOMP_ERR_UNSUPPORTED;
  }

  const uint8_t descriptor = data[size - 5u];
  if (descriptor & GCOMP_SEEK_DESC_RESERVED) {
    // The spec reserves these and says a reader must reject them; a table
    // whose shape this build does not know is one whose entries it would
    // misread.
    return GCOMP_ERR_CORRUPT;
  }
  const int has_checksums = (descriptor & GCOMP_SEEK_DESC_CHECKSUM) != 0;
  const uint64_t entry_size = has_checksums ? 12u : 8u;

  const uint32_t frames = gcomp_read_le32(data + size - GCOMP_SEEK_FOOTER_SIZE);
  uint64_t entries_bytes = 0;
  if (!gcu_safe_mul_u64((uint64_t)frames, entry_size, &entries_bytes)) {
    return GCOMP_ERR_CORRUPT;
  }
  // The skippable frame is 8 bytes of header, the entries, and the footer.
  uint64_t table_bytes = 0;
  if (!gcu_safe_add_u64(entries_bytes, 8u + GCOMP_SEEK_FOOTER_SIZE,
          &table_bytes) ||
      table_bytes > (uint64_t)size) {
    return GCOMP_ERR_CORRUPT;
  }

  const size_t table_start = size - (size_t)table_bytes;
  if (gcomp_read_le32(data + table_start) != GCOMP_SEEK_TABLE_MAGIC) {
    // The footer magic can occur by chance in compressed bytes, so failing to
    // find the skippable frame it implies means there was no table, not that
    // the file is broken.
    return GCOMP_ERR_UNSUPPORTED;
  }
  if (gcomp_read_le32(data + table_start + 4u) !=
      (uint32_t)(table_bytes - 8u)) {
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

  const uint8_t * p = data + table_start + 8u;
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
      gcomp_free(alloc, entries);
      return GCOMP_ERR_CORRUPT;
    }
  }

  // The frames the table describes must be exactly the bytes in front of it.
  // Checking here is what turns a truncated file into an error at open rather
  // than a decode failure part way through somebody's read.
  if (c_off != (uint64_t)table_start) {
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
    const gcomp_allocator_t * alloc, const uint8_t * data, size_t size,
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

  uint64_t d_off = 0;
  size_t used_total = 0;
  for (;;) {
    gcomp_walk_event_t batch[64];
    size_t used = 0, n = 0;
    const gcomp_status_t s = zstd_walk_update(&w, data + used_total,
        size - used_total, &used, batch, 64, &n);
    if (s != GCOMP_OK) {
      gcomp_free(alloc, entries);
      return GCOMP_ERR_UNSUPPORTED;
    }
    used_total += used;

    for (size_t i = 0; i < n; i++) {
      if (batch[i].kind != GCOMP_WALK_FRAME) {
        continue; // Blocks are finer than a seek needs; skippable frames hold
                  // no data.
      }
      if (batch[i].content_size == 0) {
        gcomp_free(alloc, entries);
        return GCOMP_ERR_UNSUPPORTED;
      }
      if (count == cap) {
        size_t next = 0;
        if (!gcu_safe_mul_size(cap, 2u, &next)) {
          gcomp_free(alloc, entries);
          return GCOMP_ERR_LIMIT;
        }
        gcomp_seek_entry_t * grown =
            gcomp_realloc(alloc, entries, next * sizeof(*entries));
        if (!grown) {
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
        gcomp_free(alloc, entries);
        return GCOMP_ERR_LIMIT;
      }
      count++;
    }
    if (used == 0 && n == 0) {
      break;
    }
  }

  if (used_total != size || count == 0) {
    gcomp_free(alloc, entries);
    return GCOMP_ERR_UNSUPPORTED;
  }

  *entries_out = entries;
  *count_out = count;
  *total_out = d_off;
  return GCOMP_OK;
}

gcomp_status_t gcomp_seekable_open_buffer(gcomp_registry_t * registry,
    const char * method_name, gcomp_options_t * options, const void * data,
    size_t size, gcomp_seekable_t ** out) {
  if (!method_name || !data || size == 0 || !out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  *out = NULL;

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
  const uint8_t * bytes = (const uint8_t *)data;

  gcomp_seek_entry_t * entries = NULL;
  size_t count = 0;
  uint64_t total = 0;
  int has_table = 0;

  gcomp_status_t s =
      gcomp_seek_read_table(alloc, bytes, size, &entries, &count, &total);
  if (s == GCOMP_OK) {
    has_table = 1;
  }
  else if (s == GCOMP_ERR_UNSUPPORTED) {
    s = gcomp_seek_index_by_walking(alloc, bytes, size, &entries, &count,
        &total);
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
  sk->data = bytes;
  sk->size = size;
  sk->entries = entries;
  sk->count = count;
  sk->total_decompressed = total;
  sk->has_table = has_table;
  sk->cached_frame = count; // Nothing cached yet.

  *out = sk;
  return GCOMP_OK;
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

  size_t produced = 0;
  const gcomp_status_t st = gcomp_decode_buffer(s->registry, "zstd",
      s->options, s->data + e->compressed_offset, (size_t)e->compressed_size,
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
