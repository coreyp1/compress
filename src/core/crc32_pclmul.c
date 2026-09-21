/**
 * @file crc32_pclmul.c
 *
 * CRC-32 by carry-less multiply folding (Gopal et al., "Fast CRC Computation
 * for Generic Polynomials Using PCLMULQDQ Instruction", Intel, 2009).
 *
 * The polynomial and the bit order are RFC 1952 section 8's; this computes
 * exactly what `gcomp_crc32_update_scalar()` computes, and the test asserts
 * that on ten thousand random buffers.
 *
 * WHAT THE INSTRUCTION DOES
 * =========================
 *
 * `PCLMULQDQ` multiplies two 64-bit operands as polynomials over GF(2) --
 * a multiply with no carries, which is exactly the arithmetic a CRC is made
 * of.  A CRC is a remainder mod the generator polynomial P(x), remainders
 * are linear, and so the contribution of a 16-byte chunk can be advanced
 * past any number of later bytes by multiplying it by a constant power of x.
 * That is the whole idea: instead of walking the message, multiply each
 * chunk by however many powers of x stand between it and the end, and add
 * (XOR) the results.
 *
 * The win is not that the multiply is fast.  It is that four chunks can be
 * advanced at once, on four independent registers, so the seven-cycle
 * latency of the multiply is hidden behind three other multiplies instead of
 * stalling.  Slice-by-8 does the same trick in the other currency -- it
 * breaks the dependent chain of table lookups -- and this simply has more
 * width to spend.
 *
 * THE BIT ORDER, WHICH IS WHERE THIS GOES WRONG IF IT IS GOING TO
 * ===============================================================
 *
 * RFC 1952's CRC-32 is reflected: the low-order bit of each byte is fed in
 * first, so the *first* bit of the message is the *highest* power of x.
 * Write that as one rule covering every register this file uses:
 *
 *     bit p of an N-bit value carries the coefficient of x^(N-1-p).
 *
 * It holds for the 32-bit running CRC (bit 0 is x^31 -- which is what makes
 * `crc = (crc >> 1) ^ 0xEDB88320` a multiply by x), and it holds for a
 * 16-byte chunk loaded little-endian into an XMM register (byte i bit j
 * lands at register bit 8i+j, carrying x^(127-8i-j)).  One rule, both.
 *
 * Note that "register bits 0..63", the low 64-bit lane, carry the *high*
 * powers.  The naming below says `hi` and `lo` for the powers, not the lane.
 *
 * `PCLMULQDQ` does not share that rule: it reads bit p as x^p.  Feed it two
 * lanes whose polynomials under our rule are A and K, and the 128-bit result,
 * read back under our rule, is
 *
 *     x * A(x) * K(x)
 *
 * -- the product, with one extra power of x.  (Each operand is its own
 * reciprocal under the two readings; the two reciprocals multiply to give a
 * degree-126 reciprocal, and reading that as a degree-127 one is the shift.)
 * This file never corrects that shift at run time.  It folds it into the
 * constants instead, by asking each for one power of x less than the algebra
 * wants.  That is the only reason the exponents below are odd numbers.
 *
 * THE FOLD
 * ========
 *
 * Let A be the 128-bit accumulator and M the next chunk.  Advancing A past D
 * chunks and adding M is
 *
 *     A' = A * x^(128D) + M
 *
 * Split A into its two lanes, A = A_hi * x^64 + A_lo, and that is
 *
 *     A' = A_hi * x^(128D + 64) + A_lo * x^(128D) + M
 *
 * so one constant per lane.  With the extra power of x above folded in, the
 * constants are x^(128D + 63) mod P and x^(128D - 1) mod P.  Both have
 * degree below 32, and under our rule a degree-31 polynomial in a 64-bit
 * lane occupies bits 32..63 -- which is why the constants below are 32-bit
 * words sitting in the top half of each lane.
 *
 * Reducing modulo P at each step is not required and is not done; the
 * accumulator only has to stay *congruent* to the true value, and one
 * reduction at the end settles it.
 *
 * THE FINAL REDUCTION, WHICH IS NOT BARRETT'S
 * ===========================================
 *
 * The usual ending is a Barrett reduction with two more magic constants.
 * There is a shorter way that needs none.  The identity the whole file rests
 * on is that the running CRC of a message M starting from zero is
 * (M * x^32) mod P -- and that is precisely the number wanted from the
 * accumulator.  So store the accumulator's sixteen bytes and run them
 * through the scalar CRC from zero.  It is sixteen bytes of table lookups
 * once per call, against megabytes of folding, and it is code that is
 * already tested rather than two more constants that would not be.
 *
 * READING PAST THE END
 * ====================
 *
 * Every load here is `_mm_loadu_si128` over sixteen bytes that the length
 * already guaranteed.  Nothing reads past `data + len`, so ASan and Valgrind
 * have nothing to say about this file, and no caller has to leave slack.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>

#include "checksum_internal.h"

#ifdef GCOMP_CRC32_PCLMUL

#include <stdatomic.h>
#include <wmmintrin.h>

/**
 * @brief Instruction set widened for one function.
 *
 * `sse2` is named alongside `pclmul` because the loads, stores and XORs are
 * SSE2; naming only `pclmul` leaves the rest to the command line, which is
 * the coupling this file exists to avoid.
 */
#define GCOMP_TARGET_PCLMUL __attribute__((target("pclmul,sse2")))

/**
 * @brief Advance `acc` past the distance `k` was built for and add `next`.
 *
 * `k` holds the high-lane constant in its low half and the low-lane constant
 * in its high half, so the two multiplies are selected by 0x00 and 0x11 and
 * one register carries both.
 */
GCOMP_TARGET_PCLMUL static inline __m128i gcomp_crc32_fold(
    __m128i acc, __m128i k, __m128i next) {
  __m128i hi = _mm_clmulepi64_si128(acc, k, 0x00);
  __m128i lo = _mm_clmulepi64_si128(acc, k, 0x11);
  return _mm_xor_si128(_mm_xor_si128(hi, lo), next);
}

GCOMP_TARGET_PCLMUL uint32_t gcomp_crc32_update_pclmul(
    uint32_t crc, const uint8_t * data, size_t len) {
  const __m128i k_by4 = _mm_set_epi64x(
      (long long)GCOMP_CRC32_K_BY4_LO, (long long)GCOMP_CRC32_K_BY4_HI);
  const __m128i k_by1 = _mm_set_epi64x(
      (long long)GCOMP_CRC32_K_BY1_LO, (long long)GCOMP_CRC32_K_BY1_HI);
  uint8_t folded[16];
  size_t off = 64u;
  __m128i x0;
  __m128i x1;
  __m128i x2;
  __m128i x3;

  if (!data || len < GCOMP_CRC32_PCLMUL_MIN) {
    return gcomp_crc32_update_scalar(crc, data, len);
  }

  x0 = _mm_loadu_si128((const __m128i *)(const void *)(data));
  x1 = _mm_loadu_si128((const __m128i *)(const void *)(data + 16));
  x2 = _mm_loadu_si128((const __m128i *)(const void *)(data + 32));
  x3 = _mm_loadu_si128((const __m128i *)(const void *)(data + 48));

  // The running CRC enters as x^96 times its polynomial, which under the bit
  // rule above is simply its four bytes at register bits 0..31 -- the
  // familiar "XOR the CRC into the first four bytes of the message".
  x0 = _mm_xor_si128(x0, _mm_cvtsi32_si128((int)crc));

  // Four accumulators so that four folds are in flight at once.  Each one
  // carries every fourth chunk.
  while (len - off >= 64u) {
    const uint8_t * p = data + off;
    x0 = gcomp_crc32_fold(x0, k_by4,
        _mm_loadu_si128((const __m128i *)(const void *)(p)));
    x1 = gcomp_crc32_fold(x1, k_by4,
        _mm_loadu_si128((const __m128i *)(const void *)(p + 16)));
    x2 = gcomp_crc32_fold(x2, k_by4,
        _mm_loadu_si128((const __m128i *)(const void *)(p + 32)));
    x3 = gcomp_crc32_fold(x3, k_by4,
        _mm_loadu_si128((const __m128i *)(const void *)(p + 48)));
    off += 64u;
  }

  // Collapse the four.  Folding them in one at a time costs three dependent
  // multiplies once per call and needs no constants beyond the by-one pair;
  // advancing each by its own distance in parallel would need three more
  // constants to save about fourteen cycles, once.
  x0 = gcomp_crc32_fold(x0, k_by1, x1);
  x0 = gcomp_crc32_fold(x0, k_by1, x2);
  x0 = gcomp_crc32_fold(x0, k_by1, x3);

  while (len - off >= 16u) {
    x0 = gcomp_crc32_fold(x0, k_by1,
        _mm_loadu_si128((const __m128i *)(const void *)(data + off)));
    off += 16u;
  }

  // See "the final reduction" above: the CRC of these sixteen bytes from
  // zero is the accumulator reduced mod P.
  _mm_storeu_si128((__m128i *)(void *)folded, x0);
  crc = gcomp_crc32_update_scalar(0u, folded, sizeof(folded));

  return gcomp_crc32_update_scalar(crc, data + off, len - off);
}

bool gcomp_crc32_pclmul_available(void) {
  // Three states rather than a bool plus a flag: 0 is "not asked yet", and
  // the two answers are distinguishable from it.  Relaxed is the right
  // ordering because nothing else is published alongside this -- every
  // thread that races here computes the same answer from the same CPUID.
  // It is spelled atomically all the same, because a plain int written by
  // two threads is a data race whatever the values are, and TSan is right
  // to say so.
  static atomic_int state = 0;
  int s = atomic_load_explicit(&state, memory_order_relaxed);

  if (s == 0) {
    s = __builtin_cpu_supports("pclmul") ? 1 : -1;
    atomic_store_explicit(&state, s, memory_order_relaxed);
  }

  return s > 0;
}

#else // GCOMP_CRC32_PCLMUL

/**
 * @brief The folding path is not in this build.
 *
 * Both stubs exist so that the dispatcher in crc32.c needs no `#ifdef`: it
 * asks whether the path is available and is told no, on every target.
 */
bool gcomp_crc32_pclmul_available(void) {
  return false;
}

uint32_t gcomp_crc32_update_pclmul(
    uint32_t crc, const uint8_t * data, size_t len) {
  // Unreachable through the dispatcher, and correct if some later caller
  // reaches it anyway.
  return gcomp_crc32_update_scalar(crc, data, len);
}

#endif // GCOMP_CRC32_PCLMUL
