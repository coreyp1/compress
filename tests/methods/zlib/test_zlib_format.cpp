/**
 * @file test_zlib_format.cpp
 *
 * Header and trailer tests for the zlib (RFC 1950) container.
 *
 * The container is six bytes of overhead and every one of them is specified,
 * so these check the bytes themselves rather than only that a round trip
 * works: a stream can round-trip through our own decoder while being wrong in
 * a way only somebody else's decoder would notice.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../common/test_helpers.h"
#include <algorithm>
#include <cstring>
#include <ghoti.io/compress/adler32.h>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zlib.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

class ZlibFormatTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  std::vector<uint8_t> Encode(const std::vector<uint8_t> & data,
      gcomp_options_t * opts, gcomp_status_t * status_out = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "zlib", opts, &encoder);
    if (status_out) {
      *status_out = status;
    }
    if (status != GCOMP_OK) {
      return {};
    }
    std::vector<uint8_t> out(data.size() + data.size() / 2 + 4096);
    gcomp_buffer_t in_buf = {
        const_cast<uint8_t *>(data.data()), data.size(), 0};
    gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
    while (in_buf.used < in_buf.size) {
      size_t bi = in_buf.used;
      size_t bo = out_buf.used;
      EXPECT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
      EXPECT_FALSE(in_buf.used == bi && out_buf.used == bo)
          << "update() made no progress";
      if (in_buf.used == bi && out_buf.used == bo) {
        break;
      }
    }
    for (;;) {
      gcomp_status_t s = gcomp_encoder_finish(encoder, &out_buf);
      if (s == GCOMP_OK) {
        break;
      }
      EXPECT_EQ(s, GCOMP_ERR_LIMIT);
      if (s != GCOMP_ERR_LIMIT) {
        break;
      }
    }
    out.resize(out_buf.used);
    gcomp_encoder_destroy(encoder);
    return out;
  }

  std::vector<uint8_t> Decode(const std::vector<uint8_t> & stream,
      size_t expected, gcomp_status_t * status_out = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "zlib", nullptr, &decoder);
    if (status != GCOMP_OK) {
      if (status_out) {
        *status_out = status;
      }
      return {};
    }
    std::vector<uint8_t> out(expected + 4096);
    gcomp_buffer_t in_buf = {
        const_cast<uint8_t *>(stream.data()), stream.size(), 0};
    gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
    gcomp_status_t s1 = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    gcomp_status_t s2 = (s1 == GCOMP_OK)
        ? gcomp_decoder_finish(decoder, &out_buf)
        : s1;
    if (status_out) {
      *status_out = (s1 != GCOMP_OK) ? s1 : s2;
    }
    out.resize(out_buf.used);
    gcomp_decoder_destroy(decoder);
    return out;
  }

  static std::vector<uint8_t> Text(size_t n) {
    std::vector<uint8_t> v(n);
    if (n == 0) {
      return v; // v.data() is null here, and pointer arithmetic on it is UB.
    }
    static const char * words[] = {"the ", "quick ", "brown ", "fox ",
        "jumps ", "over ", "lazy ", "dog "};
    unsigned s = 4242;
    size_t p = 0;
    while (p < n) {
      s = s * 1103515245u + 12345u;
      const char * w = words[(s >> 16) % 8];
      size_t l = strlen(w);
      if (p + l > n) {
        l = n - p;
      }
      memcpy(v.data() + p, w, l);
      p += l;
    }
    return v;
  }

  gcomp_registry_t * registry_ = nullptr;
};

/**
 * RFC 1950 section 2.2: CM must be 8, CINFO at most 7, and CMF*256+FLG a
 * multiple of 31.  FCHECK exists for no other purpose than that last one, and
 * it is the cheapest thing any reader checks first.
 */
TEST_F(ZlibFormatTest, TheHeaderIsWellFormedAtEveryLevelAndWindow) {
  const std::vector<uint8_t> data = Text(20000);

  for (int64_t level = 0; level <= 9; level++) {
    for (uint64_t wbits = 8; wbits <= 15; wbits++) {
      gcomp_options_t * opts = nullptr;
      ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", level),
          GCOMP_OK);
      ASSERT_EQ(
          gcomp_options_set_uint64(opts, "deflate.window_bits", wbits),
          GCOMP_OK);
      std::vector<uint8_t> stream = Encode(data, opts);
      gcomp_options_destroy(opts);

      ASSERT_GE(stream.size(), 6u) << "level=" << level << " wbits=" << wbits;
      unsigned cmf = stream[0];
      unsigned flg = stream[1];

      EXPECT_EQ(cmf & 0x0Fu, 8u) << "CM must be deflate";
      EXPECT_LE(cmf >> 4, 7u) << "CINFO above 7 is not allowed";
      EXPECT_EQ((cmf >> 4) + 8u, wbits) << "CINFO must describe the window";
      EXPECT_EQ(((cmf << 8) | flg) % 31u, 0u)
          << "FCHECK must make the pair a multiple of 31";
      EXPECT_EQ(flg & 0x20u, 0u) << "FDICT is not set without a dictionary";
    }
  }
}

/// FLEVEL is a hint, but a truthful one: these are zlib's own boundaries.
TEST_F(ZlibFormatTest, TheLevelHintFollowsZlibsOwnBoundaries) {
  const std::vector<uint8_t> data = Text(4000);
  struct {
    int64_t level;
    unsigned flevel;
  } cases[] = {{0, 0}, {1, 0}, {2, 1}, {5, 1}, {6, 2}, {7, 3}, {9, 3}};

  for (const auto & c : cases) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(
        gcomp_options_set_int64(opts, "deflate.level", c.level), GCOMP_OK);
    std::vector<uint8_t> stream = Encode(data, opts);
    gcomp_options_destroy(opts);
    ASSERT_GE(stream.size(), 2u);
    EXPECT_EQ((unsigned)(stream[1] >> 6), c.flevel) << "level=" << c.level;
  }
}

/// The trailer is the Adler-32 of the *uncompressed* data, most significant
/// byte first.  Both halves of that are easy to get backwards.
TEST_F(ZlibFormatTest, TheTrailerIsABigEndianAdler32OfTheInput) {
  const std::vector<uint8_t> data = Text(9000);
  std::vector<uint8_t> stream = Encode(data, nullptr);
  ASSERT_GE(stream.size(), 6u);

  uint32_t expected = gcomp_adler32(data.data(), data.size());
  const uint8_t * tail = stream.data() + stream.size() - 4;
  uint32_t stored = ((uint32_t)tail[0] << 24) | ((uint32_t)tail[1] << 16) |
      ((uint32_t)tail[2] << 8) | (uint32_t)tail[3];
  EXPECT_EQ(stored, expected);

  // And it is not the little-endian spelling, which would also "round trip"
  // against a decoder that made the same mistake.
  uint32_t reversed = ((uint32_t)tail[3] << 24) | ((uint32_t)tail[2] << 16) |
      ((uint32_t)tail[1] << 8) | (uint32_t)tail[0];
  if (expected != reversed) {
    EXPECT_NE(stored, reversed);
  }
}

/// An empty input is still a well-formed stream: header, an empty deflate
/// stream, and the checksum of nothing, which is 1.
TEST_F(ZlibFormatTest, AnEmptyInputMakesAWellFormedStream) {
  std::vector<uint8_t> stream = Encode({}, nullptr);
  ASSERT_GE(stream.size(), 6u);
  EXPECT_EQ(((unsigned)(stream[0] << 8) | stream[1]) % 31u, 0u);

  const uint8_t * tail = stream.data() + stream.size() - 4;
  uint32_t stored = ((uint32_t)tail[0] << 24) | ((uint32_t)tail[1] << 16) |
      ((uint32_t)tail[2] << 8) | (uint32_t)tail[3];
  EXPECT_EQ(stored, GCOMP_ADLER32_INIT);

  gcomp_status_t status = GCOMP_OK;
  EXPECT_TRUE(Decode(stream, 0, &status).empty());
  EXPECT_EQ(status, GCOMP_OK);
}

//
// Rejecting what should be rejected
//

TEST_F(ZlibFormatTest, ANonZlibHeaderIsRejected) {
  struct {
    const char * why;
    uint8_t cmf;
    uint8_t flg;
  } bad[] = {
      {"CM is not deflate", 0x79u, 0x01u},     // CM = 9
      {"CINFO above 7", 0x8Cu, 0x62u},         // CINFO = 8
      {"FCHECK does not check", 0x78u, 0x00u}, // 0x7800 % 31 != 0
      {"gzip magic", 0x1Fu, 0x8Bu},
  };

  for (const auto & b : bad) {
    std::vector<uint8_t> stream = {b.cmf, b.flg, 0x03, 0x00, 0x00, 0x00, 0x00,
        0x01};
    gcomp_status_t status = GCOMP_OK;
    Decode(stream, 16, &status);
    EXPECT_NE(status, GCOMP_OK) << b.why << " was accepted";
  }
}

/// A corrupted payload must be caught by the checksum, not waved through.
TEST_F(ZlibFormatTest, ACorruptedPayloadIsCaught) {
  const std::vector<uint8_t> data = Text(20000);
  std::vector<uint8_t> stream = Encode(data, nullptr);
  ASSERT_GT(stream.size(), 20u);

  int caught = 0;
  int tried = 0;
  for (size_t at = 2; at + 4 < stream.size(); at += stream.size() / 11) {
    std::vector<uint8_t> broken = stream;
    broken[at] ^= 0x55u;
    gcomp_status_t status = GCOMP_OK;
    std::vector<uint8_t> out = Decode(broken, data.size(), &status);
    tried++;
    if (status != GCOMP_OK || out != data) {
      caught++;
    }
  }
  EXPECT_EQ(caught, tried) << "a corrupted stream decoded cleanly";
}

/// Flipping a byte of the trailer alone must be caught: that is the one thing
/// only the Adler-32 can see.
TEST_F(ZlibFormatTest, ATamperedTrailerIsCaught) {
  const std::vector<uint8_t> data = Text(5000);
  std::vector<uint8_t> stream = Encode(data, nullptr);
  ASSERT_GE(stream.size(), 6u);

  for (size_t i = 0; i < 4; i++) {
    std::vector<uint8_t> broken = stream;
    broken[broken.size() - 1 - i] ^= 0x01u;
    gcomp_status_t status = GCOMP_OK;
    Decode(broken, data.size(), &status);
    EXPECT_EQ(status, GCOMP_ERR_CORRUPT)
        << "trailer byte " << i << " could be changed unnoticed";
  }
}

/// A stream cut short is truncation, not success.
TEST_F(ZlibFormatTest, ATruncatedStreamIsRejected) {
  const std::vector<uint8_t> data = Text(5000);
  std::vector<uint8_t> stream = Encode(data, nullptr);
  ASSERT_GT(stream.size(), 10u);

  const size_t cuts[] = {1, 2, 3, stream.size() / 2, stream.size() - 5,
      stream.size() - 1};
  for (size_t cut : cuts) {
    std::vector<uint8_t> partial(stream.begin(), stream.begin() + cut);
    gcomp_status_t status = GCOMP_OK;
    Decode(partial, data.size(), &status);
    EXPECT_NE(status, GCOMP_OK) << "a stream cut at " << cut << " was accepted";
  }
}

//
// Peeking
//

TEST_F(ZlibFormatTest, PeekReportsWhatTheHeaderSays) {
  const std::vector<uint8_t> data = Text(4000);
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 9), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "deflate.window_bits", 12),
      GCOMP_OK);
  std::vector<uint8_t> stream = Encode(data, opts);
  gcomp_options_destroy(opts);

  gcomp_zlib_header_info_t info;
  memset(&info, 0, sizeof(info));
  ASSERT_EQ(gcomp_zlib_peek_header(stream.data(), stream.size(), &info),
      GCOMP_OK);
  EXPECT_EQ(info.window_bits, 12u);
  EXPECT_EQ(info.level_hint, 3u);
  EXPECT_EQ(info.has_dictionary, 0u);
  EXPECT_EQ(info.header_size, 2u);
}

TEST_F(ZlibFormatTest, PeekNeedsTwoBytesAndRejectsRubbish) {
  gcomp_zlib_header_info_t info;
  const uint8_t good[] = {0x78u, 0x9Cu};
  EXPECT_EQ(gcomp_zlib_peek_header(good, 1, &info), GCOMP_ERR_LIMIT);
  EXPECT_EQ(gcomp_zlib_peek_header(good, 2, &info), GCOMP_OK);

  const uint8_t gzip_magic[] = {0x1Fu, 0x8Bu};
  EXPECT_EQ(gcomp_zlib_peek_header(gzip_magic, 2, &info), GCOMP_ERR_CORRUPT);

  EXPECT_EQ(gcomp_zlib_peek_header(nullptr, 2, &info), GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_zlib_peek_header(good, 2, nullptr), GCOMP_ERR_INVALID_ARG);
}

/// Peek must be able to tell a zlib stream from a raw deflate one, which is
/// most of what anyone wants it for.
TEST_F(ZlibFormatTest, PeekTellsZlibFromRawDeflate) {
  const std::vector<uint8_t> data = Text(4000);

  std::vector<uint8_t> wrapped = Encode(data, nullptr);
  gcomp_zlib_header_info_t info;
  EXPECT_EQ(gcomp_zlib_peek_header(wrapped.data(), wrapped.size(), &info),
      GCOMP_OK);

  gcomp_encoder_t * raw = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "deflate", nullptr, &raw), GCOMP_OK);
  std::vector<uint8_t> bare(data.size() + 4096);
  gcomp_buffer_t in_buf = {
      const_cast<uint8_t *>(data.data()), data.size(), 0};
  gcomp_buffer_t out_buf = {bare.data(), bare.size(), 0};
  while (in_buf.used < in_buf.size) {
    ASSERT_EQ(gcomp_encoder_update(raw, &in_buf, &out_buf), GCOMP_OK);
  }
  while (gcomp_encoder_finish(raw, &out_buf) == GCOMP_ERR_LIMIT) {
  }
  bare.resize(out_buf.used);
  gcomp_encoder_destroy(raw);

  EXPECT_NE(gcomp_zlib_peek_header(bare.data(), bare.size(), &info), GCOMP_OK)
      << "raw deflate was mistaken for a zlib stream";
}

//
// Preset dictionaries: the one part of RFC 1950 this does not do
//

TEST_F(ZlibFormatTest, APresetDictionaryIsRefusedRatherThanIgnored) {
  const uint8_t dict[] = {'h', 'e', 'l', 'l', 'o'};
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_bytes(opts, "zlib.dictionary", dict, sizeof(dict)),
      GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "zlib", opts, &encoder),
      GCOMP_ERR_UNSUPPORTED)
      << "silently dropping FDICT would produce a stream that decodes to the "
         "wrong bytes for whoever had the dictionary";
  gcomp_options_destroy(opts);
}

TEST_F(ZlibFormatTest, AStreamNeedingAPresetDictionaryIsRefused) {
  // CMF=0x78, FLG with FDICT set and FCHECK fixed up: 0x78BB is 30651, a
  // multiple of 31, and 0xBB has bit 5 set.
  std::vector<uint8_t> stream = {0x78u, 0xBBu, 0xDEu, 0xADu, 0xBEu, 0xEFu,
      0x03u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u};
  ASSERT_EQ(((unsigned)(stream[0] << 8) | stream[1]) % 31u, 0u)
      << "the test's own header is malformed";
  ASSERT_NE(stream[1] & 0x20u, 0u) << "the test's own header lacks FDICT";

  gcomp_status_t status = GCOMP_OK;
  Decode(stream, 64, &status);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED)
      << "guessing at a stream that needs a dictionary would produce "
         "plausible nonsense";
}


//
// Golden vectors
//
// Produced by the real zlib, checked in so that coverage of "can we read what
// zlib writes" does not depend on Python being installed.  The oracle tests
// next door do the same job dynamically and over far more data; these are the
// floor.
//

TEST_F(ZlibFormatTest, DecodesStreamsTheRealZlibProduced) {
  struct Vector {
    const char * name;
    std::vector<uint8_t> stream;
    std::string plain;
  };
  const Vector vectors[] = {
      {"empty (level 6)", {0x78, 0x9C, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01},
          ""},
      {"one byte (level 6)",
          {0x78, 0x9C, 0x73, 0x04, 0x00, 0x00, 0x42, 0x00, 0x42}, "A"},
      {"hello (level 6)",
          {0x78, 0x9C, 0xCB, 0x48, 0xCD, 0xC9, 0xC9, 0xD7, 0x51, 0x28, 0xCF,
              0x2F, 0xCA, 0x49, 0x01, 0x00, 0x1D, 0x54, 0x04, 0x89},
          "hello, world"},
      {"a repeated match (level 6)",
          {0x78, 0x9C, 0x4B, 0x4C, 0x4A, 0x4E, 0x24, 0x05, 0x01, 0x00, 0xC2,
              0x4F, 0x12, 0x61},
          "abcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabc"},
      {"pangram (level 1)",
          {0x78, 0x01, 0x2B, 0xC9, 0x48, 0x55, 0x28, 0x2C, 0xCD, 0x4C, 0xCE,
              0x56, 0x48, 0x2A, 0xCA, 0x2F, 0xCF, 0x53, 0x48, 0xCB, 0xAF, 0x50,
              0xC8, 0x2A, 0xCD, 0x2D, 0x28, 0x56, 0xC8, 0x2F, 0x4B, 0x2D, 0x52,
              0x28, 0x01, 0x4A, 0xE7, 0x24, 0x56, 0x55, 0x2A, 0xA4, 0xE4, 0xA7,
              0x03, 0x00, 0x61, 0x3C, 0x0F, 0xFA},
          "the quick brown fox jumps over the lazy dog"},
  };

  for (const auto & v : vectors) {
    gcomp_status_t status = GCOMP_OK;
    std::vector<uint8_t> out = Decode(v.stream, v.plain.size(), &status);
    EXPECT_EQ(status, GCOMP_OK) << v.name;
    EXPECT_EQ(std::string(out.begin(), out.end()), v.plain) << v.name;
  }
}

/// And the streams we produce for the same inputs are read back identically,
/// so the encoder and decoder agree with zlib rather than only with each
/// other.
TEST_F(ZlibFormatTest, OurStreamsForThoseInputsAgreeToo) {
  const char * inputs[] = {"", "A", "hello, world",
      "abcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabc",
      "the quick brown fox jumps over the lazy dog"};
  for (const char * text : inputs) {
    std::vector<uint8_t> data(text, text + strlen(text));
    std::vector<uint8_t> stream = Encode(data, nullptr);
    gcomp_status_t status = GCOMP_OK;
    EXPECT_EQ(Decode(stream, data.size(), &status), data) << "input \"" << text
                                                          << "\"";
    EXPECT_EQ(status, GCOMP_OK);
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
