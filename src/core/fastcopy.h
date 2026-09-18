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
 * Nothing here writes past `n` bytes.  That matters: this library decodes
 * into the caller's buffer, which has no slack to overshoot into, so the
 * wildcopy trick other implementations use -- rounding the length up to a
 * vector width and writing the excess -- is not available.
 *
 * **Caller contract:** the regions must not overlap, exactly as for memcpy.
 * The zstd match loops clamp each run to the match offset, which is what
 * makes that true there.
 *
 * Copyright 2026 by Corey Pennycuff
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

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_CORE_FASTCOPY_H
