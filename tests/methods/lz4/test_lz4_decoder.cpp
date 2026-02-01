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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
