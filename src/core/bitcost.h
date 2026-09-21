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
 * @file bitcost.h
 *
 * Costing symbols in fractional bits, for encoders that choose between
 * encodings before they know what the encoding will be.
 *
 * A parse that decides what to emit has to price what it is considering, and
 * the price is the number of bits an entropy coder will charge: the entropy
 * of the symbol under the statistics of what has been encoded recently.  That
 * is not a whole number, and the difference between a literal costing 4.6
 * bits and one costing 4.7 decides parses -- so prices here are in 256ths of
 * a bit rather than in bits.
 */

#ifndef GHOTI_IO_GCOMP_SRC_CORE_BITCOST_H
#define GHOTI_IO_GCOMP_SRC_CORE_BITCOST_H

#include <ghoti.io/compress/macros.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Fractional bits a price carries.  A price of GCOMP_BITCOST_ONE is one bit.
#define GCOMP_BITCOST_SHIFT 8
#define GCOMP_BITCOST_ONE (1u << GCOMP_BITCOST_SHIFT)

/**
 * @brief log2(@p x) in 256ths of a bit.
 *
 * The whole part is the position of the top set bit.  The fraction comes out
 * one bit at a time by repeatedly squaring what is left: squaring doubles a
 * logarithm, so whether the square has reached 2 is exactly the next bit of
 * the answer.  Eight rounds give eight fractional bits.
 *
 * The mantissa is held with 31 fraction bits, so the square fits a 64-bit
 * product with nothing to spare and nothing lost.
 *
 * Zero has no logarithm.  A model never holds a count of zero -- a symbol
 * that has not come up is unseen, not impossible -- but asking must still
 * give a usable number rather than wrapping, so zero is treated as one.
 */
static inline uint32_t gcomp_bitcost_log2(uint32_t x) {
  if (x < 1u) {
    x = 1u;
  }
  unsigned hb = 31u - (unsigned)__builtin_clz(x);
  uint32_t result = (uint32_t)hb << GCOMP_BITCOST_SHIFT;
  uint64_t m = ((uint64_t)x << 31) >> hb; // 1.0 <= m < 2.0, 31 fraction bits
  for (unsigned i = 0; i < GCOMP_BITCOST_SHIFT; i++) {
    m = (m * m) >> 31;
    if (m >= ((uint64_t)1 << 32)) {
      m >>= 1;
      result += 1u << (GCOMP_BITCOST_SHIFT - 1u - i);
    }
  }
  return result;
}

/**
 * @brief Turn counts into prices: -log2(count / total), floored.
 *
 * The floor is what the format can actually charge.  A Huffman-coded symbol
 * costs at least one bit however common it is, so pricing one at a third of a
 * bit would be a promise the encoder cannot keep; an FSE-coded symbol
 * genuinely can cost less, and wants a floor only to keep an edge from being
 * free.
 *
 * @param freq Counts, none of them zero.
 * @param price Receives the prices.
 * @param count Symbols in the alphabet.
 * @param floor_price Lowest price any symbol may be given.
 */
static inline void gcomp_bitcost_from_freq(const uint32_t * freq,
    uint32_t * price, size_t count, uint32_t floor_price) {
  uint32_t total = 0;
  for (size_t i = 0; i < count; i++) {
    total += freq[i];
  }
  uint32_t log_total = gcomp_bitcost_log2(total);
  for (size_t i = 0; i < count; i++) {
    uint32_t p = log_total - gcomp_bitcost_log2(freq[i]);
    price[i] = (p < floor_price) ? floor_price : p;
  }
}

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_CORE_BITCOST_H
