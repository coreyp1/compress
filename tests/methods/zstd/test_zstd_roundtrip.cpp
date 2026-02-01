/**
 * @file test_zstd_roundtrip.cpp
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>
#include <vector>

class ZstdRoundtripTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
  }

  std::vector<uint8_t> encode(
      const void * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_encoder_t * enc = nullptr;
    if (gcomp_encoder_create(registry_, "zstd", opts, &enc) != GCOMP_OK)
      return {};
    std::vector<uint8_t> out(len + 1024);
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
    gcomp_encoder_destroy(enc);
    out.resize(ob.used);
    return out;
  }

  std::vector<uint8_t> decode(
      const void * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_decoder_t * dec = nullptr;
    if (gcomp_decoder_create(registry_, "zstd", opts, &dec) != GCOMP_OK)
      return {};
    std::vector<uint8_t> out(len * 1000 + 65536);
    gcomp_buffer_t in = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};
    if (gcomp_decoder_update(dec, &in, &ob) != GCOMP_OK) {
      gcomp_decoder_destroy(dec);
      return {};
    }
    if (gcomp_decoder_finish(dec, &ob) != GCOMP_OK) {
      gcomp_decoder_destroy(dec);
      return {};
    }
    gcomp_decoder_destroy(dec);
    out.resize(ob.used);
    return out;
  }

  gcomp_registry_t * registry_ = nullptr;
};

TEST_F(ZstdRoundtripTest, BasicRoundtrip) {
  const char data[] = "Hello, Zstd roundtrip test!";
  auto compressed = encode(data, strlen(data));
  ASSERT_GT(compressed.size(), 0u);
  auto decompressed = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);
}

TEST_F(ZstdRoundtripTest, EmptyRoundtrip) {
  auto compressed = encode(nullptr, 0);
  ASSERT_GT(compressed.size(), 0u);
  auto decompressed = decode(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), 0u);
}

TEST_F(ZstdRoundtripTest, RLERoundtrip) {
  std::vector<uint8_t> data(1000, 'X');
  auto compressed = encode(data.data(), data.size());
  ASSERT_GT(compressed.size(), 0u);
  EXPECT_LT(compressed.size(), 50u) << "RLE should compress well";
  auto decompressed = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

TEST_F(ZstdRoundtripTest, LargeRandomRoundtrip) {
  std::vector<uint8_t> data(100 * 1024);
  test_helpers_generate_random(data.data(), data.size(), 54321);
  auto compressed = encode(data.data(), data.size());
  ASSERT_GT(compressed.size(), 0u);
  auto decompressed = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

TEST_F(ZstdRoundtripTest, WithChecksum) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.checksum", true);
  const char data[] = "Data with checksum verification!";
  auto compressed = encode(data, strlen(data), opts);
  ASSERT_GT(compressed.size(), 0u);
  auto decompressed = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdRoundtripTest, SingleByte) {
  uint8_t data = 0x42;
  auto compressed = encode(&data, 1);
  ASSERT_GT(compressed.size(), 0u);
  auto decompressed = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), 1u);
  EXPECT_EQ(decompressed[0], 0x42);
}

TEST_F(ZstdRoundtripTest, SequentialData) {
  std::vector<uint8_t> data(1024);
  test_helpers_generate_sequential(data.data(), data.size());
  auto compressed = encode(data.data(), data.size());
  ASSERT_GT(compressed.size(), 0u);
  auto decompressed = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
