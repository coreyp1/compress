/**
 * @file test_bounded_output.cpp
 *
 * Streaming an entire input through a small, fixed output buffer, draining
 * between calls. This is the contract examples/gzip_file.c relies on and the
 * headline feature of the library, but nothing exercised it: the existing
 * small-buffer tests use inputs of a few dozen bytes, too short to fill a
 * block, so no method ever had to pause mid-stream and resume.
 *
 * Every method was broken in some way when it did:
 *   - deflate/gzip consumed input, produced nothing and returned
 *     GCOMP_ERR_LIMIT, having already swallowed data it could not re-emit
 *   - lz4 and zstd returned GCOMP_OK from finish() while output remained,
 *     so callers stopped early and silently truncated the stream
 *   - lzw abandoned a step mid-way and re-emitted the same code on resume
 *   - rle made no progress at all below 129 bytes, without saying so
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

/** Word-shaped data: compressible enough to produce real blocks. */
std::vector<uint8_t> make_text(size_t target) {
  static const char * words[] = {"alpha", "beta", "gamma", "delta", "epsilon",
      "zeta", "eta", "theta", "iota", "kappa", "lambda", "mu", "nu", "xi"};
  std::vector<uint8_t> v;
  v.reserve(target + 16);
  unsigned seed = 1234;
  while (v.size() < target) {
    seed = seed * 1103515245u + 12345u;
    const char * w = words[(seed >> 16) % (sizeof(words) / sizeof(*words))];
    while (*w) {
      v.push_back((uint8_t)*w++);
    }
    v.push_back((uint8_t)' ');
  }
  v.resize(target);
  return v;
}

/**
 * Encode through an output buffer of exactly @p chunk bytes, draining after
 * every call, then decode and compare.
 */
void roundtrip_bounded(
    const char * method, const std::vector<uint8_t> & input, size_t chunk) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(gcomp_registry_default(), method, nullptr, &enc),
      GCOMP_OK)
      << method;

  std::vector<uint8_t> out_buf(chunk);
  std::vector<uint8_t> encoded;
  gcomp_buffer_t in = {const_cast<uint8_t *>(input.data()), input.size(), 0};

  size_t guard = 0;
  const size_t guard_max = (input.size() / chunk + 64) * 16 + 4096;
  while (in.used < in.size) {
    ASSERT_LT(++guard, guard_max) << method << ": update made no progress";
    gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
    size_t before = in.used;
    ASSERT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK) << method;
    encoded.insert(encoded.end(), out_buf.data(), out_buf.data() + ob.used);
    ASSERT_TRUE(ob.used > 0 || in.used > before)
        << method << ": update neither consumed nor produced";
  }

  for (;;) {
    ASSERT_LT(++guard, guard_max) << method << ": finish made no progress";
    gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
    gcomp_status_t s = gcomp_encoder_finish(enc, &ob);
    encoded.insert(encoded.end(), out_buf.data(), out_buf.data() + ob.used);
    if (s == GCOMP_OK) {
      break;
    }
    ASSERT_EQ(s, GCOMP_ERR_LIMIT) << method;
    ASSERT_GT(ob.used, 0u) << method << ": finish made no progress";
  }
  gcomp_encoder_destroy(enc);

  std::vector<uint8_t> decoded(input.size() + 4096);
  size_t decoded_len = 0;
  ASSERT_EQ(gcomp_decode_buffer(nullptr, method, nullptr, encoded.data(),
                encoded.size(), decoded.data(), decoded.size(), &decoded_len),
      GCOMP_OK)
      << method << " chunk=" << chunk;
  ASSERT_EQ(decoded_len, input.size()) << method << " chunk=" << chunk;
  EXPECT_EQ(memcmp(decoded.data(), input.data(), input.size()), 0)
      << method << " chunk=" << chunk;
}

/** Smallest output buffer each method can make progress with. */
struct MethodCase {
  const char * name;
  size_t min_chunk;
};

const MethodCase kMethods[] = {
    {"deflate", 64},
    {"gzip", 64},
    {"lz4", 64},
    {"zstd", 64},
    {"lzw", 64},
    // A PackBits literal block is a length byte plus up to 128 bytes, so rle
    // needs 129 bytes of free output to emit one.
    {"rle", 129},
};

} // namespace

TEST(BoundedOutputTest, StreamsThroughSmallBuffers) {
  std::vector<uint8_t> input = make_text(200000);
  for (const MethodCase & m : kMethods) {
    for (size_t chunk : {(size_t)512, (size_t)4096, (size_t)65536}) {
      roundtrip_bounded(m.name, input, chunk);
      if (::testing::Test::HasFatalFailure()) {
        return;
      }
    }
  }
}

TEST(BoundedOutputTest, StreamsThroughMinimumBuffer) {
  std::vector<uint8_t> input = make_text(40000);
  for (const MethodCase & m : kMethods) {
    roundtrip_bounded(m.name, input, m.min_chunk);
    if (::testing::Test::HasFatalFailure()) {
      return;
    }
  }
}

// Output must depend only on the data, never on how the caller chunks its
// output buffer.
TEST(BoundedOutputTest, OutputIsIndependentOfBufferSize) {
  std::vector<uint8_t> input = make_text(120000);

  for (const MethodCase & m : kMethods) {
    std::vector<std::vector<uint8_t>> streams;
    for (size_t chunk : {(size_t)256, (size_t)8192, (size_t)1u << 20}) {
      if (chunk < m.min_chunk) {
        continue;
      }
      gcomp_encoder_t * enc = nullptr;
      ASSERT_EQ(gcomp_encoder_create(
                    gcomp_registry_default(), m.name, nullptr, &enc),
          GCOMP_OK);
      std::vector<uint8_t> out_buf(chunk), encoded;
      gcomp_buffer_t in = {
          const_cast<uint8_t *>(input.data()), input.size(), 0};
      while (in.used < in.size) {
        gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
        ASSERT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
        encoded.insert(encoded.end(), out_buf.data(), out_buf.data() + ob.used);
      }
      for (;;) {
        gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
        gcomp_status_t s = gcomp_encoder_finish(enc, &ob);
        encoded.insert(encoded.end(), out_buf.data(), out_buf.data() + ob.used);
        if (s == GCOMP_OK) {
          break;
        }
        ASSERT_EQ(s, GCOMP_ERR_LIMIT);
      }
      gcomp_encoder_destroy(enc);
      streams.push_back(std::move(encoded));
    }
    for (size_t i = 1; i < streams.size(); i++) {
      EXPECT_EQ(streams[0], streams[i])
          << m.name << ": output changed with the caller's buffer size";
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
