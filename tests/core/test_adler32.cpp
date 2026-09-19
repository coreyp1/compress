/**
 * @file test_adler32.cpp
 *
 * Tests for Adler-32 (RFC 1950 section 9).
 *
 * The checksum a zlib stream ends with, and therefore the thing that decides
 * whether anyone else's decoder accepts what we produce.  These check it
 * three ways: against published values, against its own algebra, and against
 * the property that makes the batched implementation worth having -- that
 * deferring the modulo for 5552 bytes changes nothing.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
#include <algorithm>
#include <cstring>
#include <ghoti.io/compress/adler32.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

/// The definition, written out literally, with no batching and no unrolling.
uint32_t NaiveAdler32(const uint8_t * data, size_t len) {
  uint32_t s1 = 1;
  uint32_t s2 = 0;
  for (size_t i = 0; i < len; i++) {
    s1 = (s1 + data[i]) % GCOMP_ADLER32_BASE;
    s2 = (s2 + s1) % GCOMP_ADLER32_BASE;
  }
  return (s2 << 16) | s1;
}

std::vector<uint8_t> Noise(size_t n, unsigned seed) {
  std::vector<uint8_t> v(n);
  unsigned s = seed;
  for (size_t i = 0; i < n; i++) {
    s = s * 1103515245u + 12345u;
    v[i] = (uint8_t)(s >> 16);
  }
  return v;
}

TEST(Adler32, MatchesPublishedValues) {
  struct {
    const char * text;
    uint32_t expected;
  } cases[] = {
      {"", 0x00000001u},
      {"a", 0x00620062u},
      {"abc", 0x024D0127u},
      {"message digest", 0x29750586u},
      {"abcdefghijklmnopqrstuvwxyz", 0x90860B20u},
      {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
          0x8ADB150Cu},
  };
  for (const auto & c : cases) {
    EXPECT_EQ(gcomp_adler32((const uint8_t *)c.text, strlen(c.text)),
        c.expected)
        << "input \"" << c.text << "\"";
  }
}

/// The empty checksum is 1, not 0.  Getting this wrong is off-by-a-constant
/// in a way nothing notices until a decoder rejects the stream.
TEST(Adler32, TheEmptyChecksumIsOne) {
  EXPECT_EQ(GCOMP_ADLER32_INIT, 1u);
  EXPECT_EQ(gcomp_adler32(nullptr, 0), 1u);
  const uint8_t byte = 0;
  EXPECT_EQ(gcomp_adler32(&byte, 0), 1u);
}

/**
 * The batched loop defers the modulo for 5552 bytes and steps sixteen at a
 * time.  Neither may change the answer, so the lengths that straddle both
 * boundaries are checked against the definition written out longhand.
 */
TEST(Adler32, BatchingAndUnrollingChangeNothing) {
  std::vector<uint8_t> data = Noise(40000, 99);
  // Around the 16-byte unroll, around NMAX, and around twice NMAX.
  const size_t lengths[] = {0, 1, 2, 15, 16, 17, 31, 32, 33, 5551, 5552, 5553,
      11103, 11104, 11105, 40000};
  for (size_t len : lengths) {
    EXPECT_EQ(gcomp_adler32(data.data(), len), NaiveAdler32(data.data(), len))
        << "len=" << len;
  }
}

/// All-0xFF is the worst case the NMAX bound is derived from: if the sums
/// were going to overflow, they would do it here.
TEST(Adler32, TheWorstCaseForOverflowIsStillCorrect) {
  std::vector<uint8_t> ones(20000, 0xFFu);
  const size_t lengths[] = {5551, 5552, 5553, 11104, 20000};
  for (size_t len : lengths) {
    EXPECT_EQ(gcomp_adler32(ones.data(), len), NaiveAdler32(ones.data(), len))
        << "len=" << len;
  }
}

/// Feeding it in pieces must give the same answer as feeding it whole --
/// which is what lets a streaming encoder accumulate as input arrives.
TEST(Adler32, IncrementalEqualsOneShot) {
  std::vector<uint8_t> data = Noise(200000, 7);
  uint32_t whole = gcomp_adler32(data.data(), data.size());

  const size_t steps[] = {1, 7, 16, 5551, 5552, 5553, 60000};
  for (size_t step : steps) {
    uint32_t running = GCOMP_ADLER32_INIT;
    for (size_t off = 0; off < data.size(); off += step) {
      size_t take = std::min(step, data.size() - off);
      running = gcomp_adler32_update(running, data.data() + off, take);
    }
    EXPECT_EQ(running, whole) << "step=" << step;
  }
}

/// Updating with nothing is a no-op, however it is spelled.
TEST(Adler32, UpdatingWithNothingChangesNothing) {
  std::vector<uint8_t> data = Noise(1000, 3);
  uint32_t sum = gcomp_adler32(data.data(), data.size());
  EXPECT_EQ(gcomp_adler32_update(sum, nullptr, 0), sum);
  EXPECT_EQ(gcomp_adler32_update(sum, data.data(), 0), sum);
}

TEST(Adler32, CombineAgreesWithSummingTheWhole) {
  std::vector<uint8_t> data = Noise(200000, 11);
  uint32_t whole = gcomp_adler32(data.data(), data.size());

  const size_t splits[] = {0, 1, 15, 16, 5552, 100000, 199999, 200000};
  for (size_t split : splits) {
    uint32_t a = gcomp_adler32(data.data(), split);
    uint32_t b = gcomp_adler32(data.data() + split, data.size() - split);
    EXPECT_EQ(gcomp_adler32_combine(a, b, data.size() - split), whole)
        << "split=" << split;
  }
}

/// A single changed byte must change the checksum.  Adler-32 is weak, but not
/// that weak, and this is the failure a checksum exists to catch.
TEST(Adler32, ASingleFlippedByteChangesIt) {
  std::vector<uint8_t> data = Noise(4096, 5);
  uint32_t original = gcomp_adler32(data.data(), data.size());
  const size_t positions[] = {0, 1, 2047, 4094, 4095};
  for (size_t at : positions) {
    std::vector<uint8_t> tweaked = data;
    tweaked[at] ^= 0x01u;
    EXPECT_NE(gcomp_adler32(tweaked.data(), tweaked.size()), original)
        << "flipping byte " << at << " went unnoticed";
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
