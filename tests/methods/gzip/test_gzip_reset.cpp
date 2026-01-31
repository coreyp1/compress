/**
 * @file test_gzip_reset.cpp
 *
 * Reset tests for gzip encoder/decoder in the Ghoti.io Compress library.
 *
 * These tests verify:
 * - Encoder reset: encode, reset, encode different data
 * - Decoder reset: decode, reset, decode different data
 * - Reset after error recovers correctly
 * - Reset clears CRC/ISIZE counters
 * - Reset with various option combinations
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/gzip.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <vector>

//
// Test fixture
//

class GzipResetTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Compress data
  std::vector<uint8_t> compressWithEncoder(gcomp_encoder_t * encoder,
      const void * data, size_t len, gcomp_status_t * status_out = nullptr) {
    std::vector<uint8_t> result;
    result.resize(len + len / 10 + 256);

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

  // Helper: Decompress data
  std::vector<uint8_t> decompressWithDecoder(gcomp_decoder_t * decoder,
      const void * data, size_t len, gcomp_status_t * status_out = nullptr) {
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

  // Helper: Standard compression for creating test data
  std::vector<uint8_t> compress(
      const void * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "gzip", opts, &encoder);
    if (status != GCOMP_OK)
      return {};

    std::vector<uint8_t> result;
    result.resize(len + len / 10 + 256);

    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t out_buf = {result.data(), result.size(), 0};

    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    if (status != GCOMP_OK) {
      gcomp_encoder_destroy(encoder);
      return {};
    }

    status = gcomp_encoder_finish(encoder, &out_buf);
    gcomp_encoder_destroy(encoder);
    if (status != GCOMP_OK)
      return {};

    result.resize(out_buf.used);
    return result;
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Encoder Reset Tests
//

TEST_F(GzipResetTest, EncoderBasicReset) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(encoder, nullptr);

  // First compression
  const char * data1 = "First data to compress";
  auto comp1 = compressWithEncoder(encoder, data1, strlen(data1), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_FALSE(comp1.empty());

  // Reset
  status = gcomp_encoder_reset(encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Second compression with different data
  const char * data2 = "Second, different data";
  auto comp2 = compressWithEncoder(encoder, data2, strlen(data2), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_FALSE(comp2.empty());

  // Verify both decompress correctly
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  auto decomp1 = decompressWithDecoder(decoder, comp1.data(), comp1.size());
  ASSERT_EQ(decomp1.size(), strlen(data1));
  EXPECT_EQ(memcmp(decomp1.data(), data1, strlen(data1)), 0);

  status = gcomp_decoder_reset(decoder);
  ASSERT_EQ(status, GCOMP_OK);

  auto decomp2 = decompressWithDecoder(decoder, comp2.data(), comp2.size());
  ASSERT_EQ(decomp2.size(), strlen(data2));
  EXPECT_EQ(memcmp(decomp2.data(), data2, strlen(data2)), 0);

  gcomp_decoder_destroy(decoder);
  gcomp_encoder_destroy(encoder);
}

TEST_F(GzipResetTest, EncoderResetMultipleTimes) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Compress and reset multiple times
  for (int i = 0; i < 5; i++) {
    std::string data = "Iteration " + std::to_string(i);
    auto comp = compressWithEncoder(encoder, data.data(), data.size(), &status);
    ASSERT_EQ(status, GCOMP_OK) << "Failed at iteration " << i;
    ASSERT_FALSE(comp.empty()) << "Empty at iteration " << i;

    if (i < 4) {
      status = gcomp_encoder_reset(encoder);
      ASSERT_EQ(status, GCOMP_OK) << "Reset failed at iteration " << i;
    }
  }

  gcomp_encoder_destroy(encoder);
}

TEST_F(GzipResetTest, EncoderResetWithOptions) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_string(opts, "gzip.name", "test.txt");
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_int64(opts, "deflate.level", 6);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "gzip", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // First compression
  const char * data1 = "Data with options";
  auto comp1 = compressWithEncoder(encoder, data1, strlen(data1), &status);
  ASSERT_EQ(status, GCOMP_OK);

  // Reset
  status = gcomp_encoder_reset(encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Second compression - should still use same options
  const char * data2 = "More data";
  auto comp2 = compressWithEncoder(encoder, data2, strlen(data2), &status);
  ASSERT_EQ(status, GCOMP_OK);

  // Verify both have correct FNAME in header
  // FNAME flag is at byte 3, bit 3 (0x08)
  EXPECT_TRUE(comp1[3] & 0x08);
  EXPECT_TRUE(comp2[3] & 0x08);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

TEST_F(GzipResetTest, EncoderResetClearsCRC) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Compress same data twice with reset - should produce identical output
  const char * data = "CRC test data";

  auto comp1 = compressWithEncoder(encoder, data, strlen(data), &status);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_reset(encoder);
  ASSERT_EQ(status, GCOMP_OK);

  auto comp2 = compressWithEncoder(encoder, data, strlen(data), &status);
  ASSERT_EQ(status, GCOMP_OK);

  // Output should be identical (same input, same options)
  ASSERT_EQ(comp1.size(), comp2.size());
  EXPECT_EQ(memcmp(comp1.data(), comp2.data(), comp1.size()), 0);

  gcomp_encoder_destroy(encoder);
}

TEST_F(GzipResetTest, EncoderResetMidStream) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Start compression but don't finish
  const char * partial = "Partial data";
  std::vector<uint8_t> out1(256);
  gcomp_buffer_t in_buf = {const_cast<char *>(partial), strlen(partial), 0};
  gcomp_buffer_t out_buf = {out1.data(), out1.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  // Reset without finishing
  status = gcomp_encoder_reset(encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Should be able to compress new data
  const char * data = "New complete data";
  auto comp = compressWithEncoder(encoder, data, strlen(data), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_FALSE(comp.empty());

  // Verify decompression
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  auto decomp = decompressWithDecoder(decoder, comp.data(), comp.size());
  ASSERT_EQ(decomp.size(), strlen(data));
  EXPECT_EQ(memcmp(decomp.data(), data, strlen(data)), 0);

  gcomp_decoder_destroy(decoder);
  gcomp_encoder_destroy(encoder);
}

//
// Decoder Reset Tests
//

TEST_F(GzipResetTest, DecoderBasicReset) {
  // Create two different compressed streams
  const char * data1 = "First stream data";
  const char * data2 = "Second stream data";

  auto comp1 = compress(data1, strlen(data1));
  auto comp2 = compress(data2, strlen(data2));
  ASSERT_FALSE(comp1.empty());
  ASSERT_FALSE(comp2.empty());

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // First decompression
  auto decomp1 =
      decompressWithDecoder(decoder, comp1.data(), comp1.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decomp1.size(), strlen(data1));
  EXPECT_EQ(memcmp(decomp1.data(), data1, strlen(data1)), 0);

  // Reset
  status = gcomp_decoder_reset(decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Second decompression
  auto decomp2 =
      decompressWithDecoder(decoder, comp2.data(), comp2.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decomp2.size(), strlen(data2));
  EXPECT_EQ(memcmp(decomp2.data(), data2, strlen(data2)), 0);

  gcomp_decoder_destroy(decoder);
}

TEST_F(GzipResetTest, DecoderResetMultipleTimes) {
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Decompress and reset multiple times
  for (int i = 0; i < 5; i++) {
    std::string data = "Data for iteration " + std::to_string(i);
    auto comp = compress(data.data(), data.size());
    ASSERT_FALSE(comp.empty()) << "Compression failed at iteration " << i;

    auto decomp =
        decompressWithDecoder(decoder, comp.data(), comp.size(), &status);
    ASSERT_EQ(status, GCOMP_OK) << "Decompression failed at iteration " << i;
    ASSERT_EQ(decomp.size(), data.size()) << "Size mismatch at iteration " << i;
    EXPECT_EQ(memcmp(decomp.data(), data.data(), data.size()), 0)
        << "Data mismatch at iteration " << i;

    if (i < 4) {
      status = gcomp_decoder_reset(decoder);
      ASSERT_EQ(status, GCOMP_OK) << "Reset failed at iteration " << i;
    }
  }

  gcomp_decoder_destroy(decoder);
}

TEST_F(GzipResetTest, DecoderResetClearsCRC) {
  // Same data compressed twice should decompress identically after reset
  const char * data = "CRC verification data";
  auto comp = compress(data, strlen(data));
  ASSERT_FALSE(comp.empty());

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  auto decomp1 =
      decompressWithDecoder(decoder, comp.data(), comp.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_decoder_reset(decoder);
  ASSERT_EQ(status, GCOMP_OK);

  auto decomp2 =
      decompressWithDecoder(decoder, comp.data(), comp.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);

  // Both decompressions should match
  ASSERT_EQ(decomp1.size(), decomp2.size());
  EXPECT_EQ(memcmp(decomp1.data(), decomp2.data(), decomp1.size()), 0);

  gcomp_decoder_destroy(decoder);
}

TEST_F(GzipResetTest, DecoderResetMidStream) {
  const char * data1 = "First complete data";
  auto comp1 = compress(data1, strlen(data1));
  ASSERT_FALSE(comp1.empty());

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Start decompression but don't finish (partial input)
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in_buf = {
      comp1.data(), comp1.size() / 2, 0}; // Only half the data
  gcomp_buffer_t out_buf = {out.data(), out.size(), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  // May or may not produce output depending on how much data

  // Reset without finishing
  status = gcomp_decoder_reset(decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Should be able to decompress complete new data
  const char * data2 = "Second complete data";
  auto comp2 = compress(data2, strlen(data2));
  ASSERT_FALSE(comp2.empty());

  auto decomp =
      decompressWithDecoder(decoder, comp2.data(), comp2.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decomp.size(), strlen(data2));
  EXPECT_EQ(memcmp(decomp.data(), data2, strlen(data2)), 0);

  gcomp_decoder_destroy(decoder);
}

TEST_F(GzipResetTest, DecoderResetAfterError) {
  // Create invalid gzip data
  uint8_t bad_data[] = {0xFF, 0xFF, 0xFF, 0xFF}; // Invalid magic

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Try to decode invalid data - should fail
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in_buf = {bad_data, sizeof(bad_data), 0};
  gcomp_buffer_t out_buf = {out.data(), out.size(), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);

  // Reset after error
  status = gcomp_decoder_reset(decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Should be able to decode valid data now
  const char * data = "Valid data after error";
  auto comp = compress(data, strlen(data));
  ASSERT_FALSE(comp.empty());

  auto decomp =
      decompressWithDecoder(decoder, comp.data(), comp.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decomp.size(), strlen(data));
  EXPECT_EQ(memcmp(decomp.data(), data, strlen(data)), 0);

  gcomp_decoder_destroy(decoder);
}

//
// Reset with Different Data Types
//

TEST_F(GzipResetTest, EncoderResetDifferentSizes) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Compress empty, then large
  auto comp1 = compressWithEncoder(encoder, nullptr, 0, &status);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_reset(encoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> large(10000);
  test_helpers_generate_random(large.data(), large.size(), 123);
  auto comp2 =
      compressWithEncoder(encoder, large.data(), large.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_reset(encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Single byte
  uint8_t single = 'X';
  auto comp3 = compressWithEncoder(encoder, &single, 1, &status);
  ASSERT_EQ(status, GCOMP_OK);

  // Verify all decompress correctly
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  auto decomp1 = decompressWithDecoder(decoder, comp1.data(), comp1.size());
  EXPECT_EQ(decomp1.size(), 0u);

  status = gcomp_decoder_reset(decoder);
  auto decomp2 = decompressWithDecoder(decoder, comp2.data(), comp2.size());
  ASSERT_EQ(decomp2.size(), large.size());
  EXPECT_EQ(memcmp(decomp2.data(), large.data(), large.size()), 0);

  status = gcomp_decoder_reset(decoder);
  auto decomp3 = decompressWithDecoder(decoder, comp3.data(), comp3.size());
  ASSERT_EQ(decomp3.size(), 1u);
  EXPECT_EQ(decomp3[0], 'X');

  gcomp_decoder_destroy(decoder);
  gcomp_encoder_destroy(encoder);
}

TEST_F(GzipResetTest, DecoderResetWithDifferentHeaders) {
  // Create compressed data with different header options
  gcomp_options_t * opts1 = nullptr;
  gcomp_options_create(&opts1);
  gcomp_options_set_string(opts1, "gzip.name", "file1.txt");

  gcomp_options_t * opts2 = nullptr;
  gcomp_options_create(&opts2);
  gcomp_options_set_string(opts2, "gzip.comment", "File 2 comment");
  gcomp_options_set_bool(opts2, "gzip.header_crc", 1);

  const char * data1 = "Data in file 1";
  const char * data2 = "Data in file 2";

  auto comp1 = compress(data1, strlen(data1), opts1);
  auto comp2 = compress(data2, strlen(data2), opts2);
  ASSERT_FALSE(comp1.empty());
  ASSERT_FALSE(comp2.empty());

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Decode first (with FNAME)
  auto decomp1 =
      decompressWithDecoder(decoder, comp1.data(), comp1.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decomp1.size(), strlen(data1));

  status = gcomp_decoder_reset(decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Decode second (with FCOMMENT and FHCRC)
  auto decomp2 =
      decompressWithDecoder(decoder, comp2.data(), comp2.size(), &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decomp2.size(), strlen(data2));

  gcomp_decoder_destroy(decoder);
  gcomp_options_destroy(opts1);
  gcomp_options_destroy(opts2);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
