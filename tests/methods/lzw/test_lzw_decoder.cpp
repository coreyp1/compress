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

TEST_F(LzwDecoderTest, LimitMaxMemoryBytesBelowBaselineFails) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "lzw.format", "gif");
  gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 1000);

  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t s = gcomp_decoder_create(reg_, "lzw", opts, &dec);
  EXPECT_EQ(s, GCOMP_ERR_LIMIT);
  EXPECT_EQ(dec, nullptr);
  gcomp_options_destroy(opts);
}

TEST_F(LzwDecoderTest, LimitMaxMemoryBytesSufficientSucceeds) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "lzw.format", "gif");
  gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 256 * 1024);

  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t s = gcomp_decoder_create(reg_, "lzw", opts, &dec);
  ASSERT_EQ(s, GCOMP_OK);
  ASSERT_NE(dec, nullptr);
  gcomp_decoder_destroy(dec);
  gcomp_options_destroy(opts);
}


/**
 * The decoder used to treat "the window ended part-way through a code" as
 * corruption, which made it unable to accept a stream in pieces at all.  LZW
 * codes are 9 to 12 bits, so almost any split lands mid-code: 5,000 bytes
 * encoded to 557 and handed over 64 at a time failed on the very first call,
 * at offset 0.  Only a whole stream in one buffer ever worked, which is not
 * what a streaming decoder is for.
 */
TEST(LzwStreamingDecode, DecodesAStreamHandedOverInPieces) {
  gcomp_registry_t * reg = gcomp_registry_default();
  ASSERT_NE(reg, nullptr);

  std::vector<uint8_t> data(5000);
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = (uint8_t)('A' + (i * 7 % 23));
  }

  // Encode in one go; nothing here is about the encoder.
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(reg, "lzw", nullptr, &enc), GCOMP_OK);
  std::vector<uint8_t> stream(data.size() + 4096);
  gcomp_buffer_t in_buf = {data.data(), data.size(), 0};
  gcomp_buffer_t out_buf = {stream.data(), stream.size(), 0};
  ASSERT_EQ(gcomp_encoder_update(enc, &in_buf, &out_buf), GCOMP_OK);
  ASSERT_EQ(gcomp_encoder_finish(enc, &out_buf), GCOMP_OK);
  stream.resize(out_buf.used);
  gcomp_encoder_destroy(enc);
  ASSERT_LT(stream.size(), data.size());

  for (size_t piece : {(size_t)1, (size_t)7, (size_t)64}) {
    gcomp_decoder_t * dec = nullptr;
    ASSERT_EQ(gcomp_decoder_create(reg, "lzw", nullptr, &dec), GCOMP_OK);

    std::vector<uint8_t> out(data.size() + 4096);
    size_t produced = 0;
    size_t offset = 0;
    while (offset < stream.size()) {
      size_t take = piece < stream.size() - offset ? piece
                                                   : stream.size() - offset;
      gcomp_buffer_t din = {stream.data() + offset, take, 0};
      gcomp_buffer_t dout = {out.data() + produced, out.size() - produced, 0};
      ASSERT_EQ(gcomp_decoder_update(dec, &din, &dout), GCOMP_OK)
          << "piece=" << piece << " offset=" << offset;
      produced += dout.used;
      ASSERT_FALSE(din.used == 0 && dout.used == 0)
          << "decoder stalled at offset " << offset;
      offset += din.used;
    }
    gcomp_buffer_t dout = {out.data() + produced, out.size() - produced, 0};
    EXPECT_EQ(gcomp_decoder_finish(dec, &dout), GCOMP_OK) << "piece=" << piece;
    produced += dout.used;
    out.resize(produced);
    EXPECT_EQ(out, data) << "piece=" << piece;
    gcomp_decoder_destroy(dec);
  }
}

/**
 * A stream that really is truncated is still caught -- at finish(), where a
 * missing EOI is detectable, rather than by guessing at update() that the
 * caller had no more to give.
 */
TEST(LzwStreamingDecode, StillCatchesATrulyTruncatedStream) {
  gcomp_registry_t * reg = gcomp_registry_default();
  ASSERT_NE(reg, nullptr);

  std::vector<uint8_t> data(3000);
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = (uint8_t)('a' + (i % 17));
  }

  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(reg, "lzw", nullptr, &enc), GCOMP_OK);
  std::vector<uint8_t> stream(data.size() + 4096);
  gcomp_buffer_t in_buf = {data.data(), data.size(), 0};
  gcomp_buffer_t out_buf = {stream.data(), stream.size(), 0};
  ASSERT_EQ(gcomp_encoder_update(enc, &in_buf, &out_buf), GCOMP_OK);
  ASSERT_EQ(gcomp_encoder_finish(enc, &out_buf), GCOMP_OK);
  stream.resize(out_buf.used / 2); // Cut it off mid-stream.
  gcomp_encoder_destroy(enc);

  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(reg, "lzw", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> out(data.size() + 4096);
  gcomp_buffer_t din = {stream.data(), stream.size(), 0};
  gcomp_buffer_t dout = {out.data(), out.size(), 0};
  gcomp_decoder_update(dec, &din, &dout);
  EXPECT_EQ(gcomp_decoder_finish(dec, &dout), GCOMP_ERR_CORRUPT);
  gcomp_decoder_destroy(dec);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
