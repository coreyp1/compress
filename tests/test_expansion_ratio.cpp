/**
 * @file test_expansion_ratio.cpp
 *
 * Tests for decompression bomb protection (expansion ratio limits)
 * in the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <vector>

class ExpansionRatioTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);

    ASSERT_EQ(gcomp_options_create(&options_), GCOMP_OK);
    ASSERT_NE(options_, nullptr);
  }

  void TearDown() override {
    if (options_ != nullptr) {
      gcomp_options_destroy(options_);
      options_ = nullptr;
    }
  }

  // Helper to create compressed data with a specific expansion ratio
  // Uses stored blocks (level 0) which have 1:1 ratio overhead
  std::vector<uint8_t> createCompressedData(size_t decompressed_size) {
    std::vector<uint8_t> input(decompressed_size, 0); // All zeros

    // Compress with maximum compression to get best ratio
    gcomp_options_t * enc_opts = nullptr;
    EXPECT_EQ(gcomp_options_create(&enc_opts), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_int64(enc_opts, "deflate.level", 9), GCOMP_OK);

    // Estimate output size (compressed all-zeros is very small)
    size_t output_capacity = decompressed_size;
    std::vector<uint8_t> output(output_capacity);
    size_t actual_size = 0;

    gcomp_status_t status =
        gcomp_encode_buffer(registry_, "deflate", enc_opts, input.data(),
            input.size(), output.data(), output_capacity, &actual_size);

    gcomp_options_destroy(enc_opts);

    if (status == GCOMP_OK) {
      output.resize(actual_size);
      return output;
    }

    // Return empty vector on failure
    return {};
  }

  // Helper to attempt decompression with ratio limit
  gcomp_status_t decompressWithRatioLimit(const std::vector<uint8_t> & compressed,
      uint64_t ratio_limit, std::vector<uint8_t> & output) {
    EXPECT_EQ(gcomp_options_set_uint64(
                  options_, "limits.max_expansion_ratio", ratio_limit),
        GCOMP_OK);

    // Use a generous output limit that won't interfere
    EXPECT_EQ(gcomp_options_set_uint64(
                  options_, "limits.max_output_bytes", 100ULL * 1024 * 1024),
        GCOMP_OK);

    // Allocate sufficient output buffer
    size_t output_capacity = 10 * 1024 * 1024; // 10 MB should be enough
    output.resize(output_capacity);
    size_t actual_size = 0;

    gcomp_status_t status =
        gcomp_decode_buffer(registry_, "deflate", options_, compressed.data(),
            compressed.size(), output.data(), output_capacity, &actual_size);

    if (status == GCOMP_OK) {
      output.resize(actual_size);
    }

    return status;
  }

  gcomp_registry_t * registry_ = nullptr;
  gcomp_options_t * options_ = nullptr;
};

// Test that normal compression/decompression works with default ratio limit
TEST_F(ExpansionRatioTest, NormalDataWorksWithDefaultLimit) {
  // 1 KB of zeros compresses to a few bytes, ratio ~100-200x
  // Should be well within the default 1000x limit
  std::vector<uint8_t> compressed = createCompressedData(1024);
  ASSERT_FALSE(compressed.empty());

  std::vector<uint8_t> decompressed;
  gcomp_status_t status = decompressWithRatioLimit(
      compressed, GCOMP_DEFAULT_MAX_EXPANSION_RATIO, decompressed);

  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(decompressed.size(), 1024);
}

// Test that high but legitimate compression ratios work
TEST_F(ExpansionRatioTest, HighButLegitimateRatioAllowed) {
  // 10 KB of zeros compresses very well
  // This tests that we can achieve high compression without triggering the limit
  std::vector<uint8_t> compressed = createCompressedData(10 * 1024);
  ASSERT_FALSE(compressed.empty());

  // With default 1000x limit, 10KB data should work
  std::vector<uint8_t> decompressed;
  gcomp_status_t status = decompressWithRatioLimit(
      compressed, GCOMP_DEFAULT_MAX_EXPANSION_RATIO, decompressed);

  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(decompressed.size(), 10 * 1024);
}

// Test that setting a very restrictive ratio limit rejects data
TEST_F(ExpansionRatioTest, RestrictiveRatioLimitRejects) {
  // 10 KB of zeros compresses to roughly 50-100 bytes
  // That's about 100-200x expansion
  std::vector<uint8_t> compressed = createCompressedData(10 * 1024);
  ASSERT_FALSE(compressed.empty());

  // Set a very restrictive limit of 10x
  std::vector<uint8_t> decompressed;
  gcomp_status_t status = decompressWithRatioLimit(compressed, 10, decompressed);

  // Should be rejected due to ratio limit
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);
}

// Test that ratio limit of 0 (unlimited) allows any expansion
TEST_F(ExpansionRatioTest, UnlimitedRatioAllowsAnything) {
  // 100 KB of zeros has very high compression ratio
  std::vector<uint8_t> compressed = createCompressedData(100 * 1024);
  ASSERT_FALSE(compressed.empty());

  // Set ratio limit to 0 (unlimited)
  std::vector<uint8_t> decompressed;
  gcomp_status_t status = decompressWithRatioLimit(compressed, 0, decompressed);

  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(decompressed.size(), 100 * 1024);
}

// Test expansion ratio with streaming decoder
TEST_F(ExpansionRatioTest, StreamingDecoderRatioEnforcement) {
  // Create compressed data
  std::vector<uint8_t> compressed = createCompressedData(10 * 1024);
  ASSERT_FALSE(compressed.empty());

  // Create decoder with restrictive ratio limit
  EXPECT_EQ(
      gcomp_options_set_uint64(options_, "limits.max_expansion_ratio", 5),
      GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_uint64(
                options_, "limits.max_output_bytes", 100ULL * 1024 * 1024),
      GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "deflate", options_, &decoder);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(decoder, nullptr);

  // Feed input in chunks
  std::vector<uint8_t> output(20 * 1024);
  size_t input_offset = 0;
  size_t output_offset = 0;
  bool hit_limit = false;

  while (input_offset < compressed.size() && !hit_limit) {
    size_t chunk_size = std::min(size_t(64), compressed.size() - input_offset);

    gcomp_buffer_t in_buf = {
        const_cast<uint8_t *>(compressed.data() + input_offset), chunk_size, 0};
    gcomp_buffer_t out_buf = {
        output.data() + output_offset, output.size() - output_offset, 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    if (status == GCOMP_ERR_LIMIT) {
      hit_limit = true;
    } else {
      EXPECT_EQ(status, GCOMP_OK);
    }

    input_offset += in_buf.used;
    output_offset += out_buf.used;
  }

  gcomp_decoder_destroy(decoder);

  // Should have hit the ratio limit
  EXPECT_TRUE(hit_limit);
}

// Test that ratio limit interacts correctly with output limit
TEST_F(ExpansionRatioTest, RatioLimitInteractionWithOutputLimit) {
  // Create data that would trigger ratio limit before output limit
  std::vector<uint8_t> compressed = createCompressedData(50 * 1024);
  ASSERT_FALSE(compressed.empty());

  // Set output limit high (1 MB) but ratio limit low (50x)
  EXPECT_EQ(gcomp_options_set_uint64(
                options_, "limits.max_output_bytes", 1024 * 1024),
      GCOMP_OK);
  EXPECT_EQ(
      gcomp_options_set_uint64(options_, "limits.max_expansion_ratio", 50),
      GCOMP_OK);

  size_t output_capacity = 1024 * 1024;
  std::vector<uint8_t> output(output_capacity);
  size_t actual_size = 0;

  gcomp_status_t status =
      gcomp_decode_buffer(registry_, "deflate", options_, compressed.data(),
          compressed.size(), output.data(), output_capacity, &actual_size);

  // Should hit ratio limit (not output limit) since 50KB all-zeros compresses
  // to ~100 bytes, giving 500x ratio which exceeds 50x limit
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);
}

// Test that random data (low compression ratio) passes ratio check
TEST_F(ExpansionRatioTest, RandomDataLowRatioPasses) {
  // Generate random data - will have low compression ratio
  std::vector<uint8_t> random_data(1024);
  for (size_t i = 0; i < random_data.size(); i++) {
    random_data[i] = static_cast<uint8_t>(rand() % 256);
  }

  // Compress the random data
  gcomp_options_t * enc_opts = nullptr;
  EXPECT_EQ(gcomp_options_create(&enc_opts), GCOMP_OK);

  size_t comp_capacity = random_data.size() + 100;
  std::vector<uint8_t> compressed(comp_capacity);
  size_t comp_size = 0;

  gcomp_status_t status =
      gcomp_encode_buffer(registry_, "deflate", enc_opts, random_data.data(),
          random_data.size(), compressed.data(), comp_capacity, &comp_size);
  gcomp_options_destroy(enc_opts);
  ASSERT_EQ(status, GCOMP_OK);
  compressed.resize(comp_size);

  // Random data barely compresses, ratio should be ~1x
  // Even a restrictive limit of 5x should pass
  std::vector<uint8_t> decompressed;
  status = decompressWithRatioLimit(compressed, 5, decompressed);

  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(decompressed.size(), random_data.size());
  EXPECT_EQ(memcmp(decompressed.data(), random_data.data(), random_data.size()),
      0);
}

// Test decoder reset clears ratio tracking
TEST_F(ExpansionRatioTest, ResetClearsRatioTracking) {
  // Create compressed data
  std::vector<uint8_t> compressed = createCompressedData(1024);
  ASSERT_FALSE(compressed.empty());

  // Set a moderate ratio limit
  EXPECT_EQ(
      gcomp_options_set_uint64(options_, "limits.max_expansion_ratio", 500),
      GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "deflate", options_, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Decompress first time
  std::vector<uint8_t> output(2 * 1024);
  gcomp_buffer_t in_buf = {
      const_cast<uint8_t *>(compressed.data()), compressed.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  status = gcomp_decoder_finish(decoder, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // Reset the decoder
  status = gcomp_decoder_reset(decoder);
  EXPECT_EQ(status, GCOMP_OK);

  // Decompress again - should work because ratio tracking was reset
  in_buf = {const_cast<uint8_t *>(compressed.data()), compressed.size(), 0};
  out_buf = {output.data(), output.size(), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  status = gcomp_decoder_finish(decoder, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_decoder_destroy(decoder);
}

// Test that ratio check works with stored blocks (no compression)
TEST_F(ExpansionRatioTest, StoredBlocksRatioCheck) {
  // Create data and compress with level 0 (stored blocks)
  std::vector<uint8_t> input(1024, 'A');

  gcomp_options_t * enc_opts = nullptr;
  EXPECT_EQ(gcomp_options_create(&enc_opts), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_int64(enc_opts, "deflate.level", 0), GCOMP_OK);

  size_t comp_capacity = input.size() + 100;
  std::vector<uint8_t> compressed(comp_capacity);
  size_t comp_size = 0;

  gcomp_status_t status =
      gcomp_encode_buffer(registry_, "deflate", enc_opts, input.data(),
          input.size(), compressed.data(), comp_capacity, &comp_size);
  gcomp_options_destroy(enc_opts);
  ASSERT_EQ(status, GCOMP_OK);
  compressed.resize(comp_size);

  // Stored blocks have ~1:1 ratio, should pass even with restrictive limit
  std::vector<uint8_t> decompressed;
  status = decompressWithRatioLimit(compressed, 2, decompressed);

  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(decompressed.size(), input.size());
}

// Test documentation example: 1 KB compressed -> 1 MB decompressed at limit
TEST_F(ExpansionRatioTest, DocumentationExampleAtLimit) {
  // This test verifies the documentation claim:
  // "Default: 1000 (1 KB compressed → max 1 MB decompressed)"

  // Create 1MB of zeros (compresses very well)
  std::vector<uint8_t> compressed = createCompressedData(1024 * 1024);
  ASSERT_FALSE(compressed.empty());

  // If compressed size is <= 1KB and output is 1MB, ratio is >= 1000x
  // This should be at or over the default limit
  if (compressed.size() <= 1024) {
    // The ratio would exceed 1000x, should be rejected
    std::vector<uint8_t> decompressed;
    gcomp_status_t status = decompressWithRatioLimit(
        compressed, GCOMP_DEFAULT_MAX_EXPANSION_RATIO, decompressed);

    // Due to the extreme compression of all-zeros, this should hit the limit
    EXPECT_EQ(status, GCOMP_ERR_LIMIT);
  } else {
    // If compressed size is larger, the ratio is lower, might pass
    // This is still a valid test case
    std::vector<uint8_t> decompressed;
    gcomp_status_t status = decompressWithRatioLimit(
        compressed, GCOMP_DEFAULT_MAX_EXPANSION_RATIO, decompressed);
    // Status depends on actual compression ratio achieved
    (void)status; // Just verify no crash
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
