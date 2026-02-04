/**
 * @file test_rle_encoder.cpp
 *
 * Encoder tests for RLE: golden vectors (PackBits and TGA),
 * invalid arguments, and limit behavior.
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

static void encode_expect(gcomp_registry_t * reg, const char * format,
    const uint8_t * input, size_t input_len, const uint8_t * expected,
    size_t expected_len) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "rle.format", format);

  gcomp_encoder_t * enc = nullptr;
  gcomp_status_t s = gcomp_encoder_create(reg, "rle", opts, &enc);
  ASSERT_EQ(s, GCOMP_OK);
  ASSERT_NE(enc, nullptr);

  std::vector<uint8_t> out_buf(expected_len + 64);
  gcomp_buffer_t in_buf = {input, input_len, 0};
  gcomp_buffer_t out = {out_buf.data(), out_buf.size(), 0};

  s = gcomp_encoder_update(enc, &in_buf, &out);
  ASSERT_EQ(s, GCOMP_OK);
  s = gcomp_encoder_finish(enc, &out);
  ASSERT_EQ(s, GCOMP_OK);

  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);

  EXPECT_EQ(out.used, expected_len) << "format=" << format;
  if (expected_len > 0 && expected) {
    EXPECT_TRUE(test_helpers_buffers_equal(
        expected, expected_len, out_buf.data(), out.used))
        << "format=" << format;
  }
}

class RleEncoderTest : public ::testing::Test {
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

//
// PackBits golden vectors
// Control 0..127: next (n+1) bytes literal. 128: no-op. 129..255: run (256-n)
// bytes.
//

TEST_F(RleEncoderTest, PackBitsGoldenEmpty) {
  uint8_t empty[1] = {0};
  uint8_t expected[1] = {0};
  encode_expect(reg_, "packbits", empty, 0, expected, 0);
}

TEST_F(RleEncoderTest, PackBitsGoldenSingleByte) {
  uint8_t in[] = {0x42};
  uint8_t expected[] = {0x00, 0x42}; // control 0 = 1 literal
  encode_expect(reg_, "packbits", in, 1, expected, sizeof(expected));
}

TEST_F(RleEncoderTest, PackBitsGoldenRun32) {
  uint8_t in[32];
  memset(in, 0xAA, 32);
  uint8_t expected[] = {0xE0, 0xAA}; // 256-32=224
  encode_expect(reg_, "packbits", in, 32, expected, sizeof(expected));
}

TEST_F(RleEncoderTest, PackBitsGoldenLiteralRun) {
  uint8_t in[] = {0x41, 0x42, 0x43};
  uint8_t expected[] = {0x02, 0x41, 0x42, 0x43}; // control 2 = 3 literals
  encode_expect(reg_, "packbits", in, 3, expected, sizeof(expected));
}

TEST_F(RleEncoderTest, PackBitsGoldenMaxRun127) {
  uint8_t in[127];
  memset(in, 0x77, 127);
  uint8_t expected[] = {0x81, 0x77}; // 256-127=129
  encode_expect(reg_, "packbits", in, 127, expected, sizeof(expected));
}

//
// TGA golden vectors
// Raw: header bit7=0, bits0-6 = count-1, then count bytes. Run: bit7=1, bits0-6
// = count-1, then 1 byte.
//

TEST_F(RleEncoderTest, TgaGoldenEmpty) {
  uint8_t empty[1] = {0};
  uint8_t expected[1] = {0};
  encode_expect(reg_, "tga", empty, 0, expected, 0);
}

TEST_F(RleEncoderTest, TgaGoldenSingleByte) {
  uint8_t in[] = {0x99};
  uint8_t expected[] = {0x00, 0x99}; // raw count 1
  encode_expect(reg_, "tga", in, 1, expected, sizeof(expected));
}

TEST_F(RleEncoderTest, TgaGoldenRun64) {
  uint8_t in[64];
  memset(in, 0x11, 64);
  uint8_t expected[] = {0xBF, 0x11}; // 0x80 | 63
  encode_expect(reg_, "tga", in, 64, expected, sizeof(expected));
}

TEST_F(RleEncoderTest, TgaGoldenRawPacket) {
  uint8_t in[] = {0x01, 0x02, 0x03};
  uint8_t expected[] = {0x02, 0x01, 0x02, 0x03}; // raw count 3
  encode_expect(reg_, "tga", in, 3, expected, sizeof(expected));
}

//
// Invalid arguments
//

TEST_F(RleEncoderTest, UpdateNullEncoder) {
  uint8_t in[] = {0x01};
  std::vector<uint8_t> out(64);
  gcomp_buffer_t in_buf = {in, 1, 0};
  gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
  gcomp_status_t s = gcomp_encoder_update(nullptr, &in_buf, &out_buf);
  EXPECT_EQ(s, GCOMP_ERR_INVALID_ARG);
}

TEST_F(RleEncoderTest, UpdateInputNullDataWithSize) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(reg_, "rle", nullptr, &enc), GCOMP_OK);
  std::vector<uint8_t> out(64);
  gcomp_buffer_t in_buf = {nullptr, 10, 0};
  gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
  gcomp_status_t s = gcomp_encoder_update(enc, &in_buf, &out_buf);
  EXPECT_EQ(s, GCOMP_ERR_INVALID_ARG);
  gcomp_encoder_destroy(enc);
}

TEST_F(RleEncoderTest, UpdateOutputNullDataWithSize) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(reg_, "rle", nullptr, &enc), GCOMP_OK);
  uint8_t in[] = {0x01};
  gcomp_buffer_t in_buf = {in, 1, 0};
  gcomp_buffer_t out_buf = {nullptr, 64, 0};
  gcomp_status_t s = gcomp_encoder_update(enc, &in_buf, &out_buf);
  EXPECT_EQ(s, GCOMP_ERR_INVALID_ARG);
  gcomp_encoder_destroy(enc);
}

TEST_F(RleEncoderTest, FinishNullEncoder) {
  std::vector<uint8_t> out(64);
  gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
  gcomp_status_t s = gcomp_encoder_finish(nullptr, &out_buf);
  EXPECT_EQ(s, GCOMP_ERR_INVALID_ARG);
}

TEST_F(RleEncoderTest, FinishOutputNullDataWithSize) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(reg_, "rle", nullptr, &enc), GCOMP_OK);
  gcomp_buffer_t out_buf = {nullptr, 64, 0};
  gcomp_status_t s = gcomp_encoder_finish(enc, &out_buf);
  EXPECT_EQ(s, GCOMP_ERR_INVALID_ARG);
  gcomp_encoder_destroy(enc);
}

//
// Error detail is set (smoke)
//

TEST_F(RleEncoderTest, ErrorDetailSetOnInvalidArg) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(reg_, "rle", nullptr, &enc), GCOMP_OK);
  gcomp_buffer_t in_buf = {nullptr, 10, 0};
  std::vector<uint8_t> out(64);
  gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
  gcomp_encoder_update(enc, &in_buf, &out_buf);
  const char * detail = gcomp_encoder_get_error_detail(enc);
  EXPECT_NE(detail, nullptr);
  EXPECT_NE(detail[0], '\0');
  gcomp_encoder_destroy(enc);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
