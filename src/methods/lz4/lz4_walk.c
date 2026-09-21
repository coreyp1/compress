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
 * @file lz4_walk.c
 *
 * Walking an LZ4 frame stream: where its frames and blocks are, without
 * decoding any of them.
 *
 * The LZ4 frame format puts a four-byte size in front of every block, so the
 * structure of a stream can be read by stepping over payloads. The same
 * contract as the Zstandard walker: an event is reported when the unit it
 * describes has been wholly accounted for, never when its header merely says
 * how big it is.
 *
 * ## Why this one matters more than the Zstandard walker
 *
 * Blocks inside a Zstandard frame share a window and cannot be decoded apart.
 * LZ4 blocks can, when the frame sets B.Indep - which is this library's
 * default and what its parallel encoder emits. So for LZ4 the *block* is the
 * unit a decode job takes, and there are many of them per frame, where for
 * Zstandard the unit is a whole frame and a single-frame stream offers no
 * parallelism at all.
 *
 * ## Two things the format does not tell you
 *
 * There is no "last block" flag. A frame ends at an EndMark - a block size of
 * zero - so a block cannot be known to be the last one until the next four
 * bytes have been read. @ref gcomp_walk_event_t::last is therefore always zero
 * for an LZ4 block, rather than guessed at; the FRAME event is what delimits.
 *
 * And a block's size field is the size *on the wire*. For an uncompressed
 * block (high bit set) that is also its output size; for a compressed one
 * nothing in the header says what it expands to.
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/endian.h"
#include "../../core/walk_internal.h"
#include "lz4_internal.h"
#include "lz4_walk.h"
#include <string.h>

/**
 * @brief Step over up to @p avail bytes of a payload being skipped.
 *
 * @return How many were stepped over.
 */
static size_t lz4_walk_consume_skip(lz4_walk_t * w, size_t avail) {
  if (w->skip == 0) {
    return 0;
  }
  size_t take = (w->skip < (uint64_t)avail) ? (size_t)w->skip : avail;
  w->skip -= (uint64_t)take;
  w->pos += (uint64_t)take;
  return take;
}

int lz4_walk_blocks_independent(const lz4_walk_t * w) {
  return w ? w->block_independence : 0;
}

gcomp_status_t lz4_walk_update(lz4_walk_t * w, const uint8_t * data,
    size_t size, size_t * used_out, gcomp_walk_event_t * events,
    size_t max_events, size_t * n_events_out) {
  size_t used = 0;
  size_t n = 0;

  if (!w || (!data && size > 0) || !used_out || !events || max_events == 0 ||
      !n_events_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  for (;;) {
    if (w->skip > 0) {
      used += lz4_walk_consume_skip(w, size - used);
      if (w->skip > 0) {
        break; // Ran out of input part way through a payload.
      }
    }

    // A unit whose bytes have all been accounted for is owed its event; the
    // block goes before the frame it sits inside.
    if (w->has_pending) {
      if (n == max_events) {
        break;
      }
      events[n++] = w->pending;
      w->has_pending = 0;
      continue;
    }
    if (w->frame_pending) {
      if (n == max_events) {
        break;
      }
      events[n].kind = GCOMP_WALK_FRAME;
      events[n].offset = w->frame_start;
      events[n].size = w->pos - w->frame_start;
      events[n].payload_offset = w->frame_start;
      events[n].payload_size = w->pos - w->frame_start;
      events[n].stored = 0;
      events[n].content_size = w->frame_content_size;
      events[n].last = 0;
      n++;
      w->frame_pending = 0;
      continue;
    }

    if (n == max_events) {
      break;
    }

    const size_t avail = size - used;
    const uint8_t * p = data + used;

    if (!w->in_frame) {
      if (avail < 4) {
        break;
      }
      const uint32_t magic = gcomp_read_le32(p);

      if (LZ4_IS_SKIPPABLE_MAGIC(magic)) {
        if (avail < 8) {
          break;
        }
        const uint32_t payload = gcomp_read_le32(p + 4);
        w->pending.kind = GCOMP_WALK_SKIPPABLE;
        w->pending.offset = w->pos;
        w->pending.size = 8u + (uint64_t)payload;
        w->pending.payload_offset = w->pos + 8u;
        w->pending.payload_size = payload;
        w->pending.stored = 1;
        w->pending.content_size = 0;
        w->pending.last = 0;
        w->has_pending = 1;
        used += 8;
        w->pos += 8;
        w->skip = payload;
        continue;
      }

      if (magic != LZ4_MAGIC) {
        *used_out = used;
        *n_events_out = n;
        return GCOMP_ERR_CORRUPT;
      }

      lz4_frame_header_t header;
      size_t header_size = 0;
      const gcomp_status_t s =
          lz4_parse_frame_header(p, avail, &header, &header_size);
      if (s == GCOMP_ERR_LIMIT) {
        break; // Descriptor not all here yet.
      }
      if (s != GCOMP_OK) {
        *used_out = used;
        *n_events_out = n;
        return s;
      }

      w->in_frame = 1;
      w->frame_start = w->pos;
      w->block_checksum = header.block_checksum ? 1 : 0;
      w->content_checksum = header.content_checksum ? 1 : 0;
      w->block_independence = header.block_independence ? 1 : 0;
      w->frame_content_size =
          header.content_size_present ? header.content_size : 0u;
      used += header_size;
      w->pos += (uint64_t)header_size;
      continue;
    }

    // Inside a frame: a four-byte block size, then its payload.
    if (avail < 4) {
      break;
    }
    const uint32_t raw = gcomp_read_le32(p);
    const uint32_t block_size = raw & LZ4_BLOCK_SIZE_MASK;

    if (block_size == 0) {
      // EndMark. The frame ends after it and any content checksum, so its
      // size is known only once those have been stepped over.
      used += 4;
      w->pos += 4;
      w->skip = w->content_checksum ? 4u : 0u;
      w->in_frame = 0;
      w->frame_pending = 1;
      continue;
    }

    const int uncompressed = (raw & LZ4_BLOCK_UNCOMPRESSED_FLAG) != 0;
    const uint64_t trailer = w->block_checksum ? 4u : 0u;

    w->pending.kind = GCOMP_WALK_BLOCK;
    w->pending.offset = w->pos;
    w->pending.size = 4u + (uint64_t)block_size + trailer;
    // The payload is the block's own bytes: past the four-byte size field, and
    // stopping before the B.Checksum that may follow it.
    w->pending.payload_offset = w->pos + 4u;
    w->pending.payload_size = block_size;
    w->pending.stored = uncompressed ? 1 : 0;
    // An uncompressed block's stored size is also what it stands for; a
    // compressed one says nothing about its output, and a guess here would be
    // read as a promise.
    w->pending.content_size = uncompressed ? (uint64_t)block_size : 0u;
    w->pending.last = 0; // The format has no last-block flag; see the header.
    w->has_pending = 1;

    used += 4;
    w->pos += 4;
    w->skip = (uint64_t)block_size + trailer;
  }

  *used_out = used;
  *n_events_out = n;
  return GCOMP_OK;
}

gcomp_status_t lz4_walk_all(const uint8_t * data, size_t size,
    gcomp_walk_event_t * events, size_t max_events, size_t * n_events_out,
    size_t * used_out) {
  lz4_walk_t w;
  memset(&w, 0, sizeof(w));

  size_t total_used = 0;
  size_t total_events = 0;

  for (;;) {
    size_t used = 0;
    size_t n = 0;
    const gcomp_status_t s = lz4_walk_update(&w, data + total_used,
        size - total_used, &used, events + total_events,
        max_events - total_events, &n);
    total_used += used;
    total_events += n;
    if (s != GCOMP_OK) {
      *n_events_out = total_events;
      *used_out = total_used;
      return s;
    }
    if (total_events == max_events && total_used < size) {
      *n_events_out = total_events;
      *used_out = total_used;
      return GCOMP_ERR_LIMIT;
    }
    if (used == 0 && n == 0) {
      break;
    }
  }

  *n_events_out = total_events;
  *used_out = total_used;
  return GCOMP_OK;
}
