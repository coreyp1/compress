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
 * @file seek_table.c
 *
 * The one writer of the Zstandard seek table. See seek_table.h for why there
 * is only one.
 */

#include <ghoti.io/compress/macros.h>

#include "endian.h"
#include "seek_table.h"
#include <ghoti.io/compress/xxhash64.h>
#include <ghoti.io/cutil/safemath.h>
#include <string.h>

gcomp_status_t gcomp_seek_table_init(gcomp_seek_table_t * t,
    const gcomp_allocator_t * alloc, int checksum, size_t hint) {
  if (!t) {
    return GCOMP_ERR_INVALID_ARG;
  }
  memset(t, 0, sizeof(*t));
  t->alloc = alloc;
  t->checksum = checksum ? 1 : 0;

  size_t cap = hint ? hint : 64u;
  if (cap > GCOMP_SEEK_MAX_FRAMES) {
    return GCOMP_ERR_LIMIT;
  }
  size_t words = 0;
  if (!gcu_safe_mul_size(cap, 3u, &words)) {
    return GCOMP_ERR_LIMIT;
  }
  t->entries = gcomp_calloc(alloc, words, sizeof(uint32_t));
  if (!t->entries) {
    return GCOMP_ERR_MEMORY;
  }
  t->cap = cap;
  return GCOMP_OK;
}

gcomp_status_t gcomp_seek_table_append(gcomp_seek_table_t * t,
    uint64_t compressed, uint64_t decompressed, const void * content,
    size_t content_len) {
  if (!t || !t->entries) {
    return GCOMP_ERR_INVALID_ARG;
  }
  // Both sizes are 32-bit fields in the format. A frame bigger than that
  // cannot be described, so it is refused rather than truncated into a table
  // that would place every later frame wrongly.
  if (compressed > 0xFFFFFFFFu || decompressed > 0xFFFFFFFFu) {
    return GCOMP_ERR_LIMIT;
  }
  if (t->count >= GCOMP_SEEK_MAX_FRAMES) {
    return GCOMP_ERR_LIMIT;
  }
  if (t->checksum && (uint64_t)content_len != decompressed) {
    // Hashing a different number of bytes than the entry claims would write a
    // checksum that no reader could reproduce.
    return GCOMP_ERR_INVALID_ARG;
  }
  if (t->checksum && decompressed > 0 && !content) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (t->count == t->cap) {
    size_t next = 0;
    if (!gcu_safe_mul_size(t->cap, 2u, &next)) {
      return GCOMP_ERR_LIMIT;
    }
    if (next > GCOMP_SEEK_MAX_FRAMES) {
      next = GCOMP_SEEK_MAX_FRAMES;
    }
    size_t words = 0;
    if (!gcu_safe_mul_size(next, 3u, &words)) {
      return GCOMP_ERR_LIMIT;
    }
    size_t bytes = 0;
    if (!gcu_safe_mul_size(words, sizeof(uint32_t), &bytes)) {
      return GCOMP_ERR_LIMIT;
    }
    uint32_t * grown = gcomp_realloc(t->alloc, t->entries, bytes);
    if (!grown) {
      return GCOMP_ERR_MEMORY;
    }
    t->entries = grown;
    t->cap = next;
  }

  uint32_t * e = t->entries + t->count * 3u;
  e[0] = (uint32_t)compressed;
  e[1] = (uint32_t)decompressed;
  e[2] = 0;
  if (t->checksum) {
    // The format records the low 32 bits of the XXH64 of the frame's
    // decompressed content.
    gcomp_xxhash64_state_t h;
    gcomp_xxhash64_reset(&h, 0);
    if (content_len > 0) {
      gcomp_xxhash64_update(&h, content, content_len);
    }
    e[2] = (uint32_t)(gcomp_xxhash64_finalize(&h) & 0xFFFFFFFFu);
  }
  t->count++;
  return GCOMP_OK;
}

uint64_t gcomp_seek_table_bytes(const gcomp_seek_table_t * t) {
  if (!t) {
    return 0;
  }
  const uint64_t entry_size = t->checksum ? 12u : 8u;
  return (uint64_t)t->count * entry_size + 8u + GCOMP_SEEK_FOOTER_SIZE;
}

gcomp_status_t gcomp_seek_table_write(const gcomp_seek_table_t * t,
    uint8_t * out, size_t cap, size_t * written_out) {
  if (!t || !out || !written_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  *written_out = 0;

  const uint64_t need = gcomp_seek_table_bytes(t);
  if (need > (uint64_t)cap) {
    return GCOMP_ERR_LIMIT;
  }

  uint8_t * p = out;
  gcomp_write_le32(p, GCOMP_SEEK_TABLE_MAGIC);
  // A skippable frame's size field counts what follows it, so the footer is
  // inside the frame and the header's own eight bytes are not.
  gcomp_write_le32(p + 4u, (uint32_t)(need - 8u));
  p += 8u;

  for (size_t i = 0; i < t->count; i++) {
    const uint32_t * e = t->entries + i * 3u;
    gcomp_write_le32(p, e[0]);
    gcomp_write_le32(p + 4u, e[1]);
    p += 8u;
    if (t->checksum) {
      gcomp_write_le32(p, e[2]);
      p += 4u;
    }
  }

  gcomp_write_le32(p, (uint32_t)t->count);
  p[4] = t->checksum ? (uint8_t)GCOMP_SEEK_DESC_CHECKSUM : (uint8_t)0u;
  gcomp_write_le32(p + 5u, GCOMP_SEEK_FOOTER_MAGIC);

  *written_out = (size_t)need;
  return GCOMP_OK;
}

void gcomp_seek_table_free(gcomp_seek_table_t * t) {
  if (!t) {
    return;
  }
  gcomp_free(t->alloc, t->entries);
  t->entries = NULL;
  t->count = 0;
  t->cap = 0;
}
