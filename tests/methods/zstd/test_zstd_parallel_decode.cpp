/**
 * @file test_zstd_parallel_decode.cpp
 *
 * A Zstandard stream can be split only between frames: blocks inside one share
 * a window (RFC 8878 section 3.1.1.1.2). Our own encoder emits a single frame,
 * so what this exercises is the stream shape somebody made deliberately -
 * concatenated files, one frame per thread, and the seekable files Phase D
 * will write.
 *
 * The question is the same as for LZ4: are the bytes, and the errors, what one
 * thread gives?
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> make_data(size_t len, uint32_t seed, int shape) {
  std::vector<uint8_t> v(len);
  uint32_t s = seed ? seed : 1u;
  for (size_t i = 0; i < len; i++) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    switch (shape) {
    case 0: v[i] = 0; break;
    case 1: v[i] = (uint8_t)(s >> 24); break;
    case 2: v[i] = (uint8_t)('a' + ((s >> 28) & 0x0F)); break;
    default: v[i] = (uint8_t)((i / 719) & 0xFF); break;
    }
  }
  return v;
}

/// One frame, with its content size declared - which is what lets it be a job.
std::vector<uint8_t> encode_frame(
    const std::vector<uint8_t> & in, int64_t level, bool checksum,
    bool declare_size) {
  gcomp_options_t * o = nullptr;
  EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_int64(o, "zstd.level", level), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_bool(o, "zstd.checksum", checksum ? 1 : 0),
      GCOMP_OK);
  if (declare_size) {
    EXPECT_EQ(gcomp_options_set_uint64(o, "zstd.content_size", in.size()),
        GCOMP_OK);
  }
  size_t bound = 0;
  EXPECT_EQ(gcomp_encode_bound(nullptr, "zstd", o, in.size(), &bound), GCOMP_OK);
  std::vector<uint8_t> out(bound ? bound : 1);
  size_t w = 0;
  EXPECT_EQ(gcomp_encode_buffer(nullptr, "zstd", o, in.data(), in.size(),
                out.data(), out.size(), &w),
      GCOMP_OK);
  gcomp_options_destroy(o);
  out.resize(w);
  return out;
}

gcomp_status_t decode_threads(const std::vector<uint8_t> & s, uint64_t threads,
    std::vector<uint8_t> & out, size_t capacity) {
  gcomp_options_t * o = nullptr;
  EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
  if (threads > 1) {
    EXPECT_EQ(gcomp_options_set_uint64(o, "threads.count", threads), GCOMP_OK);
  }
  EXPECT_EQ(gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 0),
      GCOMP_OK);
  out.assign(capacity, 0);
  size_t produced = 0;
  const gcomp_status_t st = gcomp_decode_buffer(
      nullptr, "zstd", o, s.data(), s.size(), out.data(), out.size(), &produced);
  gcomp_options_destroy(o);
  out.resize(st == GCOMP_OK ? produced : 0);
  return st;
}

void append(std::vector<uint8_t> & dst, const std::vector<uint8_t> & src) {
  dst.insert(dst.end(), src.begin(), src.end());
}

const uint64_t k_thread_counts[] = {2, 3, 4, 12};

} // namespace

/// Several frames, decoded together, give what one thread gives.
TEST(ZstdParallelDecode, ThreadsProduceIdenticalBytes) {
  struct Piece {
    size_t len;
    int shape;
    int64_t level;
    bool checksum;
  };
  const Piece pieces[] = {
      {200000, 2, 3, true},
      {150000, 1, 1, false},
      {90000, 0, 9, true},
      {300000, 3, 6, true},
      {50000, 2, 19, false},
  };

  std::vector<uint8_t> expect;
  std::vector<uint8_t> stream;
  for (const Piece & p : pieces) {
    const std::vector<uint8_t> in = make_data(p.len, (uint32_t)p.len, p.shape);
    append(expect, in);
    append(stream, encode_frame(in, p.level, p.checksum, true));
  }

  std::vector<uint8_t> ref;
  ASSERT_EQ(decode_threads(stream, 1, ref, expect.size() + 1024), GCOMP_OK);
  ASSERT_EQ(ref.size(), expect.size());
  ASSERT_EQ(std::memcmp(ref.data(), expect.data(), expect.size()), 0);

  for (uint64_t t : k_thread_counts) {
    std::vector<uint8_t> got;
    ASSERT_EQ(decode_threads(stream, t, got, expect.size() + 1024), GCOMP_OK)
        << "threads " << t;
    ASSERT_EQ(got.size(), ref.size()) << "threads " << t;
    EXPECT_EQ(std::memcmp(got.data(), ref.data(), ref.size()), 0)
        << "threads " << t
        << ": same length, different bytes - frames written out of order?";
  }
}

/**
 * @brief Streams that cannot be split still decode.
 *
 * One frame is all our encoder emits, and a frame that does not declare its
 * size cannot be given a job buffer. Both must fall back, and falling back has
 * to mean correct output.
 */
TEST(ZstdParallelDecode, UnsplittableStreamsStillDecode) {
  const std::vector<uint8_t> a = make_data(200000, 11, 2);
  const std::vector<uint8_t> b = make_data(120000, 12, 2);

  struct Case {
    const char * why;
    std::vector<uint8_t> stream;
    std::vector<uint8_t> expect;
  };
  std::vector<Case> cases;

  // A single frame: nothing to divide.
  cases.push_back({"one frame", encode_frame(a, 3, true, true), a});

  // Several frames, none declaring its size.
  {
    std::vector<uint8_t> s, e;
    append(s, encode_frame(a, 3, true, false));
    append(s, encode_frame(b, 1, false, false));
    append(e, a);
    append(e, b);
    cases.push_back({"no declared size", s, e});
  }

  // Several frames where only some declare a size.
  {
    std::vector<uint8_t> s, e;
    append(s, encode_frame(a, 3, true, true));
    append(s, encode_frame(b, 1, false, false));
    append(e, a);
    append(e, b);
    cases.push_back({"mixed declared sizes", s, e});
  }

  for (const Case & c : cases) {
    for (uint64_t t : {(uint64_t)1, (uint64_t)4, (uint64_t)12}) {
      std::vector<uint8_t> got;
      ASSERT_EQ(decode_threads(c.stream, t, got, c.expect.size() + 1024),
          GCOMP_OK)
          << c.why << " at threads " << t;
      ASSERT_EQ(got.size(), c.expect.size()) << c.why << " at threads " << t;
      EXPECT_EQ(std::memcmp(got.data(), c.expect.data(), c.expect.size()), 0)
          << c.why << " at threads " << t;
    }
  }
}

/// Corruption is caught at every thread count, not just at one.
TEST(ZstdParallelDecode, CorruptionIsCaughtAtEveryThreadCount) {
  std::vector<uint8_t> expect;
  std::vector<uint8_t> clean;
  for (int i = 0; i < 4; i++) {
    const std::vector<uint8_t> in = make_data(120000, (uint32_t)(i + 1), 2);
    append(expect, in);
    append(clean, encode_frame(in, 3, true, true));
  }
  ASSERT_GT(clean.size(), 4096u);

  const size_t spots[] = {clean.size() / 5, clean.size() / 3, clean.size() / 2,
      2 * clean.size() / 3, clean.size() - 3};

  for (size_t spot : spots) {
    std::vector<uint8_t> bad = clean;
    bad[spot] ^= 0xFF;

    std::vector<uint8_t> ref;
    const gcomp_status_t ref_status =
        decode_threads(bad, 1, ref, expect.size() + 1024);

    for (uint64_t t : k_thread_counts) {
      std::vector<uint8_t> got;
      const gcomp_status_t st =
          decode_threads(bad, t, got, expect.size() + 1024);
      if (ref_status != GCOMP_OK) {
        EXPECT_NE(st, GCOMP_OK)
            << "byte " << spot << ": one thread refused this stream and " << t
            << " threads accepted it";
      }
      else {
        ASSERT_EQ(st, GCOMP_OK) << "byte " << spot << " threads " << t;
        ASSERT_EQ(got.size(), ref.size()) << "byte " << spot << " threads " << t;
        EXPECT_EQ(std::memcmp(got.data(), ref.data(), ref.size()), 0)
            << "byte " << spot << " threads " << t;
      }
    }
  }
}

/**
 * @brief A memory limit binds the same way however many threads there are.
 *
 * Not "it always succeeds": a Zstandard decoder needs a window, and a limit
 * below what one costs refuses the stream at one thread too. The first version
 * of this test asserted success and failed at a 4096-byte limit, which turned
 * out to be the limit working rather than the parallel path breaking - one
 * thread and four agreed exactly.
 *
 * What must hold is parity. `limits.max_memory_bytes` caps how many jobs are in
 * flight rather than adding a reason to fail, so a limit that one thread can
 * live with must not become an error just because threads.count went up.
 */
TEST(ZstdParallelDecode, AMemoryLimitBindsTheSameAtEveryThreadCount) {
  std::vector<uint8_t> expect;
  std::vector<uint8_t> stream;
  for (int i = 0; i < 4; i++) {
    const std::vector<uint8_t> in = make_data(150000, (uint32_t)(i + 7), 2);
    append(expect, in);
    append(stream, encode_frame(in, 3, true, true));
  }

  bool any_succeeded = false;
  for (uint64_t limit : {(uint64_t)4096, (uint64_t)150000, (uint64_t)(4u << 20),
           (uint64_t)(64u << 20)}) {
    std::vector<uint8_t> ref;
    gcomp_status_t ref_status = GCOMP_OK;
    {
      gcomp_options_t * o = nullptr;
      ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_memory_bytes", limit),
          GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 0),
          GCOMP_OK);
      ref.assign(expect.size() + 1024, 0);
      size_t produced = 0;
      ref_status = gcomp_decode_buffer(nullptr, "zstd", o, stream.data(),
          stream.size(), ref.data(), ref.size(), &produced);
      ref.resize(ref_status == GCOMP_OK ? produced : 0);
      gcomp_options_destroy(o);
    }
    if (ref_status == GCOMP_OK) {
      any_succeeded = true;
      ASSERT_EQ(ref.size(), expect.size()) << "memory limit " << limit;
    }

    for (uint64_t t : {(uint64_t)4, (uint64_t)12}) {
      gcomp_options_t * o = nullptr;
      ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_uint64(o, "threads.count", t), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_memory_bytes", limit),
          GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 0),
          GCOMP_OK);
      std::vector<uint8_t> out(expect.size() + 1024);
      size_t produced = 0;
      const gcomp_status_t st = gcomp_decode_buffer(nullptr, "zstd", o,
          stream.data(), stream.size(), out.data(), out.size(), &produced);
      gcomp_options_destroy(o);

      if (ref_status == GCOMP_OK) {
        EXPECT_EQ(st, GCOMP_OK)
            << "memory limit " << limit << " threads " << t
            << ": one thread lived with this limit and " << t << " did not";
        if (st == GCOMP_OK) {
          EXPECT_EQ(produced, expect.size()) << "memory limit " << limit;
          EXPECT_EQ(std::memcmp(out.data(), expect.data(), expect.size()), 0)
              << "memory limit " << limit << " threads " << t;
        }
      }
      else {
        EXPECT_NE(st, GCOMP_OK)
            << "memory limit " << limit << " threads " << t
            << ": one thread refused this limit and " << t << " accepted it";
      }
    }
  }

  // A limit no thread count could satisfy would make the loop above vacuous.
  EXPECT_TRUE(any_succeeded)
      << "every memory limit tried refused the stream, so nothing here "
      << "checked that threads and the limit can coexist";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
