/**
 * @file test_lz4_limits.cpp
 *
 * Limits enforcement tests for LZ4 encoder/decoder in the Ghoti.io Compress
 * library.
 *
 * These tests verify:
 * - max_output_bytes enforcement (decoder)
 * - max_expansion_ratio enforcement (decoder)
 * - max_block_bytes enforcement (decoder)
 * - max_memory_bytes enforcement (encoder and decoder)
 * - Limit errors return GCOMP_ERR_LIMIT
 * - Legitimate high-compression data passes within limits
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

class Lz4LimitsTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Compress data with given options
  std::vector<uint8_t> compress(const void * data, size_t len,
      gcomp_options_t * opts = nullptr, gcomp_status_t * status_out = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "lz4", opts, &encoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result;
    result.resize(len + len / 100 + 256);

    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t out_buf = {result.data(), result.size(), 0};

    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      gcomp_encoder_destroy(encoder);
      return {};
    }

    status = gcomp_encoder_finish(encoder, &out_buf);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      gcomp_encoder_destroy(encoder);
      return {};
    }

    result.resize(out_buf.used);
    gcomp_encoder_destroy(encoder);
    if (status_out)
      *status_out = GCOMP_OK;
    return result;
  }

  // Helper: Decompress data with given options
  gcomp_status_t tryDecompress(
      const void * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "lz4", opts, &decoder);
    if (status != GCOMP_OK) {
      return status;
    }

    std::vector<uint8_t> output(1024 * 1024); // 1MB output buffer
    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    if (status != GCOMP_OK) {
      gcomp_decoder_destroy(decoder);
      return status;
    }

    status = gcomp_decoder_finish(decoder, &out_buf);
    gcomp_decoder_destroy(decoder);
    return status;
  }

  // Helper: Try to create encoder with given options
  gcomp_status_t tryCreateEncoder(gcomp_options_t * opts = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "lz4", opts, &encoder);
    if (status == GCOMP_OK) {
      gcomp_encoder_destroy(encoder);
    }
    return status;
  }

  // Helper: Try to create decoder with given options
  gcomp_status_t tryCreateDecoder(gcomp_options_t * opts = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "lz4", opts, &decoder);
    if (status == GCOMP_OK) {
      gcomp_decoder_destroy(decoder);
    }
    return status;
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// max_output_bytes Tests (Decoder)
//

TEST_F(Lz4LimitsTest, MaxOutputBytesEnforced) {
  // Compress 10KB of data
  std::vector<uint8_t> original(10000);
  test_helpers_generate_random(original.data(), original.size(), 12345);

  auto compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // Try to decompress with a 5KB limit
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_output_bytes", 5000),
      GCOMP_OK);

  gcomp_status_t status =
      tryDecompress(compressed.data(), compressed.size(), opts);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_options_destroy(opts);
}

TEST_F(Lz4LimitsTest, MaxOutputBytesExactLimit) {
  // Compress exactly 1000 bytes
  std::vector<uint8_t> original(1000);
  test_helpers_generate_random(original.data(), original.size(), 11111);

  auto compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // Try to decompress with exact limit - should succeed
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_output_bytes", 1000),
      GCOMP_OK);

  gcomp_status_t status =
      tryDecompress(compressed.data(), compressed.size(), opts);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_options_destroy(opts);
}

TEST_F(Lz4LimitsTest, MaxOutputBytesJustUnderLimit) {
  // Compress 1001 bytes
  std::vector<uint8_t> original(1001);
  test_helpers_generate_random(original.data(), original.size(), 22222);

  auto compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // Try to decompress with limit of 1000 - should fail
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_output_bytes", 1000),
      GCOMP_OK);

  gcomp_status_t status =
      tryDecompress(compressed.data(), compressed.size(), opts);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_options_destroy(opts);
}

//
// max_expansion_ratio Tests (Decoder)
//

TEST_F(Lz4LimitsTest, MaxExpansionRatioEnforced) {
  // Compress highly compressible data (all zeros) - this compresses very well
  std::vector<uint8_t> original(100000, 0); // 100KB of zeros

  auto compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // The compressed size should be very small (maybe <1KB for 100KB of zeros)
  // Set a very restrictive expansion ratio (e.g., 10x)
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 10),
      GCOMP_OK);
  // Also set high output limit so we hit expansion ratio first
  ASSERT_EQ(gcomp_options_set_uint64(
                opts, "limits.max_output_bytes", 1024ULL * 1024 * 1024),
      GCOMP_OK);

  gcomp_status_t status =
      tryDecompress(compressed.data(), compressed.size(), opts);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_options_destroy(opts);
}

TEST_F(Lz4LimitsTest, MaxExpansionRatioAllowsNormalData) {
  // Compress random data (low compressibility - expansion ratio close to 1)
  std::vector<uint8_t> original(10000);
  test_helpers_generate_random(original.data(), original.size(), 33333);

  auto compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // Set expansion ratio to 100x - should pass easily for random data
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 100),
      GCOMP_OK);

  gcomp_status_t status =
      tryDecompress(compressed.data(), compressed.size(), opts);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_options_destroy(opts);
}

//
// max_block_bytes Tests (Decoder)
//

TEST_F(Lz4LimitsTest, MaxBlockBytesEnforcedAtBlockSize) {
  // Compress with 256KB block size
  gcomp_options_t * enc_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&enc_opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_uint64(enc_opts, "lz4.block_size", 262144), GCOMP_OK);

  std::vector<uint8_t> original(100000);
  test_helpers_generate_random(original.data(), original.size(), 44444);

  auto compressed = compress(original.data(), original.size(), enc_opts);
  ASSERT_FALSE(compressed.empty());
  gcomp_options_destroy(enc_opts);

  // Try to decompress with 64KB max block limit - should fail because
  // the header declares 256KB blocks
  gcomp_options_t * dec_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&dec_opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(dec_opts, "limits.max_block_bytes", 65536),
      GCOMP_OK);

  gcomp_status_t status =
      tryDecompress(compressed.data(), compressed.size(), dec_opts);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_options_destroy(dec_opts);
}

TEST_F(Lz4LimitsTest, MaxBlockBytesAllowsMatchingLimit) {
  // Compress with 64KB block size
  gcomp_options_t * enc_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&enc_opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_uint64(enc_opts, "lz4.block_size", 65536), GCOMP_OK);

  std::vector<uint8_t> original(50000);
  test_helpers_generate_random(original.data(), original.size(), 55555);

  auto compressed = compress(original.data(), original.size(), enc_opts);
  ASSERT_FALSE(compressed.empty());
  gcomp_options_destroy(enc_opts);

  // Decompress with matching 64KB limit - should succeed
  gcomp_options_t * dec_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&dec_opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(dec_opts, "limits.max_block_bytes", 65536),
      GCOMP_OK);

  gcomp_status_t status =
      tryDecompress(compressed.data(), compressed.size(), dec_opts);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_options_destroy(dec_opts);
}

//
// max_memory_bytes Tests (Encoder)
//

TEST_F(Lz4LimitsTest, EncoderMaxMemoryEnforced) {
  // Try to create encoder with very low memory limit
  // Encoder needs: state struct + block buffer (4MB default) + compressed
  // buffer + hash table (256KB)
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  // 1KB is way too small for encoder
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 1024),
      GCOMP_OK);

  gcomp_status_t status = tryCreateEncoder(opts);
  // Encoder should fail due to memory limit
  EXPECT_EQ(status, GCOMP_ERR_MEMORY);

  gcomp_options_destroy(opts);
}

TEST_F(Lz4LimitsTest, EncoderMaxMemoryWithSmallBlockSize) {
  // Try encoder with small block size (64KB) and moderate memory limit
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "lz4.block_size", 65536), GCOMP_OK);
  // 1MB should be enough for 64KB blocks
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 1048576),
      GCOMP_OK);

  gcomp_status_t status = tryCreateEncoder(opts);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_options_destroy(opts);
}

//
// max_memory_bytes Tests (Decoder)
//

TEST_F(Lz4LimitsTest, DecoderMaxMemoryEnforcedDuringInit) {
  // Decoder allocates buffers after parsing header, so we need valid data
  std::vector<uint8_t> original(1000);
  test_helpers_generate_random(original.data(), original.size(), 66666);

  auto compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // Try to decompress with very low memory limit
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  // 100 bytes is too small for decoder buffers
  ASSERT_EQ(
      gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 100), GCOMP_OK);

  gcomp_status_t status =
      tryDecompress(compressed.data(), compressed.size(), opts);
  // Should fail during buffer allocation (after header parse)
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_options_destroy(opts);
}

TEST_F(Lz4LimitsTest, DecoderMaxMemoryAllowsSufficientLimit) {
  // Compress with small block size
  gcomp_options_t * enc_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&enc_opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_uint64(enc_opts, "lz4.block_size", 65536), GCOMP_OK);

  std::vector<uint8_t> original(10000);
  test_helpers_generate_random(original.data(), original.size(), 77777);

  auto compressed = compress(original.data(), original.size(), enc_opts);
  ASSERT_FALSE(compressed.empty());
  gcomp_options_destroy(enc_opts);

  // Decoder needs: state + block buffer (64KB) + output buffer (64KB)
  // ~200KB should be enough
  gcomp_options_t * dec_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&dec_opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_uint64(dec_opts, "limits.max_memory_bytes", 300000),
      GCOMP_OK);

  gcomp_status_t status =
      tryDecompress(compressed.data(), compressed.size(), dec_opts);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_options_destroy(dec_opts);
}

//
// Legitimate Data Tests
//

TEST_F(Lz4LimitsTest, HighCompressionWithinLimits) {
  // Highly compressible data with generous limits
  std::vector<uint8_t> original(100000, 0); // All zeros

  auto compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // Set high limits that should allow decompression
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_output_bytes", 1000000),
      GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 10000),
      GCOMP_OK);

  gcomp_status_t status =
      tryDecompress(compressed.data(), compressed.size(), opts);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_options_destroy(opts);
}

TEST_F(Lz4LimitsTest, RandomDataWithinLimits) {
  // Random data with generous limits
  std::vector<uint8_t> original(50000);
  test_helpers_generate_random(original.data(), original.size(), 88888);

  auto compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // Random data has low compression ratio, so modest limits should work
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_output_bytes", 100000),
      GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 10),
      GCOMP_OK);

  gcomp_status_t status =
      tryDecompress(compressed.data(), compressed.size(), opts);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_options_destroy(opts);
}

//
// Multiple Limits Interaction Tests
//

TEST_F(Lz4LimitsTest, MultipleDecoderLimitsAllApply) {
  // Test that all limits are checked (set multiple low limits)
  std::vector<uint8_t> original(10000);
  test_helpers_generate_random(original.data(), original.size(), 99999);

  auto compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // Set all limits, but make output limit the trigger
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_output_bytes", 1000),
      GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 10000),
      GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_block_bytes", 10000000),
      GCOMP_OK);

  gcomp_status_t status =
      tryDecompress(compressed.data(), compressed.size(), opts);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_options_destroy(opts);
}

//
// Error Recovery Tests
//

TEST_F(Lz4LimitsTest, DecoderCleanupOnLimitError) {
  // Verify that hitting a limit doesn't cause memory leaks
  // (Valgrind will catch leaks - this test ensures the code path is exercised)
  std::vector<uint8_t> original(10000);
  test_helpers_generate_random(original.data(), original.size(), 11110);

  auto compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  for (int i = 0; i < 10; i++) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_output_bytes", 100),
        GCOMP_OK);

    gcomp_status_t status =
        tryDecompress(compressed.data(), compressed.size(), opts);
    EXPECT_EQ(status, GCOMP_ERR_LIMIT);

    gcomp_options_destroy(opts);
  }
}

TEST_F(Lz4LimitsTest, EncoderCleanupOnMemoryLimitError) {
  // Verify that hitting memory limit during encoder creation cleans up
  for (int i = 0; i < 10; i++) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 100),
        GCOMP_OK);

    gcomp_status_t status = tryCreateEncoder(opts);
    EXPECT_EQ(status, GCOMP_ERR_MEMORY);

    gcomp_options_destroy(opts);
  }
}

//
// Concatenated Frames Limits Tests
//

TEST_F(Lz4LimitsTest, ConcatLimitsApplyAcrossFrames) {
  // Create two compressed frames
  std::vector<uint8_t> data1(5000);
  std::vector<uint8_t> data2(5000);
  test_helpers_generate_random(data1.data(), data1.size(), 12121);
  test_helpers_generate_random(data2.data(), data2.size(), 21212);

  auto frame1 = compress(data1.data(), data1.size());
  auto frame2 = compress(data2.data(), data2.size());
  ASSERT_FALSE(frame1.empty());
  ASSERT_FALSE(frame2.empty());

  // Concatenate frames
  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  // Set limit that allows first frame but not second
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.concat", 1), GCOMP_OK);
  // 7500 bytes limit: first frame (5000) fits, but total (10000) doesn't
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_output_bytes", 7500),
      GCOMP_OK);

  gcomp_status_t status = tryDecompress(concat.data(), concat.size(), opts);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_options_destroy(opts);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
