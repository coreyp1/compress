/**
 * @file test_lzw_decoder.cpp
 *
 * Decoder tests for LZW: valid stream (encode then decode), malformed input,
 * invalid arguments, and limits.
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

static void decode_expect_ok(gcomp_registry_t * reg, const char * format,
    const uint8_t * encoded, size_t encoded_len, const uint8_t * expected,
    size_t expected_len) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "lzw.format", format);

  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t s = gcomp_decoder_create(reg, "lzw", opts, &dec);
  ASSERT_EQ(s, GCOMP_OK);
  ASSERT_NE(dec, nullptr);

  std::vector<uint8_t> out_buf(expected_len + 256);
  gcomp_buffer_t in_buf = {const_cast<uint8_t *>(encoded), encoded_len, 0};
  gcomp_buffer_t out = {out_buf.data(), out_buf.size(), 0};

  s = gcomp_decoder_update(dec, &in_buf, &out);
  ASSERT_EQ(s, GCOMP_OK);
  s = gcomp_decoder_finish(dec, &out);
  ASSERT_EQ(s, GCOMP_OK);

  gcomp_decoder_destroy(dec);
  gcomp_options_destroy(opts);

  EXPECT_EQ(out.used, expected_len) << "format=" << format;
  if (expected_len > 0 && expected) {
    EXPECT_TRUE(test_helpers_buffers_equal(
        expected, expected_len, out_buf.data(), out.used))
        << "format=" << format;
  }
}

static gcomp_status_t decode_expect_error(gcomp_registry_t * reg,
    const char * format, const uint8_t * encoded, size_t encoded_len) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "lzw.format", format);

  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t s = gcomp_decoder_create(reg, "lzw", opts, &dec);
  if (s != GCOMP_OK) {
    gcomp_options_destroy(opts);
    return s;
  }

  std::vector<uint8_t> out_buf(65536);
  gcomp_buffer_t in_buf = {const_cast<uint8_t *>(encoded), encoded_len, 0};
  gcomp_buffer_t out = {out_buf.data(), out_buf.size(), 0};

  s = gcomp_decoder_update(dec, &in_buf, &out);
  if (s == GCOMP_OK) {
    s = gcomp_decoder_finish(dec, &out);
  }
  gcomp_decoder_destroy(dec);
  gcomp_options_destroy(opts);
  return s;
}

class LzwDecoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    gcomp_registry_create(nullptr, &reg_);
    gcomp_method_lzw_register(reg_);
  }
  void TearDown() override {
    if (reg_) {
      gcomp_registry_destroy(reg_);
      reg_ = nullptr;
    }
  }
  gcomp_registry_t * reg_ = nullptr;
};

static void produce_valid_stream(gcomp_registry_t * reg, const char * format,
    const uint8_t * input, size_t input_len, std::vector<uint8_t> * encoded) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "lzw.format", format);

  gcomp_encoder_t * enc = nullptr;
  gcomp_status_t s = gcomp_encoder_create(reg, "lzw", opts, &enc);
  ASSERT_EQ(s, GCOMP_OK);
  ASSERT_NE(enc, nullptr);

  encoded->resize(input_len * 2 + 128);
  gcomp_buffer_t in_buf = {const_cast<uint8_t *>(input), input_len, 0};
  gcomp_buffer_t out_buf = {encoded->data(), encoded->size(), 0};

  s = gcomp_encoder_update(enc, &in_buf, &out_buf);
  ASSERT_EQ(s, GCOMP_OK);
  s = gcomp_encoder_finish(enc, &out_buf);
  ASSERT_EQ(s, GCOMP_OK);
  encoded->resize(out_buf.used);
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(LzwDecoderTest, GifDecodeValidStream) {
  const uint8_t data[] = "hello";
  std::vector<uint8_t> encoded;
  produce_valid_stream(reg_, "gif", data, sizeof(data) - 1, &encoded);
  decode_expect_ok(
      reg_, "gif", encoded.data(), encoded.size(), data, sizeof(data) - 1);
}

TEST_F(LzwDecoderTest, TiffDecodeValidStream) {
  const uint8_t data[] = "world";
  std::vector<uint8_t> encoded;
  produce_valid_stream(reg_, "tiff", data, sizeof(data) - 1, &encoded);
  decode_expect_ok(
      reg_, "tiff", encoded.data(), encoded.size(), data, sizeof(data) - 1);
}

TEST_F(LzwDecoderTest, MalformedTruncatedStream) {
  /* Valid stream starts with CLEAR (9 bits). Truncated after 1 byte. */
  uint8_t truncated[] = {0x00};
  gcomp_status_t s = decode_expect_error(reg_, "gif", truncated, 1);
  EXPECT_TRUE(s == GCOMP_ERR_CORRUPT || s == GCOMP_ERR_LIMIT) << "s=" << s;
}

TEST_F(LzwDecoderTest, InvalidArgNullRegistry) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t s = gcomp_decoder_create(nullptr, "lzw", opts, &dec);
  EXPECT_NE(s, GCOMP_OK);
  EXPECT_EQ(dec, nullptr);
  gcomp_options_destroy(opts);
}

TEST_F(LzwDecoderTest, InvalidArgNullOutputData) {
  const uint8_t data[] = "x";
  std::vector<uint8_t> encoded;
  produce_valid_stream(reg_, "gif", data, 1, &encoded);

  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "lzw.format", "gif");
  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t s = gcomp_decoder_create(reg_, "lzw", opts, &dec);
  ASSERT_EQ(s, GCOMP_OK);

  gcomp_buffer_t in_buf = {encoded.data(), encoded.size(), 0};
  gcomp_buffer_t out_buf = {nullptr, 64, 0};

  s = gcomp_decoder_update(dec, &in_buf, &out_buf);
  EXPECT_NE(s, GCOMP_OK);
  gcomp_decoder_destroy(dec);
  gcomp_options_destroy(opts);
}

TEST_F(LzwDecoderTest, LimitMaxOutputBytes) {
  const uint8_t data[] = "hello";
  std::vector<uint8_t> encoded;
  produce_valid_stream(reg_, "gif", data, sizeof(data) - 1, &encoded);

  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "lzw.format", "gif");
  gcomp_options_set_uint64(opts, "limits.max_output_bytes", 2);

  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t s = gcomp_decoder_create(reg_, "lzw", opts, &dec);
  ASSERT_EQ(s, GCOMP_OK);

  std::vector<uint8_t> out_buf(16);
  gcomp_buffer_t in_buf = {encoded.data(), encoded.size(), 0};
  gcomp_buffer_t out = {out_buf.data(), out_buf.size(), 0};

  s = gcomp_decoder_update(dec, &in_buf, &out);
  /* May succeed for first chunk then hit limit, or fail when limit exceeded */
  if (s == GCOMP_OK) {
    s = gcomp_decoder_finish(dec, &out);
  }
  gcomp_decoder_destroy(dec);
  gcomp_options_destroy(opts);
  EXPECT_EQ(s, GCOMP_ERR_LIMIT);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
