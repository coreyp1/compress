/**
 * @file zstd_walk.c
 *
 * Walking a Zstandard stream: where its frames and blocks are, without
 * decoding any of them.
 *
 * RFC 8878 section 3.1 - "a stream is one or more frames" - and section
 * 3.1.1.2 for the three-byte block header that makes this possible: the size
 * of a block is in its header, so the whole of a frame's structure can be read
 * by stepping over payloads without ever looking at one.
 *
 * ## The one place the on-wire size is not the block size
 *
 * Section 3.1.1.2.2: an `RLE_Block` occupies a single byte on the wire, and
 * its `Block_Size` says how many times that byte repeats. So `Block_Size` is
 * the size of the *output* there and the size of the *input* everywhere else.
 * Getting that wrong walks into the middle of the next block and reports
 * nonsense from then on, which is why it is a separate case here rather than
 * an adjustment to a shared one.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>

#include "../../core/walk_internal.h"
#include "../../core/endian.h"
#include "zstd_internal.h"
#include "zstd_walk.h"
#include <string.h>

/**
 * @brief A walk in progress.
 *
 * Zeroed to start. Nothing here is allocated, so a walker can live on the
 * stack and be abandoned without a teardown.
 */

/**
 * @brief Step over up to @p avail bytes of a payload being skipped.
 *
 * @return How many were stepped over.
 */
static size_t zstd_walk_consume_skip(zstd_walk_t * w, size_t avail) {
  if (w->skip == 0) {
    return 0;
  }
  size_t take = (w->skip < (uint64_t)avail) ? (size_t)w->skip : avail;
  w->skip -= (uint64_t)take;
  w->pos += (uint64_t)take;
  return take;
}

/**
 * @brief Feed bytes; report the units they complete.
 *
 * ## The contract
 *
 * **An event is reported when the unit it describes has been wholly accounted
 * for**, never when its header merely says how big it is. A three-byte block
 * header arrives long before the twenty kilobytes it describes, and reporting
 * the block then would hand a caller an offset and a size pointing past the
 * bytes it holds - which a parallel decoder would slice, and a seek table
 * would record, as though they were there.
 *
 * This was not so at first: FRAME waited and BLOCK did not, and a stream cut
 * to ten bytes reported a block of 21593. The test that found it is
 * ATruncatedStreamStopsAtTheLastCompleteUnit.
 *
 * Consumes as much as it can. A unit whose header is not wholly present is
 * left alone: @p used_out stops before it, and the caller feeds those bytes
 * again with more behind them. So a caller streaming in pieces sees exactly
 * what a caller handing over the whole stream sees, which is the property
 * test_zstd_walk.cpp checks across chunk sizes.
 *
 * @param w Walker state, zeroed before the first call
 * @param data Bytes, continuing from where the last call stopped
 * @param size How many
 * @param used_out Receives the bytes consumed
 * @param events Receives what was found
 * @param max_events How many @p events holds; at least 1
 * @param n_events_out Receives how many were written
 * @return ::GCOMP_OK, or ::GCOMP_ERR_CORRUPT for a bad magic number, a
 *         reserved block type, or a frame header the parser refuses
 */
gcomp_status_t zstd_walk_update(zstd_walk_t * w, const uint8_t * data,
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
      used += zstd_walk_consume_skip(w, size - used);
      if (w->skip > 0) {
        break; // Ran out of input part way through a payload.
      }
    }

    // A unit whose bytes have now all been accounted for is owed its event.
    // The block goes first: when a frame's last block completes, both are owed
    // at the same moment and the block is the one inside the frame.
    if (w->has_pending) {
      if (n == max_events) {
        break;
      }
      events[n++] = w->pending;
      w->has_pending = 0;
      continue;
    }

    // A frame whose trailer has now been stepped over is owed its event.
    if (w->frame_pending) {
      if (n == max_events) {
        break;
      }
      events[n].kind = GCOMP_WALK_FRAME;
      events[n].offset = w->frame_start;
      events[n].size = w->pos - w->frame_start;
      // A frame is handed to a decoder whole - header, blocks and checksum -
      // so its payload is itself.
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
      break; // No room to report what comes next; the caller drains and asks
             // again.
    }

    const size_t avail = size - used;
    const uint8_t * p = data + used;

    if (!w->in_frame) {
      if (avail < 4) {
        break;
      }
      const uint32_t magic = gcomp_read_le32(p);

      if (magic >= ZSTD_MAGIC_SKIPPABLE_MIN &&
          magic <= ZSTD_MAGIC_SKIPPABLE_MAX) {
        // RFC 8878 section 3.1.2: magic, a four-byte size, then that many
        // bytes. The payload is stepped over like any other.
        if (avail < 8) {
          break;
        }
        const uint32_t payload = gcomp_read_le32(p + 4);
        w->pending.kind = GCOMP_WALK_SKIPPABLE;
        w->pending.offset = w->pos;
        w->pending.size = 8u + (uint64_t)payload;
        w->pending.payload_offset = w->pos + 8u;
        w->pending.payload_size = payload;
        w->pending.stored = 1; // Its contents are whatever the writer put there.
        w->pending.content_size = 0;
        w->pending.last = 0;
        w->has_pending = 1;
        used += 8;
        w->pos += 8;
        w->skip = payload;
        continue;
      }

      if (magic != ZSTD_MAGIC) {
        *used_out = used;
        *n_events_out = n;
        return GCOMP_ERR_CORRUPT;
      }

      zstd_frame_header_t header;
      uint64_t window_size = 0;
      size_t header_len = 0;
      const gcomp_status_t s = zstd_frame_header_parse(
          p, avail, &header, &window_size, &header_len);
      if (s == GCOMP_ERR_LIMIT) {
        break; // Header not all here yet.
      }
      if (s != GCOMP_OK) {
        *used_out = used;
        *n_events_out = n;
        return s;
      }

      w->in_frame = 1;
      w->frame_start = w->pos;
      w->frame_checksum = header.content_checksum ? 1 : 0;
      w->frame_content_size =
          header.content_size_present ? header.content_size : 0u;
      used += header_len;
      w->pos += (uint64_t)header_len;
      continue;
    }

    // Inside a frame: a three-byte block header, then its payload.
    if (avail < ZSTD_BLOCK_HEADER_SIZE) {
      break;
    }
    const uint32_t bh =
        (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    const int last = (bh & ZSTD_BLOCK_LAST_FLAG) != 0;
    const uint8_t type =
        (uint8_t)((bh & ZSTD_BLOCK_TYPE_MASK) >> ZSTD_BLOCK_TYPE_SHIFT);
    const uint32_t block_size =
        (bh & ZSTD_BLOCK_SIZE_MASK) >> ZSTD_BLOCK_SIZE_SHIFT;

    if (type == ZSTD_BLOCK_TYPE_RESERVED) {
      *used_out = used;
      *n_events_out = n;
      return GCOMP_ERR_CORRUPT;
    }

    // Section 3.1.1.2.2: an RLE_Block is one byte on the wire whatever its
    // Block_Size says, because Block_Size is how far that byte repeats.
    const uint64_t on_wire =
        (type == ZSTD_BLOCK_TYPE_RLE) ? 1u : (uint64_t)block_size;

    w->pending.kind = GCOMP_WALK_BLOCK;
    w->pending.offset = w->pos;
    w->pending.size = (uint64_t)ZSTD_BLOCK_HEADER_SIZE + on_wire;
    w->pending.payload_offset = w->pos + ZSTD_BLOCK_HEADER_SIZE;
    w->pending.payload_size = on_wire;
    // A Raw_Block's payload is the output; an RLE_Block's one byte is not.
    w->pending.stored = (type == ZSTD_BLOCK_TYPE_RAW) ? 1 : 0;
    // Only a raw or RLE block says how much output it stands for; a compressed
    // one does not, and guessing would make this a promise rather than a hint.
    w->pending.content_size =
        (type == ZSTD_BLOCK_TYPE_COMPRESSED) ? 0u : (uint64_t)block_size;
    w->pending.last = last;
    w->has_pending = 1;

    used += ZSTD_BLOCK_HEADER_SIZE;
    w->pos += ZSTD_BLOCK_HEADER_SIZE;
    w->skip = on_wire;

    if (last) {
      // RFC 8878 section 3.1.1.4: the content checksum, when the header asked
      // for one, is four bytes after the last block. Stepping over it as part
      // of the same skip keeps the frame's size right whether or not the
      // bytes have arrived.
      w->skip += w->frame_checksum ? 4u : 0u;
      w->in_frame = 0;
      w->frame_pending = 1;
    }
  }

  *used_out = used;
  *n_events_out = n;
  return GCOMP_OK;
}

/**
 * @brief Walk a whole stream that is already in memory.
 *
 * The convenience form, for a caller that has the bytes and wants the list.
 * The resumable form above is the real one; this exists so that tests can ask
 * "same answer either way" and mean it.
 *
 * @param data Stream bytes
 * @param size How many
 * @param events Receives what was found
 * @param max_events How many @p events holds
 * @param n_events_out Receives how many were written
 * @param used_out Receives the bytes accounted for; short of @p size when the
 *        stream ends part way through a unit
 * @return ::GCOMP_OK, ::GCOMP_ERR_CORRUPT, or ::GCOMP_ERR_LIMIT when
 *         @p max_events was too small to hold everything
 */
gcomp_status_t zstd_walk_all(const uint8_t * data, size_t size,
    gcomp_walk_event_t * events, size_t max_events, size_t * n_events_out,
    size_t * used_out) {
  zstd_walk_t w;
  memset(&w, 0, sizeof(w));

  size_t total_used = 0;
  size_t total_events = 0;

  for (;;) {
    size_t used = 0;
    size_t n = 0;
    const gcomp_status_t s = zstd_walk_update(&w, data + total_used,
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
    // No progress and no room needed means there is nothing left that these
    // bytes describe.
    if (used == 0 && n == 0) {
      break;
    }
  }

  *n_events_out = total_events;
  *used_out = total_used;
  return GCOMP_OK;
}
