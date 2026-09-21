/**
 * @file deflate_noise.h
 *
 * Deterministic generators for decoder test corpora.
 *
 * Two of them, and the difference matters.  The encoder decides per block
 * whether to store the bytes or code them, and for data it cannot compress the
 * decision is close to the line: `Lcg` lands on the stored side and `Xorshift`
 * on the coded side, at the same size and the same apparent randomness.  Which
 * one a test uses decides whether its stream has coded blocks at all, and so
 * whether the Huffman paths under test are ever reached.  Neither is more
 * random than the other; the distinction is invisible in the bytes.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GCOMP_TESTS_DEFLATE_NOISE_H
#define GCOMP_TESTS_DEFLATE_NOISE_H

#include <cstdint>

namespace gcomp_test {

/// Noise the encoder tends to store rather than code.
class Lcg {
public:
  explicit Lcg(uint32_t seed) : state_(seed ? seed : 1u) {}
  uint32_t Next() {
    state_ = state_ * 1103515245u + 12345u;
    return state_ >> 8;
  }
  uint8_t Byte() { return (uint8_t)(Next() & 0xFFu); }

private:
  uint32_t state_;
};

/// Noise the encoder tends to code rather than store.
class Xorshift {
public:
  explicit Xorshift(uint64_t seed) : state_(seed ? seed : 1u) {}
  uint8_t Byte() {
    state_ ^= state_ << 13u;
    state_ ^= state_ >> 7u;
    state_ ^= state_ << 17u;
    return (uint8_t)(state_ & 0xFFu);
  }

private:
  uint64_t state_;
};

} // namespace gcomp_test

#endif // GCOMP_TESTS_DEFLATE_NOISE_H
