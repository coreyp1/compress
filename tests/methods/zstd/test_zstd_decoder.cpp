/**
 * @file test_zstd_decoder.cpp
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

class ZstdDecoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
  }
  gcomp_registry_t * registry_ = nullptr;
};

TEST_F(ZstdDecoderTest, CreateSuccess) {
  gcomp_decoder_t * dec = nullptr;
  EXPECT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  gcomp_decoder_destroy(dec);
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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
