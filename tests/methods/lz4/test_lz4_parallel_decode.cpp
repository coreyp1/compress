/**
 * @file test_lz4_parallel_decode.cpp
 *
 * `threads.count` on decode must be a speed setting and nothing else. So the
 * question every test here asks is the same one: are the bytes, and the
 * errors, exactly what one thread produces?
 *
 * The three ways this could go wrong are all covered:
 *
 * 1. **Different bytes.** Blocks finish out of order and must be written in
 *    order; getting that wrong on a stream of similar-looking blocks produces
 *    output of the right length that is subtly scrambled.
 * 2. **Fewer checks.** A path that skips a block checksum, a content checksum
 *    or an output limit is faster and wrong, and on valid input it looks
 *    identical. Each is corrupted deliberately below and must be caught at
 *    every thread count.
 * 3. **Splitting what cannot be split.** Linked blocks reference earlier ones
 *    and a dictionary is history outside the stream. Both must be declined and
 *    decoded the ordinary way - and "declined" has to mean correct output, not
 *    an error.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lz4.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
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
    case 1: v[i] = (uint8_t)(s >> 24); break;            // incompressible
    case 2: v[i] = (uint8_t)('a' + ((s >> 28) & 0x0F)); break;
    default: v[i] = (uint8_t)((i / 997) & 0xFF); break;  // long runs
    }
  }
  return v;
}

struct FrameOpts {
  bool block_checksum;
  bool content_checksum;
  bool independent;
  uint64_t block_size;
  uint64_t dictionary_id; ///< 0 for none
};

std::vector<uint8_t> encode(
    const std::vector<uint8_t> & in, const FrameOpts & e) {
  gcomp_options_t * o = nullptr;
  EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
  EXPECT_EQ(
      gcomp_options_set_bool(o, "lz4.block_checksum", e.block_checksum ? 1 : 0),
      GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_bool(
                o, "lz4.content_checksum", e.content_checksum ? 1 : 0),
      GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_bool(
                o, "lz4.independent_blocks", e.independent ? 1 : 0),
      GCOMP_OK);
  if (e.block_size) {
    EXPECT_EQ(gcomp_options_set_uint64(o, "lz4.block_size", e.block_size),
        GCOMP_OK);
  }
  if (e.dictionary_id) {
    EXPECT_EQ(
        gcomp_options_set_uint64(o, "lz4.dictionary_id", e.dictionary_id),
        GCOMP_OK);
  }
  size_t bound = 0;
  EXPECT_EQ(gcomp_encode_bound(nullptr, "lz4", o, in.size(), &bound), GCOMP_OK);
  std::vector<uint8_t> out(bound ? bound : 1);
  size_t w = 0;
  EXPECT_EQ(gcomp_encode_buffer(nullptr, "lz4", o, in.data(), in.size(),
                out.data(), out.size(), &w),
      GCOMP_OK);
  gcomp_options_destroy(o);
  out.resize(w);
  return out;
}

/// Decode with a given thread count; returns the status and fills @p out.
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
      nullptr, "lz4", o, s.data(), s.size(), out.data(), out.size(), &produced);
  gcomp_options_destroy(o);
  out.resize(st == GCOMP_OK ? produced : 0);
  return st;
}

const uint64_t k_thread_counts[] = {2, 3, 4, 12};

} // namespace

/**
 * @brief Threads change nothing about the bytes.
 *
 * Across frame shapes that exercise every path in the parallel decoder: stored
 * blocks (incompressible input), tiny blocks, block checksums, content
 * checksums, and enough blocks that jobs really do finish out of order.
 */
TEST(Lz4ParallelDecode, ThreadsProduceIdenticalBytes) {
  struct Case {
    size_t len;
    int shape;
    FrameOpts opts;
  };
  const Case cases[] = {
      {1u << 21, 2, {false, false, true, 65536, 0}},
      {1u << 21, 2, {true, true, true, 65536, 0}},
      {1u << 21, 1, {true, true, true, 65536, 0}},  // stored blocks
      {1u << 21, 3, {false, true, true, 65536, 0}}, // long runs
      {1u << 20, 2, {true, false, true, 262144, 0}},
      {300000, 0, {true, true, true, 65536, 0}},    // all zeros
  };

  for (const Case & c : cases) {
    const std::vector<uint8_t> in = make_data(c.len, 4242, c.shape);
    const std::vector<uint8_t> s = encode(in, c.opts);

    std::vector<uint8_t> ref;
    ASSERT_EQ(decode_threads(s, 1, ref, c.len + 1024), GCOMP_OK)
        << "len " << c.len << " shape " << c.shape;
    ASSERT_EQ(ref.size(), in.size());
    ASSERT_EQ(std::memcmp(ref.data(), in.data(), in.size()), 0);

    for (uint64_t t : k_thread_counts) {
      std::vector<uint8_t> got;
      ASSERT_EQ(decode_threads(s, t, got, c.len + 1024), GCOMP_OK)
          << "len " << c.len << " shape " << c.shape << " threads " << t;
      ASSERT_EQ(got.size(), ref.size())
          << "len " << c.len << " shape " << c.shape << " threads " << t;
      EXPECT_EQ(std::memcmp(got.data(), ref.data(), ref.size()), 0)
          << "len " << c.len << " shape " << c.shape << " threads " << t
          << ": same length, different bytes - blocks written out of order?";
    }
  }
}

/**
 * @brief A stream that cannot be split is decoded correctly anyway.
 *
 * Declining has to mean "the ordinary decoder handles it", not an error. These
 * are the three reasons to decline, and each must still produce the right
 * bytes at every thread count.
 */
TEST(Lz4ParallelDecode, UnsplittableStreamsStillDecode) {
  struct Case {
    const char * why;
    size_t len;
    FrameOpts opts;
  };
  const Case cases[] = {
      {"linked blocks", 1u << 20, {false, true, false, 65536, 0}},
      {"one block", 1000, {true, true, true, 65536, 0}},
      {"dictionary id", 1u << 20, {false, false, true, 65536, 0xABCDEF01u}},
  };

  for (const Case & c : cases) {
    const std::vector<uint8_t> in = make_data(c.len, 31337, 2);
    const std::vector<uint8_t> s = encode(in, c.opts);

    for (uint64_t t : {(uint64_t)1, (uint64_t)4, (uint64_t)12}) {
      std::vector<uint8_t> got;
      ASSERT_EQ(decode_threads(s, t, got, c.len + 1024), GCOMP_OK)
          << c.why << " at threads " << t;
      ASSERT_EQ(got.size(), in.size()) << c.why << " at threads " << t;
      EXPECT_EQ(std::memcmp(got.data(), in.data(), in.size()), 0)
          << c.why << " at threads " << t;
    }
  }
}

/**
 * @brief Corruption is caught at every thread count, not just at one.
 *
 * The failure worth guarding against is a parallel path that is faster because
 * it checks less. On valid input that is invisible, so each check is broken
 * deliberately and the parallel result must match the single-threaded one.
 */
TEST(Lz4ParallelDecode, CorruptionIsCaughtAtEveryThreadCount) {
  const std::vector<uint8_t> in = make_data(1u << 20, 777, 2);
  const std::vector<uint8_t> clean = encode(in, {true, true, true, 65536, 0});
  ASSERT_GT(clean.size(), 4096u);

  // Several places: inside a block's payload, and the frame's trailing content
  // checksum.
  const size_t spots[] = {clean.size() / 3, clean.size() / 2,
      2 * clean.size() / 3, clean.size() - 2};

  for (size_t spot : spots) {
    std::vector<uint8_t> bad = clean;
    bad[spot] ^= 0xFF;

    std::vector<uint8_t> ref;
    const gcomp_status_t ref_status = decode_threads(bad, 1, ref, in.size() + 1024);

    for (uint64_t t : k_thread_counts) {
      std::vector<uint8_t> got;
      const gcomp_status_t st = decode_threads(bad, t, got, in.size() + 1024);
      if (ref_status != GCOMP_OK) {
        EXPECT_NE(st, GCOMP_OK)
            << "byte " << spot << " flipped: one thread refused this stream "
            << "and " << t << " threads accepted it";
      }
      else {
        // The flip landed somewhere that does not change the decode; then the
        // bytes still have to agree.
        ASSERT_EQ(st, GCOMP_OK) << "byte " << spot << " threads " << t;
        ASSERT_EQ(got.size(), ref.size()) << "byte " << spot << " threads " << t;
        EXPECT_EQ(std::memcmp(got.data(), ref.data(), ref.size()), 0)
            << "byte " << spot << " threads " << t;
      }
    }
  }
}

/// An output buffer too small fails the same way however many threads there are.
TEST(Lz4ParallelDecode, ASmallOutputBufferFailsTheSameWay) {
  const std::vector<uint8_t> in = make_data(1u << 20, 5150, 2);
  const std::vector<uint8_t> s = encode(in, {false, true, true, 65536, 0});

  for (size_t cap : {(size_t)1, (size_t)1000, in.size() / 2, in.size() - 1}) {
    std::vector<uint8_t> ref;
    const gcomp_status_t ref_status = decode_threads(s, 1, ref, cap);
    EXPECT_NE(ref_status, GCOMP_OK) << "capacity " << cap;

    for (uint64_t t : k_thread_counts) {
      std::vector<uint8_t> got;
      const gcomp_status_t st = decode_threads(s, t, got, cap);
      EXPECT_NE(st, GCOMP_OK)
          << "capacity " << cap << " threads " << t
          << ": too small for one thread but accepted by " << t;
    }
  }
}

/// `limits.max_output_bytes` binds on the parallel path too.
TEST(Lz4ParallelDecode, TheOutputLimitIsEnforced) {
  const std::vector<uint8_t> in = make_data(1u << 20, 606, 2);
  const std::vector<uint8_t> s = encode(in, {false, false, true, 65536, 0});

  for (uint64_t t : {(uint64_t)1, (uint64_t)4, (uint64_t)12}) {
    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    if (t > 1) {
      ASSERT_EQ(gcomp_options_set_uint64(o, "threads.count", t), GCOMP_OK);
    }
    ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_output_bytes", 4096),
        GCOMP_OK);
    std::vector<uint8_t> out(in.size() + 1024);
    size_t produced = 0;
    EXPECT_EQ(gcomp_decode_buffer(nullptr, "lz4", o, s.data(), s.size(),
                  out.data(), out.size(), &produced),
        GCOMP_ERR_LIMIT)
        << "threads " << t << ": a 4096-byte output limit did not bind";
    gcomp_options_destroy(o);
  }
}

/**
 * @brief A tight memory limit slows the decode down; it does not fail it.
 *
 * `limits.max_memory_bytes` caps how many jobs may be in flight. Returning
 * GCOMP_ERR_LIMIT for a stream that decodes fine at one thread would make
 * `threads.count` a correctness setting rather than a speed one, which is the
 * thing this whole path must not become.
 */
TEST(Lz4ParallelDecode, ATightMemoryLimitStillDecodes) {
  const std::vector<uint8_t> in = make_data(1u << 21, 8080, 2);
  const std::vector<uint8_t> s = encode(in, {true, true, true, 65536, 0});

  // Less than one job buffer, exactly one, and a couple.
  for (uint64_t limit : {(uint64_t)1024, (uint64_t)65536, (uint64_t)200000}) {
    for (uint64_t t : {(uint64_t)4, (uint64_t)12}) {
      gcomp_options_t * o = nullptr;
      ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_uint64(o, "threads.count", t), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_memory_bytes", limit),
          GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 0),
          GCOMP_OK);
      std::vector<uint8_t> out(in.size() + 1024);
      size_t produced = 0;
      EXPECT_EQ(gcomp_decode_buffer(nullptr, "lz4", o, s.data(), s.size(),
                    out.data(), out.size(), &produced),
          GCOMP_OK)
          << "memory limit " << limit << " threads " << t
          << ": a memory limit refused a stream that decodes at one thread";
      EXPECT_EQ(produced, in.size()) << "memory limit " << limit;
      if (produced == in.size()) {
        EXPECT_EQ(std::memcmp(out.data(), in.data(), in.size()), 0)
            << "memory limit " << limit << " threads " << t;
      }
      gcomp_options_destroy(o);
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
