/**
 * @file test_zstd_decoder.cpp
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
#include "data/golden_vectors.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>
#include <vector>

class ZstdDecoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
  }

  // Helper: compress data with optional options (for producing test frames)
  std::vector<uint8_t> compress(
      const void * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_encoder_t * enc = nullptr;
    if (gcomp_encoder_create(registry_, "zstd", opts, &enc) != GCOMP_OK)
      return {};
    std::vector<uint8_t> out(len + 256);
    gcomp_buffer_t in = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};
    if (gcomp_encoder_update(enc, &in, &ob) != GCOMP_OK) {
      gcomp_encoder_destroy(enc);
      return {};
    }
    if (gcomp_encoder_finish(enc, &ob) != GCOMP_OK) {
      gcomp_encoder_destroy(enc);
      return {};
    }
    out.resize(ob.used);
    gcomp_encoder_destroy(enc);
    return out;
  }

  // Helper: decode compressed data with optional options
  std::vector<uint8_t> decode(const void * data, size_t len,
      gcomp_options_t * opts = nullptr, gcomp_status_t * status_out = nullptr) {
    gcomp_decoder_t * dec = nullptr;
    if (gcomp_decoder_create(registry_, "zstd", opts, &dec) != GCOMP_OK) {
      if (status_out)
        *status_out = GCOMP_ERR_INVALID_ARG;
      return {};
    }
    std::vector<uint8_t> out(len * 100 + 4096);
    gcomp_buffer_t in = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};
    gcomp_status_t st = gcomp_decoder_update(dec, &in, &ob);
    if (st != GCOMP_OK) {
      if (status_out)
        *status_out = st;
      gcomp_decoder_destroy(dec);
      return {};
    }
    st = gcomp_decoder_finish(dec, &ob);
    if (status_out)
      *status_out = st;
    if (st != GCOMP_OK) {
      gcomp_decoder_destroy(dec);
      return {};
    }
    out.resize(ob.used);
    gcomp_decoder_destroy(dec);
    return out;
  }

  gcomp_registry_t * registry_ = nullptr;
};

TEST_F(ZstdDecoderTest, CreateSuccess) {
  gcomp_decoder_t * dec = nullptr;
  EXPECT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  gcomp_decoder_destroy(dec);
}

TEST_F(ZstdDecoderTest, GoldenVectorsDecode) {
  // Decode minimal frame (empty payload)
  auto out1 =
      decode(zstd_v1_empty_compressed, sizeof(zstd_v1_empty_compressed));
  EXPECT_EQ(out1.size(), (size_t)zstd_v1_empty_expected_len);

  // Decode small payload "Hello"
  auto out2 =
      decode(zstd_v2_hello_compressed, sizeof(zstd_v2_hello_compressed));
  ASSERT_EQ(out2.size(), sizeof(zstd_v2_hello_expected));
  EXPECT_EQ(memcmp(out2.data(), zstd_v2_hello_expected, out2.size()), 0);
}

TEST_F(ZstdDecoderTest, DecodeInvalidMagic) {
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  uint8_t bad[] = {0x00, 0x00, 0x00, 0x00, 0x00};
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {bad, sizeof(bad), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &in, &ob), GCOMP_ERR_CORRUPT);
  gcomp_decoder_destroy(dec);
}

TEST_F(ZstdDecoderTest, DestroyImmediately) {
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  gcomp_decoder_destroy(dec);
}

TEST_F(ZstdDecoderTest, NullInputWithSize) {
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {nullptr, 100, 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &in, &ob), GCOMP_ERR_INVALID_ARG);
  gcomp_decoder_destroy(dec);
}

TEST_F(ZstdDecoderTest, ResetAndReuse) {
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);
  gcomp_decoder_destroy(dec);
}

TEST_F(ZstdDecoderTest, BasicDecode) {
  const char data[] = "Hello, Zstd!";
  auto compressed = compress(data, strlen(data));
  ASSERT_GT(compressed.size(), 0u);
  auto decoded = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decoded.size(), strlen(data));
  EXPECT_EQ(memcmp(decoded.data(), data, strlen(data)), 0);
}

TEST_F(ZstdDecoderTest, DecodeWithContentChecksum) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.checksum", true);
  const char data[] = "Data with content checksum";
  auto compressed = compress(data, strlen(data), opts);
  gcomp_options_destroy(opts);
  ASSERT_GT(compressed.size(), 0u);
  auto decoded = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decoded.size(), strlen(data));
  EXPECT_EQ(memcmp(decoded.data(), data, strlen(data)), 0);
}

TEST_F(ZstdDecoderTest, DecodeWithContentSizeValidation) {
  const char data[] = "Content size in frame header";
  const size_t data_len = strlen(data);
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "zstd.content_size", data_len);
  auto compressed = compress(data, data_len, opts);
  gcomp_options_destroy(opts);
  ASSERT_GT(compressed.size(), 0u);
  auto decoded = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decoded.size(), data_len);
  EXPECT_EQ(memcmp(decoded.data(), data, data_len), 0);
}

TEST_F(ZstdDecoderTest, DecodeWithVariousWindowSizes) {
  std::vector<uint8_t> data(4096);
  for (size_t i = 0; i < data.size(); i++)
    data[i] = (uint8_t)(i * 31 + i / 256);
  const unsigned window_logs[] = {16, 20};
  for (unsigned wlog : window_logs) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    gcomp_options_set_uint64(opts, "zstd.window_log", wlog);
    auto compressed = compress(data.data(), data.size(), opts);
    gcomp_options_destroy(opts);
    ASSERT_GT(compressed.size(), 0u) << "window_log " << wlog;
    auto decoded = decode(compressed.data(), compressed.size());
    ASSERT_EQ(decoded.size(), data.size()) << "window_log " << wlog;
    EXPECT_EQ(memcmp(decoded.data(), data.data(), data.size()), 0)
        << "window_log " << wlog;
  }
}

TEST_F(ZstdDecoderTest, Decode1ByteInputChunks) {
  const char data[] = "Chunked input decode test";
  auto compressed = compress(data, strlen(data));
  ASSERT_GT(compressed.size(), 0u);

  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> out(strlen(data) + 256);
  size_t out_offset = 0;
  size_t in_offset = 0;

  while (in_offset < compressed.size()) {
    size_t in_chunk = 1;
    if (in_offset + in_chunk > compressed.size())
      in_chunk = compressed.size() - in_offset;
    gcomp_buffer_t in_buf = {compressed.data() + in_offset, in_chunk, 0};
    gcomp_buffer_t ob = {out.data() + out_offset, out.size() - out_offset, 0};
    gcomp_status_t st = gcomp_decoder_update(dec, &in_buf, &ob);
    ASSERT_EQ(st, GCOMP_OK);
    in_offset += in_buf.used;
    out_offset += ob.used;
  }

  gcomp_buffer_t ob = {out.data() + out_offset, out.size() - out_offset, 0};
  EXPECT_EQ(gcomp_decoder_finish(dec, &ob), GCOMP_OK);
  out_offset += ob.used;
  gcomp_decoder_destroy(dec);

  ASSERT_EQ(out_offset, strlen(data));
  EXPECT_EQ(memcmp(out.data(), data, strlen(data)), 0);
}

TEST_F(ZstdDecoderTest, Decode1ByteOutputBuffer) {
  const char data[] = "Small output";
  auto compressed = compress(data, strlen(data));
  ASSERT_GT(compressed.size(), 0u);

  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> out;
  uint8_t one_byte[1];
  size_t in_offset = 0;

  while (in_offset < compressed.size()) {
    gcomp_buffer_t in_buf = {
        compressed.data() + in_offset, compressed.size() - in_offset, 0};
    gcomp_buffer_t ob = {one_byte, 1, 0};
    gcomp_status_t st = gcomp_decoder_update(dec, &in_buf, &ob);
    ASSERT_EQ(st, GCOMP_OK);
    in_offset += in_buf.used;
    if (ob.used > 0)
      out.push_back(one_byte[0]);
  }

  bool done = false;
  while (!done) {
    gcomp_buffer_t ob = {one_byte, 1, 0};
    gcomp_status_t st = gcomp_decoder_finish(dec, &ob);
    ASSERT_EQ(st, GCOMP_OK);
    if (ob.used > 0)
      out.push_back(one_byte[0]);
    else
      done = true;
  }

  gcomp_decoder_destroy(dec);
  ASSERT_EQ(out.size(), strlen(data));
  EXPECT_EQ(memcmp(out.data(), data, strlen(data)), 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
