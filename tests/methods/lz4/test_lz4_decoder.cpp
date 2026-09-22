/**
 * @file test_lz4_decoder.cpp
 *
 * Unit tests for LZ4 decoder in the Ghoti.io Compress library.
 *
 * These tests verify:
 * - Decoder creation success
 * - Basic decode (standard lz4 frame)
 * - Decode with block checksum
 * - Decode with content checksum
 * - Decode with content size validation
 * - 1-byte input chunks
 * - 1-byte output buffer
 * - Decoder reset and reuse
 * - Decoder destroy without finish
 * - Memory cleanup (valgrind)
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lz4.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include "../../../src/methods/lz4/lz4_internal.h"
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/xxhash32.h>
#include <string>
#include <gtest/gtest.h>
#include <vector>

//
// Test fixture
//

class Lz4DecoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Compress data (for creating test input)
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

  // Helper: Decompress data
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
    result.resize(len * 1000 + 65536);

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

  // Helper: Decompress with 1-byte input chunks
  std::vector<uint8_t> decompressOneByteChunks(const void * data, size_t len,
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
    result.resize(len * 1000 + 65536);
    size_t result_pos = 0;

    const uint8_t * src = static_cast<const uint8_t *>(data);

    // Feed one byte at a time
    for (size_t i = 0; i < len; i++) {
      gcomp_buffer_t in_buf = {const_cast<uint8_t *>(src + i), 1, 0};
      gcomp_buffer_t out_buf = {
          result.data() + result_pos, result.size() - result_pos, 0};

      status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
      if (status != GCOMP_OK) {
        if (status_out)
          *status_out = status;
        gcomp_decoder_destroy(decoder);
        return {};
      }
      result_pos += out_buf.used;
    }

    gcomp_buffer_t out_buf = {
        result.data() + result_pos, result.size() - result_pos, 0};
    status = gcomp_decoder_finish(decoder, &out_buf);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      gcomp_decoder_destroy(decoder);
      return {};
    }
    result_pos += out_buf.used;

    result.resize(result_pos);
    gcomp_decoder_destroy(decoder);
    if (status_out)
      *status_out = GCOMP_OK;
    return result;
  }

  // Helper: Decompress with 1-byte output buffer
  std::vector<uint8_t> decompressOneByteOutput(const void * data, size_t len,
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
    uint8_t single_byte;
    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};

    // Process with 1-byte output buffer
    while (in_buf.used < in_buf.size) {
      gcomp_buffer_t out_buf = {&single_byte, 1, 0};
      status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
      if (status != GCOMP_OK) {
        if (status_out)
          *status_out = status;
        gcomp_decoder_destroy(decoder);
        return {};
      }
      if (out_buf.used > 0) {
        result.push_back(single_byte);
      }
    }

    // Finish with 1-byte output buffer
    bool finished = false;
    while (!finished) {
      gcomp_buffer_t out_buf = {&single_byte, 1, 0};
      status = gcomp_decoder_finish(decoder, &out_buf);
      if (status != GCOMP_OK) {
        if (status_out)
          *status_out = status;
        gcomp_decoder_destroy(decoder);
        return {};
      }
      if (out_buf.used > 0) {
        result.push_back(single_byte);
      }
      else {
        finished = true;
      }
    }

    gcomp_decoder_destroy(decoder);
    if (status_out)
      *status_out = GCOMP_OK;
    return result;
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Decoder Creation Tests
//

TEST_F(Lz4DecoderTest, CreateSuccess) {
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(decoder, nullptr);
  gcomp_decoder_destroy(decoder);
}

TEST_F(Lz4DecoderTest, CreateWithOptions) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.concat", 1), GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "lz4", opts, &decoder);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(decoder, nullptr);

  gcomp_decoder_destroy(decoder);
  gcomp_options_destroy(opts);
}

TEST_F(Lz4DecoderTest, CreateWithLimits) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_output_bytes", 1024),
      GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 1048576),
      GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "lz4", opts, &decoder);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(decoder, nullptr);

  gcomp_decoder_destroy(decoder);
  gcomp_options_destroy(opts);
}

//
// Basic Decode Tests
//

TEST_F(Lz4DecoderTest, DecodeEmpty) {
  // Encode empty data
  auto compressed = compress(nullptr, 0);
  ASSERT_FALSE(compressed.empty());

  // Decode
  gcomp_status_t status;
  auto decoded =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  EXPECT_EQ(decoded.size(), 0u);
}

TEST_F(Lz4DecoderTest, DecodeSingleByte) {
  uint8_t input = 0x42;
  auto compressed = compress(&input, 1);
  ASSERT_FALSE(compressed.empty());

  gcomp_status_t status;
  auto decoded =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), 1u);
  EXPECT_EQ(decoded[0], input);
}

TEST_F(Lz4DecoderTest, DecodeSmallData) {
  const char * input = "Hello, LZ4 world!";
  size_t input_len = strlen(input);

  auto compressed = compress(input, input_len);
  ASSERT_FALSE(compressed.empty());

  gcomp_status_t status;
  auto decoded =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), input_len);
  EXPECT_EQ(memcmp(decoded.data(), input, input_len), 0);
}

TEST_F(Lz4DecoderTest, DecodeMediumData) {
  std::vector<uint8_t> input(10000);
  test_helpers_generate_random(input.data(), input.size(), 12345);

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  gcomp_status_t status;
  auto decoded =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), input.size());
  EXPECT_EQ(memcmp(decoded.data(), input.data(), input.size()), 0);
}

TEST_F(Lz4DecoderTest, DecodeLargeData) {
  std::vector<uint8_t> input(500000);
  test_helpers_generate_random(input.data(), input.size(), 54321);

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  gcomp_status_t status;
  auto decoded =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), input.size());
  EXPECT_EQ(memcmp(decoded.data(), input.data(), input.size()), 0);
}

//
// Decode with Checksums
//

TEST_F(Lz4DecoderTest, DecodeWithBlockChecksum) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.block_checksum", 1), GCOMP_OK);

  std::vector<uint8_t> input(5000);
  test_helpers_generate_random(input.data(), input.size(), 11111);

  auto compressed = compress(input.data(), input.size(), opts);
  ASSERT_FALSE(compressed.empty());

  gcomp_options_destroy(opts);

  // Decode without options (decoder reads flags from frame)
  gcomp_status_t status;
  auto decoded =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), input.size());
  EXPECT_EQ(memcmp(decoded.data(), input.data(), input.size()), 0);
}

TEST_F(Lz4DecoderTest, DecodeWithContentChecksum) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.content_checksum", 1), GCOMP_OK);

  std::vector<uint8_t> input(5000);
  test_helpers_generate_random(input.data(), input.size(), 22222);

  auto compressed = compress(input.data(), input.size(), opts);
  ASSERT_FALSE(compressed.empty());

  gcomp_options_destroy(opts);

  gcomp_status_t status;
  auto decoded =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), input.size());
  EXPECT_EQ(memcmp(decoded.data(), input.data(), input.size()), 0);
}

TEST_F(Lz4DecoderTest, DecodeWithBothChecksums) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.block_checksum", 1), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.content_checksum", 1), GCOMP_OK);

  std::vector<uint8_t> input(5000);
  test_helpers_generate_random(input.data(), input.size(), 33333);

  auto compressed = compress(input.data(), input.size(), opts);
  ASSERT_FALSE(compressed.empty());

  gcomp_options_destroy(opts);

  gcomp_status_t status;
  auto decoded =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), input.size());
  EXPECT_EQ(memcmp(decoded.data(), input.data(), input.size()), 0);
}

//
// Decode with Content Size
//

TEST_F(Lz4DecoderTest, DecodeWithContentSize) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "lz4.content_size", 1000), GCOMP_OK);

  std::vector<uint8_t> input(1000);
  test_helpers_generate_random(input.data(), input.size(), 44444);

  auto compressed = compress(input.data(), input.size(), opts);
  ASSERT_FALSE(compressed.empty());

  gcomp_options_destroy(opts);

  gcomp_status_t status;
  auto decoded =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), input.size());
  EXPECT_EQ(memcmp(decoded.data(), input.data(), input.size()), 0);
}

//
// Streaming Decode Tests
//

TEST_F(Lz4DecoderTest, DecodeOneByteInputChunks) {
  std::vector<uint8_t> input(500);
  test_helpers_generate_random(input.data(), input.size(), 55555);

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  gcomp_status_t status;
  auto decoded = decompressOneByteChunks(
      compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), input.size());
  EXPECT_EQ(memcmp(decoded.data(), input.data(), input.size()), 0);
}

TEST_F(Lz4DecoderTest, DecodeOneByteOutputBuffer) {
  std::vector<uint8_t> input(500);
  test_helpers_generate_random(input.data(), input.size(), 66666);

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  gcomp_status_t status;
  auto decoded = decompressOneByteOutput(
      compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), input.size());
  EXPECT_EQ(memcmp(decoded.data(), input.data(), input.size()), 0);
}

//
// Decoder Reset Tests
//

TEST_F(Lz4DecoderTest, ResetAndReuse) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder), GCOMP_OK);

  // First decode
  std::vector<uint8_t> input1(500);
  test_helpers_generate_random(input1.data(), input1.size(), 77777);
  auto compressed1 = compress(input1.data(), input1.size());

  std::vector<uint8_t> output1(1000);
  gcomp_buffer_t in1 = {compressed1.data(), compressed1.size(), 0};
  gcomp_buffer_t out1 = {output1.data(), output1.size(), 0};

  ASSERT_EQ(gcomp_decoder_update(decoder, &in1, &out1), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_finish(decoder, &out1), GCOMP_OK);

  ASSERT_EQ(out1.used, input1.size());
  EXPECT_EQ(memcmp(output1.data(), input1.data(), input1.size()), 0);

  // Reset
  ASSERT_EQ(gcomp_decoder_reset(decoder), GCOMP_OK);

  // Second decode (different data)
  std::vector<uint8_t> input2(700);
  test_helpers_generate_random(input2.data(), input2.size(), 88888);
  auto compressed2 = compress(input2.data(), input2.size());

  std::vector<uint8_t> output2(1500);
  gcomp_buffer_t in2 = {compressed2.data(), compressed2.size(), 0};
  gcomp_buffer_t out2 = {output2.data(), output2.size(), 0};

  ASSERT_EQ(gcomp_decoder_update(decoder, &in2, &out2), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_finish(decoder, &out2), GCOMP_OK);

  ASSERT_EQ(out2.used, input2.size());
  EXPECT_EQ(memcmp(output2.data(), input2.data(), input2.size()), 0);

  gcomp_decoder_destroy(decoder);
}

//
// Destroy Without Finish Tests
//

TEST_F(Lz4DecoderTest, DestroyWithoutFinish) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder), GCOMP_OK);

  std::vector<uint8_t> input(1000);
  test_helpers_generate_random(input.data(), input.size(), 99999);
  auto compressed = compress(input.data(), input.size());

  std::vector<uint8_t> output(2000);
  gcomp_buffer_t in = {compressed.data(), compressed.size() / 2, 0};
  gcomp_buffer_t out = {output.data(), output.size(), 0};

  // Partial decode
  gcomp_status_t status = gcomp_decoder_update(decoder, &in, &out);
  EXPECT_EQ(status, GCOMP_OK);

  // Destroy without finish - should not leak memory
  gcomp_decoder_destroy(decoder);
}

TEST_F(Lz4DecoderTest, DestroyImmediately) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder), GCOMP_OK);

  // Destroy immediately without any decoding
  gcomp_decoder_destroy(decoder);
}

// Note: Golden vector tests are available in data/golden_vectors.h
// These require verification with actual lz4 library output.
// The round-trip tests provide comprehensive coverage for correctness.

//
// Different Block Sizes
//

TEST_F(Lz4DecoderTest, DecodeWithAllBlockSizes) {
  std::vector<uint64_t> block_sizes = {65536, 262144, 1048576, 4194304};

  for (uint64_t block_size : block_sizes) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(
        gcomp_options_set_uint64(opts, "lz4.block_size", block_size), GCOMP_OK);

    std::vector<uint8_t> input(1000);
    test_helpers_generate_random(
        input.data(), input.size(), static_cast<uint32_t>(block_size));

    auto compressed = compress(input.data(), input.size(), opts);
    ASSERT_FALSE(compressed.empty());

    gcomp_options_destroy(opts);

    gcomp_status_t status;
    auto decoded =
        decompress(compressed.data(), compressed.size(), nullptr, &status);
    ASSERT_EQ(status, GCOMP_OK) << "Failed with block_size=" << block_size;
    ASSERT_EQ(decoded.size(), input.size());
    EXPECT_EQ(memcmp(decoded.data(), input.data(), input.size()), 0);
  }
}

//
// Independent vs Dependent Blocks
//

TEST_F(Lz4DecoderTest, DecodeIndependentBlocks) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_bool(opts, "lz4.independent_blocks", 1), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "lz4.block_size", 65536), GCOMP_OK);

  // Data larger than block size
  std::vector<uint8_t> input(150000);
  test_helpers_generate_random(input.data(), input.size(), 11111);

  auto compressed = compress(input.data(), input.size(), opts);
  ASSERT_FALSE(compressed.empty());

  gcomp_options_destroy(opts);

  gcomp_status_t status;
  auto decoded =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), input.size());
  EXPECT_EQ(memcmp(decoded.data(), input.data(), input.size()), 0);
}

TEST_F(Lz4DecoderTest, DecodeDependentBlocks) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_bool(opts, "lz4.independent_blocks", 0), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "lz4.block_size", 65536), GCOMP_OK);

  // Data larger than block size
  std::vector<uint8_t> input(150000);
  test_helpers_generate_random(input.data(), input.size(), 22222);

  auto compressed = compress(input.data(), input.size(), opts);
  ASSERT_FALSE(compressed.empty());

  gcomp_options_destroy(opts);

  gcomp_status_t status;
  auto decoded =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), input.size());
  EXPECT_EQ(memcmp(decoded.data(), input.data(), input.size()), 0);
}

//
// Various Input Patterns
//

TEST_F(Lz4DecoderTest, DecodeAllZeros) {
  std::vector<uint8_t> input(10000, 0);

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());
  EXPECT_LT(compressed.size(), input.size()) << "Zeros should compress well";

  gcomp_status_t status;
  auto decoded =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), input.size());

  for (size_t i = 0; i < decoded.size(); i++) {
    ASSERT_EQ(decoded[i], 0) << "Non-zero at position " << i;
  }
}

TEST_F(Lz4DecoderTest, DecodeRepeatingPattern) {
  std::vector<uint8_t> input(10000);
  const uint8_t pattern[] = {'A', 'B', 'C', 'D'};
  test_helpers_generate_pattern(input.data(), input.size(), pattern, 4);

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());
  EXPECT_LT(compressed.size(), input.size())
      << "Repeating pattern should compress";

  gcomp_status_t status;
  auto decoded =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), input.size());
  EXPECT_EQ(memcmp(decoded.data(), input.data(), input.size()), 0);
}

TEST_F(Lz4DecoderTest, DecodeHighEntropy) {
  // High entropy data (random) doesn't compress well
  std::vector<uint8_t> input(1000);
  test_helpers_generate_random(input.data(), input.size(), 99999);

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  gcomp_status_t status;
  auto decoded =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), input.size());
  EXPECT_EQ(memcmp(decoded.data(), input.data(), input.size()), 0);
}

//
// Match copying
//
// lz4_block_decompress() copies a match from one of two places: the bytes
// this block has already written, or the tail of the previous block carried
// over as history when blocks are linked.  A single match can start in the
// history and finish in the output.
//
// It used to ask which of those it was for every byte, and it now asks once
// and copies in runs.  Getting that wrong produces wrong bytes rather than a
// crash, and an ordinary round trip of ordinary data barely touches the
// interesting cases, so these aim at them.
//

namespace {

/// Bytes that force matches at a chosen offset, including offsets smaller
/// than the match length -- which is where the source and destination of the
/// copy overlap and the run has to stop at the distance between them.
std::vector<uint8_t> OverlappingMatchBytes(size_t total, size_t period) {
  std::vector<uint8_t> block;
  uint32_t x = 0x1234567u ^ static_cast<uint32_t>(period);
  for (size_t i = 0; i < period; i++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    block.push_back(static_cast<uint8_t>(x >> 19));
  }
  std::vector<uint8_t> v;
  v.reserve(total + period);
  while (v.size() < total) {
    v.insert(v.end(), block.begin(), block.end());
  }
  v.resize(total);
  return v;
}

} // namespace

// Repeats at every short period, which is what makes a match overlap its own
// source.  A period of one is a single byte repeated and takes its own path.
TEST_F(Lz4DecoderTest, MatchCopy_OverlappingMatches) {
  for (size_t period : {size_t(1), size_t(2), size_t(3), size_t(4), size_t(7),
           size_t(8), size_t(15), size_t(16), size_t(17), size_t(64),
           size_t(255), size_t(256)}) {
    std::vector<uint8_t> input = OverlappingMatchBytes(200000, period);
    for (int linked : {0, 1}) {
      gcomp_options_t * opts = nullptr;
      ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_bool(
                    opts, "lz4.independent_blocks", linked ? 0 : 1),
          GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_uint64(opts, "lz4.block_size", 65536u),
          GCOMP_OK);
      std::vector<uint8_t> encoded =
          compress(input.data(), input.size(), opts);
      gcomp_options_destroy(opts);
      ASSERT_FALSE(encoded.empty()) << "period " << period;

      std::vector<uint8_t> back = decompress(encoded.data(), encoded.size());
      EXPECT_EQ(back, input)
          << "period " << period << (linked ? " linked" : " independent");
    }
  }
}

// With linked blocks a match can reach back into the previous block, and one
// match can begin in that history and end in the block being decoded.  Small
// blocks and long-range repeats make that happen often.
TEST_F(Lz4DecoderTest, MatchCopy_ReachesIntoThePreviousBlock) {
  // A phrase that recurs at a distance longer than one block, so matches must
  // come from the history rather than from the block being written.
  std::vector<uint8_t> input;
  uint32_t x = 0xABCDEFu;
  auto noise = [&x]() {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return static_cast<uint8_t>(x >> 19);
  };
  std::vector<uint8_t> phrase;
  for (int i = 0; i < 700; i++) {
    phrase.push_back(noise());
  }
  for (int block = 0; block < 40; block++) {
    input.insert(input.end(), phrase.begin(), phrase.end());
    for (int i = 0; i < 900; i++) {
      input.push_back(noise());
    }
  }

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.independent_blocks", 0),
      GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "lz4.block_size", 65536u),
      GCOMP_OK);
  std::vector<uint8_t> encoded = compress(input.data(), input.size(), opts);
  gcomp_options_destroy(opts);
  ASSERT_FALSE(encoded.empty());

  std::vector<uint8_t> back = decompress(encoded.data(), encoded.size());
  EXPECT_EQ(back, input);
}

// And through output buffers small enough to cut matches in half, so the
// resume path is entered from every state.
TEST_F(Lz4DecoderTest, MatchCopy_SurvivesAnyOutputBufferSize) {
  std::vector<uint8_t> input = OverlappingMatchBytes(40000, 5);
  std::vector<uint8_t> tail = OverlappingMatchBytes(20000, 300);
  input.insert(input.end(), tail.begin(), tail.end());

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_bool(opts, "lz4.independent_blocks", 0), GCOMP_OK);
  std::vector<uint8_t> encoded = compress(input.data(), input.size(), opts);
  gcomp_options_destroy(opts);
  ASSERT_FALSE(encoded.empty());

  for (size_t chunk : {size_t(1), size_t(2), size_t(3), size_t(17),
           size_t(64), size_t(1000), size_t(65535), size_t(65536)}) {
    gcomp_decoder_t * dec = nullptr;
    ASSERT_EQ(gcomp_decoder_create(registry_, "lz4", nullptr, &dec), GCOMP_OK);
    std::vector<uint8_t> out_buf(chunk), decoded;
    gcomp_buffer_t in = {encoded.data(), encoded.size(), 0};
    size_t guard = 0;
    for (;;) {
      ASSERT_LT(++guard, input.size() * 4 + 100000u) << "chunk " << chunk;
      gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
      size_t before = in.used;
      ASSERT_EQ(gcomp_decoder_update(dec, &in, &ob), GCOMP_OK)
          << "chunk " << chunk;
      decoded.insert(decoded.end(), out_buf.data(), out_buf.data() + ob.used);
      if (ob.used == 0 && in.used == before) {
        break;
      }
    }
    gcomp_decoder_destroy(dec);
    EXPECT_EQ(decoded, input) << "chunk " << chunk;
  }
}

//
// The overshoot bound
//

/**
 * The block decoder copies in whole groups and overshoots the length it was
 * asked for, so where it may stop doing that is a bound, and `output_slack`
 * is the caller's statement of where that is.  Two callers depend on the two
 * answers: the streaming decoder stages a block in a buffer of its own and
 * hands over LZ4_DECODE_SLACK, and the parallel decoder decodes into a job
 * buffer but out of the caller's input, so it hands over what it has.
 *
 * Getting this wrong writes past an allocation rather than producing wrong
 * output, which a comparison of the decoded bytes alone would not see.  So
 * the destination is poisoned past the capacity and the poison is checked --
 * at `output_cap` when the slack is zero, and at `output_cap + slack` when it
 * is not.
 *
 * Every length in a range rather than one, because which path finishes a
 * block depends on where the block's last bytes fall relative to the group
 * width, and the data repeats at short periods so that the pattern fill runs
 * as well as the plain copies.
 */
TEST_F(Lz4DecoderTest, MatchCopy_WritesNoFurtherPastTheEndThanTheSlackAllows) {
  constexpr uint8_t kPoison = 0xA5u;
  constexpr size_t kMargin = 64u;

  std::vector<uint32_t> hash_table(65536u, 0u);

  for (size_t period : {size_t(1), size_t(3), size_t(4), size_t(8), size_t(16),
           size_t(17), size_t(40)}) {
    for (size_t n = 64u; n <= 320u; n++) {
      const std::vector<uint8_t> src = OverlappingMatchBytes(n, period);

      std::vector<uint8_t> block(n * 2u + 256u);
      size_t block_len = 0;
      std::fill(hash_table.begin(), hash_table.end(), 0u);
      const gcomp_status_t packed = lz4_block_compress(src.data(), src.size(),
          block.data(), block.size(), &block_len, hash_table.data(),
          hash_table.size());
      if (packed != GCOMP_OK) {
        continue; // Incompressible at this length; nothing to decode.
      }

      for (size_t slack : {size_t(0), size_t(LZ4_DECODE_SLACK)}) {
        std::vector<uint8_t> out(n + LZ4_DECODE_SLACK + kMargin, kPoison);
        size_t produced = 0;
        ASSERT_EQ(lz4_block_decompress(block.data(), block_len, out.data(), n,
                      slack, &produced, 0),
            GCOMP_OK)
            << "period " << period << ", length " << n << ", slack " << slack;
        ASSERT_EQ(produced, n) << "period " << period << ", length " << n;
        ASSERT_EQ(memcmp(out.data(), src.data(), n), 0)
            << "period " << period << ", length " << n << ", slack " << slack;

        for (size_t i = n + slack; i < out.size(); i++) {
          ASSERT_EQ(out[i], kPoison)
              << "period " << period << ", length " << n << ", slack " << slack
              << " wrote " << (i - n - slack + 1u)
              << " bytes past the slack it was given";
        }
      }
    }
  }
}

//
// The far edge of the match window
//

namespace {

void Put32(std::vector<uint8_t> & v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x));
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x >> 16));
  v.push_back(static_cast<uint8_t>(x >> 24));
}

/// A linked-block frame: one stored block of `history`, then one compressed
/// block that is a single match at distance `offset` followed by five
/// literals.  Built rather than compressed because no encoder emits an
/// offset it is not allowed to, and the offsets either side of the limit are
/// the whole point.
std::vector<uint8_t> LinkedFrameWithOneMatch(const std::vector<uint8_t> & history,
    uint16_t offset, uint8_t match_len, const std::vector<uint8_t> & literals) {
  std::vector<uint8_t> frame;
  Put32(frame, LZ4_MAGIC);

  const uint8_t flg = LZ4_FLG_VERSION_VALUE; // B.Indep clear: linked blocks
  const uint8_t bd = 0x40u;                  // block max size 4: 64 KB
  frame.push_back(flg);
  frame.push_back(bd);
  gcomp_xxhash32_state_t h;
  gcomp_xxhash32_reset(&h, 0);
  gcomp_xxhash32_update(&h, &frame[4], 2u);
  frame.push_back(static_cast<uint8_t>((gcomp_xxhash32_finalize(&h) >> 8) & 0xFFu));

  Put32(frame, static_cast<uint32_t>(history.size()) | LZ4_BLOCK_UNCOMPRESSED_FLAG);
  frame.insert(frame.end(), history.begin(), history.end());

  std::vector<uint8_t> block;
  block.push_back(static_cast<uint8_t>(match_len - LZ4_MIN_MATCH)); // 0 literals
  block.push_back(static_cast<uint8_t>(offset & 0xFFu));
  block.push_back(static_cast<uint8_t>(offset >> 8));
  block.push_back(static_cast<uint8_t>(literals.size() << 4)); // last sequence
  block.insert(block.end(), literals.begin(), literals.end());
  Put32(frame, static_cast<uint32_t>(block.size()));
  frame.insert(frame.end(), block.begin(), block.end());

  Put32(frame, 0u); // EndMark
  return frame;
}

} // namespace

/**
 * Offsets either side of how far back a match may legally reach.
 *
 * The history is the bytes immediately before the block being decoded, and
 * how many of them there are decides one bound: an offset up to
 * `output_position + history_size` names a byte that has been decoded, and
 * anything beyond it names one that has not.  Getting that wrong by a single
 * byte either refuses a legal stream or reads a byte nothing ever wrote --
 * and the second is inside the allocation, so no sanitizer sees it and the
 * output is merely wrong.
 *
 * So both sides are checked at every history size, with the bytes compared
 * against the format's own definition rather than against a round trip.
 * Three mutations of the arithmetic survived the whole lz4 suite before this
 * existed: a slide keeping one byte too few, one keeping one too many, and a
 * bound off by one.
 *
 * A match longer than the offset is included because it is the case that
 * used to be copied in two pieces, once from the history buffer and once
 * from the output: it now spans a boundary inside one allocation and is one
 * run, so nothing should notice it at all.
 */
TEST_F(Lz4DecoderTest, MatchCopy_OffsetsEitherSideOfTheHistoryEdge) {
  const std::vector<uint8_t> literals = {0xE0, 0xE1, 0xE2, 0xE3, 0xE4};

  for (size_t h : {size_t(1), size_t(2), size_t(15), size_t(16), size_t(17),
           size_t(500), size_t(1000)}) {
    std::vector<uint8_t> history(h);
    for (size_t i = 0; i < h; i++) {
      history[i] = static_cast<uint8_t>(i * 7u + 13u);
    }

    for (size_t d = 1; d <= h + 1u; d++) {
      // Only a few offsets matter, but the two either side of the edge
      // always do.
      if (d > 3u && d + 3u < h) {
        continue;
      }
      // Both nibbles stay below 15, so neither length carries an extension
      // byte and the block is exactly the four bytes built below.
      for (uint8_t match_len : {uint8_t(4), uint8_t(18)}) {
        const std::vector<uint8_t> frame = LinkedFrameWithOneMatch(
            history, static_cast<uint16_t>(d), match_len, literals);

        std::vector<uint8_t> out(h + 64u);
        size_t produced = 0;
        const gcomp_status_t status = gcomp_decode_buffer(registry_, "lz4",
            nullptr, frame.data(), frame.size(), out.data(), out.size(),
            &produced);

        const std::string where = "history " + std::to_string(h) + ", offset "
            + std::to_string(d) + ", match " + std::to_string(match_len);

        if (d > h) {
          EXPECT_EQ(status, GCOMP_ERR_CORRUPT)
              << where << ": reaches back further than anything decoded";
          continue;
        }

        ASSERT_EQ(status, GCOMP_OK) << where;

        // The format's definition, a byte at a time.
        std::vector<uint8_t> want = history;
        for (uint8_t i = 0; i < match_len; i++) {
          want.push_back(want[want.size() - d]);
        }
        want.insert(want.end(), literals.begin(), literals.end());

        out.resize(produced);
        EXPECT_EQ(out, want) << where;
      }
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
