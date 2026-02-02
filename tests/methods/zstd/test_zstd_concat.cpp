/**
 * @file test_zstd_concat.cpp
 *
 * Concatenated frame tests for Zstd decoder in the Ghoti.io Compress library.
 *
 * Zstd supports concatenated frames where multiple independent zstd frames
 * are concatenated together. The decoder (when enabled via zstd.concat option)
 * should transparently decode all frames and produce continuous output.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>
#include <vector>

class ZstdConcatTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  // Helper: Compress data with options
  std::vector<uint8_t> compress(
      const void * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "zstd", opts, &encoder);
    if (status != GCOMP_OK) {
      return {};
    }

    std::vector<uint8_t> result(len + 256);
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

  // Helper: Decompress with options
  std::vector<uint8_t> decompress(const void * data, size_t len,
      gcomp_options_t * opts = nullptr, gcomp_status_t * status_out = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "zstd", opts, &decoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result(len * 100 + 65536);
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

  // Helper: Decompress with chunked input (for streaming tests)
  std::vector<uint8_t> decompressChunked(const void * data, size_t len,
      size_t chunk_size, gcomp_options_t * opts = nullptr,
      gcomp_status_t * status_out = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "zstd", opts, &decoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result;
    std::vector<uint8_t> out_buf(65536);
    const uint8_t * src = static_cast<const uint8_t *>(data);
    size_t offset = 0;

    while (offset < len) {
      size_t this_chunk = std::min(chunk_size, len - offset);
      gcomp_buffer_t in = {src + offset, this_chunk, 0};
      gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
      status = gcomp_decoder_update(decoder, &in, &ob);
      if (status != GCOMP_OK) {
        if (status_out)
          *status_out = status;
        gcomp_decoder_destroy(decoder);
        return {};
      }
      result.insert(result.end(), out_buf.data(), out_buf.data() + ob.used);
      offset += in.used;
    }

    gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
    status = gcomp_decoder_finish(decoder, &ob);
    if (status_out)
      *status_out = status;
    result.insert(result.end(), out_buf.data(), out_buf.data() + ob.used);

    gcomp_decoder_destroy(decoder);
    return result;
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Two-frame concatenation tests
//

TEST_F(ZstdConcatTest, TwoFramesConcatEnabled) {
  // Compress two separate pieces of data
  const char data1[] = "Hello, ";
  const char data2[] = "World!";

  auto frame1 = compress(data1, strlen(data1));
  auto frame2 = compress(data2, strlen(data2));
  ASSERT_GT(frame1.size(), 0u);
  ASSERT_GT(frame2.size(), 0u);

  // Concatenate the frames
  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  // Decompress with concat enabled
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.concat", true);

  gcomp_status_t status;
  auto result = decompress(concat.data(), concat.size(), opts, &status);
  EXPECT_EQ(status, GCOMP_OK);

  // Output should be both strings concatenated
  std::string expected = std::string(data1) + std::string(data2);
  ASSERT_EQ(result.size(), expected.size());
  EXPECT_EQ(memcmp(result.data(), expected.data(), expected.size()), 0);

  gcomp_options_destroy(opts);
}

TEST_F(ZstdConcatTest, TwoFramesConcatDisabled) {
  const char data1[] = "First frame";
  const char data2[] = "Second frame";

  auto frame1 = compress(data1, strlen(data1));
  auto frame2 = compress(data2, strlen(data2));
  ASSERT_GT(frame1.size(), 0u);
  ASSERT_GT(frame2.size(), 0u);

  // Concatenate the frames
  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  // Decompress with concat disabled (default)
  gcomp_status_t status;
  auto result = decompress(concat.data(), concat.size(), nullptr, &status);

  // Should succeed but only decode first frame
  EXPECT_EQ(status, GCOMP_OK);
  ASSERT_EQ(result.size(), strlen(data1));
  EXPECT_EQ(memcmp(result.data(), data1, strlen(data1)), 0);
}

//
// Many-frame concatenation tests
//

TEST_F(ZstdConcatTest, ManyFramesConcatEnabled) {
  const int num_frames = 10;

  // Create and compress individual frames
  std::vector<std::vector<uint8_t>> frames;
  std::string expected_output;

  for (int i = 0; i < num_frames; ++i) {
    std::string data = "Frame " + std::to_string(i) + " content. ";
    expected_output += data;
    auto frame = compress(data.c_str(), data.size());
    ASSERT_GT(frame.size(), 0u) << "Failed to compress frame " << i;
    frames.push_back(std::move(frame));
  }

  // Concatenate all frames
  std::vector<uint8_t> concat;
  for (const auto & frame : frames) {
    concat.insert(concat.end(), frame.begin(), frame.end());
  }

  // Decompress with concat enabled
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.concat", true);

  gcomp_status_t status;
  auto result = decompress(concat.data(), concat.size(), opts, &status);
  EXPECT_EQ(status, GCOMP_OK);

  ASSERT_EQ(result.size(), expected_output.size())
      << "Output size mismatch for " << num_frames << " frames";
  EXPECT_EQ(
      memcmp(result.data(), expected_output.data(), expected_output.size()), 0);

  gcomp_options_destroy(opts);
}

TEST_F(ZstdConcatTest, ManySmallFramesConcatEnabled) {
  // Test with many very small frames (1 byte each)
  const int num_frames = 26;

  std::vector<uint8_t> concat;
  std::string expected_output;

  for (int i = 0; i < num_frames; ++i) {
    char c = 'A' + i;
    expected_output += c;
    auto frame = compress(&c, 1);
    ASSERT_GT(frame.size(), 0u);
    concat.insert(concat.end(), frame.begin(), frame.end());
  }

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.concat", true);

  gcomp_status_t status;
  auto result = decompress(concat.data(), concat.size(), opts, &status);
  EXPECT_EQ(status, GCOMP_OK);

  ASSERT_EQ(result.size(), expected_output.size());
  EXPECT_EQ(
      memcmp(result.data(), expected_output.data(), expected_output.size()), 0);

  gcomp_options_destroy(opts);
}

//
// Checksum validation with concatenated frames
//

TEST_F(ZstdConcatTest, TwoFramesWithChecksum) {
  gcomp_options_t * enc_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&enc_opts), GCOMP_OK);
  gcomp_options_set_bool(enc_opts, "zstd.checksum", true);

  const char data1[] = "First frame with checksum";
  const char data2[] = "Second frame with checksum";

  auto frame1 = compress(data1, strlen(data1), enc_opts);
  auto frame2 = compress(data2, strlen(data2), enc_opts);
  ASSERT_GT(frame1.size(), 0u);
  ASSERT_GT(frame2.size(), 0u);

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  gcomp_options_t * dec_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&dec_opts), GCOMP_OK);
  gcomp_options_set_bool(dec_opts, "zstd.concat", true);

  gcomp_status_t status;
  auto result = decompress(concat.data(), concat.size(), dec_opts, &status);
  EXPECT_EQ(status, GCOMP_OK);

  std::string expected = std::string(data1) + std::string(data2);
  ASSERT_EQ(result.size(), expected.size());
  EXPECT_EQ(memcmp(result.data(), expected.data(), expected.size()), 0);

  gcomp_options_destroy(enc_opts);
  gcomp_options_destroy(dec_opts);
}

TEST_F(ZstdConcatTest, SecondFrameChecksumCorrupted) {
  gcomp_options_t * enc_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&enc_opts), GCOMP_OK);
  gcomp_options_set_bool(enc_opts, "zstd.checksum", true);

  const char data1[] = "First frame";
  const char data2[] = "Second frame";

  auto frame1 = compress(data1, strlen(data1), enc_opts);
  auto frame2 = compress(data2, strlen(data2), enc_opts);
  ASSERT_GT(frame1.size(), 4u);
  ASSERT_GT(frame2.size(), 4u);

  // Corrupt checksum of second frame (last 4 bytes)
  frame2[frame2.size() - 1] ^= 0xFF;

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  gcomp_options_t * dec_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&dec_opts), GCOMP_OK);
  gcomp_options_set_bool(dec_opts, "zstd.concat", true);

  gcomp_status_t status;
  auto result = decompress(concat.data(), concat.size(), dec_opts, &status);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);

  gcomp_options_destroy(enc_opts);
  gcomp_options_destroy(dec_opts);
}

//
// Error in second frame tests
//

TEST_F(ZstdConcatTest, SecondFrameCorrupted) {
  const char data1[] = "Valid first frame";
  const char data2[] = "Second frame that will be corrupted";

  auto frame1 = compress(data1, strlen(data1));
  auto frame2 = compress(data2, strlen(data2));
  ASSERT_GT(frame1.size(), 0u);
  ASSERT_GT(frame2.size(), 10u);

  // Corrupt the magic number of second frame
  frame2[0] ^= 0xFF;
  frame2[1] ^= 0xFF;

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.concat", true);

  gcomp_status_t status;
  auto result = decompress(concat.data(), concat.size(), opts, &status);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);

  gcomp_options_destroy(opts);
}

TEST_F(ZstdConcatTest, SecondFrameTruncated) {
  const char data1[] = "First frame complete";
  const char data2[] = "Second frame will be truncated";

  auto frame1 = compress(data1, strlen(data1));
  auto frame2 = compress(data2, strlen(data2));
  ASSERT_GT(frame1.size(), 0u);
  ASSERT_GT(frame2.size(), 10u);

  // Truncate the second frame
  frame2.resize(frame2.size() / 2);

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.concat", true);

  gcomp_status_t status;
  auto result = decompress(concat.data(), concat.size(), opts, &status);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);

  gcomp_options_destroy(opts);
}

//
// Chunked input with concatenated frames
//

TEST_F(ZstdConcatTest, TwoFramesChunkedInput) {
  const char data1[] = "Chunk test frame 1";
  const char data2[] = "Chunk test frame 2";

  auto frame1 = compress(data1, strlen(data1));
  auto frame2 = compress(data2, strlen(data2));
  ASSERT_GT(frame1.size(), 0u);
  ASSERT_GT(frame2.size(), 0u);

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.concat", true);

  // Decompress with 1-byte chunks
  gcomp_status_t status;
  auto result =
      decompressChunked(concat.data(), concat.size(), 1, opts, &status);
  EXPECT_EQ(status, GCOMP_OK);

  std::string expected = std::string(data1) + std::string(data2);
  ASSERT_EQ(result.size(), expected.size());
  EXPECT_EQ(memcmp(result.data(), expected.data(), expected.size()), 0);

  gcomp_options_destroy(opts);
}

//
// Mixed frame types (RLE, raw, compressed)
//

TEST_F(ZstdConcatTest, MixedBlockTypes) {
  // Frame 1: RLE-compressible data
  std::vector<uint8_t> rle_data(500, 'X');
  auto frame1 = compress(rle_data.data(), rle_data.size());
  ASSERT_GT(frame1.size(), 0u);
  EXPECT_LT(frame1.size(), 50u) << "Frame 1 should be RLE compressed";

  // Frame 2: Random data (likely raw block)
  std::vector<uint8_t> rand_data(100);
  test_helpers_generate_random(rand_data.data(), rand_data.size(), 12345);
  auto frame2 = compress(rand_data.data(), rand_data.size());
  ASSERT_GT(frame2.size(), 0u);

  // Frame 3: Sequential data (likely compressed)
  std::vector<uint8_t> seq_data(200);
  test_helpers_generate_sequential(seq_data.data(), seq_data.size());
  auto frame3 = compress(seq_data.data(), seq_data.size());
  ASSERT_GT(frame3.size(), 0u);

  // Concatenate all
  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());
  concat.insert(concat.end(), frame3.begin(), frame3.end());

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.concat", true);

  gcomp_status_t status;
  auto result = decompress(concat.data(), concat.size(), opts, &status);
  EXPECT_EQ(status, GCOMP_OK);

  size_t expected_size = rle_data.size() + rand_data.size() + seq_data.size();
  ASSERT_EQ(result.size(), expected_size);

  // Verify each section
  size_t offset = 0;
  EXPECT_EQ(
      memcmp(result.data() + offset, rle_data.data(), rle_data.size()), 0);
  offset += rle_data.size();
  EXPECT_EQ(
      memcmp(result.data() + offset, rand_data.data(), rand_data.size()), 0);
  offset += rand_data.size();
  EXPECT_EQ(
      memcmp(result.data() + offset, seq_data.data(), seq_data.size()), 0);

  gcomp_options_destroy(opts);
}

//
// Empty frame concatenation
//

TEST_F(ZstdConcatTest, EmptyFrameBetweenNonEmpty) {
  const char data1[] = "Before empty";
  const char data2[] = "After empty";

  auto frame1 = compress(data1, strlen(data1));
  auto frame_empty = compress(nullptr, 0);
  auto frame2 = compress(data2, strlen(data2));
  ASSERT_GT(frame1.size(), 0u);
  ASSERT_GT(frame_empty.size(), 0u);
  ASSERT_GT(frame2.size(), 0u);

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame1.begin(), frame1.end());
  concat.insert(concat.end(), frame_empty.begin(), frame_empty.end());
  concat.insert(concat.end(), frame2.begin(), frame2.end());

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.concat", true);

  gcomp_status_t status;
  auto result = decompress(concat.data(), concat.size(), opts, &status);
  EXPECT_EQ(status, GCOMP_OK);

  std::string expected = std::string(data1) + std::string(data2);
  ASSERT_EQ(result.size(), expected.size());
  EXPECT_EQ(memcmp(result.data(), expected.data(), expected.size()), 0);

  gcomp_options_destroy(opts);
}

TEST_F(ZstdConcatTest, MultipleEmptyFrames) {
  auto frame_empty1 = compress(nullptr, 0);
  auto frame_empty2 = compress(nullptr, 0);
  auto frame_empty3 = compress(nullptr, 0);
  ASSERT_GT(frame_empty1.size(), 0u);
  ASSERT_GT(frame_empty2.size(), 0u);
  ASSERT_GT(frame_empty3.size(), 0u);

  std::vector<uint8_t> concat;
  concat.insert(concat.end(), frame_empty1.begin(), frame_empty1.end());
  concat.insert(concat.end(), frame_empty2.begin(), frame_empty2.end());
  concat.insert(concat.end(), frame_empty3.begin(), frame_empty3.end());

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.concat", true);

  gcomp_status_t status;
  auto result = decompress(concat.data(), concat.size(), opts, &status);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(result.size(), 0u);

  gcomp_options_destroy(opts);
}

//
// Single frame (edge case)
//

TEST_F(ZstdConcatTest, SingleFrameWithConcatEnabled) {
  const char data[] = "Single frame data";

  auto frame = compress(data, strlen(data));
  ASSERT_GT(frame.size(), 0u);

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.concat", true);

  gcomp_status_t status;
  auto result = decompress(frame.data(), frame.size(), opts, &status);
  EXPECT_EQ(status, GCOMP_OK);

  ASSERT_EQ(result.size(), strlen(data));
  EXPECT_EQ(memcmp(result.data(), data, strlen(data)), 0);

  gcomp_options_destroy(opts);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
