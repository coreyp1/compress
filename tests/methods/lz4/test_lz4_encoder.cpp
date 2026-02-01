/**
 * @file test_lz4_encoder.cpp
 *
 * Encoder-specific tests for LZ4 in the Ghoti.io Compress library.
 *
 * These tests verify:
 * - Encoder creation and configuration
 * - Block checksum functionality
 * - Content checksum functionality
 * - Content size in header
 * - Different block sizes
 * - Streaming with small buffers (1-byte input/output)
 * - Reset and reuse
 * - Destroy without finish
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
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

class Lz4EncoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Try to decode a buffer and verify it works
  std::vector<uint8_t> tryDecode(
      const void * data, size_t len, gcomp_status_t * status_out = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "lz4", nullptr, &decoder);
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
    gcomp_decoder_destroy(decoder);
    if (status_out)
      *status_out = status;
    if (status != GCOMP_OK) {
      return {};
    }

    result.resize(out_buf.used);
    return result;
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Encoder Creation Tests
//

TEST_F(Lz4EncoderTest, CreateSuccess) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "lz4", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);
  gcomp_encoder_destroy(encoder);
}

TEST_F(Lz4EncoderTest, CreateWithOptions) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_bool(opts, "lz4.block_checksum", true);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

//
// Basic Encoding Tests
//

TEST_F(Lz4EncoderTest, BasicEncode) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "lz4", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  const char test_data[] = "Hello, LZ4 compression test!";
  std::vector<uint8_t> output(256);

  gcomp_buffer_t in_buf = {const_cast<char *>(test_data), strlen(test_data), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  EXPECT_GT(out_buf.used, 0u);

  output.resize(out_buf.used);
  gcomp_encoder_destroy(encoder);

  // Verify by decoding
  gcomp_status_t dec_status;
  std::vector<uint8_t> decoded =
      tryDecode(output.data(), output.size(), &dec_status);
  EXPECT_EQ(dec_status, GCOMP_OK);
  EXPECT_EQ(decoded.size(), strlen(test_data));
  EXPECT_EQ(memcmp(decoded.data(), test_data, decoded.size()), 0);
}

TEST_F(Lz4EncoderTest, EncodeEmpty) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "lz4", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> output(256);

  gcomp_buffer_t in_buf = {nullptr, 0, 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  // Empty input
  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  output.resize(out_buf.used);
  gcomp_encoder_destroy(encoder);

  // Verify it produces a valid empty frame
  gcomp_status_t dec_status;
  std::vector<uint8_t> decoded =
      tryDecode(output.data(), output.size(), &dec_status);
  EXPECT_EQ(dec_status, GCOMP_OK);
  EXPECT_EQ(decoded.size(), 0u);
}

//
// Block Checksum Tests
//

TEST_F(Lz4EncoderTest, EncodeWithBlockChecksum) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.block_checksum", true);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  const char test_data[] = "Block checksum test data here!";
  std::vector<uint8_t> output(256);

  gcomp_buffer_t in_buf = {const_cast<char *>(test_data), strlen(test_data), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  output.resize(out_buf.used);
  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);

  // Verify block checksum flag is set (byte 4, bit 4)
  EXPECT_TRUE(output[4] & 0x10) << "Block checksum flag should be set";

  // Verify by decoding
  gcomp_status_t dec_status;
  std::vector<uint8_t> decoded =
      tryDecode(output.data(), output.size(), &dec_status);
  EXPECT_EQ(dec_status, GCOMP_OK);
  EXPECT_EQ(decoded.size(), strlen(test_data));
}

//
// Content Checksum Tests
//

TEST_F(Lz4EncoderTest, EncodeWithContentChecksum) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.content_checksum", true);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  const char test_data[] = "Content checksum test data!";
  std::vector<uint8_t> output(256);

  gcomp_buffer_t in_buf = {const_cast<char *>(test_data), strlen(test_data), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  output.resize(out_buf.used);
  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);

  // Verify content checksum flag is set (byte 4, bit 2)
  EXPECT_TRUE(output[4] & 0x04) << "Content checksum flag should be set";

  // Verify by decoding
  gcomp_status_t dec_status;
  std::vector<uint8_t> decoded =
      tryDecode(output.data(), output.size(), &dec_status);
  EXPECT_EQ(dec_status, GCOMP_OK);
  EXPECT_EQ(decoded.size(), strlen(test_data));
}

//
// Content Size in Header Tests
//

TEST_F(Lz4EncoderTest, EncodeWithContentSizeInHeader) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "lz4.content_size", 32);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  char test_data[32];
  memset(test_data, 'X', sizeof(test_data));
  std::vector<uint8_t> output(256);

  gcomp_buffer_t in_buf = {test_data, sizeof(test_data), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  output.resize(out_buf.used);
  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);

  // Verify content size flag is set (byte 4, bit 3)
  EXPECT_TRUE(output[4] & 0x08) << "Content size flag should be set";

  // Verify by decoding
  gcomp_status_t dec_status;
  std::vector<uint8_t> decoded =
      tryDecode(output.data(), output.size(), &dec_status);
  EXPECT_EQ(dec_status, GCOMP_OK);
  EXPECT_EQ(decoded.size(), sizeof(test_data));
}

//
// Block Size Tests
//

TEST_F(Lz4EncoderTest, EncodeWithBlockSize64KB) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 65536);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> test_data(1000);
  test_helpers_generate_random(test_data.data(), test_data.size(), 12345);

  std::vector<uint8_t> output(2000);
  gcomp_buffer_t in_buf = {test_data.data(), test_data.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  output.resize(out_buf.used);
  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);

  // Verify by decoding
  gcomp_status_t dec_status;
  std::vector<uint8_t> decoded =
      tryDecode(output.data(), output.size(), &dec_status);
  EXPECT_EQ(dec_status, GCOMP_OK);
  EXPECT_EQ(decoded.size(), test_data.size());
  EXPECT_EQ(memcmp(decoded.data(), test_data.data(), test_data.size()), 0);
}

TEST_F(Lz4EncoderTest, EncodeWithBlockSize256KB) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 262144);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> test_data(1000);
  test_helpers_generate_random(test_data.data(), test_data.size(), 67890);

  std::vector<uint8_t> output(2000);
  gcomp_buffer_t in_buf = {test_data.data(), test_data.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  output.resize(out_buf.used);
  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);

  // Verify by decoding
  gcomp_status_t dec_status;
  std::vector<uint8_t> decoded =
      tryDecode(output.data(), output.size(), &dec_status);
  EXPECT_EQ(dec_status, GCOMP_OK);
  EXPECT_EQ(decoded.size(), test_data.size());
}

TEST_F(Lz4EncoderTest, EncodeWithBlockSize1MB) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 1048576);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> test_data(1000);
  test_helpers_generate_random(test_data.data(), test_data.size(), 11111);

  std::vector<uint8_t> output(2000);
  gcomp_buffer_t in_buf = {test_data.data(), test_data.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  output.resize(out_buf.used);
  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);

  // Verify by decoding
  gcomp_status_t dec_status;
  std::vector<uint8_t> decoded =
      tryDecode(output.data(), output.size(), &dec_status);
  EXPECT_EQ(dec_status, GCOMP_OK);
  EXPECT_EQ(decoded.size(), test_data.size());
}

TEST_F(Lz4EncoderTest, EncodeWithBlockSize4MB) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 4194304);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> test_data(1000);
  test_helpers_generate_random(test_data.data(), test_data.size(), 22222);

  std::vector<uint8_t> output(2000);
  gcomp_buffer_t in_buf = {test_data.data(), test_data.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  output.resize(out_buf.used);
  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);

  // Verify by decoding
  gcomp_status_t dec_status;
  std::vector<uint8_t> decoded =
      tryDecode(output.data(), output.size(), &dec_status);
  EXPECT_EQ(dec_status, GCOMP_OK);
  EXPECT_EQ(decoded.size(), test_data.size());
}

//
// Streaming Tests - Small Buffers
//

TEST_F(Lz4EncoderTest, EncodeOneByteInputChunks) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "lz4", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  const char test_data[] = "Test data for one byte input chunks!";
  size_t test_len = strlen(test_data);
  std::vector<uint8_t> output(512);
  size_t output_pos = 0;

  // Feed one byte at a time
  for (size_t i = 0; i < test_len; i++) {
    gcomp_buffer_t in_buf = {const_cast<char *>(&test_data[i]), 1, 0};
    gcomp_buffer_t out_buf = {
        output.data() + output_pos, output.size() - output_pos, 0};

    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    ASSERT_EQ(status, GCOMP_OK);
    ASSERT_EQ(in_buf.used, 1u);
    output_pos += out_buf.used;
  }

  gcomp_buffer_t out_buf = {
      output.data() + output_pos, output.size() - output_pos, 0};
  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  output_pos += out_buf.used;

  output.resize(output_pos);
  gcomp_encoder_destroy(encoder);

  // Verify by decoding
  gcomp_status_t dec_status;
  std::vector<uint8_t> decoded =
      tryDecode(output.data(), output.size(), &dec_status);
  EXPECT_EQ(dec_status, GCOMP_OK);
  EXPECT_EQ(decoded.size(), test_len);
  EXPECT_EQ(memcmp(decoded.data(), test_data, test_len), 0);
}

TEST_F(Lz4EncoderTest, EncodeSmallOutputBuffer) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "lz4", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  const char test_data[] = "Small output buffer test!";
  std::vector<uint8_t> output;
  // Use buffer large enough for header/trailer but small enough to test
  // incremental output. The encoder requires space for complete blocks during
  // update(), but can output header/end mark/trailer incrementally during
  // finish().
  uint8_t chunk[16];

  gcomp_buffer_t in_buf = {const_cast<char *>(test_data), strlen(test_data), 0};

  // Process with small output buffer
  while (in_buf.used < in_buf.size) {
    gcomp_buffer_t out_buf = {chunk, sizeof(chunk), 0};
    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    ASSERT_EQ(status, GCOMP_OK);
    output.insert(output.end(), chunk, chunk + out_buf.used);
  }

  // Finish with small output buffer - call multiple times to drain output
  bool finished = false;
  size_t prev_total = 0;
  for (int iterations = 0; iterations < 100 && !finished; iterations++) {
    gcomp_buffer_t out_buf = {chunk, sizeof(chunk), 0};
    status = gcomp_encoder_finish(encoder, &out_buf);
    ASSERT_EQ(status, GCOMP_OK);
    output.insert(output.end(), chunk, chunk + out_buf.used);

    // Check if done (two consecutive calls with no output means we're done)
    if (out_buf.used == 0 && output.size() == prev_total) {
      finished = true;
    }
    prev_total = output.size();
  }
  ASSERT_TRUE(finished) << "Encoder did not finish within iteration limit";

  gcomp_encoder_destroy(encoder);

  // Verify by decoding
  gcomp_status_t dec_status;
  std::vector<uint8_t> decoded =
      tryDecode(output.data(), output.size(), &dec_status);
  EXPECT_EQ(dec_status, GCOMP_OK);
  EXPECT_EQ(decoded.size(), strlen(test_data));
  EXPECT_EQ(memcmp(decoded.data(), test_data, decoded.size()), 0);
}

//
// Reset Tests
//

TEST_F(Lz4EncoderTest, ResetAndReuse) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "lz4", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // First encoding
  const char test1[] = "First test data";
  std::vector<uint8_t> output1(256);
  gcomp_buffer_t in1 = {const_cast<char *>(test1), strlen(test1), 0};
  gcomp_buffer_t out1 = {output1.data(), output1.size(), 0};

  status = gcomp_encoder_update(encoder, &in1, &out1);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &out1);
  ASSERT_EQ(status, GCOMP_OK);
  output1.resize(out1.used);

  // Reset encoder
  status = gcomp_encoder_reset(encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Second encoding (different data)
  const char test2[] = "Second test data with more content";
  std::vector<uint8_t> output2(256);
  gcomp_buffer_t in2 = {const_cast<char *>(test2), strlen(test2), 0};
  gcomp_buffer_t out2 = {output2.data(), output2.size(), 0};

  status = gcomp_encoder_update(encoder, &in2, &out2);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &out2);
  ASSERT_EQ(status, GCOMP_OK);
  output2.resize(out2.used);

  gcomp_encoder_destroy(encoder);

  // Verify both streams decode correctly
  gcomp_status_t dec_status;
  std::vector<uint8_t> decoded1 =
      tryDecode(output1.data(), output1.size(), &dec_status);
  EXPECT_EQ(dec_status, GCOMP_OK);
  EXPECT_EQ(decoded1.size(), strlen(test1));
  EXPECT_EQ(memcmp(decoded1.data(), test1, decoded1.size()), 0);

  std::vector<uint8_t> decoded2 =
      tryDecode(output2.data(), output2.size(), &dec_status);
  EXPECT_EQ(dec_status, GCOMP_OK);
  EXPECT_EQ(decoded2.size(), strlen(test2));
  EXPECT_EQ(memcmp(decoded2.data(), test2, decoded2.size()), 0);
}

//
// Destroy Without Finish Tests
//

TEST_F(Lz4EncoderTest, DestroyWithoutFinish) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "lz4", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  const char test_data[] = "Data that will not be finished";
  std::vector<uint8_t> output(256);
  gcomp_buffer_t in_buf = {const_cast<char *>(test_data), strlen(test_data), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  // Destroy without calling finish - should not crash or leak
  gcomp_encoder_destroy(encoder);
}

TEST_F(Lz4EncoderTest, DestroyImmediately) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "lz4", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Destroy immediately without any operations
  gcomp_encoder_destroy(encoder);
}

//
// Combined Options Tests
//

TEST_F(Lz4EncoderTest, EncodeWithAllChecksums) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.block_checksum", true);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.content_checksum", true);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  const char test_data[] = "Both checksums enabled test!";
  std::vector<uint8_t> output(256);

  gcomp_buffer_t in_buf = {const_cast<char *>(test_data), strlen(test_data), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  output.resize(out_buf.used);
  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);

  // Verify both flags are set
  EXPECT_TRUE(output[4] & 0x10) << "Block checksum flag should be set";
  EXPECT_TRUE(output[4] & 0x04) << "Content checksum flag should be set";

  // Verify by decoding
  gcomp_status_t dec_status;
  std::vector<uint8_t> decoded =
      tryDecode(output.data(), output.size(), &dec_status);
  EXPECT_EQ(dec_status, GCOMP_OK);
  EXPECT_EQ(decoded.size(), strlen(test_data));
}

TEST_F(Lz4EncoderTest, EncodeWithDependentBlocks) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.independent_blocks", false);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  const char test_data[] = "Dependent blocks test data!";
  std::vector<uint8_t> output(256);

  gcomp_buffer_t in_buf = {const_cast<char *>(test_data), strlen(test_data), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  output.resize(out_buf.used);
  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);

  // Verify independence flag is NOT set (byte 4, bit 5)
  EXPECT_FALSE(output[4] & 0x20) << "Block independence flag should NOT be set";

  // Verify by decoding
  gcomp_status_t dec_status;
  std::vector<uint8_t> decoded =
      tryDecode(output.data(), output.size(), &dec_status);
  EXPECT_EQ(dec_status, GCOMP_OK);
  EXPECT_EQ(decoded.size(), strlen(test_data));
}

//
// Large Data Tests
//

TEST_F(Lz4EncoderTest, EncodeLargeData) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "lz4", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Generate 100KB of random data
  std::vector<uint8_t> test_data(100 * 1024);
  test_helpers_generate_random(test_data.data(), test_data.size(), 33333);

  std::vector<uint8_t> output(test_data.size() + 1024);
  gcomp_buffer_t in_buf = {test_data.data(), test_data.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  output.resize(out_buf.used);
  gcomp_encoder_destroy(encoder);

  // Verify by decoding
  gcomp_status_t dec_status;
  std::vector<uint8_t> decoded =
      tryDecode(output.data(), output.size(), &dec_status);
  EXPECT_EQ(dec_status, GCOMP_OK);
  EXPECT_EQ(decoded.size(), test_data.size());
  EXPECT_EQ(memcmp(decoded.data(), test_data.data(), test_data.size()), 0);
}

TEST_F(Lz4EncoderTest, EncodeHighlyCompressibleData) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "lz4", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Generate 100KB of zeros (highly compressible)
  std::vector<uint8_t> test_data(100 * 1024);
  test_helpers_generate_zeros(test_data.data(), test_data.size());

  std::vector<uint8_t> output(test_data.size() + 1024);
  gcomp_buffer_t in_buf = {test_data.data(), test_data.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  output.resize(out_buf.used);
  gcomp_encoder_destroy(encoder);

  // Compressed size should be much smaller
  EXPECT_LT(output.size(), test_data.size() / 10)
      << "Compression ratio should be high for zeros";

  // Verify by decoding
  gcomp_status_t dec_status;
  std::vector<uint8_t> decoded =
      tryDecode(output.data(), output.size(), &dec_status);
  EXPECT_EQ(dec_status, GCOMP_OK);
  EXPECT_EQ(decoded.size(), test_data.size());
  EXPECT_EQ(memcmp(decoded.data(), test_data.data(), test_data.size()), 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
