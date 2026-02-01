/**
 * @file test_zstd_encoder.cpp
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

class ZstdEncoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
  }
  gcomp_registry_t * registry_ = nullptr;
};

TEST_F(ZstdEncoderTest, CreateSuccess) {
  gcomp_encoder_t * enc = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);
  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdEncoderTest, BasicEncode) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);
  const char data[] = "Hello, Zstd!";
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {(void *)data, strlen(data), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
  EXPECT_GT(ob.used, 0u);
  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdEncoderTest, EncodeEmpty) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {nullptr, 0, 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdEncoderTest, CreateWithInvalidLevel) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_int64(opts, "zstd.level", 0);
  gcomp_encoder_t * enc = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc),
      GCOMP_ERR_INVALID_ARG);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdEncoderTest, EncodeWithChecksum) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.checksum", true);
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);
  const char data[] = "Checksum test!";
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {(void *)data, strlen(data), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
  EXPECT_TRUE(out[4] & 0x04) << "Checksum flag should be set";
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdEncoderTest, ResetAndReuse) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);
  const char d1[] = "First";
  std::vector<uint8_t> o1(256);
  gcomp_buffer_t i1 = {(void *)d1, strlen(d1), 0};
  gcomp_buffer_t b1 = {o1.data(), o1.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &i1, &b1), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &b1), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);
  const char d2[] = "Second";
  std::vector<uint8_t> o2(256);
  gcomp_buffer_t i2 = {(void *)d2, strlen(d2), 0};
  gcomp_buffer_t b2 = {o2.data(), o2.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &i2, &b2), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &b2), GCOMP_OK);
  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdEncoderTest, DestroyWithoutFinish) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);
  const char data[] = "Unfinished";
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {(void *)data, strlen(data), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  gcomp_encoder_update(enc, &in, &ob);
  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdEncoderTest, EncodeRLE) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);
  std::vector<uint8_t> data(1000, 'A');
  std::vector<uint8_t> out(2000);
  gcomp_buffer_t in = {data.data(), data.size(), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
  EXPECT_LT(ob.used, 50u) << "RLE should compress well";
  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdEncoderTest, NullInputWithSize) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {nullptr, 100, 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_ERR_INVALID_ARG);
  gcomp_encoder_destroy(enc);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
