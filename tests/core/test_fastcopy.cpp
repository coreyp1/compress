/**
 * @file test_fastcopy.cpp
 *
 * Tests for the short-copy helper the zstd sequence decoder uses.
 *
 * The requirement stated in fastcopy.h is that gcomp_copy_short() is
 * `memcpy` in every observable way for non-overlapping regions -- the same
 * bytes moved, and nothing written outside them.  Both halves of that matter
 * and only the first is obvious: the helper copies its two ends with
 * overlapping fixed-width moves, so an off-by-one in a size class writes past
 * the destination rather than producing wrong output, which a comparison of
 * the copied bytes alone would not see.
 *
 * So every length from 0 to twice the libc threshold is checked against
 * memcpy over a buffer with poisoned margins on both sides, at several source
 * and destination alignments, and the margins are checked too.
 *
 * The slack variants make the opposite promise: the bytes below `n` are what
 * memcpy would have moved, and everything they write past that is within
 * GCOMP_FASTCOPY_SLACK of it.  Both halves of that are checked here too --
 * the second is what the zstd decoder's buffers are sized from, so a helper
 * that overshoots further than it says would run off the end of an
 * allocation rather than producing wrong output.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "../../src/core/fastcopy.h"
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

namespace {

constexpr uint8_t kPoison = 0xA5u;
constexpr size_t kMargin = 64u;

// Bytes with no structure, so a copy that reads the wrong place shows up.
std::vector<uint8_t> SourceBytes(size_t n) {
  std::vector<uint8_t> v(n);
  uint32_t seed = 0x1234567u;
  for (size_t i = 0; i < n; i++) {
    seed = seed * 1103515245u + 12345u;
    v[i] = static_cast<uint8_t>(seed >> 19);
  }
  return v;
}

} // namespace

TEST(FastCopyTest, CopiesExactlyWhatMemcpyWouldForEveryLength) {
  const size_t max_len = 2u * GCOMP_FASTCOPY_LIMIT;
  std::vector<uint8_t> src = SourceBytes(max_len + 2u * kMargin);

  for (size_t len = 0; len <= max_len; len++) {
    for (size_t src_align = 0; src_align < 8u; src_align++) {
      for (size_t dst_align = 0; dst_align < 8u; dst_align++) {
        std::vector<uint8_t> ours(max_len + 2u * kMargin, kPoison);
        std::vector<uint8_t> theirs(max_len + 2u * kMargin, kPoison);

        gcomp_copy_short(ours.data() + kMargin + dst_align,
            src.data() + src_align, len);
        memcpy(theirs.data() + kMargin + dst_align, src.data() + src_align,
            len);

        ASSERT_EQ(ours, theirs)
            << "length " << len << ", source alignment " << src_align
            << ", destination alignment " << dst_align;
      }
    }
  }
}

// The margins are the point of the test above, but an equality on the whole
// buffer reports "they differ" rather than "it wrote past the end", so this
// says which it was.
TEST(FastCopyTest, WritesNothingOutsideTheLengthAsked) {
  const size_t max_len = 2u * GCOMP_FASTCOPY_LIMIT;
  std::vector<uint8_t> src = SourceBytes(max_len + 2u * kMargin);

  for (size_t len = 0; len <= max_len; len++) {
    std::vector<uint8_t> buf(max_len + 2u * kMargin, kPoison);
    gcomp_copy_short(buf.data() + kMargin, src.data(), len);

    for (size_t i = 0; i < kMargin; i++) {
      ASSERT_EQ(buf[i], kPoison)
          << "length " << len << " wrote " << (kMargin - i)
          << " bytes before the destination";
    }
    for (size_t i = kMargin + len; i < buf.size(); i++) {
      ASSERT_EQ(buf[i], kPoison)
          << "length " << len << " wrote " << (i - kMargin - len + 1u)
          << " bytes past the end of the destination";
    }
  }
}

// Long copies are handed to libc; the boundary is where a mistake would sit.
TEST(FastCopyTest, HandsLongCopiesOverUnchanged) {
  for (size_t len : {GCOMP_FASTCOPY_LIMIT - 1u, GCOMP_FASTCOPY_LIMIT,
           GCOMP_FASTCOPY_LIMIT + 1u, 4096u}) {
    std::vector<uint8_t> src = SourceBytes(len);
    std::vector<uint8_t> dst(len + kMargin, kPoison);

    gcomp_copy_short(dst.data(), src.data(), len);

    EXPECT_EQ(memcmp(dst.data(), src.data(), len), 0) << "length " << len;
    for (size_t i = len; i < dst.size(); i++) {
      ASSERT_EQ(dst[i], kPoison) << "length " << len << " overran";
    }
  }
}

//
// The slack variants
//

namespace {

// What gcomp_copy_repeat_slack() has to reproduce: an LZ match copy, one
// byte at a time, which is the definition rather than an implementation of
// it.  A byte may read one this loop has just written, which is the whole
// difficulty.
std::vector<uint8_t> RepeatReference(
    const std::vector<uint8_t> & history, size_t offset, size_t n) {
  std::vector<uint8_t> out = history;
  const size_t start = out.size();
  out.resize(start + n);
  for (size_t i = 0; i < n; i++) {
    out[start + i] = out[start + i - offset];
  }
  return out;
}

} // namespace

TEST(FastCopySlackTest, CopiesExactlyWhatMemcpyWouldForEveryLength) {
  const size_t max_len = 4u * GCOMP_FASTCOPY_SLACK;
  std::vector<uint8_t> src = SourceBytes(max_len + 2u * kMargin);

  for (size_t len = 0; len <= max_len; len++) {
    for (size_t src_align = 0; src_align < 8u; src_align++) {
      for (size_t dst_align = 0; dst_align < 8u; dst_align++) {
        std::vector<uint8_t> dst(max_len + 2u * kMargin, kPoison);

        gcomp_copy_slack(dst.data() + kMargin + dst_align,
            src.data() + src_align, len);

        ASSERT_EQ(memcmp(dst.data() + kMargin + dst_align,
                      src.data() + src_align, len),
            0)
            << "length " << len << ", source alignment " << src_align
            << ", destination alignment " << dst_align;
      }
    }
  }
}

TEST(FastCopySlackTest, WritesNoFurtherPastTheEndThanTheSlackAllows) {
  const size_t max_len = 4u * GCOMP_FASTCOPY_SLACK;
  std::vector<uint8_t> src = SourceBytes(max_len + 2u * kMargin);

  for (size_t len = 0; len <= max_len; len++) {
    std::vector<uint8_t> buf(max_len + 2u * kMargin, kPoison);
    gcomp_copy_slack(buf.data() + kMargin, src.data(), len);

    for (size_t i = 0; i < kMargin; i++) {
      ASSERT_EQ(buf[i], kPoison)
          << "length " << len << " wrote " << (kMargin - i)
          << " bytes before the destination";
    }
    for (size_t i = kMargin + len + GCOMP_FASTCOPY_SLACK; i < buf.size(); i++) {
      ASSERT_EQ(buf[i], kPoison)
          << "length " << len << " wrote "
          << (i - kMargin - len - GCOMP_FASTCOPY_SLACK + 1u)
          << " bytes past the slack it is allowed";
    }
  }
}

// Every offset the widening table covers, every offset just above it, and
// lengths either side of a move width.  An offset below the move width is
// the case the table exists for: the copy reads bytes it has just written,
// and a wrong entry produces a plausible-looking repetition of the wrong
// period rather than a crash.
TEST(FastCopySlackTest, RepeatsThePeriodForEveryOffsetAndLength) {
  const size_t max_len = 4u * GCOMP_FASTCOPY_SLACK;
  const size_t max_offset = 3u * GCOMP_FASTCOPY_MOVE;
  std::vector<uint8_t> history = SourceBytes(max_offset + kMargin);

  for (size_t offset = 1u; offset <= max_offset; offset++) {
    for (size_t len = 1u; len <= max_len; len++) {
      std::vector<uint8_t> want = RepeatReference(history, offset, len);

      std::vector<uint8_t> buf(
          history.size() + max_len + kMargin + GCOMP_FASTCOPY_SLACK, kPoison);
      memcpy(buf.data(), history.data(), history.size());

      gcomp_copy_repeat_slack(buf.data() + history.size(), offset, len);

      ASSERT_EQ(memcmp(buf.data(), want.data(), want.size()), 0)
          << "offset " << offset << ", length " << len;
    }
  }
}

TEST(FastCopySlackTest, RepeatWritesNoFurtherPastTheEndThanTheSlackAllows) {
  const size_t max_len = 4u * GCOMP_FASTCOPY_SLACK;
  const size_t max_offset = 3u * GCOMP_FASTCOPY_MOVE;
  std::vector<uint8_t> history = SourceBytes(max_offset + kMargin);

  for (size_t offset = 1u; offset <= max_offset; offset++) {
    for (size_t len = 1u; len <= max_len; len++) {
      std::vector<uint8_t> buf(
          history.size() + max_len + kMargin + GCOMP_FASTCOPY_SLACK, kPoison);
      memcpy(buf.data(), history.data(), history.size());

      gcomp_copy_repeat_slack(buf.data() + history.size(), offset, len);

      const size_t limit = history.size() + len + GCOMP_FASTCOPY_SLACK;
      for (size_t i = limit; i < buf.size(); i++) {
        ASSERT_EQ(buf[i], kPoison)
            << "offset " << offset << ", length " << len << " wrote "
            << (i - limit + 1u) << " bytes past the slack it is allowed";
      }
    }
  }
}

// The two are used together on the same buffer, so the slack one asks for has
// to cover the other.  This is the number zstd_decoder.c sizes its buffers
// from; if it ever stops covering a move it has to be raised there too.
TEST(FastCopySlackTest, TheSlackCoversAMoveOnEitherSideOfIt) {
  EXPECT_GE(GCOMP_FASTCOPY_SLACK, 2u * GCOMP_FASTCOPY_MOVE);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
