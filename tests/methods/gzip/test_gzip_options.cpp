/**
 * @file test_gzip_options.cpp
 *
 * Unit tests for gzip method option handling and pass-through.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/gzip.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <set>
#include <string>
#include <vector>

class GzipOptionsTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  bool roundtrip(const void * data, size_t len, gcomp_options_t * enc_opts,
      gcomp_options_t * dec_opts, std::vector<uint8_t> & out) {
    gcomp_encoder_t * encoder = nullptr;
    if (gcomp_encoder_create(registry_, "gzip", enc_opts, &encoder) != GCOMP_OK)
      return false;

    std::vector<uint8_t> compressed(len + len / 10 + 1024);
    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t out_buf = {compressed.data(), compressed.size(), 0};

    if (gcomp_encoder_update(encoder, &in_buf, &out_buf) != GCOMP_OK) {
      gcomp_encoder_destroy(encoder);
      return false;
    }
    if (gcomp_encoder_finish(encoder, &out_buf) != GCOMP_OK) {
      gcomp_encoder_destroy(encoder);
      return false;
    }
    compressed.resize(out_buf.used);
    gcomp_encoder_destroy(encoder);

    gcomp_decoder_t * decoder = nullptr;
    if (gcomp_decoder_create(registry_, "gzip", dec_opts, &decoder) != GCOMP_OK)
      return false;

    out.resize(len * 10 + 65536);
    gcomp_buffer_t dec_in = {compressed.data(), compressed.size(), 0};
    gcomp_buffer_t dec_out = {out.data(), out.size(), 0};

    if (gcomp_decoder_update(decoder, &dec_in, &dec_out) != GCOMP_OK) {
      gcomp_decoder_destroy(decoder);
      return false;
    }
    if (gcomp_decoder_finish(decoder, &dec_out) != GCOMP_OK) {
      gcomp_decoder_destroy(decoder);
      return false;
    }
    out.resize(dec_out.used);
    gcomp_decoder_destroy(decoder);
    return true;
  }

  uint8_t getHeaderByte(
      const void * data, size_t len, gcomp_options_t * opts, size_t offset) {
    gcomp_encoder_t * encoder = nullptr;
    if (gcomp_encoder_create(registry_, "gzip", opts, &encoder) != GCOMP_OK)
      return 0;

    std::vector<uint8_t> compressed(len + 1024);
    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t out_buf = {compressed.data(), compressed.size(), 0};

    if (gcomp_encoder_update(encoder, &in_buf, &out_buf) != GCOMP_OK) {
      gcomp_encoder_destroy(encoder);
      return 0;
    }
    gcomp_status_t status = gcomp_encoder_finish(encoder, &out_buf);
    gcomp_encoder_destroy(encoder);
    if (status != GCOMP_OK || offset >= out_buf.used)
      return 0;
    return compressed[offset];
  }

  uint8_t getXFL(const void * data, size_t len, gcomp_options_t * opts) {
    return getHeaderByte(data, len, opts, 8);
  }

  gcomp_registry_t * registry_ = nullptr;
};

TEST_F(GzipOptionsTest, MtimeOptionParsed) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "gzip.mtime", 0x12345678);

  const char * data = "test";
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "gzip", opts, &encoder), GCOMP_OK);

  std::vector<uint8_t> compressed(256);
  gcomp_buffer_t in_buf = {const_cast<char *>(data), strlen(data), 0};
  gcomp_buffer_t out_buf = {compressed.data(), compressed.size(), 0};

  ASSERT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
  ASSERT_EQ(gcomp_encoder_finish(encoder, &out_buf), GCOMP_OK);
  gcomp_encoder_destroy(encoder);

  ASSERT_GE(out_buf.used, 10u);
  uint32_t mtime = compressed[4] | (compressed[5] << 8) |
      (compressed[6] << 16) | (compressed[7] << 24);
  EXPECT_EQ(mtime, 0x12345678u);
  gcomp_options_destroy(opts);
}

TEST_F(GzipOptionsTest, OsOptionParsed) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "gzip.os", 3);

  const char * data = "test";
  EXPECT_EQ(getHeaderByte(data, strlen(data), opts, 9), 3);
  gcomp_options_destroy(opts);
}

TEST_F(GzipOptionsTest, OsDefaultsToUnknown) {
  const char * data = "test";
  EXPECT_EQ(getHeaderByte(data, strlen(data), nullptr, 9), 255);
}

TEST_F(GzipOptionsTest, XflExplicitOption) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "gzip.xfl", 4);

  const char * data = "test";
  EXPECT_EQ(getXFL(data, strlen(data), opts), 4);
  gcomp_options_destroy(opts);
}

TEST_F(GzipOptionsTest, XflAutoCalculatedFromLevel) {
  const char * data = "test data for xfl calculation";

  for (int level = 0; level <= 2; level++) {
    gcomp_options_t * opts = nullptr;
    gcomp_options_create(&opts);
    gcomp_options_set_int64(opts, "deflate.level", level);
    EXPECT_EQ(getXFL(data, strlen(data), opts), 4) << "level=" << level;
    gcomp_options_destroy(opts);
  }

  for (int level = 3; level <= 5; level++) {
    gcomp_options_t * opts = nullptr;
    gcomp_options_create(&opts);
    gcomp_options_set_int64(opts, "deflate.level", level);
    EXPECT_EQ(getXFL(data, strlen(data), opts), 0) << "level=" << level;
    gcomp_options_destroy(opts);
  }

  for (int level = 6; level <= 9; level++) {
    gcomp_options_t * opts = nullptr;
    gcomp_options_create(&opts);
    gcomp_options_set_int64(opts, "deflate.level", level);
    EXPECT_EQ(getXFL(data, strlen(data), opts), 2) << "level=" << level;
    gcomp_options_destroy(opts);
  }
}

TEST_F(GzipOptionsTest, NameOptionSetsFlag) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "gzip.name", "test.txt");
  const char * data = "test";
  EXPECT_TRUE(getHeaderByte(data, strlen(data), opts, 3) & 0x08);
  gcomp_options_destroy(opts);
}

TEST_F(GzipOptionsTest, CommentOptionSetsFlag) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "gzip.comment", "comment");
  const char * data = "test";
  EXPECT_TRUE(getHeaderByte(data, strlen(data), opts, 3) & 0x10);
  gcomp_options_destroy(opts);
}

TEST_F(GzipOptionsTest, ExtraOptionSetsFlag) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  uint8_t extra[] = {0xAB, 0xCD};
  gcomp_options_set_bytes(opts, "gzip.extra", extra, sizeof(extra));
  const char * data = "test";
  EXPECT_TRUE(getHeaderByte(data, strlen(data), opts, 3) & 0x04);
  gcomp_options_destroy(opts);
}

TEST_F(GzipOptionsTest, HeaderCrcOptionSetsFlag) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_bool(opts, "gzip.header_crc", 1);
  const char * data = "test";
  EXPECT_TRUE(getHeaderByte(data, strlen(data), opts, 3) & 0x02);
  gcomp_options_destroy(opts);
}

TEST_F(GzipOptionsTest, ConcatOptionForDecoder) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_bool(opts, "gzip.concat", 1);
  gcomp_decoder_t * decoder = nullptr;
  EXPECT_EQ(gcomp_decoder_create(registry_, "gzip", opts, &decoder), GCOMP_OK);
  gcomp_decoder_destroy(decoder);
  gcomp_options_destroy(opts);
}

TEST_F(GzipOptionsTest, DeflateLevelPassThrough) {
  const char * data = "This is test data for deflate level pass-through.";
  size_t len = strlen(data);

  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_int64(opts, "deflate.level", 9);

  std::vector<uint8_t> out;
  ASSERT_TRUE(roundtrip(data, len, opts, nullptr, out));
  EXPECT_EQ(out.size(), len);
  EXPECT_EQ(memcmp(out.data(), data, len), 0);
  gcomp_options_destroy(opts);
}

TEST_F(GzipOptionsTest, DeflateWindowBitsPassThrough) {
  std::vector<uint8_t> data(4096);
  test_helpers_generate_pattern(
      data.data(), data.size(), (const uint8_t *)"ABCD", 4);

  for (uint64_t wb = 9; wb <= 15; wb++) {
    gcomp_options_t * opts = nullptr;
    gcomp_options_create(&opts);
    gcomp_options_set_uint64(opts, "deflate.window_bits", wb);

    std::vector<uint8_t> out;
    ASSERT_TRUE(roundtrip(data.data(), data.size(), opts, nullptr, out))
        << "wb=" << wb;
    EXPECT_EQ(out.size(), data.size());
    gcomp_options_destroy(opts);
  }
}

TEST_F(GzipOptionsTest, DeflateStrategyPassThrough) {
  const char * data = "strategy test data";
  size_t len = strlen(data);

  const char * strategies[] = {"default", "filtered", "huffman_only"};
  for (auto strategy : strategies) {
    gcomp_options_t * opts = nullptr;
    gcomp_options_create(&opts);
    gcomp_options_set_string(opts, "deflate.strategy", strategy);

    std::vector<uint8_t> out;
    ASSERT_TRUE(roundtrip(data, len, opts, nullptr, out)) << strategy;
    EXPECT_EQ(out.size(), len);
    gcomp_options_destroy(opts);
  }
}

TEST_F(GzipOptionsTest, LimitsMaxOutputBytesForDecoder) {
  const char * data = "test data that will be compressed";
  gcomp_encoder_t * encoder = nullptr;
  gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);

  std::vector<uint8_t> compressed(1024);
  gcomp_buffer_t in_buf = {const_cast<char *>(data), strlen(data), 0};
  gcomp_buffer_t out_buf = {compressed.data(), compressed.size(), 0};
  gcomp_encoder_update(encoder, &in_buf, &out_buf);
  gcomp_encoder_finish(encoder, &out_buf);
  compressed.resize(out_buf.used);
  gcomp_encoder_destroy(encoder);

  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_uint64(opts, "limits.max_output_bytes", 5);

  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "gzip", opts, &decoder), GCOMP_OK);

  std::vector<uint8_t> decompressed(1024);
  gcomp_buffer_t dec_in = {compressed.data(), compressed.size(), 0};
  gcomp_buffer_t dec_out = {decompressed.data(), decompressed.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(decoder, &dec_in, &dec_out), GCOMP_ERR_LIMIT);

  gcomp_decoder_destroy(decoder);
  gcomp_options_destroy(opts);
}

TEST_F(GzipOptionsTest, EncoderFailsWithoutDeflate) {
  gcomp_registry_t * empty_reg = nullptr;
  ASSERT_EQ(gcomp_registry_create(nullptr, &empty_reg), GCOMP_OK);
  gcomp_method_gzip_register(empty_reg);

  gcomp_encoder_t * encoder = nullptr;
  EXPECT_EQ(gcomp_encoder_create(empty_reg, "gzip", nullptr, &encoder),
      GCOMP_ERR_UNSUPPORTED);
  gcomp_registry_destroy(empty_reg);
}

TEST_F(GzipOptionsTest, DecoderFailsWithoutDeflate) {
  gcomp_registry_t * empty_reg = nullptr;
  ASSERT_EQ(gcomp_registry_create(nullptr, &empty_reg), GCOMP_OK);
  gcomp_method_gzip_register(empty_reg);

  gcomp_decoder_t * decoder = nullptr;
  EXPECT_EQ(gcomp_decoder_create(empty_reg, "gzip", nullptr, &decoder),
      GCOMP_ERR_UNSUPPORTED);
  gcomp_registry_destroy(empty_reg);
}

TEST_F(GzipOptionsTest, SchemaReturnsGzipOptions) {
  const gcomp_method_t * method = gcomp_registry_find(registry_, "gzip");
  ASSERT_NE(method, nullptr);
  const gcomp_method_schema_t * schema = method->get_schema();
  ASSERT_NE(schema, nullptr);
  EXPECT_GT(schema->num_options, 0u);

  std::set<std::string> expected = {"gzip.mtime", "gzip.os", "gzip.name",
      "gzip.comment", "gzip.extra", "gzip.header_crc", "gzip.xfl",
      "gzip.concat", "gzip.max_name_bytes", "gzip.max_comment_bytes",
      "gzip.max_extra_bytes"};

  std::set<std::string> found;
  for (size_t i = 0; i < schema->num_options; i++) {
    if (schema->options[i].key)
      found.insert(schema->options[i].key);
  }

  for (const auto & k : expected) {
    EXPECT_TRUE(found.count(k) > 0) << "Missing: " << k;
  }
}

TEST_F(GzipOptionsTest, SchemaUnknownKeyPolicy) {
  const gcomp_method_t * method = gcomp_registry_find(registry_, "gzip");
  ASSERT_NE(method, nullptr);
  const gcomp_method_schema_t * schema = method->get_schema();
  EXPECT_EQ(schema->unknown_key_policy, GCOMP_UNKNOWN_KEY_IGNORE);
}

TEST_F(GzipOptionsTest, GzipAndDeflateOptionsTogether) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "gzip.name", "combined.dat");
  gcomp_options_set_uint64(opts, "gzip.mtime", 1609459200);
  gcomp_options_set_int64(opts, "deflate.level", 7);
  gcomp_options_set_uint64(opts, "deflate.window_bits", 13);

  const char * data = "Combined options test";
  std::vector<uint8_t> out;
  ASSERT_TRUE(roundtrip(data, strlen(data), opts, nullptr, out));
  EXPECT_EQ(out.size(), strlen(data));
  gcomp_options_destroy(opts);
}

TEST_F(GzipOptionsTest, MemoryCleanupWithOptions) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "gzip.name", "memtest.dat");
  gcomp_options_set_string(opts, "gzip.comment", "Memory test");
  uint8_t extra[] = {0x01, 0x02};
  gcomp_options_set_bytes(opts, "gzip.extra", extra, sizeof(extra));
  gcomp_options_set_int64(opts, "deflate.level", 6);

  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "gzip", opts, &encoder), GCOMP_OK);
  gcomp_encoder_destroy(encoder);

  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "gzip", opts, &decoder), GCOMP_OK);
  gcomp_decoder_destroy(decoder);

  gcomp_options_destroy(opts);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
