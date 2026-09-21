/**
 * @file deflate_bits.h
 *
 * A bit writer for tests that need a DEFLATE stream with a particular shape.
 *
 * Encoders are free to choose block types, block lengths and code lengths,
 * and a test that depends on one of those choices is a test that will start
 * passing for the wrong reason.  Several of the decoder's edges are only
 * reachable from a stream an encoder would never produce at all -- a
 * literal/length symbol of 286, a distance symbol of 30 -- and those have to
 * be written out by hand.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GCOMP_TESTS_DEFLATE_BITS_H
#define GCOMP_TESTS_DEFLATE_BITS_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gcomp_test {

/**
 * @brief Writes DEFLATE's two kinds of bit field (RFC 1951 section 3.1.1).
 *
 * Plain fields go in least-significant bit first; Huffman codes go in
 * most-significant bit of the code first.  Both end up packed into bytes from
 * the low bit up, which is why they need separate spellings.
 */
class BitWriter {
public:
  /// A plain field: BFINAL, BTYPE, an extra-bits value.
  void Field(uint32_t value, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
      PutBit((value >> i) & 1u);
    }
  }

  /// A Huffman code of @p n bits, written most-significant bit first.
  void Code(uint32_t code, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
      PutBit((code >> (n - 1u - i)) & 1u);
    }
  }

  /// A literal or length symbol in the fixed alphabet (RFC 1951 3.2.6).
  void FixedLitLen(unsigned sym) {
    if (sym < 144u) {
      Code(0x30u + sym, 8u); // 00110000 through 10111111
    }
    else if (sym < 256u) {
      Code(0x190u + (sym - 144u), 9u); // 110010000 through 111111111
    }
    else if (sym < 280u) {
      Code(0x0u + (sym - 256u), 7u); // 0000000 through 0010111
    }
    else {
      Code(0xC0u + (sym - 280u), 8u); // 11000000 through 11000111
    }
  }

  /// A distance symbol in the fixed alphabet: five bits, the symbol itself.
  void FixedDistance(unsigned sym) { Code(sym, 5u); }

  void FixedEndOfBlock() { FixedLitLen(256u); }

  /// Bits written so far, for a test that needs a seam in a given place.
  size_t BitsWritten() const { return bytes_.size() * 8u + nbits_; }

  /// Pad the last byte with zeros and hand over what was written.
  std::vector<uint8_t> Finish() {
    if (nbits_ != 0u) {
      bytes_.push_back(acc_);
      acc_ = 0u;
      nbits_ = 0u;
    }
    return bytes_;
  }

private:
  void PutBit(uint32_t bit) {
    acc_ = (uint8_t)(acc_ | ((bit & 1u) << nbits_));
    if (++nbits_ == 8u) {
      bytes_.push_back(acc_);
      acc_ = 0u;
      nbits_ = 0u;
    }
  }
  std::vector<uint8_t> bytes_;
  uint8_t acc_ = 0u;
  unsigned nbits_ = 0u;
};

} // namespace gcomp_test

#endif // GCOMP_TESTS_DEFLATE_BITS_H
