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
 * @file lz4_block.c
 *
 * LZ4 block format compression and decompression.
 *
 * This file implements the LZ4 block compression algorithm as specified in:
 * https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md
 *
 * ## Block Format Overview
 *
 * LZ4 block format uses a sequence-based structure optimized for fast
 * decompression. Each sequence represents either literal bytes or a
 * back-reference to previously seen data.
 *
 * ```
 * ┌─────────┬───────────────┬──────────┬────────┬───────────────┐
 * │  Token  │  Lit Length   │ Literals │ Offset │ Match Length  │
 * │ (1 byte)│   (0+ bytes)  │(variable)│(2 bytes│  (0+ bytes)   │
 * └─────────┴───────────────┴──────────┴────────┴───────────────┘
 *      │                                    │
 *      └─ High 4 bits: literal length       │
 *         Low 4 bits: match length - 4      │
 *                                           └─ Little-endian, 1-65535
 * ```
 *
 * ## Token Byte Encoding
 *
 * The token byte encodes two values:
 * - **High nibble (4 bits)**: Literal length (0-14, or 15 = extended)
 * - **Low nibble (4 bits)**: Match length minus 4 (0-14, or 15 = extended)
 *
 * When either nibble is 15, additional extension bytes follow:
 * - Each 0xFF byte adds 255 to the length
 * - The first non-0xFF byte adds its value and terminates the extension
 *
 * Example: Literal length 300 = 15 + 255 + 30 → token high nibble 15,
 * followed by bytes 0xFF (255) and 0x1E (30).
 *
 * ## Match Mechanics
 *
 * - **Minimum match**: 4 bytes (encoded length 0 in token)
 * - **Offset range**: 1-65535 bytes back (offset 0 is invalid)
 * - **Overlapping matches**: When match length > offset, the pattern repeats.
 *   For example, offset=1, length=10 copies the previous byte 10 times.
 *
 * ## Compression Algorithm (lz4_block_compress)
 *
 * The compressor uses a simple, fast LZ77-style algorithm:
 *
 * 1. **Hash table**: Maps 4-byte sequences to their last occurrence position
 *    - Hash function: multiply first 4 bytes by prime 2654435761, shift >> 16
 *    - Table size: 64K entries (covers 64KB window)
 *    - Single entry per hash (no chaining)
 *
 * 2. **Greedy matching**: At each position, check if hash table has a match
 *    - Match must be within 64KB window
 *    - Match must be at least 4 bytes
 *    - Extend match as far as possible (but not past `match_limit`)
 *
 * 3. **Literal accumulation**: Non-matching bytes accumulate as literals
 *
 * 4. **Last literals requirement** (critical for spec compliance):
 *    - The LZ4 spec mandates: "The last sequence is incomplete, and stops
 *      right after literals field."
 *    - This means the final sequence MUST be literals-only (no match).
 *    - We enforce this by:
 *      a. Not starting matches in the last 12 bytes (MFLIMIT - for safe reads)
 *      b. Not extending matches past `src_end - 5` (LZ4_LAST_LITERALS)
 *      c. Always emitting a final literals-only sequence with >= 5 bytes
 *    - Violating this produces output that reference decoders reject.
 *
 * The algorithm prioritizes speed over compression ratio. For better
 * compression, consider using higher-level strategies like HC (hash chains)
 * which are not implemented here.
 *
 * ## Decompression Algorithm (lz4_block_decompress)
 *
 * The decompressor is straightforward and very fast:
 *
 * 1. Read token byte
 * 2. Decode literal length (with optional extension)
 * 3. Copy literal bytes from input to output
 * 4. If not at end of block:
 *    a. Read 2-byte offset (little-endian)
 *    b. Decode match length (with optional extension, plus 4)
 *    c. Copy match bytes from (output - offset) to output
 * 5. Repeat until input exhausted
 *
 * ## History
 *
 * For dependent blocks (block independence = false), matches can reference
 * data from previous blocks.  Those bytes sit immediately before `output` in
 * the same allocation and `history_len` says how many of them there are, so
 * a match source is `output_position - offset` whether or not it reaches
 * past the start of this block.  The decoder that put them in a buffer of
 * their own had to ask which, once per match, and could not predict the
 * answer.
 *
 * ## Error Cases
 *
 * The decompressor returns GCOMP_ERR_CORRUPT for:
 * - Offset == 0 (invalid back-reference)
 * - Offset exceeds (output position + history length)
 * - Input exhausted before block complete
 * - Literal length exceeds remaining input
 *
 * Returns GCOMP_ERR_LIMIT if output buffer is too small.
 */

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/compress/macros.h>
#include "lz4_internal.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

//
// Hash function for match finding
//

// Bytes the hash key is taken from.  A match need only be four bytes long
// (LZ4_MIN_MATCH), but the key is five, and the difference is what decides
// how long the matches this encoder finds turn out to be.
//
// The table holds one candidate per hash: the most recent position whose key
// matched.  Keyed on four bytes, that candidate is the most recent position
// where four bytes agree - which on data with a small alphabet is usually a
// coincidence that stops there.  Keyed on five, the candidate already agrees
// on five, and agreeing on five is strong evidence of agreeing on more.  The
// minimum match stays four; what changes is which position the table offers.
//
// Measured over a 19 MB corpus, this is 7.0% smaller output.  On three
// megabytes of bytes drawn i.i.d. from a skewed alphabet - where every match
// is a coincidence and only its length matters - it is 14.6%, and the mean
// match length goes from 4.41 bytes to well past five.  liblz4 keys on five
// bytes too whenever its window is large enough to need 32-bit positions.
#define LZ4_HASH_BYTES 5

static inline uint32_t lz4_hash_position(const uint8_t * p) {
  // Five bytes, little-endian, read explicitly rather than as a wider load:
  // an eight-byte read would run past the end of the window near its tail.
  uint64_t v = (uint64_t)gcomp_read_le32(p) | ((uint64_t)p[4] << 32);
  return (uint32_t)((v * 889523592379ULL) >> 40);
}

//
// Block Compression
//

void lz4_block_index_window(const uint8_t * window, size_t len,
    uint32_t * hash_table, size_t hash_table_size) {
  if (!window || !hash_table || len < LZ4_HASH_BYTES) {
    return;
  }
  // Same hash and the same position convention lz4_block_compress_linked()
  // uses, so an entry left here is indistinguishable from one it wrote
  // itself.  Position 0 is the table's "empty" marker, so the byte at the
  // very start of the window is not indexed -- one missed match, never a
  // wrong one.
  for (size_t pos = 1; pos + LZ4_HASH_BYTES <= len; pos++) {
    uint32_t hash = lz4_hash_position(window + pos) & (hash_table_size - 1);
    hash_table[hash] = (uint32_t)pos;
  }
}

gcomp_status_t lz4_block_compress(const uint8_t * input, size_t input_len,
    uint8_t * output, size_t output_cap, size_t * output_len_out,
    uint32_t * hash_table, size_t hash_table_size) {
  if (hash_table) {
    // An independent block may not reference anything outside itself, so it
    // starts from an empty table.  lz4_block_compress_linked() deliberately
    // does not clear it -- that is what lets a linked block see the block
    // before it.
    memset(hash_table, 0, hash_table_size * sizeof(uint32_t));
  }
  return lz4_block_compress_linked(input, 0, input_len, output, output_cap,
      output_len_out, hash_table, hash_table_size);
}

gcomp_status_t lz4_block_compress_linked(const uint8_t * window,
    size_t prefix_len, size_t block_len, uint8_t * output, size_t output_cap,
    size_t * output_len_out, uint32_t * hash_table, size_t hash_table_size) {
  if (!window || !output || !output_len_out || !hash_table) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (block_len == 0) {
    *output_len_out = 0;
    return GCOMP_OK;
  }

  // `window` is prefix_len bytes of already emitted frame data followed by the
  // block to compress.  Hash positions are offsets from `window`, so a match
  // found in the prefix is expressed the same way as one found in the block.
  const uint8_t * input = window + prefix_len;
  size_t input_len = block_len;

  const uint8_t * src = input;
  const uint8_t * src_end = input + input_len;
  // The last 5 bytes MUST be literals (per LZ4 spec), and we reserve some
  // buffer for safe 4-byte reads during match finding.
  // MFLIMIT ensures we don't start a match too close to the end.
  const uint8_t * src_limit = src_end - 12; // Don't start matches in last 12 bytes
  // Match limit: matches must end before the last 5 bytes (last literals requirement)
  const uint8_t * match_limit = src_end - 5;
  const uint8_t * anchor = src;             // Start of literal run

  uint8_t * dst = output;
  uint8_t * dst_end = output + output_cap;

  // Handle small inputs: if < 5 bytes, just write as literals
  if (input_len < LZ4_LAST_LITERALS) {
    // Token for last literals (no match)
    size_t needed = 1 + (input_len >= 15 ? (input_len - 15) / 255 + 1 : 0) + input_len;
    if (needed > output_cap) {
      return GCOMP_ERR_LIMIT;
    }
    uint8_t token = (input_len >= 15) ? 0xF0 : (uint8_t)(input_len << 4);
    *dst++ = token;
    if (input_len >= 15) {
      size_t remaining = input_len - 15;
      while (remaining >= 255) {
        *dst++ = 255;
        remaining -= 255;
      }
      *dst++ = (uint8_t)remaining;
    }
    memcpy(dst, input, input_len);
    dst += input_len;
    *output_len_out = (size_t)(dst - output);
    return GCOMP_OK;
  }

  // The first byte of the frame has nothing behind it, so no match can start
  // there.  The first byte of a *linked* block does: the prefix is behind it.
  if (prefix_len == 0) {
    src++;
  }

  while (src < src_limit) {
    // Find match in hash table
    uint32_t hash = lz4_hash_position(src) & (hash_table_size - 1);
    uint32_t match_pos = hash_table[hash];
    const uint8_t * match_ref = window + match_pos;

    // Update hash table
    hash_table[hash] = (uint32_t)(src - window);

    // The offset field is two bytes (LZ4 block format, "Match"), so a match
    // reaches at most 65535 bytes back however much of the frame precedes it.
    //
    // match_ref < src is tested explicitly rather than relying on the distance
    // test: `src - match_ref` is a signed difference, so a hash entry naming a
    // position at or ahead of src would satisfy `<= 65535` and then be written
    // as a wrapped 16-bit offset.  A correctly rebased table never holds one,
    // which is exactly why the check belongs here rather than in the caller.
    if (match_pos > 0 && match_ref < src &&
        (size_t)(src - match_ref) <= LZ4_MAX_OFFSET &&
        gcomp_read_le32(match_ref) == gcomp_read_le32(src)) {
      // Found a match.  Before measuring it forward, walk both cursors back
      // over bytes that also agree: those bytes are sitting in the pending
      // literal run, and every one of them moved into the match is a byte
      // that stops being sent verbatim.  The offset does not change, since
      // both cursors move together.
      //
      // The walk stops at `anchor`, because anything before it has already
      // been emitted, and at the start of the window, because there is
      // nothing before that to compare.
      size_t back = 0;
      while ((size_t)(src - anchor) > back &&
          (size_t)(match_ref - window) > back &&
          match_ref[-(ptrdiff_t)back - 1] == src[-(ptrdiff_t)back - 1]) {
        back++;
      }
      src -= back;
      match_ref -= back;

      // Extend forward, but not past match_limit.  The four bytes the hash
      // agreed on sit just after the bytes walked back over.
      //
      // Eight bytes at a time while eight remain inside match_limit.  The
      // match source is always behind src, so its read is further inside the
      // buffer than src's and needs no separate bound.  Where the two words
      // differ, the first differing byte is the lowest differing bit of
      // their exclusive-or over eight; reading both little-endian puts the
      // earliest byte in memory in the low bits, so this counts forwards
      // through memory on either byte order.
      //
      // One byte per iteration here was 18% of LZ4 encoding.
      size_t match_len = back + 4;
      while (src + match_len + 8u <= match_limit) {
        uint64_t a = gcomp_read_le64(src + match_len);
        uint64_t b = gcomp_read_le64(match_ref + match_len);
        if (a != b) {
          match_len += (size_t)((unsigned)__builtin_ctzll(a ^ b) >> 3);
          break;
        }
        match_len += 8u;
      }
      while (
          src + match_len < match_limit && match_ref[match_len] == src[match_len]) {
        match_len++;
      }

      // Encode literal length
      size_t lit_len = src - anchor;
      uint8_t token;

      if (lit_len >= 15) {
        token = 0xF0;
      }
      else {
        token = (uint8_t)(lit_len << 4);
      }

      // Encode match length (minus 4 for minmatch)
      size_t match_len_adj = match_len - LZ4_MIN_MATCH;
      if (match_len_adj >= 15) {
        token |= 0x0F;
      }
      else {
        token |= (uint8_t)match_len_adj;
      }

      // Check output space
      size_t needed = 1 + (lit_len >= 15 ? (lit_len - 15) / 255 + 1 : 0) +
          lit_len + 2 +
          (match_len_adj >= 15 ? (match_len_adj - 15) / 255 + 1 : 0);
      if (dst + needed > dst_end) {
        return GCOMP_ERR_LIMIT; // Compression expanded
      }

      // Write token
      *dst++ = token;

      // Write literal length extension
      if (lit_len >= 15) {
        size_t remaining = lit_len - 15;
        while (remaining >= 255) {
          *dst++ = 255;
          remaining -= 255;
        }
        *dst++ = (uint8_t)remaining;
      }

      // Write literals
      memcpy(dst, anchor, lit_len);
      dst += lit_len;

      // Write match offset (little-endian)
      uint16_t offset = (uint16_t)(src - match_ref);
      *dst++ = (uint8_t)offset;
      *dst++ = (uint8_t)(offset >> 8);

      // Write match length extension
      if (match_len_adj >= 15) {
        size_t remaining = match_len_adj - 15;
        while (remaining >= 255) {
          *dst++ = 255;
          remaining -= 255;
        }
        *dst++ = (uint8_t)remaining;
      }

      // Advance past match
      src += match_len;
      anchor = src;
    }
    else {
      src++;
    }
  }

  // Write remaining literals (last sequence has no match)
  size_t last_lit_len = src_end - anchor;
  if (last_lit_len > 0) {
    // Check output space
    size_t needed = 1 +
        (last_lit_len >= 15 ? (last_lit_len - 15) / 255 + 1 : 0) + last_lit_len;
    if (dst + needed > dst_end) {
      return GCOMP_ERR_LIMIT;
    }

    // Token for last literals (no match)
    uint8_t token;
    if (last_lit_len >= 15) {
      token = 0xF0;
    }
    else {
      token = (uint8_t)(last_lit_len << 4);
    }
    *dst++ = token;

    // Literal length extension
    if (last_lit_len >= 15) {
      size_t remaining = last_lit_len - 15;
      while (remaining >= 255) {
        *dst++ = 255;
        remaining -= 255;
      }
      *dst++ = (uint8_t)remaining;
    }

    // Literals
    memcpy(dst, anchor, last_lit_len);
    dst += last_lit_len;
  }

  *output_len_out = (size_t)(dst - output);
  return GCOMP_OK;
}

//
// Block Decompression
//

gcomp_status_t lz4_block_decompress(const uint8_t * input, size_t input_len,
    uint8_t * output, size_t output_cap, size_t output_slack,
    size_t * output_len_out, size_t history_len) {
  if (!input || !output || !output_len_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (input_len == 0) {
    *output_len_out = 0;
    return GCOMP_OK;
  }

  const uint8_t * src = input;
  const uint8_t * src_end = input + input_len;
  uint8_t * dst = output;
  uint8_t * dst_end = output + output_cap;

  // Copies below are made in whole 16-byte groups where there is room, rather
  // than asked of libc by an exact count.  Decoding spent 29% of its
  // instructions inside `memcpy` for copies averaging a dozen bytes, which is
  // call and dispatch overhead rather than moving: a group of sixteen is two
  // instructions.
  //
  // A group may write up to LZ4_WIDE_SLACK bytes past the last byte actually
  // wanted, so the wide path runs only while that much room is left, and what
  // is left of a block after that goes through the exact copies below it.
  // `output_slack` is what says where that room ends: given
  // LZ4_DECODE_SLACK the wide path covers the whole block, and given zero it
  // stops short of `dst_end` and nothing is written at or past it.
  //
  // `DECODER-PERFORMANCE.md` item 2 read the API's promise -- no slack past
  // the caller's buffer -- as ruling the whole thing out.  It rules out
  // assuming slack, not having it: this decoder stages a block in a buffer of
  // its own and copies out afterwards, so the slack goes there, and the one
  // caller with nothing to offer (the parallel decoder, which decodes
  // straight out of the caller's input) passes zero and gets what it had.
  uint8_t * const dst_slack_end = dst_end + output_slack;

  while (src < src_end) {
    // Read token
    uint8_t token = *src++;
    size_t lit_len = (token >> 4) & 0x0F;
    size_t match_len = (token & 0x0F) + LZ4_MIN_MATCH;

    // Read literal length extension (with overflow protection)
    if (lit_len == 15) {
      while (src < src_end) {
        uint8_t b = *src++;
        if (!gcu_safe_add_size(lit_len, b, &lit_len)) {
          return GCOMP_ERR_CORRUPT; // Overflow in literal length
        }
        if (b != 255) {
          break;
        }
      }
    }

    // Check bounds
    if (src + lit_len > src_end) {
      return GCOMP_ERR_CORRUPT;
    }
    if (dst + lit_len > dst_end) {
      return GCOMP_ERR_LIMIT;
    }

    // Copy literals.  A whole group where both buffers have room for one --
    // the read needs the room too, since a group reads sixteen bytes whatever
    // `lit_len` says, and the last literals of a block sit against `src_end`.
    // The input is never ours to overshoot: on the parallel path it is the
    // caller's buffer, so that half stays a bound and not a slack.
    if (lit_len <= LZ4_WIDE_GROUP && dst + LZ4_WIDE_GROUP <= dst_slack_end
        && src + LZ4_WIDE_GROUP <= src_end) {
      memcpy(dst, src, LZ4_WIDE_GROUP);
    }
    else {
      memcpy(dst, src, lit_len);
    }
    src += lit_len;
    dst += lit_len;

    // Check if this is the last sequence (no match section)
    if (src >= src_end) {
      break;
    }

    // Read match offset
    if (src + 2 > src_end) {
      return GCOMP_ERR_CORRUPT;
    }
    uint16_t offset = gcomp_read_le16(src);
    src += 2;

    if (offset == 0) {
      return GCOMP_ERR_CORRUPT; // Invalid offset
    }

    // Read match length extension (with overflow protection)
    if ((token & 0x0F) == 15) {
      while (src < src_end) {
        uint8_t b = *src++;
        if (!gcu_safe_add_size(match_len, b, &match_len)) {
          return GCOMP_ERR_CORRUPT; // Overflow in match length
        }
        if (b != 255) {
          break;
        }
      }
    }

    // Find match source.
    //
    // A match reaches back into this block's own output, or past its start
    // into what earlier blocks decoded.  Those used to be two buffers, and
    // this used to be the branch that said which -- 54% of a linked decode's
    // branch mispredictions, because 39% of matches took the second side and
    // nothing can predict a coin toss.  A match that was both was then copied
    // in two pieces, with a flag carried from here to say so.
    //
    // The history now sits immediately before `output` in the same
    // allocation, `history_len` bytes of it, so the source is `dst - offset`
    // whichever side of the boundary it falls on and there is nothing to ask.
    // What is left is the bounds check, and `offset` is the only untrusted
    // value in it: 1 <= offset <= dst_offset + history_len puts
    // `dst - offset` inside [output - history_len, dst), and anything else
    // is refused here.
    const size_t dst_offset = (size_t)(dst - output);

    if (offset > dst_offset + history_len) {
      return GCOMP_ERR_CORRUPT; // Invalid back-reference
    }
    const uint8_t * match_src = dst - offset;

    // Check output bounds
    if (dst + match_len > dst_end) {
      return GCOMP_ERR_LIMIT;
    }

    // Copy the match.
    //
    // This used to ask, for every single byte, whether the source was still
    // inside the history buffer and whether it had reached the write cursor.
    // Neither question changes more than once in a whole match, so they were
    // asked once and the copies became memcpy.  With the history in front of
    // the output rather than beside it there is nothing left to ask: one run
    // covers a match wherever it starts, and a match that spans the boundary
    // is no longer a special case at all.
    //
    // `match_src` is inside [output - history_len, dst) and stays there --
    // the bound above put it there, and each pass moves source and
    // destination together by the same amount, so the distance between them
    // never shrinks.  A run stops at that distance; up to it the two ranges
    // cannot overlap and memcpy is exact.
    size_t remaining = match_len;

    // A short match far enough behind is two groups and no loop at all.
    //
    // "Far enough" is one group, not two.  Each group reads sixteen bytes
    // starting sixteen or more bytes behind the byte it writes, so every byte
    // it reads was already final -- the second group may read bytes the first
    // one wrote, and those are exactly the bytes a byte-at-a-time copy would
    // have read there.  Neither group's source overlaps its own destination,
    // so neither is the overlapping `memcpy` that C leaves undefined.
    if (remaining <= LZ4_WIDE_SLACK
        && dst + LZ4_WIDE_SLACK <= dst_slack_end
        && (size_t)(dst - match_src) >= LZ4_WIDE_GROUP) {
      memcpy(dst, match_src, LZ4_WIDE_GROUP);
      memcpy(dst + LZ4_WIDE_GROUP, match_src + LZ4_WIDE_GROUP, LZ4_WIDE_GROUP);
      dst += remaining;
      remaining = 0u;
    }

    // A match nearer than the run it is filling: LZ4 means the pattern
    // repeats, and the loop below does that a period at a time -- which is a
    // call to libc per period, for periods of a few bytes.
    //
    // One byte repeated is memset, which splats.  Handing it to
    // gcomp_copy_repeat_slack() instead costs 3% of the instructions and
    // two and a half times the wall clock on a stream made of short-period
    // repeats: that function widens a period of one with sixteen byte-wide
    // stores and then moves sixteen bytes at a time out of the bytes
    // immediately behind them, and every one of those loads waits on the
    // store before it.  The instruction count cannot see a dependency chain
    // through the store buffer, and here it is nearly all of the cost.  The
    // case belongs here and not in the helper: zstd's matches are longer and
    // rarely a single byte, and it measured 2% slower carrying the test.
    //
    // Anything else goes to the helper, which widens the period first -- a
    // multiple of a period is also a period -- and then moves whole groups,
    // so it is one call whatever the period is.
    if (remaining > 0u && (size_t)(dst - match_src) == 1u) {
      memset(dst, *match_src, remaining);
      dst += remaining;
      remaining = 0u;
    }
    else if (remaining > 0u && (size_t)(dst - match_src) < remaining
        && dst + remaining + LZ4_WIDE_SLACK <= dst_slack_end) {
      gcomp_copy_repeat_slack(dst, (size_t)(dst - match_src), remaining);
      dst += remaining;
      remaining = 0u;
    }

    while (remaining > 0u) {
      size_t distance = (size_t)(dst - match_src);
      size_t run = (distance < remaining) ? distance : remaining;
      if (distance == 1u) {
        memset(dst, *match_src, run);
      }
      else {
        memcpy(dst, match_src, run);
      }
      dst += run;
      match_src += run;
      remaining -= run;
    }
  }

  *output_len_out = (size_t)(dst - output);
  return GCOMP_OK;
}
