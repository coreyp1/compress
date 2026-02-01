/**
 * @file test_lz4_reset.cpp
 *
 * Reset method tests for LZ4 encoder/decoder in the Ghoti.io Compress library.
 *
 * These tests verify:
 * - Encoder reset allows reusing encoder for new stream
 * - Decoder reset allows reusing decoder for new stream
 * - Reset clears all state correctly
 * - Reset after error recovers correctly
 * - Reset clears checksum counters
 * - Reset with various option combinations
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lz4.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <vector>

//
// Test fixture
//

class Lz4ResetTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Compress data using streaming API
  std::vector<uint8_t> compress(gcomp_encoder_t * encoder, const void * data,
      size_t len, gcomp_status_t * status_out = nullptr) {
    std::vector<uint8_t> result;
    result.resize(len + len / 100 + 256);

    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t out_buf = {result.data(), result.size(), 0};

    gcomp_status_t status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    status = gcomp_encoder_finish(encoder, &out_buf);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    result.resize(out_buf.used);
    if (status_out)
      *status_out = GCOMP_OK;
    return result;
  }

  // Helper: Decompress data using streaming API
  std::vector<uint8_t> decompress(gcomp_decoder_t * decoder, const void * data,
      size_t len, gcomp_status_t * status_out = nullptr) {
    std::vector<uint8_t> result;
    result.resize(len * 1000 + 65536);

    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t out_buf = {result.data(), result.size(), 0};

    gcomp_status_t status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    status = gcomp_decoder_finish(decoder, &out_buf);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    result.resize(out_buf.used);
    if (status_out)
      *status_out = GCOMP_OK;
    return result;
  }

  // Helper: Compress data fresh (for creating test data)
  std::vector<uint8_t> compressFresh(const void * data, size_t len,
      gcomp_options_t * opts = nullptr, gcomp_status_t * status_out = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "lz4", opts, &encoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    auto result = compress(encoder, data, len, status_out);
    gcomp_encoder_destroy(encoder);
    return result;
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Encoder Reset Tests
//

TEST_F(Lz4ResetTest, EncoderResetBasic) {
  // Create encoder
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "lz4", nullptr, &encoder), GCOMP_OK);

  // First stream
  std::vector<uint8_t> data1(1000);
  test_helpers_generate_random(data1.data(), data1.size(), 11111);

  gcomp_status_t status;
  auto compressed1 = compress(encoder, data1.data(), data1.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_FALSE(compressed1.empty());

  // Reset encoder
  ASSERT_EQ(gcomp_encoder_reset(encoder), GCOMP_OK);

  // Second stream (different data)
  std::vector<uint8_t> data2(500);
  test_helpers_generate_random(data2.data(), data2.size(), 22222);

  auto compressed2 = compress(encoder, data2.data(), data2.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_FALSE(compressed2.empty());

  // Verify both can be decompressed
  auto decoded1 =
      decompress(nullptr, compressed1.data(), compressed1.size(), &status);
  // Need fresh decoder for each
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder), GCOMP_OK);

  decoded1 =
      decompress(decoder, compressed1.data(), compressed1.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded1.size(), data1.size());
  EXPECT_EQ(memcmp(decoded1.data(), data1.data(), data1.size()), 0);

  gcomp_decoder_reset(decoder);

  auto decoded2 =
      decompress(decoder, compressed2.data(), compressed2.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded2.size(), data2.size());
  EXPECT_EQ(memcmp(decoded2.data(), data2.data(), data2.size()), 0);

  gcomp_decoder_destroy(decoder);
  gcomp_encoder_destroy(encoder);
}

TEST_F(Lz4ResetTest, EncoderResetMultipleTimes) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "lz4", nullptr, &encoder), GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder), GCOMP_OK);

  // Compress and decode multiple streams with reset
  for (int i = 0; i < 5; i++) {
    std::vector<uint8_t> data((i + 1) * 200);
    test_helpers_generate_random(
        data.data(), data.size(), static_cast<uint32_t>(33333 + i * 1000));

    gcomp_status_t status;
    auto compressed = compress(encoder, data.data(), data.size(), &status);
    ASSERT_EQ(status, GCOMP_OK) << "Compression failed on iteration " << i;

    auto decoded =
        decompress(decoder, compressed.data(), compressed.size(), &status);
    ASSERT_EQ(status, GCOMP_OK) << "Decompression failed on iteration " << i;
    ASSERT_EQ(decoded.size(), data.size())
        << "Size mismatch on iteration " << i;
    EXPECT_EQ(memcmp(decoded.data(), data.data(), data.size()), 0)
        << "Data mismatch on iteration " << i;

    // Reset for next iteration
    ASSERT_EQ(gcomp_encoder_reset(encoder), GCOMP_OK);
    ASSERT_EQ(gcomp_decoder_reset(decoder), GCOMP_OK);
  }

  gcomp_decoder_destroy(decoder);
  gcomp_encoder_destroy(encoder);
}

TEST_F(Lz4ResetTest, EncoderResetWithChecksum) {
  // Create encoder with content checksum enabled
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.content_checksum", 1), GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "lz4", opts, &encoder), GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder), GCOMP_OK);

  // First stream
  std::vector<uint8_t> data1(500);
  test_helpers_generate_random(data1.data(), data1.size(), 44444);

  gcomp_status_t status;
  auto compressed1 = compress(encoder, data1.data(), data1.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);

  auto decoded1 =
      decompress(decoder, compressed1.data(), compressed1.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded1.size(), data1.size());

  // Reset both
  ASSERT_EQ(gcomp_encoder_reset(encoder), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_reset(decoder), GCOMP_OK);

  // Second stream - checksum must be recalculated from scratch
  std::vector<uint8_t> data2(700);
  test_helpers_generate_random(data2.data(), data2.size(), 55555);

  auto compressed2 = compress(encoder, data2.data(), data2.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);

  auto decoded2 =
      decompress(decoder, compressed2.data(), compressed2.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded2.size(), data2.size());
  EXPECT_EQ(memcmp(decoded2.data(), data2.data(), data2.size()), 0);

  gcomp_decoder_destroy(decoder);
  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

//
// Decoder Reset Tests
//

TEST_F(Lz4ResetTest, DecoderResetBasic) {
  // Create two different compressed streams
  std::vector<uint8_t> data1(800);
  std::vector<uint8_t> data2(1200);
  test_helpers_generate_random(data1.data(), data1.size(), 66666);
  test_helpers_generate_random(data2.data(), data2.size(), 77777);

  auto compressed1 = compressFresh(data1.data(), data1.size());
  auto compressed2 = compressFresh(data2.data(), data2.size());
  ASSERT_FALSE(compressed1.empty());
  ASSERT_FALSE(compressed2.empty());

  // Create single decoder, decode both with reset in between
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder), GCOMP_OK);

  gcomp_status_t status;
  auto decoded1 =
      decompress(decoder, compressed1.data(), compressed1.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded1.size(), data1.size());
  EXPECT_EQ(memcmp(decoded1.data(), data1.data(), data1.size()), 0);

  // Reset decoder
  ASSERT_EQ(gcomp_decoder_reset(decoder), GCOMP_OK);

  auto decoded2 =
      decompress(decoder, compressed2.data(), compressed2.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded2.size(), data2.size());
  EXPECT_EQ(memcmp(decoded2.data(), data2.data(), data2.size()), 0);

  gcomp_decoder_destroy(decoder);
}

TEST_F(Lz4ResetTest, DecoderResetAfterPartialDecode) {
  // Create compressed data
  std::vector<uint8_t> data(1000);
  test_helpers_generate_random(data.data(), data.size(), 88888);

  auto compressed = compressFresh(data.data(), data.size());
  ASSERT_FALSE(compressed.empty());

  // Create decoder and do partial decode
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder), GCOMP_OK);

  std::vector<uint8_t> partial_out(100);
  // Feed only part of the compressed data
  gcomp_buffer_t in_buf = {compressed.data(), compressed.size() / 2, 0};
  gcomp_buffer_t out_buf = {partial_out.data(), partial_out.size(), 0};

  gcomp_status_t status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);
  // Partial decode should produce some output (or none if header not complete)

  // Reset decoder
  ASSERT_EQ(gcomp_decoder_reset(decoder), GCOMP_OK);

  // Now decode complete stream - should work correctly
  auto decoded =
      decompress(decoder, compressed.data(), compressed.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), data.size());
  EXPECT_EQ(memcmp(decoded.data(), data.data(), data.size()), 0);

  gcomp_decoder_destroy(decoder);
}

TEST_F(Lz4ResetTest, DecoderResetAfterError) {
  // Create valid compressed data
  std::vector<uint8_t> data(500);
  test_helpers_generate_random(data.data(), data.size(), 99999);

  auto compressed = compressFresh(data.data(), data.size());
  ASSERT_FALSE(compressed.empty());

  // Create invalid data (corrupt magic)
  std::vector<uint8_t> corrupt_data = {0x00, 0x00, 0x00, 0x00};

  // Create decoder
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder), GCOMP_OK);

  // Try to decode corrupt data - should fail
  std::vector<uint8_t> out_buf(1000);
  gcomp_buffer_t in = {corrupt_data.data(), corrupt_data.size(), 0};
  gcomp_buffer_t out = {out_buf.data(), out_buf.size(), 0};

  gcomp_status_t status = gcomp_decoder_update(decoder, &in, &out);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);

  // Reset decoder
  ASSERT_EQ(gcomp_decoder_reset(decoder), GCOMP_OK);

  // Now decode valid data - should work
  auto decoded =
      decompress(decoder, compressed.data(), compressed.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), data.size());
  EXPECT_EQ(memcmp(decoded.data(), data.data(), data.size()), 0);

  gcomp_decoder_destroy(decoder);
}

TEST_F(Lz4ResetTest, DecoderResetWithDependentBlocks) {
  // Create encoder with dependent blocks
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_bool(opts, "lz4.independent_blocks", 0), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "lz4.block_size", 65536), GCOMP_OK);

  // Create data larger than block size to ensure history is used
  std::vector<uint8_t> data1(100000);
  std::vector<uint8_t> data2(80000);
  test_helpers_generate_random(data1.data(), data1.size(), 11111);
  test_helpers_generate_random(data2.data(), data2.size(), 22222);

  auto compressed1 = compressFresh(data1.data(), data1.size(), opts);
  auto compressed2 = compressFresh(data2.data(), data2.size(), opts);
  ASSERT_FALSE(compressed1.empty());
  ASSERT_FALSE(compressed2.empty());

  // Decode with reset between streams
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder), GCOMP_OK);

  gcomp_status_t status;
  auto decoded1 =
      decompress(decoder, compressed1.data(), compressed1.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded1.size(), data1.size());
  EXPECT_EQ(memcmp(decoded1.data(), data1.data(), data1.size()), 0);

  // Reset clears history buffer
  ASSERT_EQ(gcomp_decoder_reset(decoder), GCOMP_OK);

  auto decoded2 =
      decompress(decoder, compressed2.data(), compressed2.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded2.size(), data2.size());
  EXPECT_EQ(memcmp(decoded2.data(), data2.data(), data2.size()), 0);

  gcomp_decoder_destroy(decoder);
  gcomp_options_destroy(opts);
}

//
// Reset with Various Options
//

TEST_F(Lz4ResetTest, ResetWithBlockChecksum) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.block_checksum", 1), GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "lz4", opts, &encoder), GCOMP_OK);
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder), GCOMP_OK);

  for (int i = 0; i < 3; i++) {
    std::vector<uint8_t> data((i + 1) * 300);
    test_helpers_generate_random(
        data.data(), data.size(), static_cast<uint32_t>(10000 + i * 1000));

    gcomp_status_t status;
    auto compressed = compress(encoder, data.data(), data.size(), &status);
    ASSERT_EQ(status, GCOMP_OK);

    auto decoded =
        decompress(decoder, compressed.data(), compressed.size(), &status);
    ASSERT_EQ(status, GCOMP_OK);
    ASSERT_EQ(decoded.size(), data.size());
    EXPECT_EQ(memcmp(decoded.data(), data.data(), data.size()), 0);

    ASSERT_EQ(gcomp_encoder_reset(encoder), GCOMP_OK);
    ASSERT_EQ(gcomp_decoder_reset(decoder), GCOMP_OK);
  }

  gcomp_decoder_destroy(decoder);
  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

TEST_F(Lz4ResetTest, ResetWithSmallBlockSize) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "lz4.block_size", 65536), GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "lz4", opts, &encoder), GCOMP_OK);
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder), GCOMP_OK);

  // Data larger than block size
  std::vector<uint8_t> data1(150000);
  std::vector<uint8_t> data2(200000);
  test_helpers_generate_random(data1.data(), data1.size(), 12345);
  test_helpers_generate_random(data2.data(), data2.size(), 54321);

  gcomp_status_t status;
  auto compressed1 = compress(encoder, data1.data(), data1.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  auto decoded1 =
      decompress(decoder, compressed1.data(), compressed1.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded1.size(), data1.size());

  ASSERT_EQ(gcomp_encoder_reset(encoder), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_reset(decoder), GCOMP_OK);

  auto compressed2 = compress(encoder, data2.data(), data2.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  auto decoded2 =
      decompress(decoder, compressed2.data(), compressed2.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded2.size(), data2.size());
  EXPECT_EQ(memcmp(decoded2.data(), data2.data(), data2.size()), 0);

  gcomp_decoder_destroy(decoder);
  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

//
// Edge Cases
//

TEST_F(Lz4ResetTest, ResetImmediatelyAfterCreate) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "lz4", nullptr, &encoder), GCOMP_OK);
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder), GCOMP_OK);

  // Reset immediately - should be a no-op but still work
  ASSERT_EQ(gcomp_encoder_reset(encoder), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_reset(decoder), GCOMP_OK);

  // Should still work normally
  std::vector<uint8_t> data(500);
  test_helpers_generate_random(data.data(), data.size(), 11111);

  gcomp_status_t status;
  auto compressed = compress(encoder, data.data(), data.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);

  auto decoded =
      decompress(decoder, compressed.data(), compressed.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), data.size());
  EXPECT_EQ(memcmp(decoded.data(), data.data(), data.size()), 0);

  gcomp_decoder_destroy(decoder);
  gcomp_encoder_destroy(encoder);
}

TEST_F(Lz4ResetTest, ResetWithEmptyStream) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "lz4", nullptr, &encoder), GCOMP_OK);
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder), GCOMP_OK);

  // First: empty stream
  gcomp_status_t status;
  auto compressed_empty = compress(encoder, nullptr, 0, &status);
  ASSERT_EQ(status, GCOMP_OK);

  auto decoded_empty = decompress(
      decoder, compressed_empty.data(), compressed_empty.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  EXPECT_EQ(decoded_empty.size(), 0u);

  ASSERT_EQ(gcomp_encoder_reset(encoder), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_reset(decoder), GCOMP_OK);

  // Second: non-empty stream
  std::vector<uint8_t> data(1000);
  test_helpers_generate_random(data.data(), data.size(), 22222);

  auto compressed = compress(encoder, data.data(), data.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);

  auto decoded =
      decompress(decoder, compressed.data(), compressed.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), data.size());
  EXPECT_EQ(memcmp(decoded.data(), data.data(), data.size()), 0);

  gcomp_decoder_destroy(decoder);
  gcomp_encoder_destroy(encoder);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
