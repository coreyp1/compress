/**
 * @file adler32.c
 *
 * Adler-32, as RFC 1950 section 9 defines it.
 *
 * ## The algorithm
 *
 * Two running sums over the input:
 *
 *     s1 = 1 + d1 + d2 + ... + dn           (mod 65521)
 *     s2 = n*1 + n*d1 + (n-1)*d2 + ... + dn (mod 65521)
 *
 * written out as `(s2 << 16) | s1`.  Equivalently, and how it is actually
 * computed: for each byte, add it to s1 and then add s1 to s2.
 *
 * ## Why the loop is shaped the way it is
 *
 * The obvious loop takes two divisions per byte.  Neither is needed that
 * often: the sums can be allowed to grow and reduced later, as long as they
 * cannot overflow 32 bits in between.
 *
 * The bound is the standard one.  Starting from the largest values the sums
 * can hold after a reduction (both below 65521), the worst case after n bytes
 * of 0xFF is
 *
 *     s2 <= 65520 + n*65520 + n*(n+1)/2 * 255
 *
 * and the largest n for which that stays below 2^32 is 5552.  So the modulo
 * is taken once every 5552 bytes instead of once per byte -- the same NMAX
 * zlib uses, for the same reason.
 *
 * Inside a run the loop goes sixteen bytes at a time.  Each step is an add
 * and an add; unrolling removes the loop overhead between them and lets the
 * compiler keep both sums in registers across the whole block.
 *
 * ## Where the checksum is weak
 *
 * Adler-32 is poor on short inputs: for a handful of bytes s1 barely moves
 * from 1 and s2 is close to their sum, so the space of reachable checksums is
 * a small corner of 32 bits.  That is inherent, and it is why this is used
 * where RFC 1950 asks for it and not as a general integrity check -- gzip
 * uses CRC-32 for that, and so does this library everywhere it has the
 * choice.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/adler32.h>

/**
 * @brief Bytes that may be summed before the modulo is needed.
 *
 * See the note above: the largest n for which the worst-case s2 stays inside
 * 32 bits.
 */
#define GCOMP_ADLER32_NMAX 5552u

uint32_t gcomp_adler32_update(
    uint32_t adler, const uint8_t * data, size_t len) {
  uint32_t s1 = adler & 0xFFFFu;
  uint32_t s2 = (adler >> 16) & 0xFFFFu;

  if (!data || len == 0) {
    return adler;
  }

  // One byte is common enough -- a decoder feeding a stream through -- to be
  // worth not entering the block machinery for.
  if (len == 1) {
    s1 += data[0];
    if (s1 >= GCOMP_ADLER32_BASE) {
      s1 -= GCOMP_ADLER32_BASE;
    }
    s2 += s1;
    if (s2 >= GCOMP_ADLER32_BASE) {
      s2 -= GCOMP_ADLER32_BASE;
    }
    return (s2 << 16) | s1;
  }

  while (len >= GCOMP_ADLER32_NMAX) {
    len -= GCOMP_ADLER32_NMAX;
    // 5552 = 16 * 347, so a full run is exactly 347 unrolled blocks.
    for (unsigned n = GCOMP_ADLER32_NMAX / 16u; n > 0; n--) {
      for (unsigned i = 0; i < 16u; i++) {
        s1 += data[i];
        s2 += s1;
      }
      data += 16;
    }
    s1 %= GCOMP_ADLER32_BASE;
    s2 %= GCOMP_ADLER32_BASE;
  }

  if (len > 0) {
    while (len >= 16u) {
      for (unsigned i = 0; i < 16u; i++) {
        s1 += data[i];
        s2 += s1;
      }
      data += 16;
      len -= 16u;
    }
    while (len-- > 0) {
      s1 += *data++;
      s2 += s1;
    }
    s1 %= GCOMP_ADLER32_BASE;
    s2 %= GCOMP_ADLER32_BASE;
  }

  return (s2 << 16) | s1;
}

uint32_t gcomp_adler32(const uint8_t * data, size_t len) {
  return gcomp_adler32_update(GCOMP_ADLER32_INIT, data, len);
}

uint32_t gcomp_adler32_combine(
    uint32_t adler1, uint32_t adler2, uint64_t len2) {
  // s1 is additive, less the 1 the second sum started from.  s2 gains the
  // first part's s2, the second part's s2, and len2 copies of the first
  // part's s1 -- because every byte of the second part adds the first part's
  // final s1 into s2 once.
  uint32_t rem = (uint32_t)(len2 % GCOMP_ADLER32_BASE);
  uint32_t s1_1 = adler1 & 0xFFFFu;
  uint32_t s2_1 = (adler1 >> 16) & 0xFFFFu;
  uint32_t s1_2 = adler2 & 0xFFFFu;
  uint32_t s2_2 = (adler2 >> 16) & 0xFFFFu;

  uint32_t s1 = s1_1 + s1_2 - 1u;
  if (s1 >= GCOMP_ADLER32_BASE) {
    s1 -= GCOMP_ADLER32_BASE;
  }

  uint64_t s2 = (uint64_t)s2_1 + (uint64_t)s2_2 +
      (uint64_t)rem * (uint64_t)s1_1 - (uint64_t)rem;
  s2 %= GCOMP_ADLER32_BASE;

  return ((uint32_t)s2 << 16) | s1;
}
