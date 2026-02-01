/**
 * @file test_lz4_roundtrip.cpp
 *
 * Round-trip tests for LZ4 encoder/decoder in the Ghoti.io Compress library.
 *
 * These tests verify:
 * - Compress -> decompress matches original input
 * - Various input types (empty, single byte, large, patterns, random)
 * - Various option combinations (block size, checksums, independent blocks)
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

class Lz4RoundtripTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Compress data using streaming API
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
    // LZ4 worst case: incompressible data can expand slightly
    // Plus header (7-19 bytes) and trailer (0-4 bytes)
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

  // Helper: Decompress data using streaming API
  std::vector<uint8_t> decompress(const void * data, size_t len,
      gcomp_options_t * opts = nullptr, gcomp_status_t * status_out = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "lz4", opts, &decoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result;
    // For highly compressible data, expansion can be large
    size_t max_expansion = len * 1000 + 65536;
    size_t capped_size =
        max_expansion < 16 * 1024 * 1024 ? max_expansion : 16 * 1024 * 1024;
    result.resize(capped_size);

    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t out_buf = {result.data(), result.size(), 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      gcomp_decoder_destroy(decoder);
      return {};
    }

    status = gcomp_decoder_finish(decoder, &out_buf);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      gcomp_decoder_destroy(decoder);
      return {};
    }

    result.resize(out_buf.used);
    gcomp_decoder_destroy(decoder);
    if (status_out)
      *status_out = GCOMP_OK;
    return result;
  }

  // Helper: Perform round-trip and verify data matches
  void verifyRoundtrip(const void * data, size_t len,
      gcomp_options_t * opts = nullptr, const char * description = nullptr) {
    gcomp_status_t status;

    // Compress
    auto compressed = compress(data, len, opts, &status);
    ASSERT_EQ(status, GCOMP_OK)
        << "Compression failed" << (description ? " for " : "")
        << (description ? description : "");
    if (len > 0) {
      ASSERT_FALSE(compressed.empty())
          << "Compression produced no output" << (description ? " for " : "")
          << (description ? description : "");
    }

    // Decompress
    auto decompressed =
        decompress(compressed.data(), compressed.size(), nullptr, &status);
    ASSERT_EQ(status, GCOMP_OK)
        << "Decompression failed" << (description ? " for " : "")
        << (description ? description : "");

    // Verify round-trip
    ASSERT_EQ(decompressed.size(), len)
        << "Round-trip size mismatch" << (description ? " for " : "")
        << (description ? description : "");

    if (len > 0) {
      EXPECT_EQ(memcmp(decompressed.data(), data, len), 0)
          << "Round-trip data mismatch" << (description ? " for " : "")
          << (description ? description : "");
    }
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Basic Round-trip Tests
//

TEST_F(Lz4RoundtripTest, EmptyInput) {
  // Empty input should produce valid LZ4 frame
  verifyRoundtrip(nullptr, 0, nullptr, "empty input");
}

TEST_F(Lz4RoundtripTest, SingleByte) {
  uint8_t single = 'X';
  verifyRoundtrip(&single, 1, nullptr, "single byte");
}

TEST_F(Lz4RoundtripTest, SmallData) {
  const char * data = "Hello, LZ4 world!";
  verifyRoundtrip(data, strlen(data), nullptr, "small data");
}

TEST_F(Lz4RoundtripTest, MediumData) {
  // 1 KB of text
  std::string data;
  for (int i = 0; i < 100; i++) {
    data += "The quick brown fox jumps over the lazy dog. ";
  }
  verifyRoundtrip(data.data(), data.size(), nullptr, "medium data (1KB)");
}

TEST_F(Lz4RoundtripTest, LargeData) {
  // 1 MB of random-ish data
  std::vector<uint8_t> data(1024 * 1024);
  test_helpers_generate_random(data.data(), data.size(), 12345);
  verifyRoundtrip(data.data(), data.size(), nullptr, "large data (1MB)");
}

//
// Pattern Tests
//

TEST_F(Lz4RoundtripTest, AllZeros) {
  // Highly compressible: all zeros
  std::vector<uint8_t> data(10000, 0);
  verifyRoundtrip(data.data(), data.size(), nullptr, "all zeros");
}

TEST_F(Lz4RoundtripTest, AllOnes) {
  // Highly compressible: all 0xFF
  std::vector<uint8_t> data(10000, 0xFF);
  verifyRoundtrip(data.data(), data.size(), nullptr, "all 0xFF");
}

TEST_F(Lz4RoundtripTest, RepeatingPattern) {
  // Repeating pattern
  std::vector<uint8_t> data(10000);
  uint8_t pattern[] = {0xAB, 0xCD, 0xEF, 0x01};
  test_helpers_generate_pattern(data.data(), data.size(), pattern, 4);
  verifyRoundtrip(data.data(), data.size(), nullptr, "repeating pattern");
}

TEST_F(Lz4RoundtripTest, SequentialData) {
  // Sequential bytes (0, 1, 2, ...)
  std::vector<uint8_t> data(1000);
  test_helpers_generate_sequential(data.data(), data.size());
  verifyRoundtrip(data.data(), data.size(), nullptr, "sequential data");
}

TEST_F(Lz4RoundtripTest, RandomData) {
  // Random data (low compressibility)
  std::vector<uint8_t> data(10000);
  test_helpers_generate_random(data.data(), data.size(), 42);
  verifyRoundtrip(data.data(), data.size(), nullptr, "random data");
}

TEST_F(Lz4RoundtripTest, HighEntropyRandom) {
  // Truly random-like data
  std::vector<uint8_t> data(50000);
  test_helpers_generate_random(data.data(), data.size(), 9999);
  verifyRoundtrip(data.data(), data.size(), nullptr, "high-entropy random");
}

//
// Block Size Tests
//

TEST_F(Lz4RoundtripTest, BlockSize64KB) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 65536);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> data(100000);
  test_helpers_generate_random(data.data(), data.size(), 111);
  verifyRoundtrip(data.data(), data.size(), opts, "block_size=64KB");

  gcomp_options_destroy(opts);
}

TEST_F(Lz4RoundtripTest, BlockSize256KB) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 262144);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> data(500000);
  test_helpers_generate_random(data.data(), data.size(), 222);
  verifyRoundtrip(data.data(), data.size(), opts, "block_size=256KB");

  gcomp_options_destroy(opts);
}

TEST_F(Lz4RoundtripTest, BlockSize1MB) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 1048576);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> data(500000);
  test_helpers_generate_random(data.data(), data.size(), 333);
  verifyRoundtrip(data.data(), data.size(), opts, "block_size=1MB");

  gcomp_options_destroy(opts);
}

TEST_F(Lz4RoundtripTest, BlockSize4MB) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 4194304);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> data(500000);
  test_helpers_generate_random(data.data(), data.size(), 444);
  verifyRoundtrip(data.data(), data.size(), opts, "block_size=4MB (default)");

  gcomp_options_destroy(opts);
}

//
// Checksum Tests
//

TEST_F(Lz4RoundtripTest, WithBlockChecksum) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.block_checksum", 1);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> data(5000);
  test_helpers_generate_random(data.data(), data.size(), 555);
  verifyRoundtrip(data.data(), data.size(), opts, "with block checksum");

  gcomp_options_destroy(opts);
}

TEST_F(Lz4RoundtripTest, WithContentChecksum) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.content_checksum", 1);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> data(5000);
  test_helpers_generate_random(data.data(), data.size(), 666);
  verifyRoundtrip(data.data(), data.size(), opts, "with content checksum");

  gcomp_options_destroy(opts);
}

TEST_F(Lz4RoundtripTest, WithBothChecksums) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.block_checksum", 1);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.content_checksum", 1);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> data(5000);
  test_helpers_generate_random(data.data(), data.size(), 777);
  verifyRoundtrip(data.data(), data.size(), opts, "with both checksums");

  gcomp_options_destroy(opts);
}

//
// Block Mode Tests
//

TEST_F(Lz4RoundtripTest, IndependentBlocks) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.independent_blocks", 1);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> data(100000);
  test_helpers_generate_random(data.data(), data.size(), 888);
  verifyRoundtrip(data.data(), data.size(), opts, "independent blocks");

  gcomp_options_destroy(opts);
}

TEST_F(Lz4RoundtripTest, DependentBlocks) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.independent_blocks", 0);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> data(100000);
  test_helpers_generate_random(data.data(), data.size(), 999);
  verifyRoundtrip(data.data(), data.size(), opts, "dependent blocks");

  gcomp_options_destroy(opts);
}

//
// Combined Options Tests
//

TEST_F(Lz4RoundtripTest, AllOptionsEnabled) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Enable all optional features
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 65536);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.block_checksum", 1);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.content_checksum", 1);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.independent_blocks", 1);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> data(50000);
  test_helpers_generate_random(data.data(), data.size(), 1234);
  verifyRoundtrip(data.data(), data.size(), opts, "all options enabled");

  gcomp_options_destroy(opts);
}

//
// Edge Cases
//

TEST_F(Lz4RoundtripTest, BinaryContent) {
  // Binary data with all byte values (0x00-0xFF)
  std::vector<uint8_t> data(256 * 10);
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = (uint8_t)(i % 256);
  }
  verifyRoundtrip(data.data(), data.size(), nullptr, "binary content");
}

TEST_F(Lz4RoundtripTest, MultipleSizes) {
  // Test various sizes: 0, 1, 2, 10, 100, 1000, 10000, 100000
  std::vector<size_t> sizes = {0, 1, 2, 10, 100, 1000, 10000, 100000};

  for (size_t size : sizes) {
    std::vector<uint8_t> data(size);
    if (size > 0) {
      test_helpers_generate_random(
          data.data(), data.size(), static_cast<uint32_t>(size));
    }

    std::string desc = "size " + std::to_string(size);
    verifyRoundtrip(data.data(), data.size(), nullptr, desc.c_str());
  }
}

TEST_F(Lz4RoundtripTest, ExactBlockBoundary) {
  // Test data that's exactly one 64KB block
  std::vector<uint8_t> data(65536);
  test_helpers_generate_random(data.data(), data.size(), 64000);

  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 65536);
  ASSERT_EQ(status, GCOMP_OK);

  verifyRoundtrip(data.data(), data.size(), opts, "exact block boundary");

  gcomp_options_destroy(opts);
}

TEST_F(Lz4RoundtripTest, JustOverBlockBoundary) {
  // Test data that's just over one 64KB block (triggers second block)
  std::vector<uint8_t> data(65537);
  test_helpers_generate_random(data.data(), data.size(), 65001);

  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 65536);
  ASSERT_EQ(status, GCOMP_OK);

  verifyRoundtrip(data.data(), data.size(), opts, "just over block boundary");

  gcomp_options_destroy(opts);
}

TEST_F(Lz4RoundtripTest, MultipleBlocks) {
  // Test data spanning multiple blocks (3+ blocks with 64KB size)
  std::vector<uint8_t> data(200000);
  test_helpers_generate_random(data.data(), data.size(), 200);

  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 65536);
  ASSERT_EQ(status, GCOMP_OK);

  verifyRoundtrip(data.data(), data.size(), opts, "multiple blocks");

  gcomp_options_destroy(opts);
}

//
// Stress Tests
//

TEST_F(Lz4RoundtripTest, VaryingBlockSizes) {
  // Test all valid block sizes
  std::vector<uint64_t> block_sizes = {65536, 262144, 1048576, 4194304};

  std::vector<uint8_t> data(100000);
  test_helpers_generate_random(data.data(), data.size(), 5555);

  for (uint64_t block_size : block_sizes) {
    gcomp_options_t * opts = nullptr;
    gcomp_status_t status = gcomp_options_create(&opts);
    ASSERT_EQ(status, GCOMP_OK);
    status = gcomp_options_set_uint64(opts, "lz4.block_size", block_size);
    ASSERT_EQ(status, GCOMP_OK);

    std::string desc = "block_size=" + std::to_string(block_size);
    verifyRoundtrip(data.data(), data.size(), opts, desc.c_str());

    gcomp_options_destroy(opts);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
