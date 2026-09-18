/**
 * @file test_stepdowns.cpp
 *
 * Nothing an encoder emits should be weaker than it meant to emit, unless it
 * chose that.
 *
 * Every encoder here can produce a stored block, a fixed Huffman code, or
 * stored literals in place of something better.  Sometimes that is the right
 * answer and sometimes it is a failure - a code refused, memory missing,
 * output space short - and both arrive at the same line of code.  Three
 * defects in this library have hidden in that ambiguity, each one making an
 * encoder fail on every block of some file while looking exactly like an
 * encoder deciding it could not do better, and each one passing the entire
 * suite.  See src/core/stepdown.h.
 *
 * These tests compress a spread of shapes and ask that the forced count stay
 * at zero.  They say nothing about the chosen count: choosing the stored form
 * when it is smaller is the encoder working.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "../../src/core/stepdown.h"
#include "../../src/methods/deflate/deflate_internal.h"
#include "../../src/methods/lz4/lz4_internal.h"
#include "../../src/methods/zstd/zstd_internal.h"
#include <cstdint>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

// The shapes that have caught something before.  Each one is named, because a
// failure wants to say which kind of data provoked it.
struct NamedInput {
  const char * name;
  std::vector<uint8_t> data;
};

std::vector<uint8_t> Repeat(const char * const * parts, size_t count,
    size_t n, uint32_t seed) {
  std::vector<uint8_t> v;
  v.reserve(n + 32);
  while (v.size() < n) {
    seed = seed * 1103515245u + 12345u;
    const char * p = parts[(seed >> 16) % count];
    for (const char * c = p; *c; c++) {
      v.push_back((uint8_t)*c);
    }
  }
  v.resize(n);
  return v;
}

std::vector<NamedInput> Shapes() {
  std::vector<NamedInput> shapes;
  const size_t kSize = 400u * 1024u;

  // Text.  The ordinary case.
  static const char * words[] = {"the ", "quick ", "brown ", "fox ", "jumps ",
      "over ", "lazy ", "dog ", "and ", "then ", "with ", "a ", "small "};
  shapes.push_back({"text", Repeat(words, 13, kSize, 7u)});

  // Repetitive enough that after the first block there is nothing left to
  // emit as a literal.  This is the shape that sent six of eight Zstandard
  // blocks to stored blocks.
  //
  // It has to stay this side of a decompression bomb: one phrase repeated
  // would compress by a factor of seven thousand, and the decoder's default
  // expansion limit of 1000 (GCOMP_DEFAULT_MAX_EXPANSION_RATIO) refuses that
  // - correctly, and it would be the limit under test rather than the
  // encoder.  Drawing from a handful of phrases keeps the ratio realistic
  // while still leaving later blocks nothing new to say.
  static const char * phrases[] = {"alpha-", "bravo-", "charlie-", "delta-",
      "echo-", "foxtrot-", "golf-", "hotel-"};
  shapes.push_back({"highly repetitive", Repeat(phrases, 8, kSize, 3u)});

  // Structured text with no run of identical bytes, so distance code 0 is
  // never used.  This is the shape that sent every DEFLATE block of an XML
  // registry to a fixed-Huffman block.
  {
    std::vector<uint8_t> v;
    v.reserve(kSize + 64);
    uint32_t seed = 99u;
    while (v.size() < kSize) {
      seed = seed * 1103515245u + 12345u;
      char buf[64];
      int n = snprintf(buf, sizeof(buf), "<entry id=\"%u\" kind=\"phrase\"/>",
          (seed >> 16) % 500u);
      for (int i = 0; i < n; i++) {
        if (!v.empty() && v.back() == (uint8_t)buf[i]) {
          v.push_back((uint8_t)(buf[i] ^ 0x20));
        }
        v.push_back((uint8_t)buf[i]);
      }
    }
    v.resize(kSize);
    shapes.push_back({"no distance-one matches", std::move(v)});
  }

  // Bytes from a skewed alphabet, which is what makes a Huffman length cap
  // bind, and the shape that sent every match-less block to a stored block.
  {
    std::vector<uint8_t> v;
    v.reserve(kSize);
    uint32_t x = 4242u;
    while (v.size() < kSize) {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      uint32_t r = x >> 8;
      unsigned symbol = 0;
      while (symbol < 120 && (r & 1u)) {
        symbol++;
        r >>= 1;
      }
      v.push_back((uint8_t)symbol);
    }
    shapes.push_back({"skewed alphabet", std::move(v)});
  }

  // Incompressible.  Stored blocks are the right answer here, which is why
  // the assertion is on the forced count and not on the total.
  {
    std::vector<uint8_t> v;
    v.reserve(kSize);
    uint32_t x = 88675123u;
    while (v.size() < kSize) {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      v.push_back((uint8_t)(x >> 19));
    }
    shapes.push_back({"incompressible", std::move(v)});
  }

  // Runs, where distance-one matches are everything.
  {
    std::vector<uint8_t> v;
    v.reserve(kSize);
    uint32_t seed = 555u;
    while (v.size() < kSize) {
      seed = seed * 1103515245u + 12345u;
      size_t run = ((seed >> 16) % 40u) + 1u;
      uint8_t value = (uint8_t)((seed >> 8) % 11u);
      for (size_t i = 0; i < run && v.size() < kSize; i++) {
        v.push_back(value);
      }
    }
    shapes.push_back({"runs", std::move(v)});
  }

  return shapes;
}

std::string Describe(const gcomp_stepdown_tally_t & tally) {
  std::string out;
  for (int i = 0; i < GCOMP_STEPDOWN_COUNT; i++) {
    if (tally.counts[i] > 0) {
      out += std::string(gcomp_stepdown_name((gcomp_stepdown_t)i)) + "=" +
          std::to_string((unsigned long long)tally.counts[i]) + " ";
    }
  }
  return out.empty() ? "none" : out;
}

} // namespace

class StepdownTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }
  gcomp_registry_t * registry_ = nullptr;
};

TEST_F(StepdownTest, DeflateIsNeverForcedIntoAWeakerEncoding) {
  for (const NamedInput & shape : Shapes()) {
    for (int level = 1; level <= 9; level++) {
      gcomp_options_t * options = nullptr;
      ASSERT_EQ(gcomp_options_create(&options), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_int64(options, "deflate.level", level),
          GCOMP_OK);

      gcomp_encoder_t * encoder = nullptr;
      ASSERT_EQ(
          gcomp_encoder_create(registry_, "deflate", options, &encoder),
          GCOMP_OK);

      std::vector<uint8_t> out(shape.data.size() * 2 + 4096);
      gcomp_buffer_t in_buf = {
          (void *)shape.data.data(), shape.data.size(), 0};
      gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
      while (in_buf.used < in_buf.size) {
        ASSERT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
      }
      ASSERT_EQ(gcomp_encoder_finish(encoder, &out_buf), GCOMP_OK);

      const gcomp_stepdown_tally_t * tally =
          gcomp_deflate_encoder_stepdowns(encoder);
      ASSERT_NE(tally, nullptr);
      EXPECT_EQ(gcomp_stepdown_forced_total(tally), 0u)
          << shape.name << " at level " << level << ": " << Describe(*tally);

      // And what it produced still has to read back, so that a clean tally
      // cannot be bought by not encoding anything.
      std::vector<uint8_t> back(shape.data.size() + 64);
      size_t back_used = 0;
      EXPECT_EQ(gcomp_decode_buffer(registry_, "deflate", nullptr, out.data(),
                    out_buf.used, back.data(), back.size(), &back_used),
          GCOMP_OK);
      EXPECT_EQ(back_used, shape.data.size());
      EXPECT_EQ(memcmp(back.data(), shape.data.data(), shape.data.size()), 0);

      gcomp_encoder_destroy(encoder);
      gcomp_options_destroy(options);
    }
  }
}

// The counter has to be able to say yes, or a test that asks it can only ever
// pass.  A fixed-Huffman block is the one step-down that can be provoked from
// outside: the strategy asks for it by name.
TEST_F(StepdownTest, ChoosingTheFixedCodeIsCountedAsChosen) {
  std::vector<uint8_t> data = Shapes()[0].data;

  gcomp_options_t * options = nullptr;
  ASSERT_EQ(gcomp_options_create(&options), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(options, "deflate.level", 6), GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "deflate", options, &encoder),
      GCOMP_OK);

  // A short, near-uniform block: the table costs more than it saves, so the
  // encoder should price the fixed code as the smaller one and say so.
  std::vector<uint8_t> tiny;
  for (int i = 0; i < 300; i++) {
    tiny.push_back((uint8_t)(i * 7));
  }
  std::vector<uint8_t> out(4096);
  gcomp_buffer_t in_buf = {(void *)tiny.data(), tiny.size(), 0};
  gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
  ASSERT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
  ASSERT_EQ(gcomp_encoder_finish(encoder, &out_buf), GCOMP_OK);

  const gcomp_stepdown_tally_t * tally =
      gcomp_deflate_encoder_stepdowns(encoder);
  ASSERT_NE(tally, nullptr);
  EXPECT_EQ(gcomp_stepdown_forced_total(tally), 0u) << Describe(*tally);
  EXPECT_GT(tally->counts[GCOMP_STEPDOWN_FIXED_IS_SMALLER], 0u)
      << "nothing was counted: " << Describe(*tally);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(options);
}

namespace {

// One pass of a method over one input, returning what it settled for.
void EncodeAndCheck(gcomp_registry_t * registry, const char * method,
    gcomp_options_t * options, const std::vector<uint8_t> & data,
    const char * label,
    const gcomp_stepdown_tally_t * (*read)(const gcomp_encoder_t *)) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry, method, options, &encoder),
      GCOMP_OK);

  std::vector<uint8_t> out(data.size() * 2 + 65536);
  gcomp_buffer_t in_buf = {(void *)data.data(), data.size(), 0};
  gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
  while (in_buf.used < in_buf.size) {
    ASSERT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
  }
  ASSERT_EQ(gcomp_encoder_finish(encoder, &out_buf), GCOMP_OK);

  const gcomp_stepdown_tally_t * tally = read(encoder);
  ASSERT_NE(tally, nullptr);
  EXPECT_EQ(gcomp_stepdown_forced_total(tally), 0u)
      << method << " on " << label << ": " << Describe(*tally);

  std::vector<uint8_t> back(data.size() + 64);
  size_t back_used = 0;
  EXPECT_EQ(gcomp_decode_buffer(registry, method, nullptr, out.data(),
                out_buf.used, back.data(), back.size(), &back_used),
      GCOMP_OK)
      << method << " on " << label;
  EXPECT_EQ(back_used, data.size());
  EXPECT_EQ(memcmp(back.data(), data.data(), data.size()), 0);

  gcomp_encoder_destroy(encoder);
}

} // namespace

TEST_F(StepdownTest, ZstdIsNeverForcedIntoAWeakerEncoding) {
  for (const NamedInput & shape : Shapes()) {
    for (int level : {1, 3, 9}) {
      gcomp_options_t * options = nullptr;
      ASSERT_EQ(gcomp_options_create(&options), GCOMP_OK);
      ASSERT_EQ(
          gcomp_options_set_int64(options, "zstd.level", level), GCOMP_OK);
      std::string label = std::string(shape.name) + " at level " +
          std::to_string(level);
      EncodeAndCheck(registry_, "zstd", options, shape.data, label.c_str(),
          gcomp_zstd_encoder_stepdowns);
      gcomp_options_destroy(options);
    }
  }
}

TEST_F(StepdownTest, Lz4IsNeverForcedIntoAWeakerEncoding) {
  for (const NamedInput & shape : Shapes()) {
    for (uint64_t block : {65536u, 4194304u}) {
      for (int linked = 0; linked <= 1; linked++) {
        gcomp_options_t * options = nullptr;
        ASSERT_EQ(gcomp_options_create(&options), GCOMP_OK);
        ASSERT_EQ(gcomp_options_set_uint64(options, "lz4.block_size", block),
            GCOMP_OK);
        ASSERT_EQ(gcomp_options_set_bool(options, "lz4.independent_blocks",
                      linked ? false : true),
            GCOMP_OK);
        std::string label = std::string(shape.name) + " with " +
            std::to_string((unsigned long long)block) + " byte " +
            (linked ? "linked" : "independent") + " blocks";
        EncodeAndCheck(registry_, "lz4", options, shape.data, label.c_str(),
            gcomp_lz4_encoder_stepdowns);
        gcomp_options_destroy(options);
      }
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
