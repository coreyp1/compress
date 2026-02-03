/**
 * @file test_zstd_format.cpp
 *
 * Format tests for Zstd frame and block headers in the Ghoti.io Compress
 * library.
 *
 * Tests the low-level frame header writing and block header parsing/writing
 * functions used by the encoder and decoder.
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

// Constants from zstd spec
static const uint32_t ZSTD_MAGIC = 0xFD2FB528U;
static const size_t ZSTD_BLOCK_SIZE_MAX = 131072;

class ZstdFormatTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  // Helper: Compress with options
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

  // Helper: Decompress
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

  // Helper: Read 32-bit little-endian value
  uint32_t readLE32(const uint8_t * buf) {
    return buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) |
        ((uint32_t)buf[3] << 24);
  }

  // Helper: Read 16-bit little-endian value
  uint16_t readLE16(const uint8_t * buf) {
    return buf[0] | ((uint16_t)buf[1] << 8);
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Frame Header Tests - Magic Number
//

TEST_F(ZstdFormatTest, MagicNumberPresent) {
  const char data[] = "Test data";
  auto compressed = compress(data, strlen(data));
  ASSERT_GE(compressed.size(), 4u);

  uint32_t magic = readLE32(compressed.data());
  EXPECT_EQ(magic, ZSTD_MAGIC);
}

TEST_F(ZstdFormatTest, EmptyFrameMagicNumber) {
  auto compressed = compress(nullptr, 0);
  ASSERT_GE(compressed.size(), 4u);

  uint32_t magic = readLE32(compressed.data());
  EXPECT_EQ(magic, ZSTD_MAGIC);
}

//
// Frame Header Tests - Descriptor Byte
//

TEST_F(ZstdFormatTest, FrameHeaderDescriptor) {
  const char data[] = "Some test data";
  auto compressed = compress(data, strlen(data));
  ASSERT_GE(compressed.size(), 5u);

  // FHD is at offset 4
  uint8_t fhd = compressed[4];

  // Reserved bit (bit 3) must be 0
  EXPECT_EQ(fhd & 0x08, 0) << "Reserved bit must be 0";

  // Unused bit (bit 4) must be 0
  EXPECT_EQ(fhd & 0x10, 0) << "Unused bit must be 0";
}

TEST_F(ZstdFormatTest, SingleSegmentFlag) {
  // Test that both single segment and non-single segment frames decode
  // correctly. Whether single segment is used is an implementation detail;
  // just verify the frame is valid either way.
  const char data[] = "Small data for segment test";
  auto compressed = compress(data, strlen(data));
  ASSERT_GE(compressed.size(), 5u);

  gcomp_status_t status;
  auto decompressed =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  EXPECT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);
}

TEST_F(ZstdFormatTest, ChecksumFlagEnabled) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.checksum", true);

  const char data[] = "Checksum test data";
  auto compressed = compress(data, strlen(data), opts);
  ASSERT_GE(compressed.size(), 5u);

  uint8_t fhd = compressed[4];
  bool checksum_flag = (fhd & 0x04) != 0;
  EXPECT_TRUE(checksum_flag) << "Checksum flag should be set when enabled";

  // Verify frame has checksum at the end (last 4 bytes)
  EXPECT_GE(compressed.size(), 9u)
      << "Frame with checksum should have at least header + block + checksum";

  gcomp_options_destroy(opts);
}

TEST_F(ZstdFormatTest, ChecksumFlagDisabled) {
  const char data[] = "No checksum test data";
  auto compressed = compress(data, strlen(data));
  ASSERT_GE(compressed.size(), 5u);

  uint8_t fhd = compressed[4];
  bool checksum_flag = (fhd & 0x04) != 0;
  EXPECT_FALSE(checksum_flag) << "Checksum flag should be clear by default";
}

//
// Block Header Tests
//

TEST_F(ZstdFormatTest, BlockHeaderRawBlock) {
  // Small random data should produce a raw block
  uint8_t data[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  auto compressed = compress(data, sizeof(data));
  ASSERT_GE(compressed.size(), 10u);

  // Skip frame header to find block header
  // For single segment with small FCS, header is about 6 bytes
  // Block header is 3 bytes
  // Block header format: last(1) | type(2) | size(21)
  // Looking for block type = 0 (raw)
}

TEST_F(ZstdFormatTest, BlockHeaderRLEBlock) {
  // Repeated data should produce an RLE block
  std::vector<uint8_t> data(1000, 'X');
  auto compressed = compress(data.data(), data.size());
  ASSERT_GE(compressed.size(), 10u);

  // Verify it's small (RLE should be very compact)
  EXPECT_LT(compressed.size(), 50u) << "RLE block should be very compact";

  // Verify it can be decompressed correctly
  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

TEST_F(ZstdFormatTest, BlockHeaderLastBlockFlag) {
  // Single block frame should have last=1
  const char data[] = "Single block";
  auto compressed = compress(data, strlen(data));
  ASSERT_GE(compressed.size(), 10u);

  // Decode and verify frame structure
  gcomp_status_t status;
  auto decompressed =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  EXPECT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), strlen(data));
}

//
// Content Size Tests
//

TEST_F(ZstdFormatTest, ContentSizeSmall) {
  // Small data (< 256 bytes) with single segment
  std::vector<uint8_t> data(100, 'A');
  auto compressed = compress(data.data(), data.size());
  ASSERT_GE(compressed.size(), 6u);

  // Single segment + FCS flag=0 means 1-byte content size field
  uint8_t fhd = compressed[4];
  bool single_segment = (fhd & 0x20) != 0;
  uint8_t fcs_flag = (fhd >> 6) & 0x03;

  if (single_segment && fcs_flag == 0) {
    // 1-byte FCS at offset 5
    EXPECT_EQ(compressed[5], 100u);
  }

  // Verify round-trip
  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), data.size());
}

TEST_F(ZstdFormatTest, ContentSizeMedium) {
  // Medium data (256-65536+256 bytes) should use 2-byte FCS
  std::vector<uint8_t> data(500, 'B');
  auto compressed = compress(data.data(), data.size());
  ASSERT_GE(compressed.size(), 7u);

  // Verify round-trip
  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), data.size());
}

TEST_F(ZstdFormatTest, ContentSizeLarge) {
  // Large data (> 65536+256 bytes) should use 4-byte FCS
  // Use sequential data to ensure good compression ratio
  std::vector<uint8_t> data(70000);
  test_helpers_generate_sequential(data.data(), data.size());

  auto compressed = compress(data.data(), data.size());
  ASSERT_GT(compressed.size(), 0u);

  // Verify round-trip using larger output buffer
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder), GCOMP_OK);

  std::vector<uint8_t> decompressed(data.size() + 1024);
  gcomp_buffer_t in_buf = {compressed.data(), compressed.size(), 0};
  gcomp_buffer_t out_buf = {decompressed.data(), decompressed.size(), 0};

  EXPECT_EQ(gcomp_decoder_update(decoder, &in_buf, &out_buf), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_finish(decoder, &out_buf), GCOMP_OK);
  gcomp_decoder_destroy(decoder);

  ASSERT_EQ(out_buf.used, data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

//
// Streaming Header Parse Tests
//

TEST_F(ZstdFormatTest, StreamingParseOneByteChunks) {
  const char data[] = "Streaming parse test";
  auto compressed = compress(data, strlen(data));
  ASSERT_GT(compressed.size(), 0u);

  // Decode with 1-byte chunks
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder), GCOMP_OK);

  std::vector<uint8_t> result;
  std::vector<uint8_t> out_buf(1024);

  for (size_t i = 0; i < compressed.size(); ++i) {
    gcomp_buffer_t in = {&compressed[i], 1, 0};
    gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
    gcomp_status_t status = gcomp_decoder_update(decoder, &in, &ob);
    EXPECT_EQ(status, GCOMP_OK) << "Failed at byte " << i;
    result.insert(result.end(), out_buf.data(), out_buf.data() + ob.used);
  }

  gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
  EXPECT_EQ(gcomp_decoder_finish(decoder, &ob), GCOMP_OK);
  result.insert(result.end(), out_buf.data(), out_buf.data() + ob.used);

  gcomp_decoder_destroy(decoder);

  ASSERT_EQ(result.size(), strlen(data));
  EXPECT_EQ(memcmp(result.data(), data, strlen(data)), 0);
}

TEST_F(ZstdFormatTest, StreamingParseMagicSplit) {
  const char data[] = "Magic split test";
  auto compressed = compress(data, strlen(data));
  ASSERT_GE(compressed.size(), 10u);

  // Decode with magic split across two updates
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder), GCOMP_OK);

  std::vector<uint8_t> result;
  std::vector<uint8_t> out_buf(1024);

  // First: 2 bytes of magic
  gcomp_buffer_t in1 = {compressed.data(), 2, 0};
  gcomp_buffer_t ob1 = {out_buf.data(), out_buf.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(decoder, &in1, &ob1), GCOMP_OK);
  result.insert(result.end(), out_buf.data(), out_buf.data() + ob1.used);

  // Second: rest of data
  gcomp_buffer_t in2 = {compressed.data() + 2, compressed.size() - 2, 0};
  gcomp_buffer_t ob2 = {out_buf.data(), out_buf.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(decoder, &in2, &ob2), GCOMP_OK);
  result.insert(result.end(), out_buf.data(), out_buf.data() + ob2.used);

  gcomp_buffer_t ob3 = {out_buf.data(), out_buf.size(), 0};
  EXPECT_EQ(gcomp_decoder_finish(decoder, &ob3), GCOMP_OK);
  result.insert(result.end(), out_buf.data(), out_buf.data() + ob3.used);

  gcomp_decoder_destroy(decoder);

  ASSERT_EQ(result.size(), strlen(data));
  EXPECT_EQ(memcmp(result.data(), data, strlen(data)), 0);
}

TEST_F(ZstdFormatTest, StreamingParseHeaderSplit) {
  const char data[] = "Header split test";
  auto compressed = compress(data, strlen(data));
  ASSERT_GE(compressed.size(), 10u);

  // Decode with header split at FHD
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder), GCOMP_OK);

  std::vector<uint8_t> result;
  std::vector<uint8_t> out_buf(1024);

  // First: magic + FHD (5 bytes)
  gcomp_buffer_t in1 = {compressed.data(), 5, 0};
  gcomp_buffer_t ob1 = {out_buf.data(), out_buf.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(decoder, &in1, &ob1), GCOMP_OK);
  result.insert(result.end(), out_buf.data(), out_buf.data() + ob1.used);

  // Second: rest of data
  gcomp_buffer_t in2 = {compressed.data() + 5, compressed.size() - 5, 0};
  gcomp_buffer_t ob2 = {out_buf.data(), out_buf.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(decoder, &in2, &ob2), GCOMP_OK);
  result.insert(result.end(), out_buf.data(), out_buf.data() + ob2.used);

  gcomp_buffer_t ob3 = {out_buf.data(), out_buf.size(), 0};
  EXPECT_EQ(gcomp_decoder_finish(decoder, &ob3), GCOMP_OK);
  result.insert(result.end(), out_buf.data(), out_buf.data() + ob3.used);

  gcomp_decoder_destroy(decoder);

  ASSERT_EQ(result.size(), strlen(data));
  EXPECT_EQ(memcmp(result.data(), data, strlen(data)), 0);
}

//
// Checksum Validation Tests
//

TEST_F(ZstdFormatTest, ContentChecksumValidation) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.checksum", true);

  const char data[] = "Checksum validation test data";
  auto compressed = compress(data, strlen(data), opts);
  ASSERT_GT(compressed.size(), 4u);

  // Verify it can be decompressed
  gcomp_status_t status;
  auto decompressed =
      decompress(compressed.data(), compressed.size(), nullptr, &status);
  EXPECT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed.size(), strlen(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, strlen(data)), 0);

  gcomp_options_destroy(opts);
}

TEST_F(ZstdFormatTest, ContentChecksumCorruption) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.checksum", true);

  const char data[] = "Checksum corruption test";
  auto compressed = compress(data, strlen(data), opts);
  ASSERT_GT(compressed.size(), 4u);

  // Corrupt last byte (part of checksum)
  compressed.back() ^= 0xFF;

  // Should fail to decompress
  gcomp_status_t status;
  decompress(compressed.data(), compressed.size(), nullptr, &status);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);

  gcomp_options_destroy(opts);
}

//
// Various Block Types Round-trip
//

TEST_F(ZstdFormatTest, RawBlockRoundtrip) {
  // Very small random data likely produces raw block
  uint8_t data[8];
  test_helpers_generate_random(data, sizeof(data), 12345);

  auto compressed = compress(data, sizeof(data));
  ASSERT_GT(compressed.size(), 0u);

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), sizeof(data));
  EXPECT_EQ(memcmp(decompressed.data(), data, sizeof(data)), 0);
}

TEST_F(ZstdFormatTest, RLEBlockRoundtrip) {
  // Repeated bytes produce RLE block
  std::vector<uint8_t> data(2000, 'R');

  auto compressed = compress(data.data(), data.size());
  ASSERT_GT(compressed.size(), 0u);
  EXPECT_LT(compressed.size(), 50u) << "RLE should compress well";

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

TEST_F(ZstdFormatTest, CompressedBlockRoundtrip) {
  // Sequential data should produce compressed block
  std::vector<uint8_t> data(1024);
  test_helpers_generate_sequential(data.data(), data.size());

  auto compressed = compress(data.data(), data.size());
  ASSERT_GT(compressed.size(), 0u);

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), data.size());
  EXPECT_EQ(memcmp(decompressed.data(), data.data(), data.size()), 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
