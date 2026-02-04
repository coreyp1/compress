/**
 * @file test_lz4_streaming.cpp
 *
 * Streaming behavior tests for LZ4 encoder/decoder in the Ghoti.io Compress
 * library.
 *
 * These tests verify:
 * - State transitions across chunk boundaries
 * - 1-byte input chunk handling
 * - 1-byte output buffer handling
 * - Partial header/block/trailer handling
 * - Concatenated frame support
 * - State machine correctness
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

class Lz4StreamingTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Compress data in one shot (for creating test data)
  std::vector<uint8_t> compressOneShot(const void * data, size_t len,
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

  // Helper: Decompress with chunked input
  std::vector<uint8_t> decompressChunked(const void * data, size_t len,
      size_t chunk_size, gcomp_options_t * opts = nullptr,
      gcomp_status_t * status_out = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "lz4", opts, &decoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result;
    result.resize(len * 1000 + 65536); // Allow for expansion
    size_t result_pos = 0;

    const uint8_t * src = static_cast<const uint8_t *>(data);
    size_t src_pos = 0;

    // Process input in chunks
    while (src_pos < len) {
      size_t this_chunk = std::min(chunk_size, len - src_pos);

      gcomp_buffer_t in_buf = {
          const_cast<uint8_t *>(src + src_pos), this_chunk, 0};
      gcomp_buffer_t out_buf = {
          result.data() + result_pos, result.size() - result_pos, 0};

      status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
      if (status != GCOMP_OK) {
        if (status_out)
          *status_out = status;
        gcomp_decoder_destroy(decoder);
        return {};
      }

      src_pos += in_buf.used;
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

  // Helper: Compress with chunked input
  std::vector<uint8_t> compressChunked(const void * data, size_t len,
      size_t chunk_size, gcomp_options_t * opts = nullptr,
      gcomp_status_t * status_out = nullptr) {
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
    size_t result_pos = 0;

    const uint8_t * src = static_cast<const uint8_t *>(data);
    size_t src_pos = 0;

    // Process input in chunks
    while (src_pos < len) {
      size_t this_chunk = std::min(chunk_size, len - src_pos);

      gcomp_buffer_t in_buf = {
          const_cast<uint8_t *>(src + src_pos), this_chunk, 0};
      gcomp_buffer_t out_buf = {
          result.data() + result_pos, result.size() - result_pos, 0};

      status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
      if (status != GCOMP_OK) {
        if (status_out)
          *status_out = status;
        gcomp_encoder_destroy(encoder);
        return {};
      }

      src_pos += in_buf.used;
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

  // Helper: Decompress with small output buffer (returns partial results)
  std::vector<uint8_t> decompressSmallOutput(const void * data, size_t len,
      size_t output_chunk_size, gcomp_options_t * opts = nullptr,
      gcomp_status_t * status_out = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "lz4", opts, &decoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result;
    std::vector<uint8_t> chunk_buf(output_chunk_size);

    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};

    // Process with small output buffer, looping until all input consumed
    while (in_buf.used < in_buf.size) {
      gcomp_buffer_t out_buf = {chunk_buf.data(), chunk_buf.size(), 0};

      status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
      if (status != GCOMP_OK) {
        if (status_out)
          *status_out = status;
        gcomp_decoder_destroy(decoder);
        return {};
      }

      result.insert(result.end(), chunk_buf.begin(),
          chunk_buf.begin() + static_cast<ptrdiff_t>(out_buf.used));

      // If no progress made and input still available, something is wrong
      if (out_buf.used == 0 && in_buf.used < in_buf.size) {
        // Try again - output buffer might have been too small for header
        continue;
      }
    }

    // Finish - also with small output buffer
    bool finished = false;
    while (!finished) {
      gcomp_buffer_t out_buf = {chunk_buf.data(), chunk_buf.size(), 0};
      status = gcomp_decoder_finish(decoder, &out_buf);
      if (status != GCOMP_OK) {
        if (status_out)
          *status_out = status;
        gcomp_decoder_destroy(decoder);
        return {};
      }

      result.insert(result.end(), chunk_buf.begin(),
          chunk_buf.begin() + static_cast<ptrdiff_t>(out_buf.used));

      // If no output produced, we're done
      if (out_buf.used == 0) {
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
// Chunked Input Tests (Decoder)
//

TEST_F(Lz4StreamingTest, DecoderOneByteChunks) {
  // Compress some test data
  std::vector<uint8_t> original(1000);
  test_helpers_generate_random(original.data(), original.size(), 12345);

  auto compressed = compressOneShot(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // Decompress with 1-byte input chunks
  gcomp_status_t status;
  auto decompressed = decompressChunked(
      compressed.data(), compressed.size(), 1, nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  ASSERT_EQ(decompressed.size(), original.size());
  EXPECT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0);
}

TEST_F(Lz4StreamingTest, DecoderSmallChunks) {
  // Test various small chunk sizes
  std::vector<size_t> chunk_sizes = {2, 3, 5, 7, 11, 13, 17};

  std::vector<uint8_t> original(2000);
  test_helpers_generate_random(original.data(), original.size(), 54321);

  auto compressed = compressOneShot(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  for (size_t chunk_size : chunk_sizes) {
    gcomp_status_t status;
    auto decompressed = decompressChunked(
        compressed.data(), compressed.size(), chunk_size, nullptr, &status);
    ASSERT_EQ(status, GCOMP_OK) << "Failed with chunk_size=" << chunk_size;

    ASSERT_EQ(decompressed.size(), original.size())
        << "Size mismatch with chunk_size=" << chunk_size;
    EXPECT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
        << "Data mismatch with chunk_size=" << chunk_size;
  }
}

TEST_F(Lz4StreamingTest, DecoderRandomChunks) {
  // Random chunk sizes
  std::vector<uint8_t> original(5000);
  test_helpers_generate_random(original.data(), original.size(), 99999);

  auto compressed = compressOneShot(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // Decompress with varying random chunk sizes
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> result;
  result.resize(original.size() * 2);
  size_t result_pos = 0;

  size_t src_pos = 0;
  uint32_t seed = 11111;

  while (src_pos < compressed.size()) {
    // Random chunk size 1-50
    seed = seed * 1103515245 + 12345;
    size_t chunk_size = (seed % 50) + 1;
    chunk_size = std::min(chunk_size, compressed.size() - src_pos);

    gcomp_buffer_t in_buf = {compressed.data() + src_pos, chunk_size, 0};
    gcomp_buffer_t out_buf = {
        result.data() + result_pos, result.size() - result_pos, 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    ASSERT_EQ(status, GCOMP_OK);

    src_pos += in_buf.used;
    result_pos += out_buf.used;
  }

  gcomp_buffer_t out_buf = {
      result.data() + result_pos, result.size() - result_pos, 0};
  status = gcomp_decoder_finish(decoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  result_pos += out_buf.used;

  gcomp_decoder_destroy(decoder);

  result.resize(result_pos);
  ASSERT_EQ(result.size(), original.size());
  EXPECT_EQ(memcmp(result.data(), original.data(), original.size()), 0);
}

//
// Small Output Buffer Tests (Decoder)
//

TEST_F(Lz4StreamingTest, DecoderOneByteOutput) {
  std::vector<uint8_t> original(500);
  test_helpers_generate_random(original.data(), original.size(), 77777);

  auto compressed = compressOneShot(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // Decompress with 1-byte output buffer
  gcomp_status_t status;
  auto decompressed = decompressSmallOutput(
      compressed.data(), compressed.size(), 1, nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  ASSERT_EQ(decompressed.size(), original.size());
  EXPECT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0);
}

TEST_F(Lz4StreamingTest, DecoderSmallOutput) {
  // Test various small output buffer sizes
  std::vector<size_t> output_sizes = {2, 5, 10, 17, 32, 64};

  std::vector<uint8_t> original(1000);
  test_helpers_generate_random(original.data(), original.size(), 88888);

  auto compressed = compressOneShot(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  for (size_t output_size : output_sizes) {
    gcomp_status_t status;
    auto decompressed = decompressSmallOutput(
        compressed.data(), compressed.size(), output_size, nullptr, &status);
    ASSERT_EQ(status, GCOMP_OK) << "Failed with output_size=" << output_size;

    ASSERT_EQ(decompressed.size(), original.size())
        << "Size mismatch with output_size=" << output_size;
    EXPECT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
        << "Data mismatch with output_size=" << output_size;
  }
}

//
// Chunked Input Tests (Encoder)
//

TEST_F(Lz4StreamingTest, EncoderOneByteChunks) {
  std::vector<uint8_t> original(500);
  test_helpers_generate_random(original.data(), original.size(), 11111);

  // Compress with 1-byte input chunks
  gcomp_status_t status;
  auto compressed =
      compressChunked(original.data(), original.size(), 1, nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_FALSE(compressed.empty());

  // Verify by decompressing
  auto decompressed = decompressChunked(
      compressed.data(), compressed.size(), 1024, nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  ASSERT_EQ(decompressed.size(), original.size());
  EXPECT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0);
}

TEST_F(Lz4StreamingTest, EncoderSmallChunks) {
  std::vector<size_t> chunk_sizes = {2, 5, 10, 17, 32};

  std::vector<uint8_t> original(2000);
  test_helpers_generate_random(original.data(), original.size(), 22222);

  for (size_t chunk_size : chunk_sizes) {
    gcomp_status_t status;
    auto compressed = compressChunked(
        original.data(), original.size(), chunk_size, nullptr, &status);
    ASSERT_EQ(status, GCOMP_OK)
        << "Compress failed with chunk_size=" << chunk_size;
    ASSERT_FALSE(compressed.empty());

    auto decompressed = decompressChunked(
        compressed.data(), compressed.size(), 1024, nullptr, &status);
    ASSERT_EQ(status, GCOMP_OK)
        << "Decompress failed with chunk_size=" << chunk_size;

    ASSERT_EQ(decompressed.size(), original.size())
        << "Size mismatch with chunk_size=" << chunk_size;
    EXPECT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
        << "Data mismatch with chunk_size=" << chunk_size;
  }
}

//
// Concatenated Frame Tests
//

TEST_F(Lz4StreamingTest, ConcatenatedTwoFrames) {
  // Create two separate compressed frames
  std::vector<uint8_t> data1(100);
  std::vector<uint8_t> data2(200);
  test_helpers_generate_random(data1.data(), data1.size(), 33333);
  test_helpers_generate_random(data2.data(), data2.size(), 44444);

  auto frame1 = compressOneShot(data1.data(), data1.size());
  auto frame2 = compressOneShot(data2.data(), data2.size());
  ASSERT_FALSE(frame1.empty());
  ASSERT_FALSE(frame2.empty());

  // Concatenate frames
  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  // Expected result: data1 + data2
  std::vector<uint8_t> expected;
  expected.insert(expected.end(), data1.begin(), data1.end());
  expected.insert(expected.end(), data2.begin(), data2.end());

  // Decompress with concat enabled
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.concat", 1), GCOMP_OK);

  gcomp_status_t status;
  auto decompressed =
      decompressChunked(concat.data(), concat.size(), 1024, opts, &status);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_options_destroy(opts);

  ASSERT_EQ(decompressed.size(), expected.size());
  EXPECT_EQ(memcmp(decompressed.data(), expected.data(), expected.size()), 0);
}

TEST_F(Lz4StreamingTest, ConcatenatedManyFrames) {
  // Create 5 separate compressed frames
  std::vector<std::vector<uint8_t>> datas;
  std::vector<std::vector<uint8_t>> frames;

  for (int i = 0; i < 5; i++) {
    std::vector<uint8_t> data((i + 1) * 50);
    test_helpers_generate_random(
        data.data(), data.size(), static_cast<uint32_t>(55555 + i));
    datas.push_back(data);

    auto frame = compressOneShot(data.data(), data.size());
    ASSERT_FALSE(frame.empty());
    frames.push_back(frame);
  }

  // Concatenate all frames
  std::vector<uint8_t> concat;
  for (const auto & frame : frames) {
    concat.insert(concat.end(), frame.begin(), frame.end());
  }

  // Expected result: all data concatenated
  std::vector<uint8_t> expected;
  for (const auto & data : datas) {
    expected.insert(expected.end(), data.begin(), data.end());
  }

  // Decompress with concat enabled
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.concat", 1), GCOMP_OK);

  gcomp_status_t status;
  auto decompressed =
      decompressChunked(concat.data(), concat.size(), 1024, opts, &status);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_options_destroy(opts);

  ASSERT_EQ(decompressed.size(), expected.size());
  EXPECT_EQ(memcmp(decompressed.data(), expected.data(), expected.size()), 0);
}

TEST_F(Lz4StreamingTest, ConcatenatedWithOneByteChunks) {
  // Test concatenated frames with 1-byte input chunks
  std::vector<uint8_t> data1(50);
  std::vector<uint8_t> data2(75);
  test_helpers_generate_random(data1.data(), data1.size(), 66666);
  test_helpers_generate_random(data2.data(), data2.size(), 77777);

  auto frame1 = compressOneShot(data1.data(), data1.size());
  auto frame2 = compressOneShot(data2.data(), data2.size());

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  std::vector<uint8_t> expected;
  expected.insert(expected.end(), data1.begin(), data1.end());
  expected.insert(expected.end(), data2.begin(), data2.end());

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.concat", 1), GCOMP_OK);

  gcomp_status_t status;
  auto decompressed =
      decompressChunked(concat.data(), concat.size(), 1, opts, &status);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_options_destroy(opts);

  ASSERT_EQ(decompressed.size(), expected.size());
  EXPECT_EQ(memcmp(decompressed.data(), expected.data(), expected.size()), 0);
}

TEST_F(Lz4StreamingTest, ConcatenatedDisabledStopsAtFirstFrame) {
  // With concat disabled, decoder should stop after first frame
  std::vector<uint8_t> data1(100);
  std::vector<uint8_t> data2(200);
  test_helpers_generate_random(data1.data(), data1.size(), 88888);
  test_helpers_generate_random(data2.data(), data2.size(), 99999);

  auto frame1 = compressOneShot(data1.data(), data1.size());
  auto frame2 = compressOneShot(data2.data(), data2.size());

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  // Decompress with concat disabled (default)
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> result(data1.size() + data2.size() + 100);
  gcomp_buffer_t in_buf = {concat.data(), concat.size(), 0};
  gcomp_buffer_t out_buf = {result.data(), result.size(), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_decoder_finish(decoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  // Should only have decompressed data1
  EXPECT_EQ(out_buf.used, data1.size());
  EXPECT_EQ(memcmp(result.data(), data1.data(), data1.size()), 0);

  // Input should have stopped at the end of first frame
  EXPECT_EQ(in_buf.used, frame1.size());

  gcomp_decoder_destroy(decoder);
}

//
// Header Boundary Tests
//

TEST_F(Lz4StreamingTest, DecoderPartialHeaderMagic) {
  // Test handling when magic number spans multiple chunks
  std::vector<uint8_t> original(100);
  test_helpers_generate_random(original.data(), original.size(), 11111);

  auto compressed = compressOneShot(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());
  ASSERT_GE(compressed.size(), 4u); // At least magic number

  // Feed magic number byte by byte
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> result(original.size() * 2);
  size_t result_pos = 0;

  // Feed first 4 bytes one at a time
  for (size_t i = 0; i < 4; i++) {
    gcomp_buffer_t in_buf = {compressed.data() + i, 1, 0};
    gcomp_buffer_t out_buf = {
        result.data() + result_pos, result.size() - result_pos, 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    ASSERT_EQ(status, GCOMP_OK) << "Failed at byte " << i;
    result_pos += out_buf.used;
  }

  // Feed rest of data
  gcomp_buffer_t in_buf = {compressed.data() + 4, compressed.size() - 4, 0};
  gcomp_buffer_t out_buf = {
      result.data() + result_pos, result.size() - result_pos, 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  result_pos += out_buf.used;

  out_buf = {result.data() + result_pos, result.size() - result_pos, 0};
  status = gcomp_decoder_finish(decoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  result_pos += out_buf.used;

  gcomp_decoder_destroy(decoder);

  result.resize(result_pos);
  ASSERT_EQ(result.size(), original.size());
  EXPECT_EQ(memcmp(result.data(), original.data(), original.size()), 0);
}

TEST_F(Lz4StreamingTest, DecoderPartialBlockSize) {
  // Test handling when block size field spans multiple chunks
  // Use a larger data to ensure we have block data after header
  std::vector<uint8_t> original(1000);
  test_helpers_generate_random(original.data(), original.size(), 22222);

  auto compressed = compressOneShot(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // LZ4 minimal header is 7 bytes (magic + FLG + BD + HC)
  // Block size starts at byte 7

  // Decompress with chunks that split the block size field
  gcomp_status_t status;
  auto decompressed = decompressChunked(
      compressed.data(), compressed.size(), 1, nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  ASSERT_EQ(decompressed.size(), original.size());
  EXPECT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0);
}

//
// State Persistence Tests
//

TEST_F(Lz4StreamingTest, DecoderStatePersistsAcrossCalls) {
  // Verify state is preserved correctly across multiple update() calls
  std::vector<uint8_t> original(2000);
  test_helpers_generate_random(original.data(), original.size(), 33333);

  auto compressed = compressOneShot(original.data(), original.size());
  ASSERT_FALSE(compressed.empty());

  // Create decoder
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> result(original.size() * 2);
  size_t result_pos = 0;
  size_t src_pos = 0;

  // Feed data in varying chunk sizes
  std::vector<size_t> chunk_pattern = {3, 7, 11, 5, 13, 17, 2, 19, 1, 100};
  size_t pattern_idx = 0;

  while (src_pos < compressed.size()) {
    size_t chunk_size = chunk_pattern[pattern_idx % chunk_pattern.size()];
    chunk_size = std::min(chunk_size, compressed.size() - src_pos);
    pattern_idx++;

    gcomp_buffer_t in_buf = {compressed.data() + src_pos, chunk_size, 0};
    gcomp_buffer_t out_buf = {
        result.data() + result_pos, result.size() - result_pos, 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    ASSERT_EQ(status, GCOMP_OK);

    src_pos += in_buf.used;
    result_pos += out_buf.used;
  }

  gcomp_buffer_t out_buf = {
      result.data() + result_pos, result.size() - result_pos, 0};
  status = gcomp_decoder_finish(decoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  result_pos += out_buf.used;

  gcomp_decoder_destroy(decoder);

  result.resize(result_pos);
  ASSERT_EQ(result.size(), original.size());
  EXPECT_EQ(memcmp(result.data(), original.data(), original.size()), 0);
}

//
// With Various Options
//

TEST_F(Lz4StreamingTest, StreamingWithBlockChecksum) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.block_checksum", 1), GCOMP_OK);

  std::vector<uint8_t> original(1000);
  test_helpers_generate_random(original.data(), original.size(), 44444);

  auto compressed = compressOneShot(original.data(), original.size(), opts);
  ASSERT_FALSE(compressed.empty());

  // Decompress with 1-byte chunks
  gcomp_status_t status;
  auto decompressed = decompressChunked(
      compressed.data(), compressed.size(), 1, nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  ASSERT_EQ(decompressed.size(), original.size());
  EXPECT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0);

  gcomp_options_destroy(opts);
}

TEST_F(Lz4StreamingTest, StreamingWithContentChecksum) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.content_checksum", 1), GCOMP_OK);

  std::vector<uint8_t> original(1000);
  test_helpers_generate_random(original.data(), original.size(), 55555);

  auto compressed = compressOneShot(original.data(), original.size(), opts);
  ASSERT_FALSE(compressed.empty());

  // Decompress with 1-byte chunks
  gcomp_status_t status;
  auto decompressed = decompressChunked(
      compressed.data(), compressed.size(), 1, nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  ASSERT_EQ(decompressed.size(), original.size());
  EXPECT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0);

  gcomp_options_destroy(opts);
}

TEST_F(Lz4StreamingTest, StreamingWithSmallBlockSize) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "lz4.block_size", 65536), GCOMP_OK);

  // Data larger than one block
  std::vector<uint8_t> original(150000);
  test_helpers_generate_random(original.data(), original.size(), 66666);

  auto compressed = compressOneShot(original.data(), original.size(), opts);
  ASSERT_FALSE(compressed.empty());

  // Decompress with small chunks
  gcomp_status_t status;
  auto decompressed = decompressChunked(
      compressed.data(), compressed.size(), 7, nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  ASSERT_EQ(decompressed.size(), original.size());
  EXPECT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0);

  gcomp_options_destroy(opts);
}

//
// Empty and Minimal Data Tests
//

TEST_F(Lz4StreamingTest, StreamingEmptyData) {
  auto compressed = compressOneShot(nullptr, 0);
  ASSERT_FALSE(compressed.empty()); // Should have valid LZ4 frame

  gcomp_status_t status;
  auto decompressed = decompressChunked(
      compressed.data(), compressed.size(), 1, nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);
  EXPECT_EQ(decompressed.size(), 0u);
}

TEST_F(Lz4StreamingTest, StreamingSingleByte) {
  uint8_t single = 0x42;
  auto compressed = compressOneShot(&single, 1);
  ASSERT_FALSE(compressed.empty());

  gcomp_status_t status;
  auto decompressed = decompressChunked(
      compressed.data(), compressed.size(), 1, nullptr, &status);
  ASSERT_EQ(status, GCOMP_OK);

  ASSERT_EQ(decompressed.size(), 1u);
  EXPECT_EQ(decompressed[0], single);
}

//
// Encoder: small output buffer at block emission boundary
//

TEST_F(Lz4StreamingTest, EncoderSmallOutputAtBlockBoundary) {
  // Configure a small block size so that blocks are frequent.
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "lz4.block_size", 65536), GCOMP_OK);

  // Create encoder.
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Input slightly larger than one block to ensure we hit a block boundary.
  std::vector<uint8_t> original(65536 + 100);
  test_helpers_generate_random(original.data(), original.size(), 424242);

  gcomp_buffer_t in_buf = {original.data(), original.size(), 0};

  // Use a very small output buffer to force partial block emission.
  uint8_t out_chunk[7];
  std::vector<uint8_t> compressed;

  // Stream through update() until all input is consumed.
  while (in_buf.used < in_buf.size) {
    gcomp_buffer_t out_buf = {out_chunk, sizeof(out_chunk), 0};
    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    ASSERT_EQ(status, GCOMP_OK);
    compressed.insert(compressed.end(), out_chunk,
        out_chunk + static_cast<ptrdiff_t>(out_buf.used));
  }

  // Drain finish() with the same tiny output buffer.
  bool finished = false;
  size_t previous_size = 0;
  for (int i = 0; i < 100 && !finished; i++) {
    gcomp_buffer_t out_buf = {out_chunk, sizeof(out_chunk), 0};
    status = gcomp_encoder_finish(encoder, &out_buf);
    ASSERT_EQ(status, GCOMP_OK);
    compressed.insert(compressed.end(), out_chunk,
        out_chunk + static_cast<ptrdiff_t>(out_buf.used));

    // Two consecutive finish calls that produce no new output indicate
    // completion.
    if (out_buf.used == 0 && compressed.size() == previous_size) {
      finished = true;
    }
    previous_size = compressed.size();
  }
  ASSERT_TRUE(finished) << "Encoder did not finish within iteration limit";

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);

  // Verify by decoding with chunked input.
  gcomp_status_t dec_status;
  auto decompressed = decompressChunked(
      compressed.data(), compressed.size(), 1024, nullptr, &dec_status);
  ASSERT_EQ(dec_status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), original.size());
  EXPECT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
