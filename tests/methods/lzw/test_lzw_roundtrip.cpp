/**
 * @file test_lzw_roundtrip.cpp
 *
 * Tests for LZW profile, bit I/O, and full encode/decode round-trip
 * (GIF and TIFF).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lzw.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <vector>

extern "C" {
#include "methods/lzw/lzw_bitio.h"
#include "methods/lzw/lzw_profile.h"
}

static void lzw_roundtrip_one(gcomp_registry_t * reg, const char * format,
    const uint8_t * data, size_t len) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "lzw.format", format);

  gcomp_encoder_t * enc = nullptr;
  gcomp_status_t s = gcomp_encoder_create(reg, "lzw", opts, &enc);
  ASSERT_EQ(s, GCOMP_OK);
  ASSERT_NE(enc, nullptr);

  std::vector<uint8_t> encoded(len * 2 + 128);
  gcomp_buffer_t in_buf = {const_cast<uint8_t *>(data), len, 0};
  gcomp_buffer_t out_buf = {encoded.data(), encoded.size(), 0};

  s = gcomp_encoder_update(enc, &in_buf, &out_buf);
  ASSERT_EQ(s, GCOMP_OK);
  s = gcomp_encoder_finish(enc, &out_buf);
  ASSERT_EQ(s, GCOMP_OK);
  size_t encoded_len = out_buf.used;
  gcomp_encoder_destroy(enc);

  gcomp_decoder_t * dec = nullptr;
  s = gcomp_decoder_create(reg, "lzw", opts, &dec);
  ASSERT_EQ(s, GCOMP_OK);
  ASSERT_NE(dec, nullptr);

  std::vector<uint8_t> decoded(len + 64);
  gcomp_buffer_t enc_in = {encoded.data(), encoded_len, 0};
  gcomp_buffer_t dec_out = {decoded.data(), decoded.size(), 0};

  s = gcomp_decoder_update(dec, &enc_in, &dec_out);
  ASSERT_EQ(s, GCOMP_OK);
  s = gcomp_decoder_finish(dec, &dec_out);
  ASSERT_EQ(s, GCOMP_OK);
  gcomp_decoder_destroy(dec);
  gcomp_options_destroy(opts);

  EXPECT_EQ(dec_out.used, len) << "format=" << format;
  if (len > 0 && data != nullptr) {
    EXPECT_TRUE(
        test_helpers_buffers_equal(data, len, decoded.data(), dec_out.used))
        << "format=" << format;
  }
}

class LzwRoundtripTest : public ::testing::Test {
protected:
  void SetUp() override {
    gcomp_registry_create(nullptr, &registry_);
    ASSERT_NE(registry_, nullptr);
    gcomp_method_lzw_register(registry_);
  }

  void TearDown() override {
    if (registry_) {
      gcomp_registry_destroy(registry_);
      registry_ = nullptr;
    }
  }

  gcomp_registry_t * registry_ = nullptr;
};

TEST_F(LzwRoundtripTest, ProfileFromString) {
  EXPECT_EQ(lzw_profile_from_string("gif"), LZW_PROFILE_GIF);
  EXPECT_EQ(lzw_profile_from_string("tiff"), LZW_PROFILE_TIFF);
  EXPECT_EQ(lzw_profile_from_string(""), LZW_PROFILE_UNKNOWN);
  EXPECT_EQ(lzw_profile_from_string("unknown"), LZW_PROFILE_UNKNOWN);
}

TEST_F(LzwRoundtripTest, ProfileClearEoi) {
  // At eight-bit literals - TIFF always, GIF with a full colour table - these
  // are the familiar 256 and 257.  They are not constants, though: they are
  // where the literals end, and this used to return 256 and 257 whatever it
  // was asked.  The TIFF line below passed 9 and expected 256, which is how
  // the error stayed invisible - 9 is TIFF's opening code width, not its
  // literal width, and the function ignored the argument anyway.
  EXPECT_EQ(lzw_profile_clear_code(LZW_PROFILE_GIF, 8), 256u);
  EXPECT_EQ(lzw_profile_eoi_code(LZW_PROFILE_GIF, 8), 257u);
  EXPECT_EQ(lzw_profile_clear_code(LZW_PROFILE_TIFF, 8), 256u);
  EXPECT_EQ(lzw_profile_eoi_code(LZW_PROFILE_TIFF, 8), 257u);

  // Every width GIF allows (89a 22).  A four-colour image clears at 4.
  for (unsigned lit = 2; lit <= 8; lit++) {
    EXPECT_EQ(lzw_profile_clear_code(LZW_PROFILE_GIF, lit), 1u << lit)
        << "lit_width=" << lit;
    EXPECT_EQ(lzw_profile_eoi_code(LZW_PROFILE_GIF, lit), (1u << lit) + 1u)
        << "lit_width=" << lit;
    EXPECT_EQ(lzw_profile_initial_code_bits(LZW_PROFILE_GIF, lit), lit + 1u)
        << "lit_width=" << lit;
  }
}

TEST_F(LzwRoundtripTest, RoundTripAtEveryLiteralWidth) {
  // The widths below eight were unreachable: the profile opened every stream
  // at nine bits, the bit reader refused anything narrower, and the table
  // treated code 257 as the top literal, so a narrow stream either failed or
  // came back short.  Of 121 GIFs on one Debian machine, 71 held an image
  // below width 8.
  for (unsigned lit = 2; lit <= 8; lit++) {
    const unsigned colors = 1u << lit;
    std::vector<uint8_t> data(512);
    for (size_t i = 0; i < data.size(); i++) {
      // Runs and repeats, so the dictionary actually grows and the code width
      // has to widen more than once.
      data[i] = (uint8_t)(((i / 3) + (i % 7)) % colors);
    }
    gcomp_options_t * opts = nullptr;
    gcomp_options_create(&opts);
    gcomp_options_set_string(opts, "lzw.format", "gif");
    gcomp_options_set_uint64(opts, "lzw.lit_width", lit);

    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(gcomp_encoder_create(registry_, "lzw", opts, &enc), GCOMP_OK)
        << "lit_width=" << lit;
    std::vector<uint8_t> encoded(data.size() * 2 + 128);
    gcomp_buffer_t in_buf = {data.data(), data.size(), 0};
    gcomp_buffer_t out_buf = {encoded.data(), encoded.size(), 0};
    ASSERT_EQ(gcomp_encoder_update(enc, &in_buf, &out_buf), GCOMP_OK);
    ASSERT_EQ(gcomp_encoder_finish(enc, &out_buf), GCOMP_OK);
    const size_t encoded_len = out_buf.used;
    gcomp_encoder_destroy(enc);

    gcomp_decoder_t * dec = nullptr;
    ASSERT_EQ(gcomp_decoder_create(registry_, "lzw", opts, &dec), GCOMP_OK)
        << "lit_width=" << lit;
    std::vector<uint8_t> decoded(data.size() + 64);
    gcomp_buffer_t enc_in = {encoded.data(), encoded_len, 0};
    gcomp_buffer_t dec_out = {decoded.data(), decoded.size(), 0};
    ASSERT_EQ(gcomp_decoder_update(dec, &enc_in, &dec_out), GCOMP_OK)
        << "lit_width=" << lit;
    ASSERT_EQ(gcomp_decoder_finish(dec, &dec_out), GCOMP_OK)
        << "lit_width=" << lit;
    gcomp_decoder_destroy(dec);
    gcomp_options_destroy(opts);

    // Length first: the failure this guards against returned success with a
    // short buffer, which a content-only check reads as a prefix match.
    ASSERT_EQ(dec_out.used, data.size()) << "lit_width=" << lit;
    EXPECT_TRUE(test_helpers_buffers_equal(
        data.data(), data.size(), decoded.data(), dec_out.used))
        << "lit_width=" << lit;
  }
}

TEST_F(LzwRoundtripTest, LiteralWidthOutOfRangeIsRefused) {
  // Two is the floor GIF states; eight is the ceiling a byte imposes, because
  // a literal decodes to one.  Nothing rejected either end while the value
  // was ignored.
  for (unsigned lit : {0u, 1u, 9u, 12u, 64u}) {
    gcomp_options_t * opts = nullptr;
    gcomp_options_create(&opts);
    gcomp_options_set_string(opts, "lzw.format", "gif");
    gcomp_options_set_uint64(opts, "lzw.lit_width", lit);
    gcomp_decoder_t * dec = nullptr;
    const gcomp_status_t s =
        gcomp_decoder_create(registry_, "lzw", opts, &dec);
    if (lit == 0u) {
      // Zero means "unset" at the option layer, so it takes the format's
      // default rather than being refused.
      EXPECT_EQ(s, GCOMP_OK) << "lit_width=" << lit;
    }
    else {
      EXPECT_EQ(s, GCOMP_ERR_INVALID_ARG) << "lit_width=" << lit;
    }
    if (dec) {
      gcomp_decoder_destroy(dec);
    }
    gcomp_options_destroy(opts);
  }
}

TEST_F(LzwRoundtripTest, ProfileBitOrder) {
  EXPECT_EQ(lzw_profile_bit_order(LZW_PROFILE_GIF), LZW_BITIO_LSB);
  EXPECT_EQ(lzw_profile_bit_order(LZW_PROFILE_TIFF), LZW_BITIO_MSB);
}

TEST_F(LzwRoundtripTest, ProfileShouldIncrementBits) {
  EXPECT_TRUE(lzw_profile_should_increment_bits(LZW_PROFILE_GIF, 512u, 9));
  EXPECT_FALSE(lzw_profile_should_increment_bits(LZW_PROFILE_GIF, 511u, 9));
  EXPECT_TRUE(lzw_profile_should_increment_bits(LZW_PROFILE_TIFF, 511u, 9));
  EXPECT_FALSE(lzw_profile_should_increment_bits(LZW_PROFILE_TIFF, 512u, 9));
}

TEST_F(LzwRoundtripTest, BitioLsBWriteRead) {
  uint8_t buf[16];
  memset(buf, 0xCC, sizeof(buf));
  lzw_bitwriter_t w;
  lzw_bitwriter_init(&w, buf, sizeof(buf), LZW_BITIO_LSB);
  EXPECT_EQ(lzw_bitwriter_write_bits(&w, 256u, 9), GCOMP_OK);
  EXPECT_EQ(lzw_bitwriter_write_bits(&w, 257u, 9), GCOMP_OK);
  EXPECT_EQ(lzw_bitwriter_flush(&w), GCOMP_OK);
  size_t written = lzw_bitwriter_bytes_written(&w);

  lzw_bitreader_t r;
  lzw_bitreader_init(&r, buf, written, LZW_BITIO_LSB);
  uint32_t c1 = 0, c2 = 0;
  EXPECT_EQ(lzw_bitreader_read_bits(&r, 9, &c1), GCOMP_OK);
  EXPECT_EQ(lzw_bitreader_read_bits(&r, 9, &c2), GCOMP_OK);
  EXPECT_EQ(c1, 256u);
  EXPECT_EQ(c2, 257u);
}

TEST_F(LzwRoundtripTest, BitioMsBWriteRead) {
  uint8_t buf[16];
  memset(buf, 0xCC, sizeof(buf));
  lzw_bitwriter_t w;
  lzw_bitwriter_init(&w, buf, sizeof(buf), LZW_BITIO_MSB);
  EXPECT_EQ(lzw_bitwriter_write_bits(&w, 256u, 9), GCOMP_OK);
  EXPECT_EQ(lzw_bitwriter_write_bits(&w, 257u, 9), GCOMP_OK);
  EXPECT_EQ(lzw_bitwriter_flush(&w), GCOMP_OK);
  size_t written = lzw_bitwriter_bytes_written(&w);

  lzw_bitreader_t r;
  lzw_bitreader_init(&r, buf, written, LZW_BITIO_MSB);
  uint32_t c1 = 0, c2 = 0;
  EXPECT_EQ(lzw_bitreader_read_bits(&r, 9, &c1), GCOMP_OK);
  EXPECT_EQ(lzw_bitreader_read_bits(&r, 9, &c2), GCOMP_OK);
  EXPECT_EQ(c1, 256u);
  EXPECT_EQ(c2, 257u);
}

TEST_F(LzwRoundtripTest, GifEncodeDecodeRoundtripEmpty) {
  lzw_roundtrip_one(registry_, "gif", nullptr, 0);
}

TEST_F(LzwRoundtripTest, GifEncodeDecodeRoundtripSingleByte) {
  const uint8_t one[] = {0x41};
  lzw_roundtrip_one(registry_, "gif", one, sizeof(one));
}

TEST_F(LzwRoundtripTest, GifEncodeDecodeRoundtripShort) {
  const uint8_t data[] = "hello world";
  lzw_roundtrip_one(registry_, "gif", data, sizeof(data) - 1);
}

TEST_F(LzwRoundtripTest, GifEncodeDecodeRoundtripRepeated) {
  const uint8_t data[] = "ABCDEFGHIJK"; /* 11 bytes, exercises table build */
  lzw_roundtrip_one(registry_, "gif", data, sizeof(data) - 1);
}

TEST_F(LzwRoundtripTest, TiffEncodeDecodeRoundtripEmpty) {
  lzw_roundtrip_one(registry_, "tiff", nullptr, 0);
}

TEST_F(LzwRoundtripTest, TiffEncodeDecodeRoundtripSingleByte) {
  const uint8_t one[] = {0x41};
  lzw_roundtrip_one(registry_, "tiff", one, sizeof(one));
}

TEST_F(LzwRoundtripTest, TiffEncodeDecodeRoundtripShort) {
  const uint8_t data[] = "hello world";
  lzw_roundtrip_one(registry_, "tiff", data, sizeof(data) - 1);
}

TEST_F(LzwRoundtripTest, TiffEncodeDecodeRoundtripRepeated) {
  const uint8_t data[] = "ABCDEFGHIJK"; /* 11 bytes, exercises table build */
  lzw_roundtrip_one(registry_, "tiff", data, sizeof(data) - 1);
}

static std::vector<uint8_t> lzw_text_like(size_t target) {
  // Repetitive, word-shaped data: long enough to reuse dictionary entries,
  // grow the code width past 9 bits, and fill/clear the table.
  const char * words[] = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta",
      "eta", "theta", "iota", "kappa", "lambda", "mu", "nu", "xi", "omicron"};
  std::vector<uint8_t> v;
  v.reserve(target + 16);
  unsigned seed = 4242;
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

// Regression: the decoder added a table entry for the first code after a
// reset, when there is no previous string to extend.  That bogus entry
// shifted every later entry down by one relative to the encoder's table, so
// any stream long enough to *reference* an entry decoded to the wrong bytes.
//
// The pre-existing round trips above use at most 11 bytes, which is short
// enough that every code is a literal and the table is only ever written,
// never read - so they passed throughout.
TEST_F(LzwRoundtripTest, GifRoundtripUsesDictionaryEntries) {
  const uint8_t data[] = "ABCDEFGHIJKAB"; /* 13 bytes: first entry reuse */
  lzw_roundtrip_one(registry_, "gif", data, sizeof(data) - 1);
}

TEST_F(LzwRoundtripTest, TiffRoundtripUsesDictionaryEntries) {
  const uint8_t data[] = "ABCDEFGHIJKAB";
  lzw_roundtrip_one(registry_, "tiff", data, sizeof(data) - 1);
}

// Regression: the encoder widened codes one entry too early (it asked whether
// next_code fit in the current width, rather than the largest code it could
// actually emit, next_code - 1).  Streams past the first width boundary were
// then unreadable by conforming decoders.  Needs enough input to cross 9->10
// bits, which the 11-byte cases never did.
TEST_F(LzwRoundtripTest, GifRoundtripAcrossCodeWidthGrowth) {
  std::vector<uint8_t> data = lzw_text_like(80000);
  lzw_roundtrip_one(registry_, "gif", data.data(), data.size());
}

TEST_F(LzwRoundtripTest, TiffRoundtripAcrossCodeWidthGrowth) {
  std::vector<uint8_t> data = lzw_text_like(80000);
  lzw_roundtrip_one(registry_, "tiff", data.data(), data.size());
}


// Regression: the last code in the stream was written at the wrong width.
//
// The encoder emits the final code without adding a dictionary entry -- there
// is no following byte to extend the string with -- and so never ran its
// widening check on the entry a decoder still adds when it reads that code.
// The decoder does add it, applies its own rule, and can widen once more than
// the encoder ever did, reading End_of_Information at a width the encoder did
// not write it at.  The decoder produced the whole output correctly and then
// reported GCOMP_ERR_CORRUPT on the trailing code.
//
// TIFF 6.0 section 13 is where this shows: the early change puts the boundary
// one entry below GIF's, so these lengths land on it in the TIFF profile and
// not in the GIF one.  The lengths below are every failure in 0..3000 for this
// pattern; they are listed explicitly because the window is one to three bytes
// wide and a coarser sweep steps over it.
//
// Verified against libtiff (via Pillow) after the fix: for every length from 1
// to 2599 our encoder's output is byte-for-byte what libtiff produces, and
// each library reads the other's streams.
TEST_F(LzwRoundtripTest, TiffFinalCodeWidthMatchesTheDecoderAtEveryBoundary) {
  struct Case {
    uint64_t max_code_bits;
    std::vector<size_t> lengths;
  };
  const std::vector<Case> cases = {
      {10, {270, 1712}},
      {11, {270, 1436, 1437, 1438}},
      {12, {270, 1436, 1437, 1438}},
  };

  for (const auto & c : cases) {
    for (size_t len : c.lengths) {
      std::vector<uint8_t> data(len);
      for (size_t i = 0; i < len; i++) {
        data[i] = (uint8_t)(i * 7 + (i / 13));
      }

      gcomp_options_t * opts = nullptr;
      ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_string(opts, "lzw.format", "tiff"), GCOMP_OK);
      ASSERT_EQ(
          gcomp_options_set_uint64(opts, "lzw.max_code_bits", c.max_code_bits),
          GCOMP_OK);

      gcomp_encoder_t * enc = nullptr;
      ASSERT_EQ(gcomp_encoder_create(registry_, "lzw", opts, &enc), GCOMP_OK);
      std::vector<uint8_t> encoded(len * 2 + 128);
      gcomp_buffer_t in_buf = {data.data(), len, 0};
      gcomp_buffer_t out_buf = {encoded.data(), encoded.size(), 0};
      ASSERT_EQ(gcomp_encoder_update(enc, &in_buf, &out_buf), GCOMP_OK);
      ASSERT_EQ(gcomp_encoder_finish(enc, &out_buf), GCOMP_OK);
      size_t encoded_len = out_buf.used;
      gcomp_encoder_destroy(enc);

      gcomp_decoder_t * dec = nullptr;
      ASSERT_EQ(gcomp_decoder_create(registry_, "lzw", opts, &dec), GCOMP_OK);
      std::vector<uint8_t> decoded(len + 64);
      gcomp_buffer_t enc_in = {encoded.data(), encoded_len, 0};
      gcomp_buffer_t dec_out = {decoded.data(), decoded.size(), 0};

      gcomp_status_t update_status = gcomp_decoder_update(dec, &enc_in, &dec_out);
      gcomp_status_t finish_status = GCOMP_OK;
      if (update_status == GCOMP_OK) {
        finish_status = gcomp_decoder_finish(dec, &dec_out);
      }
      gcomp_decoder_destroy(dec);
      gcomp_options_destroy(opts);

      EXPECT_EQ(update_status, GCOMP_OK)
          << "max_code_bits=" << c.max_code_bits << " len=" << len;
      EXPECT_EQ(finish_status, GCOMP_OK)
          << "max_code_bits=" << c.max_code_bits << " len=" << len;
      EXPECT_EQ(dec_out.used, len)
          << "max_code_bits=" << c.max_code_bits << " len=" << len;
      EXPECT_TRUE(test_helpers_buffers_equal(
          data.data(), len, decoded.data(), dec_out.used))
          << "max_code_bits=" << c.max_code_bits << " len=" << len;
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
