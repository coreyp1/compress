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
  EXPECT_EQ(lzw_profile_clear_code(LZW_PROFILE_GIF, 8), 256u);
  EXPECT_EQ(lzw_profile_eoi_code(LZW_PROFILE_GIF, 8), 257u);
  EXPECT_EQ(lzw_profile_clear_code(LZW_PROFILE_TIFF, 9), 256u);
  EXPECT_EQ(lzw_profile_eoi_code(LZW_PROFILE_TIFF, 9), 257u);
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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
