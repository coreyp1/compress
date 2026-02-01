/**
 * @file test_lz4_concat.cpp
 *
 * Concatenated frame tests for LZ4 decoder in the Ghoti.io Compress library.
 *
 * These tests verify:
 * - Decode of 2-frame concatenated lz4
 * - Decode of many-frame concatenated lz4
 * - Each frame has correct checksum validation
 * - Output is continuous across frames
 * - Limits apply across all frames
 * - Error in second frame after first succeeds
 * - Concat disabled ignores extra bytes
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

class Lz4ConcatTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Compress data with optional settings
  std::vector<uint8_t> compress(
      const void * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "lz4", opts, &encoder);
    if (status != GCOMP_OK) {
      return {};
    }

    std::vector<uint8_t> result;
    result.resize(len + len / 100 + 256);

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

  // Helper: Decompress with concat option
  std::vector<uint8_t> decompressConcat(const void * data, size_t len,
      bool concat_enabled = true, gcomp_status_t * status_out = nullptr) {
    gcomp_options_t * opts = nullptr;
    EXPECT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    EXPECT_EQ(
        gcomp_options_set_bool(opts, "lz4.concat", concat_enabled ? 1 : 0),
        GCOMP_OK);

    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "lz4", opts, &decoder);
    gcomp_options_destroy(opts);

    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result;
    result.resize(len * 100 + 65536);

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

  // Helper: Decompress without concat (default)
  std::vector<uint8_t> decompressNoConcat(const void * data, size_t len,
      size_t * input_consumed = nullptr,
      gcomp_status_t * status_out = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "lz4", nullptr, &decoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result;
    result.resize(len * 100 + 65536);

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

    if (input_consumed) {
      *input_consumed = in_buf.used;
    }

    result.resize(out_buf.used);
    gcomp_decoder_destroy(decoder);
    if (status_out)
      *status_out = GCOMP_OK;
    return result;
  }

  // Helper: Decompress with chunked input and concat enabled
  std::vector<uint8_t> decompressConcatChunked(const void * data, size_t len,
      size_t chunk_size, gcomp_status_t * status_out = nullptr) {
    gcomp_options_t * opts = nullptr;
    EXPECT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_bool(opts, "lz4.concat", 1), GCOMP_OK);

    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "lz4", opts, &decoder);
    gcomp_options_destroy(opts);

    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result;
    result.resize(len * 100 + 65536);
    size_t result_pos = 0;

    const uint8_t * src = static_cast<const uint8_t *>(data);
    size_t src_pos = 0;

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

  gcomp_registry_t * registry_ = nullptr;
};

//
// Basic Concatenation Tests
//

TEST_F(Lz4ConcatTest, TwoFrames) {
  // Create two separate compressed frames
  std::vector<uint8_t> data1(100);
  std::vector<uint8_t> data2(200);
  test_helpers_generate_random(data1.data(), data1.size(), 11111);
  test_helpers_generate_random(data2.data(), data2.size(), 22222);

  auto frame1 = compress(data1.data(), data1.size());
  auto frame2 = compress(data2.data(), data2.size());
  ASSERT_FALSE(frame1.empty());
  ASSERT_FALSE(frame2.empty());

  // Concatenate frames
  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  // Decompress with concat enabled
  gcomp_status_t status;
  auto decoded = decompressConcat(concat.data(), concat.size(), true, &status);
  ASSERT_EQ(status, GCOMP_OK);

  // Verify output is data1 + data2
  ASSERT_EQ(decoded.size(), data1.size() + data2.size());
  EXPECT_EQ(memcmp(decoded.data(), data1.data(), data1.size()), 0);
  EXPECT_EQ(
      memcmp(decoded.data() + data1.size(), data2.data(), data2.size()), 0);
}

TEST_F(Lz4ConcatTest, ThreeFrames) {
  std::vector<uint8_t> data1(50);
  std::vector<uint8_t> data2(100);
  std::vector<uint8_t> data3(150);
  test_helpers_generate_random(data1.data(), data1.size(), 33333);
  test_helpers_generate_random(data2.data(), data2.size(), 44444);
  test_helpers_generate_random(data3.data(), data3.size(), 55555);

  auto frame1 = compress(data1.data(), data1.size());
  auto frame2 = compress(data2.data(), data2.size());
  auto frame3 = compress(data3.data(), data3.size());

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());
  concat.insert(concat.end(), frame3.begin(), frame3.end());

  gcomp_status_t status;
  auto decoded = decompressConcat(concat.data(), concat.size(), true, &status);
  ASSERT_EQ(status, GCOMP_OK);

  size_t expected_size = data1.size() + data2.size() + data3.size();
  ASSERT_EQ(decoded.size(), expected_size);

  size_t pos = 0;
  EXPECT_EQ(memcmp(decoded.data() + pos, data1.data(), data1.size()), 0);
  pos += data1.size();
  EXPECT_EQ(memcmp(decoded.data() + pos, data2.data(), data2.size()), 0);
  pos += data2.size();
  EXPECT_EQ(memcmp(decoded.data() + pos, data3.data(), data3.size()), 0);
}

TEST_F(Lz4ConcatTest, ManyFrames) {
  // Create 10 separate frames
  std::vector<std::vector<uint8_t>> datas;
  std::vector<std::vector<uint8_t>> frames;

  for (int i = 0; i < 10; i++) {
    std::vector<uint8_t> data((i + 1) * 30);
    test_helpers_generate_random(
        data.data(), data.size(), static_cast<uint32_t>(10000 + i * 1000));
    datas.push_back(data);

    auto frame = compress(data.data(), data.size());
    ASSERT_FALSE(frame.empty());
    frames.push_back(frame);
  }

  // Concatenate all frames
  std::vector<uint8_t> concat;
  for (const auto & frame : frames) {
    concat.insert(concat.end(), frame.begin(), frame.end());
  }

  gcomp_status_t status;
  auto decoded = decompressConcat(concat.data(), concat.size(), true, &status);
  ASSERT_EQ(status, GCOMP_OK);

  // Verify total size
  size_t expected_size = 0;
  for (const auto & data : datas) {
    expected_size += data.size();
  }
  ASSERT_EQ(decoded.size(), expected_size);

  // Verify data integrity
  size_t pos = 0;
  for (const auto & data : datas) {
    EXPECT_EQ(memcmp(decoded.data() + pos, data.data(), data.size()), 0);
    pos += data.size();
  }
}

//
// Concat with Checksums
//

TEST_F(Lz4ConcatTest, TwoFramesWithBlockChecksum) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.block_checksum", 1), GCOMP_OK);

  std::vector<uint8_t> data1(100);
  std::vector<uint8_t> data2(200);
  test_helpers_generate_random(data1.data(), data1.size(), 66666);
  test_helpers_generate_random(data2.data(), data2.size(), 77777);

  auto frame1 = compress(data1.data(), data1.size(), opts);
  auto frame2 = compress(data2.data(), data2.size(), opts);
  gcomp_options_destroy(opts);

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  gcomp_status_t status;
  auto decoded = decompressConcat(concat.data(), concat.size(), true, &status);
  ASSERT_EQ(status, GCOMP_OK);

  ASSERT_EQ(decoded.size(), data1.size() + data2.size());
  EXPECT_EQ(memcmp(decoded.data(), data1.data(), data1.size()), 0);
  EXPECT_EQ(
      memcmp(decoded.data() + data1.size(), data2.data(), data2.size()), 0);
}

TEST_F(Lz4ConcatTest, TwoFramesWithContentChecksum) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.content_checksum", 1), GCOMP_OK);

  std::vector<uint8_t> data1(100);
  std::vector<uint8_t> data2(200);
  test_helpers_generate_random(data1.data(), data1.size(), 88888);
  test_helpers_generate_random(data2.data(), data2.size(), 99999);

  auto frame1 = compress(data1.data(), data1.size(), opts);
  auto frame2 = compress(data2.data(), data2.size(), opts);
  gcomp_options_destroy(opts);

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  gcomp_status_t status;
  auto decoded = decompressConcat(concat.data(), concat.size(), true, &status);
  ASSERT_EQ(status, GCOMP_OK);

  ASSERT_EQ(decoded.size(), data1.size() + data2.size());
}

TEST_F(Lz4ConcatTest, MixedChecksumOptions) {
  // First frame: no checksums
  // Second frame: block checksum
  // Third frame: content checksum

  std::vector<uint8_t> data1(50);
  std::vector<uint8_t> data2(75);
  std::vector<uint8_t> data3(100);
  test_helpers_generate_random(data1.data(), data1.size(), 11111);
  test_helpers_generate_random(data2.data(), data2.size(), 22222);
  test_helpers_generate_random(data3.data(), data3.size(), 33333);

  auto frame1 = compress(data1.data(), data1.size());

  gcomp_options_t * opts2 = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts2), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts2, "lz4.block_checksum", 1), GCOMP_OK);
  auto frame2 = compress(data2.data(), data2.size(), opts2);
  gcomp_options_destroy(opts2);

  gcomp_options_t * opts3 = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts3), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts3, "lz4.content_checksum", 1), GCOMP_OK);
  auto frame3 = compress(data3.data(), data3.size(), opts3);
  gcomp_options_destroy(opts3);

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());
  concat.insert(concat.end(), frame3.begin(), frame3.end());

  gcomp_status_t status;
  auto decoded = decompressConcat(concat.data(), concat.size(), true, &status);
  ASSERT_EQ(status, GCOMP_OK);

  ASSERT_EQ(decoded.size(), data1.size() + data2.size() + data3.size());
}

//
// Concat Disabled Tests
//

TEST_F(Lz4ConcatTest, ConcatDisabledStopsAtFirstFrame) {
  std::vector<uint8_t> data1(100);
  std::vector<uint8_t> data2(200);
  test_helpers_generate_random(data1.data(), data1.size(), 44444);
  test_helpers_generate_random(data2.data(), data2.size(), 55555);

  auto frame1 = compress(data1.data(), data1.size());
  auto frame2 = compress(data2.data(), data2.size());

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  // Decompress with concat disabled
  size_t input_consumed = 0;
  gcomp_status_t status;
  auto decoded = decompressNoConcat(
      concat.data(), concat.size(), &input_consumed, &status);
  ASSERT_EQ(status, GCOMP_OK);

  // Should only decode first frame
  ASSERT_EQ(decoded.size(), data1.size());
  EXPECT_EQ(memcmp(decoded.data(), data1.data(), data1.size()), 0);

  // Input should stop at end of first frame
  EXPECT_EQ(input_consumed, frame1.size());
}

TEST_F(Lz4ConcatTest, ConcatDisabledIgnoresTrailingGarbage) {
  std::vector<uint8_t> data(100);
  test_helpers_generate_random(data.data(), data.size(), 66666);

  auto frame = compress(data.data(), data.size());

  // Add garbage after the frame
  std::vector<uint8_t> with_garbage = frame;
  for (int i = 0; i < 50; i++) {
    with_garbage.push_back(static_cast<uint8_t>(i));
  }

  size_t input_consumed = 0;
  gcomp_status_t status;
  auto decoded = decompressNoConcat(
      with_garbage.data(), with_garbage.size(), &input_consumed, &status);
  ASSERT_EQ(status, GCOMP_OK);

  // Should decode correctly
  ASSERT_EQ(decoded.size(), data.size());
  EXPECT_EQ(memcmp(decoded.data(), data.data(), data.size()), 0);

  // Garbage should be ignored
  EXPECT_EQ(input_consumed, frame.size());
}

//
// Chunked Concat Tests
//

TEST_F(Lz4ConcatTest, ConcatWithOneByteChunks) {
  std::vector<uint8_t> data1(50);
  std::vector<uint8_t> data2(75);
  test_helpers_generate_random(data1.data(), data1.size(), 77777);
  test_helpers_generate_random(data2.data(), data2.size(), 88888);

  auto frame1 = compress(data1.data(), data1.size());
  auto frame2 = compress(data2.data(), data2.size());

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  gcomp_status_t status;
  auto decoded =
      decompressConcatChunked(concat.data(), concat.size(), 1, &status);
  ASSERT_EQ(status, GCOMP_OK);

  ASSERT_EQ(decoded.size(), data1.size() + data2.size());
  EXPECT_EQ(memcmp(decoded.data(), data1.data(), data1.size()), 0);
  EXPECT_EQ(
      memcmp(decoded.data() + data1.size(), data2.data(), data2.size()), 0);
}

TEST_F(Lz4ConcatTest, ConcatWithSmallChunks) {
  std::vector<uint8_t> data1(200);
  std::vector<uint8_t> data2(300);
  test_helpers_generate_random(data1.data(), data1.size(), 11111);
  test_helpers_generate_random(data2.data(), data2.size(), 22222);

  auto frame1 = compress(data1.data(), data1.size());
  auto frame2 = compress(data2.data(), data2.size());

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  // Test with various chunk sizes
  std::vector<size_t> chunk_sizes = {3, 7, 11, 17, 23};
  for (size_t chunk_size : chunk_sizes) {
    gcomp_status_t status;
    auto decoded = decompressConcatChunked(
        concat.data(), concat.size(), chunk_size, &status);
    ASSERT_EQ(status, GCOMP_OK) << "Failed with chunk_size=" << chunk_size;

    ASSERT_EQ(decoded.size(), data1.size() + data2.size())
        << "Size mismatch with chunk_size=" << chunk_size;
    EXPECT_EQ(memcmp(decoded.data(), data1.data(), data1.size()), 0)
        << "Data1 mismatch with chunk_size=" << chunk_size;
    EXPECT_EQ(
        memcmp(decoded.data() + data1.size(), data2.data(), data2.size()), 0)
        << "Data2 mismatch with chunk_size=" << chunk_size;
  }
}

//
// Error Handling in Concatenated Frames
//

TEST_F(Lz4ConcatTest, ErrorInSecondFrame) {
  std::vector<uint8_t> data1(100);
  test_helpers_generate_random(data1.data(), data1.size(), 33333);

  auto frame1 = compress(data1.data(), data1.size());

  // Create invalid second frame (bad magic)
  std::vector<uint8_t> bad_frame = {0x00, 0x00, 0x00, 0x00, 0x60, 0x70, 0xDF};

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), bad_frame.begin(), bad_frame.end());

  gcomp_status_t status;
  auto decoded = decompressConcat(concat.data(), concat.size(), true, &status);

  // Should fail when reaching second frame
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4ConcatTest, ChecksumErrorInSecondFrame) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.content_checksum", 1), GCOMP_OK);

  std::vector<uint8_t> data1(100);
  std::vector<uint8_t> data2(200);
  test_helpers_generate_random(data1.data(), data1.size(), 44444);
  test_helpers_generate_random(data2.data(), data2.size(), 55555);

  auto frame1 = compress(data1.data(), data1.size(), opts);
  auto frame2 = compress(data2.data(), data2.size(), opts);
  gcomp_options_destroy(opts);

  // Corrupt the content checksum of frame2 (last 4 bytes)
  if (frame2.size() >= 4) {
    frame2[frame2.size() - 1] ^= 0xFF;
  }

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  gcomp_status_t status;
  auto decoded = decompressConcat(concat.data(), concat.size(), true, &status);

  // Should fail due to checksum error in second frame
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

//
// Limits Across Concatenated Frames
//

TEST_F(Lz4ConcatTest, OutputLimitAcrossFrames) {
  // Create frames that together exceed the output limit
  std::vector<uint8_t> data1(500);
  std::vector<uint8_t> data2(500);
  test_helpers_generate_random(data1.data(), data1.size(), 66666);
  test_helpers_generate_random(data2.data(), data2.size(), 77777);

  auto frame1 = compress(data1.data(), data1.size());
  auto frame2 = compress(data2.data(), data2.size());

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  // Set output limit to 750 bytes (less than 500+500=1000)
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.concat", 1), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_uint64(opts, "limits.max_output_bytes", 750), GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "lz4", opts, &decoder);
  ASSERT_EQ(status, GCOMP_OK);
  gcomp_options_destroy(opts);

  std::vector<uint8_t> output(2000);
  gcomp_buffer_t in_buf = {concat.data(), concat.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  // May fail during update or finish
  if (status == GCOMP_OK) {
    status = gcomp_decoder_finish(decoder, &out_buf);
  }

  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_decoder_destroy(decoder);
}

//
// Empty Frames in Concatenation
//

TEST_F(Lz4ConcatTest, EmptyFrameConcatenated) {
  // Empty frame followed by non-empty frame
  auto frame1 = compress(nullptr, 0);
  ASSERT_FALSE(frame1.empty());

  std::vector<uint8_t> data2(100);
  test_helpers_generate_random(data2.data(), data2.size(), 88888);
  auto frame2 = compress(data2.data(), data2.size());

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  gcomp_status_t status;
  auto decoded = decompressConcat(concat.data(), concat.size(), true, &status);
  ASSERT_EQ(status, GCOMP_OK);

  // Output should be just data2
  ASSERT_EQ(decoded.size(), data2.size());
  EXPECT_EQ(memcmp(decoded.data(), data2.data(), data2.size()), 0);
}

TEST_F(Lz4ConcatTest, NonEmptyThenEmpty) {
  // Non-empty frame followed by empty frame
  std::vector<uint8_t> data1(100);
  test_helpers_generate_random(data1.data(), data1.size(), 99999);
  auto frame1 = compress(data1.data(), data1.size());

  auto frame2 = compress(nullptr, 0);
  ASSERT_FALSE(frame2.empty());

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  gcomp_status_t status;
  auto decoded = decompressConcat(concat.data(), concat.size(), true, &status);
  ASSERT_EQ(status, GCOMP_OK);

  // Output should be just data1
  ASSERT_EQ(decoded.size(), data1.size());
  EXPECT_EQ(memcmp(decoded.data(), data1.data(), data1.size()), 0);
}

TEST_F(Lz4ConcatTest, MultipleEmptyFrames) {
  auto frame1 = compress(nullptr, 0);
  auto frame2 = compress(nullptr, 0);
  auto frame3 = compress(nullptr, 0);

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());
  concat.insert(concat.end(), frame3.begin(), frame3.end());

  gcomp_status_t status;
  auto decoded = decompressConcat(concat.data(), concat.size(), true, &status);
  ASSERT_EQ(status, GCOMP_OK);

  // Output should be empty
  EXPECT_EQ(decoded.size(), 0u);
}

//
// Different Block Settings Across Frames
//

TEST_F(Lz4ConcatTest, DifferentBlockSizes) {
  // First frame: 64KB blocks
  // Second frame: 4MB blocks
  gcomp_options_t * opts1 = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts1), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts1, "lz4.block_size", 65536), GCOMP_OK);

  gcomp_options_t * opts2 = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts2), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_uint64(opts2, "lz4.block_size", 4194304), GCOMP_OK);

  std::vector<uint8_t> data1(100);
  std::vector<uint8_t> data2(200);
  test_helpers_generate_random(data1.data(), data1.size(), 12345);
  test_helpers_generate_random(data2.data(), data2.size(), 54321);

  auto frame1 = compress(data1.data(), data1.size(), opts1);
  auto frame2 = compress(data2.data(), data2.size(), opts2);
  gcomp_options_destroy(opts1);
  gcomp_options_destroy(opts2);

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  gcomp_status_t status;
  auto decoded = decompressConcat(concat.data(), concat.size(), true, &status);
  ASSERT_EQ(status, GCOMP_OK);

  ASSERT_EQ(decoded.size(), data1.size() + data2.size());
}

TEST_F(Lz4ConcatTest, IndependentAndDependentBlocks) {
  // First frame: independent blocks
  // Second frame: dependent blocks
  gcomp_options_t * opts1 = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts1), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_bool(opts1, "lz4.independent_blocks", 1), GCOMP_OK);

  gcomp_options_t * opts2 = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts2), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_bool(opts2, "lz4.independent_blocks", 0), GCOMP_OK);

  std::vector<uint8_t> data1(1000);
  std::vector<uint8_t> data2(2000);
  test_helpers_generate_random(data1.data(), data1.size(), 11111);
  test_helpers_generate_random(data2.data(), data2.size(), 22222);

  auto frame1 = compress(data1.data(), data1.size(), opts1);
  auto frame2 = compress(data2.data(), data2.size(), opts2);
  gcomp_options_destroy(opts1);
  gcomp_options_destroy(opts2);

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  gcomp_status_t status;
  auto decoded = decompressConcat(concat.data(), concat.size(), true, &status);
  ASSERT_EQ(status, GCOMP_OK);

  ASSERT_EQ(decoded.size(), data1.size() + data2.size());
  EXPECT_EQ(memcmp(decoded.data(), data1.data(), data1.size()), 0);
  EXPECT_EQ(
      memcmp(decoded.data() + data1.size(), data2.data(), data2.size()), 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
