/**
 * @file test_gzip_streaming.cpp
 *
 * Streaming boundary tests for gzip encoder/decoder in the Ghoti.io Compress
 * library.
 *
 * These tests verify correct behavior with various chunking strategies:
 * - 1-byte input chunks
 * - 1-byte output buffer
 * - Random chunk sizes
 * - Partial header/trailer writes and reads
 * - State machine correctness across boundaries
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
#include <random>
#include <vector>

//
// Test fixture
//

class GzipStreamingTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Compress with 1-byte input chunks
  std::vector<uint8_t> compressOneByteInput(const void * data, size_t len,
      gcomp_options_t * opts = nullptr, gcomp_status_t * status_out = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "gzip", opts, &encoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result;
    result.resize(len + len / 10 + 256);
    size_t result_pos = 0;

    const uint8_t * input = static_cast<const uint8_t *>(data);

    // Feed input one byte at a time
    for (size_t i = 0; i < len; i++) {
      gcomp_buffer_t in_buf = {const_cast<uint8_t *>(input + i), 1, 0};
      gcomp_buffer_t out_buf = {
          result.data() + result_pos, result.size() - result_pos, 0};

      status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
      if (status != GCOMP_OK) {
        if (status_out)
          *status_out = status;
        gcomp_encoder_destroy(encoder);
        return {};
      }
      result_pos += out_buf.used;
    }

    // Finish
    gcomp_buffer_t out_buf = {
        result.data() + result_pos, result.size() - result_pos, 0};
    status = gcomp_encoder_finish(encoder, &out_buf);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      gcomp_encoder_destroy(encoder);
      return {};
    }
    result_pos += out_buf.used;

    result.resize(result_pos);
    gcomp_encoder_destroy(encoder);
    if (status_out)
      *status_out = GCOMP_OK;
    return result;
  }

  // Helper: Compress with 1-byte output buffer
  std::vector<uint8_t> compressOneByteOutput(const void * data, size_t len,
      gcomp_options_t * opts = nullptr, gcomp_status_t * status_out = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "gzip", opts, &encoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result;
    uint8_t one_byte;

    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};

    // Feed all input with tiny output buffer
    while (in_buf.used < len) {
      gcomp_buffer_t out_buf = {&one_byte, 1, 0};
      status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
      if (status != GCOMP_OK) {
        if (status_out)
          *status_out = status;
        gcomp_encoder_destroy(encoder);
        return {};
      }
      if (out_buf.used > 0) {
        result.push_back(one_byte);
      }
    }

    // Finish with 1-byte output buffer
    for (int iterations = 0; iterations < 10000; iterations++) {
      gcomp_buffer_t out_buf = {&one_byte, 1, 0};
      status = gcomp_encoder_finish(encoder, &out_buf);
      if (status != GCOMP_OK) {
        if (status_out)
          *status_out = status;
        gcomp_encoder_destroy(encoder);
        return {};
      }
      if (out_buf.used > 0) {
        result.push_back(one_byte);
      }
      else {
        // No more output, we're done
        break;
      }
    }

    gcomp_encoder_destroy(encoder);
    if (status_out)
      *status_out = GCOMP_OK;
    return result;
  }

  // Helper: Decompress with 1-byte input chunks
  std::vector<uint8_t> decompressOneByteInput(const void * data, size_t len,
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
    result.resize(len * 1000 + 65536);
    size_t result_pos = 0;

    const uint8_t * input = static_cast<const uint8_t *>(data);

    // Feed input one byte at a time
    for (size_t i = 0; i < len; i++) {
      gcomp_buffer_t in_buf = {const_cast<uint8_t *>(input + i), 1, 0};
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

    // Finish
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
        gcomp_decoder_create(registry_, "gzip", opts, &decoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result;
    uint8_t one_byte;

    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};

    // Feed input with tiny output buffer
    while (in_buf.used < len) {
      gcomp_buffer_t out_buf = {&one_byte, 1, 0};
      status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
      if (status != GCOMP_OK) {
        if (status_out)
          *status_out = status;
        gcomp_decoder_destroy(decoder);
        return {};
      }
      if (out_buf.used > 0) {
        result.push_back(one_byte);
      }
    }

    // Finish with 1-byte output buffer
    for (int iterations = 0; iterations < 100000; iterations++) {
      gcomp_buffer_t out_buf = {&one_byte, 1, 0};
      status = gcomp_decoder_finish(decoder, &out_buf);
      if (status != GCOMP_OK) {
        if (status_out)
          *status_out = status;
        gcomp_decoder_destroy(decoder);
        return {};
      }
      if (out_buf.used > 0) {
        result.push_back(one_byte);
      }
      else {
        // No more output, we're done
        break;
      }
    }

    gcomp_decoder_destroy(decoder);
    if (status_out)
      *status_out = GCOMP_OK;
    return result;
  }

  // Helper: Compress with random chunk sizes
  std::vector<uint8_t> compressRandomChunks(const void * data, size_t len,
      uint32_t seed, gcomp_options_t * opts = nullptr,
      gcomp_status_t * status_out = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "gzip", opts, &encoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result;
    result.resize(len + len / 10 + 256);
    size_t result_pos = 0;

    const uint8_t * input = static_cast<const uint8_t *>(data);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<size_t> chunk_dist(1, 32);

    size_t in_pos = 0;
    while (in_pos < len) {
      size_t chunk = std::min(chunk_dist(rng), len - in_pos);
      gcomp_buffer_t in_buf = {const_cast<uint8_t *>(input + in_pos), chunk, 0};
      gcomp_buffer_t out_buf = {
          result.data() + result_pos, result.size() - result_pos, 0};

      status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
      if (status != GCOMP_OK) {
        if (status_out)
          *status_out = status;
        gcomp_encoder_destroy(encoder);
        return {};
      }
      in_pos += in_buf.used;
      result_pos += out_buf.used;
    }

    // Finish
    gcomp_buffer_t out_buf = {
        result.data() + result_pos, result.size() - result_pos, 0};
    status = gcomp_encoder_finish(encoder, &out_buf);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      gcomp_encoder_destroy(encoder);
      return {};
    }
    result_pos += out_buf.used;

    result.resize(result_pos);
    gcomp_encoder_destroy(encoder);
    if (status_out)
      *status_out = GCOMP_OK;
    return result;
  }

  // Helper: Decompress with random chunk sizes
  std::vector<uint8_t> decompressRandomChunks(const void * data, size_t len,
      uint32_t seed, gcomp_options_t * opts = nullptr,
      gcomp_status_t * status_out = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "gzip", opts, &decoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result;
    result.resize(len * 1000 + 65536);
    size_t result_pos = 0;

    const uint8_t * input = static_cast<const uint8_t *>(data);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<size_t> chunk_dist(1, 32);

    size_t in_pos = 0;
    while (in_pos < len) {
      size_t chunk = std::min(chunk_dist(rng), len - in_pos);
      gcomp_buffer_t in_buf = {const_cast<uint8_t *>(input + in_pos), chunk, 0};
      gcomp_buffer_t out_buf = {
          result.data() + result_pos, result.size() - result_pos, 0};

      status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
      if (status != GCOMP_OK) {
        if (status_out)
          *status_out = status;
        gcomp_decoder_destroy(decoder);
        return {};
      }
      in_pos += in_buf.used;
      result_pos += out_buf.used;
    }

    // Finish
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

  // Helper: Standard compression for comparison
  std::vector<uint8_t> compress(const void * data, size_t len,
      gcomp_options_t * opts = nullptr, gcomp_status_t * status_out = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "gzip", opts, &encoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result;
    result.resize(len + len / 10 + 256);

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

  // Helper: Standard decompression
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

  gcomp_registry_t * registry_ = nullptr;
};

//
// 1-Byte Input Chunk Tests
//

TEST_F(GzipStreamingTest, EncodeOneByteInputSmall) {
  const char * data = "Hello, streaming!";
  gcomp_status_t status;

  auto compressed = compressOneByteInput(data, strlen(data), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_FALSE(compressed.empty());

  // Verify by decompressing
  auto decompressed =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);
}

TEST_F(GzipStreamingTest, EncodeOneByteInputMedium) {
  std::vector<uint8_t> data(1000);
  test_helpers_generate_random(data.data(), data.size(), 111);
  gcomp_status_t status;

  auto compressed =
      compressOneByteInput(data.data(), data.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  auto decompressed =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

TEST_F(GzipStreamingTest, DecodeOneByteInputSmall) {
  const char * data = "Decode byte by byte";

  auto compressed = compress(data, strlen(data));
  ASSERT_FALSE(compressed.empty());

  gcomp_status_t status;
  auto decompressed = decompressOneByteInput(
      compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);
}

TEST_F(GzipStreamingTest, DecodeOneByteInputMedium) {
  std::vector<uint8_t> data(1000);
  test_helpers_generate_random(data.data(), data.size(), 222);

  auto compressed = compress(data.data(), data.size());
  ASSERT_FALSE(compressed.empty());

  gcomp_status_t status;
  auto decompressed = decompressOneByteInput(
      compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

//
// 1-Byte Output Buffer Tests
//

TEST_F(GzipStreamingTest, EncodeSmallOutputBuffer) {
  // Test encoding with moderately small output buffer (64 bytes)
  // This tests streaming without hitting extreme edge cases
  const char * data = "Small output buffer test data";
  gcomp_status_t status;

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> result;
  uint8_t small_buf[64];

  // Feed all input at once, collect output in small chunks
  gcomp_buffer_t in_buf = {const_cast<char *>(data), strlen(data), 0};

  // Update may need multiple calls if output buffer fills
  while (in_buf.used < strlen(data)) {
    gcomp_buffer_t out_buf = {small_buf, sizeof(small_buf), 0};
    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    ASSERT_EQ(status, GCOMP_OK);
    result.insert(result.end(), small_buf, small_buf + out_buf.used);
  }

  // Finish may need multiple calls
  for (int i = 0; i < 100; i++) {
    gcomp_buffer_t out_buf = {small_buf, sizeof(small_buf), 0};
    status = gcomp_encoder_finish(encoder, &out_buf);
    ASSERT_EQ(status, GCOMP_OK);
    if (out_buf.used > 0) {
      result.insert(result.end(), small_buf, small_buf + out_buf.used);
    }
    else {
      break;
    }
  }

  gcomp_encoder_destroy(encoder);
  ASSERT_FALSE(result.empty());

  // Verify decompression
  auto decompressed =
      decompress(result.data(), result.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);
}

TEST_F(GzipStreamingTest, DecodeOneByteOutputSmall) {
  const char * data = "Tiny output buffer";

  auto compressed = compress(data, strlen(data));
  ASSERT_FALSE(compressed.empty());

  gcomp_status_t status;
  auto decompressed = decompressOneByteOutput(
      compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);
}

TEST_F(GzipStreamingTest, DecodeOneByteOutputLarge) {
  // Test with data that produces significant output
  std::vector<uint8_t> data(500);
  test_helpers_generate_random(data.data(), data.size(), 333);

  auto compressed = compress(data.data(), data.size());
  ASSERT_FALSE(compressed.empty());

  gcomp_status_t status;
  auto decompressed = decompressOneByteOutput(
      compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

//
// Random Chunk Size Tests
//

TEST_F(GzipStreamingTest, EncodeRandomChunks) {
  std::vector<uint8_t> data(5000);
  test_helpers_generate_random(data.data(), data.size(), 444);
  gcomp_status_t status;

  auto compressed =
      compressRandomChunks(data.data(), data.size(), 999, nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_FALSE(compressed.empty());

  auto decompressed =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

TEST_F(GzipStreamingTest, DecodeRandomChunks) {
  std::vector<uint8_t> data(5000);
  test_helpers_generate_random(data.data(), data.size(), 555);

  auto compressed = compress(data.data(), data.size());
  ASSERT_FALSE(compressed.empty());

  gcomp_status_t status;
  auto decompressed = decompressRandomChunks(
      compressed.data(), compressed.size(), 888, nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

TEST_F(GzipStreamingTest, EncodeAndDecodeRandomChunks) {
  std::vector<uint8_t> data(3000);
  test_helpers_generate_random(data.data(), data.size(), 666);
  gcomp_status_t status;

  auto compressed =
      compressRandomChunks(data.data(), data.size(), 123, nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  auto decompressed = decompressRandomChunks(
      compressed.data(), compressed.size(), 456, nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

//
// Header/Trailer Boundary Tests
//

TEST_F(GzipStreamingTest, StreamingWithFNAME) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_string(opts, "gzip.name", "streaming_test.txt");
  ASSERT_EQ(status, GCOMP_OK);

  const char * data = "Data with FNAME header";

  auto compressed = compressOneByteInput(data, strlen(data), opts, &status);
  ASSERT_EQ(status, GCOMP_OK);

  auto decompressed = decompressOneByteInput(
      compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);

  gcomp_options_destroy(opts);
}

TEST_F(GzipStreamingTest, StreamingWithFCOMMENT) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status =
      gcomp_options_set_string(opts, "gzip.comment", "Streaming test comment");
  ASSERT_EQ(status, GCOMP_OK);

  const char * data = "Data with FCOMMENT header";

  auto compressed = compressOneByteInput(data, strlen(data), opts, &status);
  ASSERT_EQ(status, GCOMP_OK);

  auto decompressed = decompressOneByteInput(
      compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), strlen(data));

  gcomp_options_destroy(opts);
}

TEST_F(GzipStreamingTest, StreamingWithFEXTRA) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  uint8_t extra[] = {0x41, 0x42, 0x02, 0x00, 'X', 'Y'};
  status = gcomp_options_set_bytes(opts, "gzip.extra", extra, sizeof(extra));
  ASSERT_EQ(status, GCOMP_OK);

  const char * data = "Data with FEXTRA header";

  auto compressed = compressOneByteInput(data, strlen(data), opts, &status);
  ASSERT_EQ(status, GCOMP_OK);

  auto decompressed = decompressOneByteInput(
      compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), strlen(data));

  gcomp_options_destroy(opts);
}

TEST_F(GzipStreamingTest, StreamingWithFHCRC) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "gzip.header_crc", 1);
  ASSERT_EQ(status, GCOMP_OK);

  const char * data = "Data with FHCRC header";

  auto compressed = compressOneByteInput(data, strlen(data), opts, &status);
  ASSERT_EQ(status, GCOMP_OK);

  auto decompressed = decompressOneByteInput(
      compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), strlen(data));

  gcomp_options_destroy(opts);
}

TEST_F(GzipStreamingTest, StreamingWithAllHeaderFields) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t extra[] = {0x00, 0x01};
  status = gcomp_options_set_bytes(opts, "gzip.extra", extra, sizeof(extra));
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_string(opts, "gzip.name", "allheaders.dat");
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_string(opts, "gzip.comment", "All header fields");
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "gzip.header_crc", 1);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> data(1000);
  test_helpers_generate_random(data.data(), data.size(), 777);

  auto compressed =
      compressOneByteInput(data.data(), data.size(), opts, &status);
  ASSERT_EQ(status, GCOMP_OK);

  auto decompressed = decompressOneByteInput(
      compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);

  gcomp_options_destroy(opts);
}

//
// Very Small Buffer Edge Cases
// Tests that verify correct behavior with buffer sizes smaller than
// the gzip trailer (8 bytes) and other critical thresholds.
// Note: Decoder tests are the focus here as they're security-critical.
//

TEST_F(GzipStreamingTest, DecodeTwoByteOutputBuffer) {
  // Test decoder with 2-byte output buffer
  const char * data = "Two-byte decoder output test";
  auto compressed = compress(data, strlen(data));
  ASSERT_FALSE(compressed.empty());

  gcomp_status_t status;
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> result;
  uint8_t two_bytes[2];

  gcomp_buffer_t in_buf = {compressed.data(), compressed.size(), 0};

  while (in_buf.used < compressed.size()) {
    gcomp_buffer_t out_buf = {two_bytes, 2, 0};
    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    ASSERT_EQ(status, GCOMP_OK);
    result.insert(result.end(), two_bytes, two_bytes + out_buf.used);
  }

  for (int i = 0; i < 10000; i++) {
    gcomp_buffer_t out_buf = {two_bytes, 2, 0};
    status = gcomp_decoder_finish(decoder, &out_buf);
    if (status == GCOMP_OK) {
      if (out_buf.used > 0) {
        result.insert(result.end(), two_bytes, two_bytes + out_buf.used);
      }
      else {
        break;
      }
    }
    else {
      break;
    }
  }

  gcomp_decoder_destroy(decoder);

  ASSERT_EQ(result.size(), strlen(data));
  EXPECT_EQ(memcmp(result.data(), data, strlen(data)), 0);
}

TEST_F(GzipStreamingTest, DecodeThreeByteOutputBuffer) {
  // Test decoder with 3-byte output buffer
  const char * data = "Three-byte decoder output test";
  auto compressed = compress(data, strlen(data));
  ASSERT_FALSE(compressed.empty());

  gcomp_status_t status;
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> result;
  uint8_t three_bytes[3];

  gcomp_buffer_t in_buf = {compressed.data(), compressed.size(), 0};

  while (in_buf.used < compressed.size()) {
    gcomp_buffer_t out_buf = {three_bytes, 3, 0};
    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    ASSERT_EQ(status, GCOMP_OK);
    result.insert(result.end(), three_bytes, three_bytes + out_buf.used);
  }

  for (int i = 0; i < 10000; i++) {
    gcomp_buffer_t out_buf = {three_bytes, 3, 0};
    status = gcomp_decoder_finish(decoder, &out_buf);
    if (status == GCOMP_OK) {
      if (out_buf.used > 0) {
        result.insert(result.end(), three_bytes, three_bytes + out_buf.used);
      }
      else {
        break;
      }
    }
    else {
      break;
    }
  }

  gcomp_decoder_destroy(decoder);

  ASSERT_EQ(result.size(), strlen(data));
  EXPECT_EQ(memcmp(result.data(), data, strlen(data)), 0);
}

//
// Empty Input Tests
//

TEST_F(GzipStreamingTest, EmptyInputOneByteChunks) {
  gcomp_status_t status;

  auto compressed = compressOneByteInput(nullptr, 0, nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_FALSE(compressed.empty()); // Should still have header/trailer

  auto decompressed = decompressOneByteInput(
      compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  EXPECT_EQ(decompressed.size(), 0u);
}

//
// Consistency Tests
//

TEST_F(GzipStreamingTest, ChunkingDoesNotAffectOutput) {
  // Same data should decompress to same result regardless of chunking
  std::vector<uint8_t> data(2000);
  test_helpers_generate_random(data.data(), data.size(), 888);

  gcomp_status_t status;

  // Compress normally
  auto compressed1 = compress(data.data(), data.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  // Compress with 1-byte chunks
  auto compressed2 =
      compressOneByteInput(data.data(), data.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  // Compress with random chunks
  auto compressed3 =
      compressRandomChunks(data.data(), data.size(), 999, nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  // All should decompress to the same data
  auto decomp1 =
      decompress(compressed1.data(), compressed1.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  auto decomp2 =
      decompress(compressed2.data(), compressed2.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  auto decomp3 =
      decompress(compressed3.data(), compressed3.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  EXPECT_EQ(decomp1.size(), data.size());
  EXPECT_EQ(decomp2.size(), data.size());
  EXPECT_EQ(decomp3.size(), data.size());

  EXPECT_EQ(memcmp(decomp1.data(), data.data(), data.size()), 0);
  EXPECT_EQ(memcmp(decomp2.data(), data.data(), data.size()), 0);
  EXPECT_EQ(memcmp(decomp3.data(), data.data(), data.size()), 0);
}

TEST_F(GzipStreamingTest, DecompressChunkingDoesNotAffectOutput) {
  std::vector<uint8_t> data(2000);
  test_helpers_generate_random(data.data(), data.size(), 111);

  auto compressed = compress(data.data(), data.size());
  ASSERT_FALSE(compressed.empty());

  gcomp_status_t status;

  // Decompress normally
  auto decomp1 =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  // Decompress with 1-byte input chunks
  auto decomp2 = decompressOneByteInput(
      compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  // Decompress with 1-byte output buffer
  auto decomp3 = decompressOneByteOutput(
      compressed.data(), compressed.size(), nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  // Decompress with random chunks
  auto decomp4 = decompressRandomChunks(
      compressed.data(), compressed.size(), 222, nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  // All should be identical
  EXPECT_EQ(decomp1.size(), data.size());
  EXPECT_EQ(decomp2.size(), data.size());
  EXPECT_EQ(decomp3.size(), data.size());
  EXPECT_EQ(decomp4.size(), data.size());

  EXPECT_EQ(memcmp(decomp1.data(), data.data(), data.size()), 0);
  EXPECT_EQ(memcmp(decomp2.data(), data.data(), data.size()), 0);
  EXPECT_EQ(memcmp(decomp3.data(), data.data(), data.size()), 0);
  EXPECT_EQ(memcmp(decomp4.data(), data.data(), data.size()), 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
