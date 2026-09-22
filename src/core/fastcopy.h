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
 * @file fastcopy.h
 *
 * Short non-overlapping copies, without the call into libc.
 *
 * WHY THIS EXISTS
 * ===============
 *
 * A Zstandard block is a few hundred thousand sequences, and every one of
 * them copies its literals and then its match.  Both are usually short --
 * literal runs of a handful of bytes, matches of a few dozen -- and at that
 * size the call into libc's memcpy costs more than the copy does.  On 3 MB
 * of manuals, `__memcpy_avx_unaligned_erms` was 18.7% of a decode, spread
 * over 433,000 calls averaging well under thirty-two bytes each.
 *
 * The libc routine is the right thing for a long copy: it dispatches to a
 * vectorised loop that this cannot beat.  It is the wrong thing for eight
 * bytes.  So this picks by size, and hands anything substantial straight
 * back to memcpy.
 *
 * HOW THE SHORT CASES WORK
 * ========================
 *
 * Each size class copies its two ends with overlapping fixed-width moves --
 * the first N bytes and the last N bytes, which together cover everything
 * between when the length is under 2N.  That is two loads and two stores
 * with no loop and no branch inside the class, and because the widths are
 * compile-time constants, `memcpy` of one is an instruction rather than a
 * call.
 *
 * Nothing in gcomp_copy_short() writes past `n` bytes.  That matters
 * wherever this library copies into the caller's buffer, which has no slack
 * to overshoot into.
 *
 * **Caller contract:** the regions must not overlap, exactly as for memcpy.
 *
 * THE SLACK VARIANTS
 * ==================
 *
 * Choosing a size class costs a branch per class, and the lengths are the
 * lengths a compressor chose -- so the branch is unpredictable by
 * construction.  On 3 MB of manuals at zstd level 1 the ladder was 8% of the
 * decode's instructions and 73% of its branch mispredictions.
 *
 * The way out is the one every other implementation takes: write a fixed
 * width regardless of the length and let the excess land somewhere it does
 * not matter.  That needs a destination with room past the bytes asked for,
 * which the caller's buffer does not have -- but the zstd decoder does not
 * decode into the caller's buffer.  It decodes a block into
 * `zstd_decoder_state_t::output_buffer` and copies out of that, so the slack
 * can be allocated there and the API promise is untouched.
 *
 * gcomp_copy_slack() and gcomp_copy_repeat_slack() may write up to
 * @ref GCOMP_FASTCOPY_SLACK bytes past the end of what was asked for, and
 * read that far past the end of their source.  **Both ends must own that
 * many spare bytes.**  Use gcomp_copy_short() where they do not.
 */

#ifndef GHOTI_IO_GCOMP_SRC_CORE_FASTCOPY_H
#define GHOTI_IO_GCOMP_SRC_CORE_FASTCOPY_H

#include <ghoti.io/compress/macros.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Lengths at or above this go to libc, which is better at them.
#define GCOMP_FASTCOPY_LIMIT 32u

/**
 * @brief Copy @p n bytes from @p src to @p dst, which must not overlap.
 *
 * Equivalent to `memcpy(dst, src, n)` in every observable way; it differs
 * only in how it gets there for short lengths.
 *
 * @param dst Destination, at least @p n bytes
 * @param src Source, at least @p n bytes, not overlapping @p dst
 * @param n Number of bytes to copy
 */
static inline void gcomp_copy_short(
    uint8_t * dst, const uint8_t * src, size_t n) {
  if (n >= GCOMP_FASTCOPY_LIMIT) {
    memcpy(dst, src, n);
    return;
  }
  if (n >= 16u) {
    uint8_t head[16];
    uint8_t tail[16];
    memcpy(head, src, 16);
    memcpy(tail, src + n - 16u, 16);
    memcpy(dst, head, 16);
    memcpy(dst + n - 16u, tail, 16);
    return;
  }
  if (n >= 8u) {
    uint64_t head;
    uint64_t tail;
    memcpy(&head, src, 8);
    memcpy(&tail, src + n - 8u, 8);
    memcpy(dst, &head, 8);
    memcpy(dst + n - 8u, &tail, 8);
    return;
  }
  if (n >= 4u) {
    uint32_t head;
    uint32_t tail;
    memcpy(&head, src, 4);
    memcpy(&tail, src + n - 4u, 4);
    memcpy(dst, &head, 4);
    memcpy(dst + n - 4u, &tail, 4);
    return;
  }
  if (n >= 1u) {
    dst[0] = src[0];
    if (n >= 2u) {
      dst[1] = src[1];
      dst[n - 1u] = src[n - 1u];
    }
  }
}

/// Bytes a slack copy may write past its destination and read past its source.
#define GCOMP_FASTCOPY_SLACK 32u

/// Width of one move in a slack copy; the slack is two of these.
#define GCOMP_FASTCOPY_MOVE 16u

/**
 * @brief Copy @p n bytes from @p src to @p dst, overshooting both.
 *
 * Writes up to @ref GCOMP_FASTCOPY_SLACK bytes past `dst + n` and reads that
 * far past `src + n`; both buffers must own that much room beyond the bytes
 * named here.  The bytes below `n` are exactly what memcpy would have moved.
 *
 * @param dst Destination, with @ref GCOMP_FASTCOPY_SLACK bytes to spare
 * @param src Source, with @ref GCOMP_FASTCOPY_SLACK bytes to spare, not
 *            overlapping @p dst
 * @param n Number of bytes that matter; may be zero
 */
static inline void gcomp_copy_slack(
    uint8_t * dst, const uint8_t * src, size_t n) {
  // Two fixed-width moves cover everything up to the slack, which is where
  // nearly all of these lengths are; past that libc's vectorised loop is
  // better than anything written here.  Both tests are on the length rather
  // than on a size class, so neither has to be right to a byte.
  if (n > GCOMP_FASTCOPY_SLACK) {
    memcpy(dst, src, n);
    return;
  }
  memcpy(dst, src, GCOMP_FASTCOPY_MOVE);
  memcpy(dst + GCOMP_FASTCOPY_MOVE, src + GCOMP_FASTCOPY_MOVE,
      GCOMP_FASTCOPY_MOVE);
}

/**
 * @brief Write @p n bytes at @p dst that repeat the @p offset bytes before it.
 *
 * This is an LZ match copy: `dst[i] == dst[i - offset]` for every byte, which
 * for an offset below the length means the copy reads what it has just
 * written.  Doing that with memcpy means splitting the length into runs of
 * `offset` bytes, and a short offset makes that a loop with a call in it.
 *
 * Instead the period is widened first -- a multiple of a period is a period,
 * so a short one is turned into one at least a move wide by materialising
 * sixteen bytes a byte at a time and then stepping back by that wider
 * period -- and the rest is fixed-width moves that cannot overlap.
 *
 * Writes up to @ref GCOMP_FASTCOPY_SLACK bytes past `dst + n` and reads no
 * further than that.
 *
 * @param dst Destination, with @ref GCOMP_FASTCOPY_SLACK bytes to spare
 * @param offset Distance back to the source; at least 1, and at most the
 *               number of bytes already written before @p dst
 * @param n Number of bytes that matter; at least 1
 */
static inline void gcomp_copy_repeat_slack(
    uint8_t * dst, size_t offset, size_t n) {
  uint8_t * op = dst;
  uint8_t * const end = dst + n;
  const uint8_t * mp;

  if (offset >= GCOMP_FASTCOPY_MOVE) {
    mp = dst - offset;
  }
  else {
    // The smallest multiple of the offset that is at least a move wide: any
    // multiple of a period is also a period, and that is the width source
    // and destination have to be apart for a move not to overlap.  Index 0
    // is unused; the rest is `offset * ceil(16 / offset)`, at most
    // `offset + 15`, so stepping back by it never reaches past
    // `dst - offset`.
    static const uint8_t period_16[GCOMP_FASTCOPY_MOVE] = {
        0, 16, 16, 18, 16, 20, 18, 21, 16, 18, 20, 22, 24, 26, 28, 30};

    // Sixteen fixed iterations with no branch in them.  The serial
    // dependency between them is what a period this short costs, and it is
    // paid once rather than per run.
    for (size_t i = 0; i < GCOMP_FASTCOPY_MOVE; i++) {
      op[i] = *(op + (ptrdiff_t)i - (ptrdiff_t)offset);
    }
    op += GCOMP_FASTCOPY_MOVE;
    mp = op - period_16[offset];
  }

  while (op < end) {
    memcpy(op, mp, GCOMP_FASTCOPY_MOVE);
    op += GCOMP_FASTCOPY_MOVE;
    mp += GCOMP_FASTCOPY_MOVE;
  }
}

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_CORE_FASTCOPY_H
