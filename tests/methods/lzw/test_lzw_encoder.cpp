/**
 * @file test_lzw_encoder.cpp
 *
 * Encoder tests for LZW: round-trip as golden check (GIF and TIFF),
 * invalid arguments, and output limit behavior.
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

static void encode_then_decode(gcomp_registry_t * reg, const char * format,
    const uint8_t * input, size_t input_len) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "lzw.format", format);

  gcomp_encoder_t * enc = nullptr;
  gcomp_status_t s = gcomp_encoder_create(reg, "lzw", opts, &enc);
  ASSERT_EQ(s, GCOMP_OK);
  ASSERT_NE(enc, nullptr);

  std::vector<uint8_t> encoded(input_len * 2 + 128);
  gcomp_buffer_t in_buf = {const_cast<uint8_t *>(input), input_len, 0};
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

  std::vector<uint8_t> decoded(input_len + 64);
  gcomp_buffer_t enc_in = {encoded.data(), encoded_len, 0};
  gcomp_buffer_t dec_out = {decoded.data(), decoded.size(), 0};

  s = gcomp_decoder_update(dec, &enc_in, &dec_out);
  ASSERT_EQ(s, GCOMP_OK);
  s = gcomp_decoder_finish(dec, &dec_out);
  ASSERT_EQ(s, GCOMP_OK);
  gcomp_decoder_destroy(dec);
  gcomp_options_destroy(opts);

  EXPECT_EQ(dec_out.used, input_len) << "format=" << format;
  if (input_len > 0 && input) {
    EXPECT_TRUE(test_helpers_buffers_equal(
        input, input_len, decoded.data(), dec_out.used))
        << "format=" << format;
  }
}

class LzwEncoderTest : public ::testing::Test {
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

TEST_F(LzwEncoderTest, GifEncodeDecodeEmpty) {
  encode_then_decode(reg_, "gif", nullptr, 0);
}

TEST_F(LzwEncoderTest, GifEncodeDecodeSingleByte) {
  const uint8_t one[] = {0x41};
  encode_then_decode(reg_, "gif", one, sizeof(one));
}

TEST_F(LzwEncoderTest, GifEncodeDecodeShort) {
  const uint8_t data[] = "hello";
  encode_then_decode(reg_, "gif", data, sizeof(data) - 1);
}

TEST_F(LzwEncoderTest, TiffEncodeDecodeEmpty) {
  encode_then_decode(reg_, "tiff", nullptr, 0);
}

TEST_F(LzwEncoderTest, TiffEncodeDecodeShort) {
  const uint8_t data[] = "world";
  encode_then_decode(reg_, "tiff", data, sizeof(data) - 1);
}

TEST_F(LzwEncoderTest, InvalidArgNullRegistry) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_encoder_t * enc = nullptr;
  gcomp_status_t s = gcomp_encoder_create(nullptr, "lzw", opts, &enc);
  EXPECT_NE(s, GCOMP_OK);
  EXPECT_EQ(enc, nullptr);
  gcomp_options_destroy(opts);
}

TEST_F(LzwEncoderTest, InvalidArgNullOutputData) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_encoder_t * enc = nullptr;
  gcomp_status_t s = gcomp_encoder_create(reg_, "lzw", opts, &enc);
  ASSERT_EQ(s, GCOMP_OK);
  ASSERT_NE(enc, nullptr);

  uint8_t in[] = {0x41};
  gcomp_buffer_t in_buf = {in, 1, 0};
  gcomp_buffer_t out_buf = {nullptr, 64, 0};

  s = gcomp_encoder_update(enc, &in_buf, &out_buf);
  EXPECT_NE(s, GCOMP_OK);
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(LzwEncoderTest, LimitMaxMemoryBytesBelowBaselineFails) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "lzw.format", "gif");
  gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 1000);

  gcomp_encoder_t * enc = nullptr;
  gcomp_status_t s = gcomp_encoder_create(reg_, "lzw", opts, &enc);
  EXPECT_EQ(s, GCOMP_ERR_LIMIT);
  EXPECT_EQ(enc, nullptr);
  gcomp_options_destroy(opts);
}

TEST_F(LzwEncoderTest, LimitMaxMemoryBytesSufficientSucceeds) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "lzw.format", "gif");
  gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 256 * 1024);

  gcomp_encoder_t * enc = nullptr;
  gcomp_status_t s = gcomp_encoder_create(reg_, "lzw", opts, &enc);
  ASSERT_EQ(s, GCOMP_OK);
  ASSERT_NE(enc, nullptr);
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

// Encode with given encoder_lookup mode; return encoded size (buffer in
// encoded_out).
static size_t encode_with_lookup(gcomp_registry_t * reg, const char * lookup,
    const uint8_t * input, size_t input_len,
    std::vector<uint8_t> * encoded_out) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "lzw.format", "gif");
  gcomp_options_set_string(opts, "lzw.encoder_lookup", lookup);

  gcomp_encoder_t * enc = nullptr;
  gcomp_status_t s = gcomp_encoder_create(reg, "lzw", opts, &enc);
  if (s != GCOMP_OK || enc == nullptr) {
    gcomp_options_destroy(opts);
    return 0;
  }

  encoded_out->resize(input_len * 2 + 128);
  gcomp_buffer_t in_buf = {const_cast<uint8_t *>(input), input_len, 0};
  gcomp_buffer_t out_buf = {encoded_out->data(), encoded_out->size(), 0};

  s = gcomp_encoder_update(enc, &in_buf, &out_buf);
  if (s != GCOMP_OK) {
    gcomp_encoder_destroy(enc);
    gcomp_options_destroy(opts);
    return 0;
  }
  s = gcomp_encoder_finish(enc, &out_buf);
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
  return (s == GCOMP_OK) ? out_buf.used : 0;
}

TEST_F(LzwEncoderTest, EncoderLookupLinearVsHashEquivalence) {
  const uint8_t data[] = "hello";
  const size_t len = sizeof(data) - 1;

  std::vector<uint8_t> encoded_linear, encoded_hash;
  size_t len_linear =
      encode_with_lookup(reg_, "linear", data, len, &encoded_linear);
  size_t len_hash = encode_with_lookup(reg_, "hash", data, len, &encoded_hash);

  ASSERT_GT(len_linear, 0u) << "linear encode failed";
  ASSERT_GT(len_hash, 0u) << "hash encode failed";

  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "lzw.format", "gif");

  std::vector<uint8_t> decoded_linear(len + 64), decoded_hash(len + 64);
  gcomp_buffer_t dec_out_linear = {
      decoded_linear.data(), decoded_linear.size(), 0};
  gcomp_buffer_t dec_out_hash = {decoded_hash.data(), decoded_hash.size(), 0};

  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t s = gcomp_decoder_create(reg_, "lzw", opts, &dec);
  ASSERT_EQ(s, GCOMP_OK);
  gcomp_buffer_t enc_in = {encoded_linear.data(), len_linear, 0};
  s = gcomp_decoder_update(dec, &enc_in, &dec_out_linear);
  ASSERT_EQ(s, GCOMP_OK);
  s = gcomp_decoder_finish(dec, &dec_out_linear);
  ASSERT_EQ(s, GCOMP_OK);
  gcomp_decoder_destroy(dec);

  dec = nullptr;
  s = gcomp_decoder_create(reg_, "lzw", opts, &dec);
  ASSERT_EQ(s, GCOMP_OK);
  enc_in.data = encoded_hash.data();
  enc_in.size = len_hash;
  enc_in.used = 0;
  s = gcomp_decoder_update(dec, &enc_in, &dec_out_hash);
  ASSERT_EQ(s, GCOMP_OK);
  s = gcomp_decoder_finish(dec, &dec_out_hash);
  ASSERT_EQ(s, GCOMP_OK);
  gcomp_decoder_destroy(dec);
  gcomp_options_destroy(opts);

  EXPECT_EQ(dec_out_linear.used, len);
  EXPECT_EQ(dec_out_hash.used, len);
  EXPECT_TRUE(test_helpers_buffers_equal(
      data, len, decoded_linear.data(), dec_out_linear.used));
  EXPECT_TRUE(test_helpers_buffers_equal(
      data, len, decoded_hash.data(), dec_out_hash.used));
  EXPECT_TRUE(test_helpers_buffers_equal(decoded_linear.data(),
      dec_out_linear.used, decoded_hash.data(), dec_out_hash.used));
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
