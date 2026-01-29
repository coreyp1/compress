/**
 * @file test_deflate_decoder.cpp
 *
 * Unit tests for the DEFLATE decoder implementation in the Ghoti.io Compress
 * library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <vector>

class DeflateDecoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_EQ(gcomp_registry_create(nullptr, &registry_), GCOMP_OK);
    ASSERT_NE(registry_, nullptr);
    ASSERT_EQ(gcomp_method_deflate_register(registry_), GCOMP_OK);
  }

  void TearDown() override {
    if (decoder_) {
      gcomp_decoder_destroy(decoder_);
      decoder_ = nullptr;
    }
    if (registry_) {
      gcomp_registry_destroy(registry_);
      registry_ = nullptr;
    }
  }

  gcomp_registry_t * registry_ = nullptr;
  gcomp_decoder_t * decoder_ = nullptr;
};

TEST_F(DeflateDecoderTest, StoredBlock_HelloSingleCall) {
  // Raw DEFLATE stream:
  // BFINAL=1, BTYPE=00 (stored), align to byte, LEN=5, NLEN=~LEN, payload.
  const uint8_t deflate_stream[] = {
      0x01, 0x05, 0x00, 0xFA, 0xFF, 'H', 'e', 'l', 'l', 'o'};

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  uint8_t out[64] = {};
  gcomp_buffer_t in_buf = {deflate_stream, sizeof(deflate_stream), 0};
  gcomp_buffer_t out_buf = {out, sizeof(out), 0};

  ASSERT_EQ(gcomp_decoder_update(decoder_, &in_buf, &out_buf), GCOMP_OK);
  ASSERT_EQ(in_buf.used, sizeof(deflate_stream));
  ASSERT_EQ(out_buf.used, 5u);
  ASSERT_EQ(std::memcmp(out, "Hello", 5u), 0);

  gcomp_buffer_t finish_out = {
      out + out_buf.used, sizeof(out) - out_buf.used, 0};
  ASSERT_EQ(gcomp_decoder_finish(decoder_, &finish_out), GCOMP_OK);
  ASSERT_EQ(finish_out.used, 0u);
}

TEST_F(DeflateDecoderTest, StoredBlock_ChunkedInputAndOutput) {
  const uint8_t deflate_stream[] = {
      0x01, 0x05, 0x00, 0xFA, 0xFF, 'H', 'e', 'l', 'l', 'o'};

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  std::vector<uint8_t> out;
  out.resize(5);
  size_t out_written = 0;

  // Feed in two 5-byte chunks: header+LEN+NLEN (5 bytes) then payload (5
  // bytes).
  const size_t chunk = 5;
  for (size_t i = 0; i < sizeof(deflate_stream); i += chunk) {
    size_t n = (i + chunk <= sizeof(deflate_stream))
        ? chunk
        : (sizeof(deflate_stream) - i);
    gcomp_buffer_t in_buf = {deflate_stream + i, n, 0};
    uint8_t small_out[8] = {};
    gcomp_buffer_t out_buf = {small_out, sizeof(small_out), 0};

    ASSERT_EQ(gcomp_decoder_update(decoder_, &in_buf, &out_buf), GCOMP_OK);
    ASSERT_EQ(in_buf.used, n);

    for (size_t j = 0; j < out_buf.used; j++) {
      ASSERT_LT(out_written, out.size());
      out[out_written++] = small_out[j];
    }
  }

  ASSERT_EQ(out_written, 5u);
  ASSERT_EQ(std::memcmp(out.data(), "Hello", 5u), 0);

  uint8_t finish_buf[8] = {};
  gcomp_buffer_t finish_out = {finish_buf, sizeof(finish_buf), 0};
  ASSERT_EQ(gcomp_decoder_finish(decoder_, &finish_out), GCOMP_OK);
}

TEST_F(DeflateDecoderTest, Finish_IncompleteStreamReturnsCorrupt) {
  const uint8_t partial[] = {0x01, 0x05, 0x00}; // header + partial LEN

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  uint8_t out[8] = {};
  gcomp_buffer_t in_buf = {partial, sizeof(partial), 0};
  gcomp_buffer_t out_buf = {out, sizeof(out), 0};
  ASSERT_EQ(gcomp_decoder_update(decoder_, &in_buf, &out_buf), GCOMP_OK);

  gcomp_buffer_t finish_out = {out, sizeof(out), 0};
  ASSERT_EQ(gcomp_decoder_finish(decoder_, &finish_out), GCOMP_ERR_CORRUPT);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
