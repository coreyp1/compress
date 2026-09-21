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

#include "../../src/core/checksum_internal.h"

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


// ---------------------------------------------------------------------------
// The vector implementation, against the scalar one
// ---------------------------------------------------------------------------

// As in test_crc32.cpp: gcomp_adler32_update() chooses at run time, so these
// call each implementation by name.  See checksum_internal.h.

#if defined(__x86_64__) && defined(__GNUC__) &&                                \
    !defined(GCOMP_NO_CHECKSUM_SIMD) && !defined(GCOMP_ADLER32_NO_SSSE3)
#ifndef GCOMP_ADLER32_SSSE3
#error "the vector Adler-32 is not in this build, on a target that supports it"
#endif
#endif

namespace {

struct AdlerRng {
  uint64_t state;
  explicit AdlerRng(uint64_t seed) : state(seed) {}
  uint32_t Next() {
    state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    return static_cast<uint32_t>(state >> 33);
  }
};

} // namespace

// Ten thousand random buffers, random lengths and random alignments.
TEST(Adler32Variants, VectorAgreesWithScalarOnRandomBuffers) {
  constexpr size_t kMaxLen = 1024;
  constexpr size_t kMaxOffset = 64;
  std::vector<uint8_t> buf(kMaxLen + kMaxOffset);
  AdlerRng rng(0xD1B54A32D192ED03ull);
  size_t vector_runs = 0;

  for (int trial = 0; trial < 10000; trial++) {
    size_t offset = rng.Next() % kMaxOffset;
    size_t len = rng.Next() % (kMaxLen + 1u);
    for (size_t i = 0; i < offset + len; i++) {
      buf[i] = static_cast<uint8_t>(rng.Next() >> 3);
    }
    // A running Adler-32 from the middle of a stream, not just the initial
    // value: s1 and s2 both enter the vector accumulators, and seeding only
    // with GCOMP_ADLER32_INIT would leave s2's entry untested.
    uint32_t s1 = rng.Next() % GCOMP_ADLER32_BASE;
    uint32_t s2 = rng.Next() % GCOMP_ADLER32_BASE;
    uint32_t seed = (s2 << 16) | s1;
    const uint8_t * p = buf.data() + offset;

    uint32_t want = gcomp_adler32_update_scalar(seed, p, len);

    if (gcomp_adler32_ssse3_available()) {
      EXPECT_EQ(gcomp_adler32_update_ssse3(seed, p, len), want)
          << "ssse3, trial " << trial << " offset " << offset
          << " len " << len;
      if (len >= GCOMP_ADLER32_SSSE3_MIN) {
        vector_runs++;
      }
    }

    EXPECT_EQ(gcomp_adler32_update(seed, p, len), want)
        << "dispatch, trial " << trial << " offset " << offset
        << " len " << len;
  }

  if (!gcomp_adler32_ssse3_available()) {
    GTEST_SKIP() << "this CPU has no SSSE3; the vector path was not among "
                    "the implementations compared";
  }
  EXPECT_GT(vector_runs, 5000u);
}

// Every length across the 32-byte group boundary, so the vector loop, the
// scalar tail and the transition between them are each exercised alone.
TEST(Adler32Variants, VectorMatchesAtEveryLength) {
  if (!gcomp_adler32_ssse3_available()) {
    GTEST_SKIP() << "no SSSE3 on this CPU";
  }
  std::vector<uint8_t> buf(300);
  for (size_t i = 0; i < buf.size(); i++) {
    buf[i] = static_cast<uint8_t>(i * 97u + (i >> 2));
  }
  for (size_t len = 0; len <= buf.size(); len++) {
    EXPECT_EQ(
        gcomp_adler32_update_ssse3(GCOMP_ADLER32_INIT, buf.data(), len),
        gcomp_adler32_update_scalar(GCOMP_ADLER32_INIT, buf.data(), len))
        << "length " << len;
  }
}

// The sums are allowed to grow for 5552 bytes before they are reduced, and
// the vector accumulators inherit that bound exactly (adler32_ssse3.c says
// why).  Reaching the bound takes a deliberately hostile input: all-0xFF
// bytes, and -- the part that matters -- a running checksum that already
// sits at the largest value a reduction can leave behind.  Started from
// GCOMP_ADLER32_INIT this test cannot fail at all: s1 begins at 1 rather
// than 65520, the sums never climb high enough, and a reduction interval
// stretched past NMAX goes unnoticed.  Asked for directly, an interval of
// 5568 is caught on the first call.
TEST(Adler32Variants, VectorSurvivesTheWorstCaseForOverflow) {
  if (!gcomp_adler32_ssse3_available()) {
    GTEST_SKIP() << "no SSSE3 on this CPU";
  }
  constexpr uint32_t kMax = GCOMP_ADLER32_BASE - 1u; // what a reduction leaves
  std::vector<uint8_t> buf(64 * 1024, 0xFFu);
  const uint32_t seeds[] = {
      GCOMP_ADLER32_INIT,
      (kMax << 16) | kMax, // both sums as large as they can legally be
      (kMax << 16) | 1u,
      (1u << 16) | kMax,
  };
  // Around each reduction boundary, not only at round numbers, and past
  // several of them so a later run starts from whatever the earlier ones
  // left rather than from something chosen.
  const size_t lengths[] = {5535, 5536, 5537, 5551, 5552, 5553, 11071, 11072,
      11073, 16607, 16608, buf.size()};

  for (uint32_t seed : seeds) {
    for (size_t len : lengths) {
      EXPECT_EQ(gcomp_adler32_update_ssse3(seed, buf.data(), len),
          gcomp_adler32_update_scalar(seed, buf.data(), len))
          << "seed " << seed << " length " << len;
    }
  }
}

// Chunking must not change the answer, including across the dispatch
// threshold: a 20-byte call takes the scalar path and the next 200-byte call
// takes the vector one, from the sums the first left behind.
TEST(Adler32Variants, DispatchAgreesAtEverySplitPoint) {
  std::vector<uint8_t> buf(400);
  for (size_t i = 0; i < buf.size(); i++) {
    buf[i] = static_cast<uint8_t>(i * 29u + 11u);
  }
  uint32_t whole = gcomp_adler32(buf.data(), buf.size());
  for (size_t split = 0; split <= buf.size(); split++) {
    uint32_t a = GCOMP_ADLER32_INIT;
    a = gcomp_adler32_update(a, buf.data(), split);
    a = gcomp_adler32_update(a, buf.data() + split, buf.size() - split);
    EXPECT_EQ(a, whole) << "split at " << split;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
