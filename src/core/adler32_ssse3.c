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
 * @file adler32_ssse3.c
 *
 * Adler-32 over 32-byte groups, with SSSE3's `PMADDUBSW`.
 *
 * RFC 1950 section 9 defines the two sums this computes; adler32.c carries
 * the statement of them and the derivation of the 5552-byte reduction
 * interval, and is the oracle this is checked against.
 *
 * WHAT IS BEING VECTORISED
 * ========================
 *
 * The scalar loop is `s1 += byte; s2 += s1`, which is a dependent chain two
 * adds long per byte -- nothing a vector unit can do with it as written.
 * Written over a whole group of 32 bytes instead, it is
 *
 *     s1' = s1 + sum(d[i])
 *     s2' = s2 + 32*s1 + sum((32 - i) * d[i])
 *
 * and both of those are reductions the hardware is good at.  The dependence
 * between the two sums is now once per group rather than once per byte.
 *
 * THE THREE INSTRUCTIONS THAT MATTER
 * ==================================
 *
 * - `PSADBW` against zero (`_mm_sad_epu8`) sums sixteen unsigned bytes into
 *   two 64-bit lanes.  That is `sum(d[i])` in one instruction.
 * - `PMADDUBSW` (`_mm_maddubs_epi16`, the SSSE3 one) multiplies unsigned
 *   bytes by signed bytes and adds adjacent pairs into sixteen-bit lanes.
 *   The weights 32..1 fit in a signed byte, which is the whole reason the
 *   group is 32 bytes wide and not 64.  The largest pair it can produce is
 *   255*32 + 255*31 = 16065, comfortably inside a signed sixteen-bit lane.
 * - `PMADDWD` against ones (`_mm_madd_epi16`) widens those to 32 bits so
 *   they can accumulate across the whole run.
 *
 * WHY THE ACCUMULATORS CANNOT OVERFLOW
 * ====================================
 *
 * They cannot overflow for exactly the reason the scalar loop's cannot, and
 * not by a separate argument.  Each run starts by seeding lane 0 of `vs1`
 * with s1 and lane 0 of `vs2` with s2 and clearing the rest, so at every
 * point the horizontal sum of each vector is precisely the scalar sum at the
 * same point.  A single lane is therefore never larger than the scalar sum,
 * and adler32.c's bound -- s2 below 2^32 after at most 5552 bytes -- covers
 * all four lanes at once.  The run length is the largest multiple of 32 not
 * exceeding that interval, 5536.
 *
 * The one value that is not a partial sum is `vs1 << 5`, the `32*s1` term.
 * Its lanes are bounded by 32 * (65520 + 5536*255), about 47 million.
 */

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/adler32.h>

#include "checksum_internal.h"

#ifdef GCOMP_ADLER32_SSSE3

#include <stdatomic.h>
#include <tmmintrin.h>

/**
 * @brief Instruction set widened for one function.
 *
 * SSSE3 implies SSE2, so the loads, adds and shifts come with it.
 */
#define GCOMP_TARGET_SSSE3 __attribute__((target("ssse3")))

/**
 * @brief Bytes per pass of the vector loop.
 */
#define GCOMP_ADLER32_SSSE3_GROUP 32u

/**
 * @brief Bytes between reductions: adler32.c's interval, rounded down to a
 *        whole number of groups.
 */
#define GCOMP_ADLER32_SSSE3_RUN 5536u

/**
 * @brief Sum the four 32-bit lanes.
 */
GCOMP_TARGET_SSSE3 static inline uint32_t gcomp_adler32_hsum(__m128i v) {
  v = _mm_add_epi32(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(1, 0, 3, 2)));
  v = _mm_add_epi32(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(2, 3, 0, 1)));
  return (uint32_t)_mm_cvtsi128_si32(v);
}

GCOMP_TARGET_SSSE3 uint32_t gcomp_adler32_update_ssse3(
    uint32_t adler, const uint8_t * data, size_t len) {
  // Byte i of the group is weighted (32 - i), split across the two halves.
  const __m128i weights_hi = _mm_setr_epi8(32, 31, 30, 29, 28, 27, 26, 25,
      24, 23, 22, 21, 20, 19, 18, 17);
  const __m128i weights_lo = _mm_setr_epi8(16, 15, 14, 13, 12, 11, 10, 9,
      8, 7, 6, 5, 4, 3, 2, 1);
  const __m128i ones = _mm_set1_epi16(1);
  const __m128i zero = _mm_setzero_si128();
  uint32_t s1 = adler & 0xFFFFu;
  uint32_t s2 = (adler >> 16) & 0xFFFFu;

  if (!data) {
    return adler;
  }

  while (len >= GCOMP_ADLER32_SSSE3_GROUP) {
    size_t run = (len > GCOMP_ADLER32_SSSE3_RUN)
        ? GCOMP_ADLER32_SSSE3_RUN
        : len - (len % GCOMP_ADLER32_SSSE3_GROUP);
    // Seeding lane 0 with the running sums is what makes the overflow
    // argument above the scalar one rather than a new one.
    __m128i vs1 = _mm_cvtsi32_si128((int)s1);
    __m128i vs2 = _mm_cvtsi32_si128((int)s2);

    len -= run;
    for (size_t k = 0; k < run; k += GCOMP_ADLER32_SSSE3_GROUP) {
      __m128i a = _mm_loadu_si128((const __m128i *)(const void *)(data + k));
      __m128i b =
          _mm_loadu_si128((const __m128i *)(const void *)(data + k + 16));
      // The 32*s1 term, carried before this group's bytes are added in.
      vs2 = _mm_add_epi32(vs2, _mm_slli_epi32(vs1, 5));
      vs1 = _mm_add_epi32(vs1,
          _mm_add_epi32(_mm_sad_epu8(a, zero), _mm_sad_epu8(b, zero)));
      vs2 = _mm_add_epi32(vs2,
          _mm_madd_epi16(_mm_maddubs_epi16(a, weights_hi), ones));
      vs2 = _mm_add_epi32(vs2,
          _mm_madd_epi16(_mm_maddubs_epi16(b, weights_lo), ones));
    }
    data += run;
    s1 = gcomp_adler32_hsum(vs1) % GCOMP_ADLER32_BASE;
    s2 = gcomp_adler32_hsum(vs2) % GCOMP_ADLER32_BASE;
  }

  // Under 32 bytes left, from sums both below 65521: s2 cannot exceed
  // 65520 + 31*(65520 + 31*255), which is well inside 32 bits.
  while (len-- > 0) {
    s1 += *data++;
    s2 += s1;
  }
  s1 %= GCOMP_ADLER32_BASE;
  s2 %= GCOMP_ADLER32_BASE;

  return (s2 << 16) | s1;
}

bool gcomp_adler32_ssse3_available(void) {
  // See the matching note in crc32_pclmul.c for why this is an atomic.
  static atomic_int state = 0;
  int s = atomic_load_explicit(&state, memory_order_relaxed);

  if (s == 0) {
    s = __builtin_cpu_supports("ssse3") ? 1 : -1;
    atomic_store_explicit(&state, s, memory_order_relaxed);
  }

  return s > 0;
}

#else // GCOMP_ADLER32_SSSE3

/**
 * @brief The vector path is not in this build; see crc32_pclmul.c's stubs.
 */
bool gcomp_adler32_ssse3_available(void) {
  return false;
}

uint32_t gcomp_adler32_update_ssse3(
    uint32_t adler, const uint8_t * data, size_t len) {
  return gcomp_adler32_update_scalar(adler, data, len);
}

#endif // GCOMP_ADLER32_SSSE3
