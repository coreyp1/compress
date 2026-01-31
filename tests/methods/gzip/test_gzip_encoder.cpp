/**
 * @file test_gzip_encoder.cpp
 *
 * Unit tests for the gzip encoder in the Ghoti.io Compress library.
 *
 * These tests verify:
 * - Encoder creation and destruction
 * - Basic encoding functionality
 * - Encoding with various options (FNAME, FCOMMENT, etc.)
 * - Streaming with various buffer sizes
 * - Encoder reset and reuse
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

class GzipEncoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Compress data and return compressed bytes
  std::vector<uint8_t> compress(
      const void * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "gzip", opts, &encoder);
    if (status != GCOMP_OK) {
      return {};
    }

    std::vector<uint8_t> result;
    // Worst-case: truly random data can expand by ~3-5% in deflate due to
    // Huffman encoding overhead. Use len * 1.10 + 1024 for safety.
    result.resize(len + len / 10 + 1024);

    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t out_buf = {result.data(), result.size(), 0};

    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    if (status != GCOMP_OK) {
      gcomp_encoder_destroy(encoder);
      return {};
    }

    status = gcomp_encoder_finish(encoder, &out_buf);
    if (status != GCOMP_OK) {
      gcomp_encoder_destroy(encoder);
      return {};
    }

    result.resize(out_buf.used);
    gcomp_encoder_destroy(encoder);
    return result;
  }

  // Helper: Decompress data and return decompressed bytes
  std::vector<uint8_t> decompress(
      const void * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "gzip", opts, &decoder);
    if (status != GCOMP_OK) {
      return {};
    }

    std::vector<uint8_t> result;
    // For highly compressible data (e.g., zeros), expansion can be 1000x+
    // But cap at 16MB to avoid huge allocations for large compressed inputs.
    // For random data, the actual decompressed size will be similar to input.
    size_t max_expansion = len * 1000 + 65536;
    size_t capped_size =
        max_expansion < 16 * 1024 * 1024 ? max_expansion : 16 * 1024 * 1024;
    result.resize(capped_size);

    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t out_buf = {result.data(), result.size(), 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    if (status != GCOMP_OK) {
      gcomp_decoder_destroy(decoder);
      return {};
    }

    status = gcomp_decoder_finish(decoder, &out_buf);
    if (status != GCOMP_OK) {
      gcomp_decoder_destroy(decoder);
      return {};
    }

    result.resize(out_buf.used);
    gcomp_decoder_destroy(decoder);
    return result;
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Creation Tests
//

TEST_F(GzipEncoderTest, CreateSuccess) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  gcomp_encoder_destroy(encoder);
}

TEST_F(GzipEncoderTest, CreateWithOptions) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_int64(opts, "deflate.level", 6);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "gzip", opts, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

TEST_F(GzipEncoderTest, CreateFailsWithoutDeflate) {
  // Create a fresh registry without deflate
  gcomp_registry_t * empty_reg = nullptr;
  gcomp_status_t status = gcomp_registry_create(nullptr, &empty_reg);
  ASSERT_EQ(status, GCOMP_OK);

  // Register only gzip
  status = gcomp_method_gzip_register(empty_reg);
  EXPECT_EQ(status, GCOMP_OK);

  // Try to create encoder - should fail
  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(empty_reg, "gzip", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(encoder, nullptr);

  gcomp_registry_destroy(empty_reg);
}

//
// Basic Encoding Tests
//

TEST_F(GzipEncoderTest, EncodeEmpty) {
  // Encode empty data
  std::vector<uint8_t> compressed = compress(nullptr, 0);
  EXPECT_FALSE(compressed.empty());

  // Verify it has header (at least 10 bytes) + deflate empty + trailer (8
  // bytes)
  EXPECT_GE(compressed.size(), 18u);

  // Verify magic
  EXPECT_EQ(compressed[0], 0x1F);
  EXPECT_EQ(compressed[1], 0x8B);
}

TEST_F(GzipEncoderTest, EncodeSmall) {
  const char * test_data = "Hello, World!";
  std::vector<uint8_t> compressed = compress(test_data, strlen(test_data));
  EXPECT_FALSE(compressed.empty());

  // Decompress and verify
  std::vector<uint8_t> decompressed =
      decompress(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), strlen(test_data));
  EXPECT_EQ(memcmp(decompressed.data(), test_data, strlen(test_data)), 0);
}

TEST_F(GzipEncoderTest, EncodeLarge) {
  // Create 1 MB of test data
  std::vector<uint8_t> input(1024 * 1024);
  test_helpers_generate_random(input.data(), input.size(), 12345);

  std::vector<uint8_t> compressed = compress(input.data(), input.size());
  EXPECT_FALSE(compressed.empty());

  // Decompress and verify
  std::vector<uint8_t> decompressed =
      decompress(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), input.size());
  EXPECT_TRUE(test_helpers_buffers_equal(
      input.data(), input.size(), decompressed.data(), decompressed.size()));
}

//
// Options Tests
//

TEST_F(GzipEncoderTest, EncodeWithFNAME) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_string(opts, "gzip.name", "testfile.txt");
  ASSERT_EQ(status, GCOMP_OK);

  const char * test_data = "Data with filename";
  std::vector<uint8_t> compressed =
      compress(test_data, strlen(test_data), opts);
  EXPECT_FALSE(compressed.empty());

  // Verify FNAME flag is set
  EXPECT_TRUE(compressed[3] & 0x08);

  // Decompress and verify
  std::vector<uint8_t> decompressed =
      decompress(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), strlen(test_data));
  EXPECT_EQ(memcmp(decompressed.data(), test_data, strlen(test_data)), 0);

  gcomp_options_destroy(opts);
}

TEST_F(GzipEncoderTest, EncodeWithFCOMMENT) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_string(opts, "gzip.comment", "This is a comment");
  ASSERT_EQ(status, GCOMP_OK);

  const char * test_data = "Data with comment";
  std::vector<uint8_t> compressed =
      compress(test_data, strlen(test_data), opts);
  EXPECT_FALSE(compressed.empty());

  // Verify FCOMMENT flag is set
  EXPECT_TRUE(compressed[3] & 0x10);

  // Decompress and verify
  std::vector<uint8_t> decompressed =
      decompress(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), strlen(test_data));

  gcomp_options_destroy(opts);
}

TEST_F(GzipEncoderTest, EncodeWithAllOptions) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Set all gzip options
  uint8_t extra[] = {0xAB, 0xCD};
  status = gcomp_options_set_bytes(opts, "gzip.extra", extra, sizeof(extra));
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_string(opts, "gzip.name", "file.dat");
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_string(opts, "gzip.comment", "All options");
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "gzip.header_crc", 1);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "gzip.mtime", 1234567890);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "gzip.os", 3); // Unix
  ASSERT_EQ(status, GCOMP_OK);

  const char * test_data = "Full options test data";
  std::vector<uint8_t> compressed =
      compress(test_data, strlen(test_data), opts);
  EXPECT_FALSE(compressed.empty());

  // Verify all flags are set
  uint8_t flg = compressed[3];
  EXPECT_TRUE(flg & 0x02); // FHCRC
  EXPECT_TRUE(flg & 0x04); // FEXTRA
  EXPECT_TRUE(flg & 0x08); // FNAME
  EXPECT_TRUE(flg & 0x10); // FCOMMENT

  // Decompress and verify
  std::vector<uint8_t> decompressed =
      decompress(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), strlen(test_data));
  EXPECT_EQ(memcmp(decompressed.data(), test_data, strlen(test_data)), 0);

  gcomp_options_destroy(opts);
}

TEST_F(GzipEncoderTest, EncodeWithDifferentLevels) {
  const char * test_data =
      "This is test data that will be compressed at different levels.";
  size_t data_len = strlen(test_data);

  for (int level = 0; level <= 9; level++) {
    gcomp_options_t * opts = nullptr;
    gcomp_status_t status = gcomp_options_create(&opts);
    ASSERT_EQ(status, GCOMP_OK);

    status = gcomp_options_set_int64(opts, "deflate.level", level);
    ASSERT_EQ(status, GCOMP_OK);

    std::vector<uint8_t> compressed = compress(test_data, data_len, opts);
    EXPECT_FALSE(compressed.empty()) << "Failed at level " << level;

    // Decompress and verify
    std::vector<uint8_t> decompressed =
        decompress(compressed.data(), compressed.size());
    EXPECT_EQ(decompressed.size(), data_len) << "Failed at level " << level;
    EXPECT_EQ(memcmp(decompressed.data(), test_data, data_len), 0)
        << "Failed at level " << level;

    gcomp_options_destroy(opts);
  }
}

//
// Streaming Tests
//

TEST_F(GzipEncoderTest, StreamingOneByteInput) {
  const char * test_data = "Streaming test data for one byte input chunks.";
  size_t data_len = strlen(test_data);

  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> compressed(data_len + 1024);
  size_t comp_pos = 0;

  // Feed one byte at a time
  for (size_t i = 0; i < data_len; i++) {
    gcomp_buffer_t in_buf = {const_cast<char *>(test_data + i), 1, 0};
    gcomp_buffer_t out_buf = {
        compressed.data() + comp_pos, compressed.size() - comp_pos, 0};

    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    EXPECT_EQ(status, GCOMP_OK);
    comp_pos += out_buf.used;
  }

  // Finish
  gcomp_buffer_t final_out = {
      compressed.data() + comp_pos, compressed.size() - comp_pos, 0};
  status = gcomp_encoder_finish(encoder, &final_out);
  EXPECT_EQ(status, GCOMP_OK);
  comp_pos += final_out.used;

  compressed.resize(comp_pos);
  gcomp_encoder_destroy(encoder);

  // Decompress and verify
  std::vector<uint8_t> decompressed =
      decompress(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), data_len);
  EXPECT_EQ(memcmp(decompressed.data(), test_data, data_len), 0);
}

TEST_F(GzipEncoderTest, StreamingOneByteOutput) {
  const char * test_data = "Small data for 1-byte output test.";
  size_t data_len = strlen(test_data);

  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> compressed(data_len + 1024);
  size_t comp_pos = 0;

  // Feed all input, but only allow 1 byte output at a time
  gcomp_buffer_t in_buf = {const_cast<char *>(test_data), data_len, 0};

  while (in_buf.used < in_buf.size) {
    uint8_t byte;
    gcomp_buffer_t out_buf = {&byte, 1, 0};

    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    EXPECT_EQ(status, GCOMP_OK);
    if (out_buf.used > 0) {
      compressed[comp_pos++] = byte;
    }
  }

  // Finish with 1-byte output chunks
  // Note: finish may return GCOMP_ERR_LIMIT when there's more data but the
  // output buffer is full. Keep calling until GCOMP_OK with no output.
  bool done = false;
  while (!done) {
    uint8_t byte;
    gcomp_buffer_t out_buf = {&byte, 1, 0};

    status = gcomp_encoder_finish(encoder, &out_buf);
    // Accept either OK (done or partial) or LIMIT (more data waiting)
    EXPECT_TRUE(status == GCOMP_OK || status == GCOMP_ERR_LIMIT);

    if (out_buf.used > 0) {
      compressed[comp_pos++] = byte;
    }

    // Done when we get GCOMP_OK and no output was written
    if (status == GCOMP_OK && out_buf.used == 0) {
      done = true;
    }
  }

  compressed.resize(comp_pos);
  gcomp_encoder_destroy(encoder);

  // Decompress and verify
  std::vector<uint8_t> decompressed =
      decompress(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), data_len);
  EXPECT_EQ(memcmp(decompressed.data(), test_data, data_len), 0);
}

//
// Reset Tests
//

TEST_F(GzipEncoderTest, ResetAndReuse) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  const char * test_data1 = "First compression run";
  const char * test_data2 = "Second compression run after reset";

  // First compression
  std::vector<uint8_t> compressed1(strlen(test_data1) + 1024);
  gcomp_buffer_t in1 = {const_cast<char *>(test_data1), strlen(test_data1), 0};
  gcomp_buffer_t out1 = {compressed1.data(), compressed1.size(), 0};

  status = gcomp_encoder_update(encoder, &in1, &out1);
  EXPECT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &out1);
  EXPECT_EQ(status, GCOMP_OK);
  compressed1.resize(out1.used);

  // Reset
  status = gcomp_encoder_reset(encoder);
  EXPECT_EQ(status, GCOMP_OK);

  // Second compression
  std::vector<uint8_t> compressed2(strlen(test_data2) + 1024);
  gcomp_buffer_t in2 = {const_cast<char *>(test_data2), strlen(test_data2), 0};
  gcomp_buffer_t out2 = {compressed2.data(), compressed2.size(), 0};

  status = gcomp_encoder_update(encoder, &in2, &out2);
  EXPECT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &out2);
  EXPECT_EQ(status, GCOMP_OK);
  compressed2.resize(out2.used);

  gcomp_encoder_destroy(encoder);

  // Verify both decompress correctly
  std::vector<uint8_t> decomp1 =
      decompress(compressed1.data(), compressed1.size());
  EXPECT_EQ(decomp1.size(), strlen(test_data1));
  EXPECT_EQ(memcmp(decomp1.data(), test_data1, strlen(test_data1)), 0);

  std::vector<uint8_t> decomp2 =
      decompress(compressed2.data(), compressed2.size());
  EXPECT_EQ(decomp2.size(), strlen(test_data2));
  EXPECT_EQ(memcmp(decomp2.data(), test_data2, strlen(test_data2)), 0);
}

TEST_F(GzipEncoderTest, ResetMidStream) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Start encoding some data
  const char * partial_data = "Partial data";
  uint8_t output[256];
  gcomp_buffer_t in_buf = {
      const_cast<char *>(partial_data), strlen(partial_data), 0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // Reset mid-stream
  status = gcomp_encoder_reset(encoder);
  EXPECT_EQ(status, GCOMP_OK);

  // Encode different data
  const char * new_data = "New data after reset";
  gcomp_buffer_t in_buf2 = {const_cast<char *>(new_data), strlen(new_data), 0};
  gcomp_buffer_t out_buf2 = {output, sizeof(output), 0};

  status = gcomp_encoder_update(encoder, &in_buf2, &out_buf2);
  EXPECT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &out_buf2);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_encoder_destroy(encoder);

  // Verify the output decompresses to new_data (not partial_data)
  std::vector<uint8_t> decomp = decompress(output, out_buf2.used);
  EXPECT_EQ(decomp.size(), strlen(new_data));
  EXPECT_EQ(memcmp(decomp.data(), new_data, strlen(new_data)), 0);
}

//
// Destroy Tests
//

TEST_F(GzipEncoderTest, DestroyWithoutFinish) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Feed some data
  const char * test_data = "Data that won't be finished";
  uint8_t output[256];
  gcomp_buffer_t in_buf = {const_cast<char *>(test_data), strlen(test_data), 0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // Destroy without calling finish - should not leak or crash
  gcomp_encoder_destroy(encoder);
  // If we get here without crashing or valgrind issues, test passes
}

TEST_F(GzipEncoderTest, DestroyNull) {
  // Should handle NULL gracefully
  gcomp_encoder_destroy(nullptr);
  // If we get here without crashing, test passes
}

//
// Data Pattern Tests
//

TEST_F(GzipEncoderTest, EncodeRepeatingPattern) {
  // Highly compressible data
  std::vector<uint8_t> input(64 * 1024);
  uint8_t pattern[] = {0xAA, 0xBB, 0xCC, 0xDD};
  test_helpers_generate_pattern(input.data(), input.size(), pattern, 4);

  std::vector<uint8_t> compressed = compress(input.data(), input.size());
  EXPECT_FALSE(compressed.empty());

  // Should compress well
  EXPECT_LT(compressed.size(), input.size() / 10);

  // Decompress and verify
  std::vector<uint8_t> decompressed =
      decompress(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), input.size());
  EXPECT_TRUE(test_helpers_buffers_equal(
      input.data(), input.size(), decompressed.data(), decompressed.size()));
}

TEST_F(GzipEncoderTest, EncodeZeros) {
  std::vector<uint8_t> input(32 * 1024);
  test_helpers_generate_zeros(input.data(), input.size());

  std::vector<uint8_t> compressed = compress(input.data(), input.size());
  EXPECT_FALSE(compressed.empty());

  // Should compress very well
  EXPECT_LT(compressed.size(), input.size() / 100);

  // Decompress and verify
  std::vector<uint8_t> decompressed =
      decompress(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), input.size());
  EXPECT_TRUE(test_helpers_buffers_equal(
      input.data(), input.size(), decompressed.data(), decompressed.size()));
}

TEST_F(GzipEncoderTest, EncodeHighEntropy) {
  // Random data (doesn't compress well)
  std::vector<uint8_t> input(16 * 1024);
  test_helpers_generate_random(input.data(), input.size(), 98765);

  std::vector<uint8_t> compressed = compress(input.data(), input.size());
  EXPECT_FALSE(compressed.empty());

  // May expand slightly due to headers and no compression benefit
  EXPECT_LT(compressed.size(), input.size() + 1024);

  // Decompress and verify
  std::vector<uint8_t> decompressed =
      decompress(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), input.size());
  EXPECT_TRUE(test_helpers_buffers_equal(
      input.data(), input.size(), decompressed.data(), decompressed.size()));
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
