/**
 * @file test_rle_roundtrip.cpp
 *
 * Round-trip tests for RLE PackBits and TGA: encode then decode,
 * compare to original.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/rle.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <vector>

static void roundtrip_one(gcomp_registry_t * reg, const char * format,
    const uint8_t * data, size_t len) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "rle.format", format);

  gcomp_encoder_t * enc = nullptr;
  gcomp_status_t s = gcomp_encoder_create(reg, "rle", opts, &enc);
  ASSERT_EQ(s, GCOMP_OK);
  ASSERT_NE(enc, nullptr);

  std::vector<uint8_t> encoded(len * 2 + 64);
  gcomp_buffer_t in_buf = {data, len, 0};
  gcomp_buffer_t out_buf = {encoded.data(), encoded.size(), 0};

  s = gcomp_encoder_update(enc, &in_buf, &out_buf);
  EXPECT_EQ(s, GCOMP_OK);
  s = gcomp_encoder_finish(enc, &out_buf);
  EXPECT_EQ(s, GCOMP_OK);
  size_t encoded_len = out_buf.used;
  gcomp_encoder_destroy(enc);

  gcomp_decoder_t * dec = nullptr;
  s = gcomp_decoder_create(reg, "rle", opts, &dec);
  ASSERT_EQ(s, GCOMP_OK);
  ASSERT_NE(dec, nullptr);

  std::vector<uint8_t> decoded(len + 64);
  gcomp_buffer_t enc_in = {encoded.data(), encoded_len, 0};
  gcomp_buffer_t dec_out = {decoded.data(), decoded.size(), 0};

  s = gcomp_decoder_update(dec, &enc_in, &dec_out);
  EXPECT_EQ(s, GCOMP_OK);
  s = gcomp_decoder_finish(dec, &dec_out);
  EXPECT_EQ(s, GCOMP_OK);
  gcomp_decoder_destroy(dec);

  gcomp_options_destroy(opts);

  EXPECT_EQ(dec_out.used, len) << "format=" << format;
  EXPECT_TRUE(
      test_helpers_buffers_equal(data, len, decoded.data(), dec_out.used))
      << "format=" << format;
}

class RleRoundtripTest : public ::testing::Test {
protected:
  void SetUp() override {
    gcomp_registry_create(nullptr, &reg_);
    gcomp_method_rle_register(reg_);
  }
  void TearDown() override {
    if (reg_) {
      gcomp_registry_destroy(reg_);
      reg_ = nullptr;
    }
  }
  gcomp_registry_t * reg_ = nullptr;
};

TEST_F(RleRoundtripTest, PackBitsEmpty) {
  uint8_t empty[1] = {0};
  roundtrip_one(reg_, "packbits", empty, 0);
}

TEST_F(RleRoundtripTest, PackBitsSingleByte) {
  uint8_t one[] = {0x42};
  roundtrip_one(reg_, "packbits", one, 1);
}

TEST_F(RleRoundtripTest, PackBitsLiteralRun) {
  uint8_t lit[] = {0x01, 0x02, 0x03, 0x04, 0x05};
  roundtrip_one(reg_, "packbits", lit, 5);
}

TEST_F(RleRoundtripTest, PackBitsByteRun) {
  uint8_t run[32];
  for (size_t i = 0; i < 32; i++)
    run[i] = 0xAA;
  roundtrip_one(reg_, "packbits", run, 32);
}

TEST_F(RleRoundtripTest, PackBitsMixed) {
  uint8_t mixed[] = {0x01, 0x02, 0xFF, 0xFF, 0xFF, 0x03, 0x04};
  roundtrip_one(reg_, "packbits", mixed, 7);
}

TEST_F(RleRoundtripTest, PackBitsMaxLiteral) {
  uint8_t buf[128];
  for (int i = 0; i < 128; i++)
    buf[i] = (uint8_t)i;
  roundtrip_one(reg_, "packbits", buf, 128);
}

TEST_F(RleRoundtripTest, PackBitsMaxRun) {
  uint8_t buf[128];
  memset(buf, 0x77, 128);
  roundtrip_one(reg_, "packbits", buf, 128);
}

TEST_F(RleRoundtripTest, TgaEmpty) {
  uint8_t empty[1] = {0};
  roundtrip_one(reg_, "tga", empty, 0);
}

TEST_F(RleRoundtripTest, TgaSingleByte) {
  uint8_t one[] = {0x99};
  roundtrip_one(reg_, "tga", one, 1);
}

TEST_F(RleRoundtripTest, TgaRawPacket) {
  uint8_t raw[] = {0x01, 0x02, 0x03};
  roundtrip_one(reg_, "tga", raw, 3);
}

TEST_F(RleRoundtripTest, TgaRunPacket) {
  uint8_t run[64];
  memset(run, 0x11, 64);
  roundtrip_one(reg_, "tga", run, 64);
}

TEST_F(RleRoundtripTest, TgaMixed) {
  uint8_t mixed[] = {0x01, 0x02, 0x03, 0x04, 0x04, 0x04, 0x05};
  roundtrip_one(reg_, "tga", mixed, 7);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
