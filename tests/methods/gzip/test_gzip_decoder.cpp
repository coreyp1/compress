/**
 * @file test_gzip_decoder.cpp
 *
 * Unit tests for the gzip decoder in the Ghoti.io Compress library.
 *
 * These tests verify:
 * - Decoder creation and destruction
 * - Basic decoding functionality
 * - Decoding with various header options (FNAME, FCOMMENT, etc.)
 * - Streaming with various buffer sizes
 * - Decoder reset and reuse
 * - Error handling for malformed data
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

class GzipDecoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Compress data
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

  // Helper: Decompress data
  std::vector<uint8_t> decompress(const void * data, size_t len,
      gcomp_options_t * opts = nullptr, gcomp_status_t * status_out = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "gzip", opts, &decoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result;
    // For highly compressible data (e.g., zeros), expansion can be 1000x+
    // But cap at 16MB to avoid huge allocations for large compressed inputs.
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
    if (status_out)
      *status_out = status;
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

TEST_F(GzipDecoderTest, CreateSuccess) {
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(decoder, nullptr);

  gcomp_decoder_destroy(decoder);
}

TEST_F(GzipDecoderTest, CreateWithOptions) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_bool(opts, "gzip.concat", 1);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", opts, &decoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(decoder, nullptr);

  gcomp_decoder_destroy(decoder);
  gcomp_options_destroy(opts);
}

TEST_F(GzipDecoderTest, CreateFailsWithoutDeflate) {
  // Create a fresh registry without deflate
  gcomp_registry_t * empty_reg = nullptr;
  gcomp_status_t status = gcomp_registry_create(nullptr, &empty_reg);
  ASSERT_EQ(status, GCOMP_OK);

  // Register only gzip
  status = gcomp_method_gzip_register(empty_reg);
  EXPECT_EQ(status, GCOMP_OK);

  // Try to create decoder - should fail
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(empty_reg, "gzip", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(decoder, nullptr);

  gcomp_registry_destroy(empty_reg);
}

//
// Basic Decoding Tests
//

TEST_F(GzipDecoderTest, DecodeEmpty) {
  // Compress empty data
  std::vector<uint8_t> compressed = compress(nullptr, 0);
  ASSERT_FALSE(compressed.empty());

  // Decompress
  std::vector<uint8_t> decompressed =
      decompress(compressed.data(), compressed.size());
  EXPECT_TRUE(decompressed.empty());
}

TEST_F(GzipDecoderTest, DecodeSmall) {
  const char * test_data = "Hello, World!";
  std::vector<uint8_t> compressed = compress(test_data, strlen(test_data));
  ASSERT_FALSE(compressed.empty());

  std::vector<uint8_t> decompressed =
      decompress(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), strlen(test_data));
  EXPECT_EQ(memcmp(decompressed.data(), test_data, strlen(test_data)), 0);
}

TEST_F(GzipDecoderTest, DecodeLarge) {
  // Create 1 MB of test data
  std::vector<uint8_t> original(1024 * 1024);
  test_helpers_generate_random(original.data(), original.size(), 54321);

  std::vector<uint8_t> compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  std::vector<uint8_t> decompressed =
      decompress(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), original.size());
  EXPECT_TRUE(test_helpers_buffers_equal(original.data(), original.size(),
      decompressed.data(), decompressed.size()));
}

//
// Header Options Tests
//

TEST_F(GzipDecoderTest, DecodeWithFNAME) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_string(opts, "gzip.name", "document.txt");
  ASSERT_EQ(status, GCOMP_OK);

  const char * test_data = "Data with filename header";
  std::vector<uint8_t> compressed =
      compress(test_data, strlen(test_data), opts);
  ASSERT_FALSE(compressed.empty());

  // Verify FNAME flag
  EXPECT_TRUE(compressed[3] & 0x08);

  // Decode
  std::vector<uint8_t> decompressed =
      decompress(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), strlen(test_data));
  EXPECT_EQ(memcmp(decompressed.data(), test_data, strlen(test_data)), 0);

  gcomp_options_destroy(opts);
}

TEST_F(GzipDecoderTest, DecodeWithFCOMMENT) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status =
      gcomp_options_set_string(opts, "gzip.comment", "This is a test comment");
  ASSERT_EQ(status, GCOMP_OK);

  const char * test_data = "Data with comment header";
  std::vector<uint8_t> compressed =
      compress(test_data, strlen(test_data), opts);
  ASSERT_FALSE(compressed.empty());

  // Verify FCOMMENT flag
  EXPECT_TRUE(compressed[3] & 0x10);

  // Decode
  std::vector<uint8_t> decompressed =
      decompress(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), strlen(test_data));
  EXPECT_EQ(memcmp(decompressed.data(), test_data, strlen(test_data)), 0);

  gcomp_options_destroy(opts);
}

TEST_F(GzipDecoderTest, DecodeWithAllOptions) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t extra[] = {0x12, 0x34};
  status = gcomp_options_set_bytes(opts, "gzip.extra", extra, sizeof(extra));
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_string(opts, "gzip.name", "allopt.bin");
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_string(opts, "gzip.comment", "All options test");
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "gzip.header_crc", 1);
  ASSERT_EQ(status, GCOMP_OK);

  const char * test_data = "Data with all optional header fields";
  std::vector<uint8_t> compressed =
      compress(test_data, strlen(test_data), opts);
  ASSERT_FALSE(compressed.empty());

  // Verify all flags
  uint8_t flg = compressed[3];
  EXPECT_TRUE(flg & 0x02); // FHCRC
  EXPECT_TRUE(flg & 0x04); // FEXTRA
  EXPECT_TRUE(flg & 0x08); // FNAME
  EXPECT_TRUE(flg & 0x10); // FCOMMENT

  // Decode
  std::vector<uint8_t> decompressed =
      decompress(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), strlen(test_data));
  EXPECT_EQ(memcmp(decompressed.data(), test_data, strlen(test_data)), 0);

  gcomp_options_destroy(opts);
}

//
// Streaming Tests
//

TEST_F(GzipDecoderTest, StreamingOneByteInput) {
  const char * test_data = "Test data for byte-by-byte decoding stream.";
  std::vector<uint8_t> compressed = compress(test_data, strlen(test_data));
  ASSERT_FALSE(compressed.empty());

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> decompressed(strlen(test_data) + 256);
  size_t dec_pos = 0;

  // Feed one byte at a time
  for (size_t i = 0; i < compressed.size(); i++) {
    gcomp_buffer_t in_buf = {compressed.data() + i, 1, 0};
    gcomp_buffer_t out_buf = {
        decompressed.data() + dec_pos, decompressed.size() - dec_pos, 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    EXPECT_EQ(status, GCOMP_OK);
    dec_pos += out_buf.used;
  }

  // Finish
  gcomp_buffer_t final_out = {
      decompressed.data() + dec_pos, decompressed.size() - dec_pos, 0};
  status = gcomp_decoder_finish(decoder, &final_out);
  EXPECT_EQ(status, GCOMP_OK);
  dec_pos += final_out.used;

  gcomp_decoder_destroy(decoder);

  // Verify
  EXPECT_EQ(dec_pos, strlen(test_data));
  EXPECT_EQ(memcmp(decompressed.data(), test_data, strlen(test_data)), 0);
}

TEST_F(GzipDecoderTest, StreamingOneByteOutput) {
  const char * test_data = "Test for 1-byte output chunks during decode.";
  std::vector<uint8_t> compressed = compress(test_data, strlen(test_data));
  ASSERT_FALSE(compressed.empty());

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> decompressed(strlen(test_data) + 256);
  size_t dec_pos = 0;

  gcomp_buffer_t in_buf = {compressed.data(), compressed.size(), 0};

  // Decode with 1-byte output buffer
  while (in_buf.used < in_buf.size || dec_pos == 0) {
    uint8_t byte;
    gcomp_buffer_t out_buf = {&byte, 1, 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    EXPECT_EQ(status, GCOMP_OK);
    if (out_buf.used > 0) {
      decompressed[dec_pos++] = byte;
    }
    if (in_buf.used >= in_buf.size && out_buf.used == 0) {
      break;
    }
  }

  // Finish
  gcomp_buffer_t final_out = {
      decompressed.data() + dec_pos, decompressed.size() - dec_pos, 0};
  status = gcomp_decoder_finish(decoder, &final_out);
  EXPECT_EQ(status, GCOMP_OK);
  dec_pos += final_out.used;

  gcomp_decoder_destroy(decoder);

  // Verify
  EXPECT_EQ(dec_pos, strlen(test_data));
  EXPECT_EQ(memcmp(decompressed.data(), test_data, strlen(test_data)), 0);
}

//
// Reset Tests
//

TEST_F(GzipDecoderTest, ResetAndReuse) {
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // First decode
  const char * data1 = "First decode operation";
  std::vector<uint8_t> comp1 = compress(data1, strlen(data1));
  ASSERT_FALSE(comp1.empty());

  uint8_t output1[256];
  gcomp_buffer_t in1 = {comp1.data(), comp1.size(), 0};
  gcomp_buffer_t out1 = {output1, sizeof(output1), 0};

  status = gcomp_decoder_update(decoder, &in1, &out1);
  EXPECT_EQ(status, GCOMP_OK);
  status = gcomp_decoder_finish(decoder, &out1);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(out1.used, strlen(data1));
  EXPECT_EQ(memcmp(output1, data1, strlen(data1)), 0);

  // Reset
  status = gcomp_decoder_reset(decoder);
  EXPECT_EQ(status, GCOMP_OK);

  // Second decode
  const char * data2 = "Second decode operation after reset";
  std::vector<uint8_t> comp2 = compress(data2, strlen(data2));
  ASSERT_FALSE(comp2.empty());

  uint8_t output2[256];
  gcomp_buffer_t in2 = {comp2.data(), comp2.size(), 0};
  gcomp_buffer_t out2 = {output2, sizeof(output2), 0};

  status = gcomp_decoder_update(decoder, &in2, &out2);
  EXPECT_EQ(status, GCOMP_OK);
  status = gcomp_decoder_finish(decoder, &out2);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(out2.used, strlen(data2));
  EXPECT_EQ(memcmp(output2, data2, strlen(data2)), 0);

  gcomp_decoder_destroy(decoder);
}

TEST_F(GzipDecoderTest, ResetMidStream) {
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Start decoding some data
  const char * data1 = "Partial decode";
  std::vector<uint8_t> comp1 = compress(data1, strlen(data1));
  ASSERT_FALSE(comp1.empty());

  // Only feed part of the compressed data
  size_t partial_len = comp1.size() / 2;
  uint8_t output[256];
  gcomp_buffer_t in_partial = {comp1.data(), partial_len, 0};
  gcomp_buffer_t out_partial = {output, sizeof(output), 0};

  status = gcomp_decoder_update(decoder, &in_partial, &out_partial);
  EXPECT_EQ(status, GCOMP_OK);

  // Reset mid-stream
  status = gcomp_decoder_reset(decoder);
  EXPECT_EQ(status, GCOMP_OK);

  // Decode different data completely
  const char * data2 = "Complete new decode after reset";
  std::vector<uint8_t> comp2 = compress(data2, strlen(data2));
  ASSERT_FALSE(comp2.empty());

  gcomp_buffer_t in2 = {comp2.data(), comp2.size(), 0};
  gcomp_buffer_t out2 = {output, sizeof(output), 0};

  status = gcomp_decoder_update(decoder, &in2, &out2);
  EXPECT_EQ(status, GCOMP_OK);
  status = gcomp_decoder_finish(decoder, &out2);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(out2.used, strlen(data2));
  EXPECT_EQ(memcmp(output, data2, strlen(data2)), 0);

  gcomp_decoder_destroy(decoder);
}

//
// Destroy Tests
//

TEST_F(GzipDecoderTest, DestroyWithoutFinish) {
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Feed some compressed data but don't finish
  const char * data = "Incomplete decode";
  std::vector<uint8_t> compressed = compress(data, strlen(data));
  ASSERT_FALSE(compressed.empty());

  uint8_t output[256];
  gcomp_buffer_t in_buf = {compressed.data(), compressed.size() / 2, 0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // Destroy without calling finish - should not leak or crash
  gcomp_decoder_destroy(decoder);
}

TEST_F(GzipDecoderTest, DestroyNull) {
  // Should handle NULL gracefully
  gcomp_decoder_destroy(nullptr);
}

//
// Error Handling Tests
//

TEST_F(GzipDecoderTest, ErrorInvalidMagic) {
  uint8_t bad_data[] = {
      0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x03, 0x00};

  gcomp_status_t status;
  std::vector<uint8_t> result =
      decompress(bad_data, sizeof(bad_data), nullptr, &status);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
  EXPECT_TRUE(result.empty());
}

TEST_F(GzipDecoderTest, ErrorUnsupportedCM) {
  // Valid magic, wrong compression method
  uint8_t bad_data[] = {
      0x1F, 0x8B, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF};

  gcomp_status_t status;
  std::vector<uint8_t> result =
      decompress(bad_data, sizeof(bad_data), nullptr, &status);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_TRUE(result.empty());
}

TEST_F(GzipDecoderTest, ErrorReservedFLGBits) {
  // Valid magic/CM, reserved FLG bits set
  uint8_t bad_data[] = {
      0x1F, 0x8B, 0x08, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF};

  gcomp_status_t status;
  std::vector<uint8_t> result =
      decompress(bad_data, sizeof(bad_data), nullptr, &status);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
  EXPECT_TRUE(result.empty());
}

TEST_F(GzipDecoderTest, ErrorTruncatedHeader) {
  // Only partial header
  uint8_t truncated[] = {0x1F, 0x8B, 0x08, 0x00, 0x12};

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_buffer_t in_buf = {truncated, sizeof(truncated), 0};
  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  // Update may succeed (consumed what it could)
  if (status == GCOMP_OK) {
    status = gcomp_decoder_finish(decoder, &out_buf);
    EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
  }

  gcomp_decoder_destroy(decoder);
}

TEST_F(GzipDecoderTest, ErrorCRCMismatch) {
  const char * data = "Test data for CRC corruption check";
  std::vector<uint8_t> compressed = compress(data, strlen(data));
  ASSERT_FALSE(compressed.empty());

  // Corrupt the CRC (last 8 bytes, first 4 are CRC)
  size_t crc_offset = compressed.size() - 8;
  compressed[crc_offset] ^= 0xFF;

  gcomp_status_t status;
  std::vector<uint8_t> result =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipDecoderTest, ErrorISIZEMismatch) {
  const char * data = "Test data for ISIZE corruption check";
  std::vector<uint8_t> compressed = compress(data, strlen(data));
  ASSERT_FALSE(compressed.empty());

  // Corrupt the ISIZE (last 4 bytes)
  size_t isize_offset = compressed.size() - 4;
  compressed[isize_offset] ^= 0xFF;

  gcomp_status_t status;
  std::vector<uint8_t> result =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipDecoderTest, ErrorFNAMEExceedsLimit) {
  gcomp_options_t * enc_opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&enc_opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Use a long filename
  std::string long_name(100, 'x');
  status = gcomp_options_set_string(enc_opts, "gzip.name", long_name.c_str());
  ASSERT_EQ(status, GCOMP_OK);

  const char * data = "Data";
  std::vector<uint8_t> compressed = compress(data, strlen(data), enc_opts);
  ASSERT_FALSE(compressed.empty());
  gcomp_options_destroy(enc_opts);

  // Now try to decode with a small limit
  gcomp_options_t * dec_opts = nullptr;
  status = gcomp_options_create(&dec_opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_uint64(dec_opts, "gzip.max_name_bytes", 10);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_status_t dec_status;
  std::vector<uint8_t> result =
      decompress(compressed.data(), compressed.size(), dec_opts, &dec_status);
  EXPECT_EQ(dec_status, GCOMP_ERR_LIMIT);

  gcomp_options_destroy(dec_opts);
}

//
// Concatenated Members Test
//

TEST_F(GzipDecoderTest, ConcatenatedMembersEnabled) {
  // Create two separate gzip streams
  const char * data1 = "First member data";
  const char * data2 = "Second member data";

  std::vector<uint8_t> comp1 = compress(data1, strlen(data1));
  std::vector<uint8_t> comp2 = compress(data2, strlen(data2));
  ASSERT_FALSE(comp1.empty());
  ASSERT_FALSE(comp2.empty());

  // Concatenate them
  std::vector<uint8_t> concat;
  concat.insert(concat.end(), comp1.begin(), comp1.end());
  concat.insert(concat.end(), comp2.begin(), comp2.end());

  // Decode with concat enabled
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_bool(opts, "gzip.concat", 1);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> decompressed =
      decompress(concat.data(), concat.size(), opts);

  // Should get both members' data
  size_t expected_len = strlen(data1) + strlen(data2);
  EXPECT_EQ(decompressed.size(), expected_len);

  // Verify content
  EXPECT_EQ(memcmp(decompressed.data(), data1, strlen(data1)), 0);
  EXPECT_EQ(
      memcmp(decompressed.data() + strlen(data1), data2, strlen(data2)), 0);

  gcomp_options_destroy(opts);
}

TEST_F(GzipDecoderTest, ConcatenatedMembersDisabledByDefault) {
  // Create two separate gzip streams
  const char * data1 = "First member";
  const char * data2 = "Second member";

  std::vector<uint8_t> comp1 = compress(data1, strlen(data1));
  std::vector<uint8_t> comp2 = compress(data2, strlen(data2));
  ASSERT_FALSE(comp1.empty());
  ASSERT_FALSE(comp2.empty());

  // Concatenate them
  std::vector<uint8_t> concat;
  concat.insert(concat.end(), comp1.begin(), comp1.end());
  concat.insert(concat.end(), comp2.begin(), comp2.end());

  // Decode with default options (concat disabled)
  // Current behavior: decoder stops after first member
  std::vector<uint8_t> decompressed = decompress(concat.data(), concat.size());

  // Should only get first member's data
  EXPECT_EQ(decompressed.size(), strlen(data1));
  EXPECT_EQ(memcmp(decompressed.data(), data1, strlen(data1)), 0);
}

//
// Data Pattern Tests
//

TEST_F(GzipDecoderTest, DecodeRepeatingPattern) {
  std::vector<uint8_t> original(64 * 1024);
  uint8_t pattern[] = {0xDE, 0xAD, 0xBE, 0xEF};
  test_helpers_generate_pattern(original.data(), original.size(), pattern, 4);

  std::vector<uint8_t> compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  std::vector<uint8_t> decompressed =
      decompress(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), original.size());
  EXPECT_TRUE(test_helpers_buffers_equal(original.data(), original.size(),
      decompressed.data(), decompressed.size()));
}

TEST_F(GzipDecoderTest, DecodeSequential) {
  std::vector<uint8_t> original(32 * 1024);
  test_helpers_generate_sequential(original.data(), original.size());

  std::vector<uint8_t> compressed = compress(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  std::vector<uint8_t> decompressed =
      decompress(compressed.data(), compressed.size());
  EXPECT_EQ(decompressed.size(), original.size());
  EXPECT_TRUE(test_helpers_buffers_equal(original.data(), original.size(),
      decompressed.data(), decompressed.size()));
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
