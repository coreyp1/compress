/**
 * @file test_lz4_corruption.cpp
 *
 * Corruption detection tests for LZ4 decoder in the Ghoti.io Compress library.
 *
 * These tests verify that the decoder correctly detects and reports various
 * forms of corrupt or malformed input:
 * - Bad magic number
 * - Bad FLG version
 * - Bad header checksum
 * - Truncated header
 * - Truncated block size
 * - Truncated block data
 * - Truncated trailer
 * - Invalid offset 0
 * - Match out of bounds
 * - Block checksum mismatch
 * - Content checksum mismatch
 * - Content size mismatch
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
#include <ghoti.io/compress/xxhash32.h>
#include <gtest/gtest.h>
#include <vector>

//
// Test fixture
//

class Lz4CorruptionTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Try to decode data and return status
  gcomp_status_t tryDecode(
      const uint8_t * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "lz4", opts, &decoder);
    if (status != GCOMP_OK) {
      return status;
    }

    std::vector<uint8_t> output(65536);
    gcomp_buffer_t in_buf = {const_cast<uint8_t *>(data), len, 0};
    gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    if (status != GCOMP_OK) {
      gcomp_decoder_destroy(decoder);
      return status;
    }

    status = gcomp_decoder_finish(decoder, &out_buf);
    gcomp_decoder_destroy(decoder);
    return status;
  }

  // Helper: Compress data (for creating test input)
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

  // Helper: Create minimal valid LZ4 frame header
  std::vector<uint8_t> createMinimalHeader(
      uint8_t flg = 0x60, uint8_t bd = 0x70) {
    // Magic: 04 22 4D 18
    // FLG: flg
    // BD: bd
    // HC: computed
    uint8_t descriptor[] = {flg, bd};
    uint32_t hash = gcomp_xxhash32(descriptor, sizeof(descriptor), 0);
    uint8_t hc = (uint8_t)((hash >> 8) & 0xFF);

    return {0x04, 0x22, 0x4D, 0x18, flg, bd, hc};
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Bad Magic Number Tests
//

TEST_F(Lz4CorruptionTest, BadMagicAllZeros) {
  uint8_t data[] = {
      0x00, 0x00, 0x00, 0x00, // Wrong magic
      0x60,                   // FLG
      0x70,                   // BD
      0xDF,                   // HC
      0x00, 0x00, 0x00, 0x00, // End mark
  };

  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, BadMagicSingleBitFlip) {
  uint8_t data[] = {
      0x05, 0x22, 0x4D, 0x18, // Magic with bit flip (0x04 -> 0x05)
      0x60,                   // FLG
      0x70,                   // BD
      0xDF,                   // HC
      0x00, 0x00, 0x00, 0x00, // End mark
  };

  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, BadMagicReversed) {
  uint8_t data[] = {
      0x18, 0x4D, 0x22, 0x04, // Magic in wrong byte order
      0x60,                   // FLG
      0x70,                   // BD
      0xDF,                   // HC
      0x00, 0x00, 0x00, 0x00, // End mark
  };

  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

//
// Bad FLG Byte Tests
//

TEST_F(Lz4CorruptionTest, BadFLGVersionZero) {
  // FLG version bits = 00 (should be 01)
  uint8_t data[] = {
      0x04, 0x22, 0x4D, 0x18, // Magic
      0x20,                   // FLG: version 00
      0x70,                   // BD
      0x00,                   // HC (wrong, but version check comes first)
      0x00, 0x00, 0x00, 0x00, // End mark
  };

  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, BadFLGVersionTwo) {
  // FLG version bits = 10 (should be 01)
  uint8_t data[] = {
      0x04, 0x22, 0x4D, 0x18, // Magic
      0xA0,                   // FLG: version 10
      0x70,                   // BD
      0x00,                   // HC
      0x00, 0x00, 0x00, 0x00, // End mark
  };

  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, BadFLGVersionThree) {
  // FLG version bits = 11 (should be 01)
  uint8_t data[] = {
      0x04, 0x22, 0x4D, 0x18, // Magic
      0xE0,                   // FLG: version 11
      0x70,                   // BD
      0x00,                   // HC
      0x00, 0x00, 0x00, 0x00, // End mark
  };

  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, BadFLGReservedBitSet) {
  // FLG reserved bit 1 set (should be 0)
  uint8_t data[] = {
      0x04, 0x22, 0x4D, 0x18, // Magic
      0x62,                   // FLG: version 01, reserved bit set
      0x70,                   // BD
      0x00,                   // HC
      0x00, 0x00, 0x00, 0x00, // End mark
  };

  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

//
// Bad BD Byte Tests
//

TEST_F(Lz4CorruptionTest, BadBDBlockSizeZero) {
  // BD block max size = 0 (valid values are 4-7)
  uint8_t data[] = {
      0x04, 0x22, 0x4D, 0x18, // Magic
      0x60,                   // FLG
      0x00,                   // BD: block size 0 (invalid)
      0x00,                   // HC
      0x00, 0x00, 0x00, 0x00, // End mark
  };

  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, BadBDBlockSizeThree) {
  // BD block max size = 3 (valid values are 4-7)
  uint8_t data[] = {
      0x04, 0x22, 0x4D, 0x18, // Magic
      0x60,                   // FLG
      0x30,                   // BD: block size 3 (invalid)
      0x00,                   // HC
      0x00, 0x00, 0x00, 0x00, // End mark
  };

  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, BadBDReservedBitsSet) {
  // BD reserved bits set
  uint8_t data[] = {
      0x04, 0x22, 0x4D, 0x18, // Magic
      0x60,                   // FLG
      0x7F,                   // BD: 4MB but low reserved bits set
      0x00,                   // HC
      0x00, 0x00, 0x00, 0x00, // End mark
  };

  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, BadBDHighReservedBit) {
  // BD high reserved bit set
  uint8_t data[] = {
      0x04, 0x22, 0x4D, 0x18, // Magic
      0x60,                   // FLG
      0xF0,                   // BD: 4MB but high reserved bit set
      0x00,                   // HC
      0x00, 0x00, 0x00, 0x00, // End mark
  };

  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

//
// Bad Header Checksum Tests
//

TEST_F(Lz4CorruptionTest, BadHeaderChecksumZero) {
  uint8_t data[] = {
      0x04, 0x22, 0x4D, 0x18, // Magic
      0x60,                   // FLG
      0x70,                   // BD
      0x00,                   // HC: wrong (should be 0xDF)
      0x00, 0x00, 0x00, 0x00, // End mark
  };

  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, BadHeaderChecksumFF) {
  uint8_t data[] = {
      0x04, 0x22, 0x4D, 0x18, // Magic
      0x60,                   // FLG
      0x70,                   // BD
      0xFF,                   // HC: wrong
      0x00, 0x00, 0x00, 0x00, // End mark
  };

  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, BadHeaderChecksumOffByOne) {
  uint8_t data[] = {
      0x04, 0x22, 0x4D, 0x18, // Magic
      0x60,                   // FLG
      0x70,                   // BD
      0xDE,                   // HC: off by one (should be 0xDF)
      0x00, 0x00, 0x00, 0x00, // End mark
  };

  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

//
// Truncated Frame Tests
//

TEST_F(Lz4CorruptionTest, TruncatedAtMagic1Byte) {
  uint8_t data[] = {0x04};
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, TruncatedAtMagic2Bytes) {
  uint8_t data[] = {0x04, 0x22};
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, TruncatedAtMagic3Bytes) {
  uint8_t data[] = {0x04, 0x22, 0x4D};
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, TruncatedAfterMagic) {
  uint8_t data[] = {0x04, 0x22, 0x4D, 0x18};
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, TruncatedAfterFLG) {
  uint8_t data[] = {0x04, 0x22, 0x4D, 0x18, 0x60};
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, TruncatedAfterBD) {
  uint8_t data[] = {0x04, 0x22, 0x4D, 0x18, 0x60, 0x70};
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, TruncatedEndMark1Byte) {
  // Valid header but only 1 byte of end mark
  auto header = createMinimalHeader();
  std::vector<uint8_t> data = header;
  data.push_back(0x00); // Only 1 byte of end mark

  EXPECT_EQ(tryDecode(data.data(), data.size()), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, TruncatedEndMark3Bytes) {
  // Valid header but only 3 bytes of end mark
  auto header = createMinimalHeader();
  std::vector<uint8_t> data = header;
  data.push_back(0x00);
  data.push_back(0x00);
  data.push_back(0x00);

  EXPECT_EQ(tryDecode(data.data(), data.size()), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, TruncatedBlockSize) {
  // Valid header + partial block size (2 bytes)
  auto header = createMinimalHeader();
  std::vector<uint8_t> data = header;
  data.push_back(0x05);
  data.push_back(0x00);
  // Missing 2 more bytes of block size

  EXPECT_EQ(tryDecode(data.data(), data.size()), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, TruncatedBlockData) {
  // Valid header + block size claiming 10 bytes but only 5 provided
  auto header = createMinimalHeader();
  std::vector<uint8_t> data = header;
  data.push_back(0x0A); // Block size: 10 bytes
  data.push_back(0x00);
  data.push_back(0x00);
  data.push_back(0x80); // Uncompressed block
  // Only 5 bytes of block data
  data.push_back('H');
  data.push_back('e');
  data.push_back('l');
  data.push_back('l');
  data.push_back('o');

  EXPECT_EQ(tryDecode(data.data(), data.size()), GCOMP_ERR_CORRUPT);
}

//
// Block Checksum Mismatch Tests
//

TEST_F(Lz4CorruptionTest, BlockChecksumMismatch) {
  // Create valid frame with block checksum enabled
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.block_checksum", 1), GCOMP_OK);

  const char * input = "Hello, LZ4!";
  auto compressed = compress(input, strlen(input), opts);
  ASSERT_FALSE(compressed.empty());
  gcomp_options_destroy(opts);

  // Find the block checksum (after block data, before end mark)
  // Corrupt it
  // The block checksum is 4 bytes before the end mark (4 zero bytes)
  // For simplicity, corrupt byte near the end
  if (compressed.size() > 10) {
    compressed[compressed.size() - 5] ^= 0xFF; // Corrupt checksum
  }

  EXPECT_EQ(tryDecode(compressed.data(), compressed.size()), GCOMP_ERR_CORRUPT);
}

//
// Content Checksum Mismatch Tests
//

TEST_F(Lz4CorruptionTest, ContentChecksumMismatch) {
  // Create valid frame with content checksum enabled
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.content_checksum", 1), GCOMP_OK);

  const char * input = "Hello, LZ4!";
  auto compressed = compress(input, strlen(input), opts);
  ASSERT_FALSE(compressed.empty());
  gcomp_options_destroy(opts);

  // Content checksum is last 4 bytes
  // Corrupt it
  if (compressed.size() >= 4) {
    compressed[compressed.size() - 1] ^= 0xFF;
  }

  EXPECT_EQ(tryDecode(compressed.data(), compressed.size()), GCOMP_ERR_CORRUPT);
}

//
// Content Size Mismatch Tests
//

TEST_F(Lz4CorruptionTest, ContentSizeTooSmall) {
  // Create frame with content size that's smaller than actual
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  // Declare 5 bytes, but we'll compress 11 bytes
  ASSERT_EQ(gcomp_options_set_uint64(opts, "lz4.content_size", 5), GCOMP_OK);

  const char * input = "Hello, LZ4!"; // 11 bytes
  auto compressed = compress(input, strlen(input), opts);
  ASSERT_FALSE(compressed.empty());
  gcomp_options_destroy(opts);

  // Decoder should detect size mismatch
  EXPECT_EQ(tryDecode(compressed.data(), compressed.size()), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, ContentSizeTooLarge) {
  // Create frame with content size that's larger than actual
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  // Declare 100 bytes, but we'll compress 11 bytes
  ASSERT_EQ(gcomp_options_set_uint64(opts, "lz4.content_size", 100), GCOMP_OK);

  const char * input = "Hello, LZ4!"; // 11 bytes
  auto compressed = compress(input, strlen(input), opts);
  ASSERT_FALSE(compressed.empty());
  gcomp_options_destroy(opts);

  // Decoder should detect size mismatch
  EXPECT_EQ(tryDecode(compressed.data(), compressed.size()), GCOMP_ERR_CORRUPT);
}

//
// Corrupt Block Data Tests
//

TEST_F(Lz4CorruptionTest, CorruptedBlockDataByte) {
  // Create valid frame and corrupt a byte in the block
  std::vector<uint8_t> input(1000);
  test_helpers_generate_random(input.data(), input.size(), 12345);

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  // Corrupt a byte in the block data (after header, in the middle)
  size_t corrupt_pos = compressed.size() / 2;
  compressed[corrupt_pos] ^= 0xFF;

  // May or may not detect corruption depending on where we corrupt
  // The block decompression might produce wrong output or fail
  gcomp_status_t status = tryDecode(compressed.data(), compressed.size());
  // Either CORRUPT or potentially produces wrong output (can't always detect)
  // For LZ4 without checksums, corruption may silently produce wrong output
  (void)status; // Result depends on corruption location
}

TEST_F(Lz4CorruptionTest, CorruptedBlockDataWithBlockChecksum) {
  // Create valid frame with block checksum and corrupt block data
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.block_checksum", 1), GCOMP_OK);

  std::vector<uint8_t> input(1000);
  test_helpers_generate_random(input.data(), input.size(), 54321);

  auto compressed = compress(input.data(), input.size(), opts);
  ASSERT_FALSE(compressed.empty());
  gcomp_options_destroy(opts);

  // Corrupt a byte in the block data (after header, before checksum)
  // Header is ~7 bytes, block size is 4 bytes, so data starts around byte 11
  if (compressed.size() > 20) {
    compressed[15] ^= 0xFF; // Corrupt block data
  }

  EXPECT_EQ(tryDecode(compressed.data(), compressed.size()), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, CorruptedBlockDataWithContentChecksum) {
  // Create valid frame with content checksum and corrupt block data
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "lz4.content_checksum", 1), GCOMP_OK);

  std::vector<uint8_t> input(1000);
  test_helpers_generate_random(input.data(), input.size(), 11111);

  auto compressed = compress(input.data(), input.size(), opts);
  ASSERT_FALSE(compressed.empty());
  gcomp_options_destroy(opts);

  // Corrupt a byte in the block data
  if (compressed.size() > 20) {
    compressed[15] ^= 0xFF;
  }

  EXPECT_EQ(tryDecode(compressed.data(), compressed.size()), GCOMP_ERR_CORRUPT);
}

//
// Invalid LZ4 Block Format Tests
//

TEST_F(Lz4CorruptionTest, InvalidMatchOffset) {
  // Create a frame with a compressed block that has invalid match offset
  // LZ4 block: token byte, literals, offset (0 is invalid), match extension
  auto header = createMinimalHeader();
  std::vector<uint8_t> data = header;

  // Block size: 4 bytes compressed
  data.push_back(0x04);
  data.push_back(0x00);
  data.push_back(0x00);
  data.push_back(0x00); // Compressed (not uncompressed)

  // LZ4 block with invalid offset 0:
  // Token: 0x10 (1 literal, match_len=0+4=4)
  data.push_back(0x10);
  data.push_back('A'); // Literal
  data.push_back(0x00);
  data.push_back(0x00); // Offset: 0 (INVALID)

  // End mark
  data.push_back(0x00);
  data.push_back(0x00);
  data.push_back(0x00);
  data.push_back(0x00);

  EXPECT_EQ(tryDecode(data.data(), data.size()), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, MatchOffsetTooLarge) {
  // Create a frame with match offset larger than history
  auto header = createMinimalHeader();
  std::vector<uint8_t> data = header;

  // Block size: 5 bytes compressed
  data.push_back(0x05);
  data.push_back(0x00);
  data.push_back(0x00);
  data.push_back(0x00);

  // LZ4 block with offset larger than output so far:
  // Token: 0x10 (1 literal, match_len=0+4=4)
  data.push_back(0x10);
  data.push_back('A'); // Literal (1 byte output)
  data.push_back(0x64);
  data.push_back(0x00); // Offset: 100 (but only 1 byte in history)

  // End mark
  data.push_back(0x00);
  data.push_back(0x00);
  data.push_back(0x00);
  data.push_back(0x00);

  EXPECT_EQ(tryDecode(data.data(), data.size()), GCOMP_ERR_CORRUPT);
}

//
// Empty Input Tests
//

TEST_F(Lz4CorruptionTest, EmptyInput) {
  EXPECT_EQ(tryDecode(nullptr, 0), GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4CorruptionTest, SingleByteInput) {
  uint8_t data[] = {0x04};
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

//
// Memory Cleanup on Error Tests
//
// These tests verify no memory leaks occur when errors are encountered.
// Run with valgrind to verify.
//

TEST_F(Lz4CorruptionTest, MemoryCleanupOnBadMagic) {
  uint8_t data[] = {0x00, 0x00, 0x00, 0x00};
  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
  // Valgrind should show no leaks
}

TEST_F(Lz4CorruptionTest, MemoryCleanupOnBadChecksum) {
  uint8_t data[] = {
      0x04, 0x22, 0x4D, 0x18, // Magic
      0x60,                   // FLG
      0x70,                   // BD
      0x00,                   // Bad HC
      0x00, 0x00, 0x00, 0x00, // End mark
  };
  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
  // Valgrind should show no leaks
}

TEST_F(Lz4CorruptionTest, MemoryCleanupOnTruncatedData) {
  // Create valid compressed data and truncate it
  std::vector<uint8_t> input(1000);
  test_helpers_generate_random(input.data(), input.size(), 99999);

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  // Truncate to half
  size_t truncated_len = compressed.size() / 2;
  gcomp_status_t status = tryDecode(compressed.data(), truncated_len);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
  // Valgrind should show no leaks
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
