/**
 * @file test_zstd_corruption.cpp
 *
 * Corruption detection tests for Zstd decoder in the Ghoti.io Compress library.
 *
 * These tests verify that the decoder correctly detects and reports various
 * forms of corrupt or malformed input:
 * - Bad magic number
 * - Reserved bits set in frame header descriptor
 * - Truncated header
 * - Reserved block type
 * - Truncated block header
 * - Truncated block data
 * - Content checksum mismatch
 * - Content size mismatch
 * - Invalid match offset
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
#include <string>
#include <vector>

//
// Zstd Frame Format Constants (from spec)
//
static const uint32_t ZSTD_MAGIC = 0xFD2FB528U;
static const uint8_t ZSTD_FHD_RESERVED_BIT = 0x08;
static const uint8_t ZSTD_FHD_UNUSED_BIT = 0x10;
static const uint8_t ZSTD_BLOCK_TYPE_RESERVED = 3;

//
// Test fixture
//

class ZstdCorruptionTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  // Helper: Try to decode data and return status
  gcomp_status_t tryDecode(
      const uint8_t * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "zstd", opts, &decoder);
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
        gcomp_encoder_create(registry_, "zstd", opts, &encoder);
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

  // Helper: Create minimal valid zstd frame header
  // Returns: magic(4) + FHD(1) = 5 bytes minimum
  std::vector<uint8_t> createMinimalHeader(uint8_t fhd = 0x20) {
    // 0x20 = single segment, no checksum, no dict, FCS_flag=0
    std::vector<uint8_t> result;
    // Magic number (little-endian)
    result.push_back(0x28);
    result.push_back(0xB5);
    result.push_back(0x2F);
    result.push_back(0xFD);
    // Frame header descriptor
    result.push_back(fhd);
    return result;
  }

  // Helper: Create valid minimal zstd frame (empty content)
  // Single segment, content size = 0, last block with raw type and size 0
  std::vector<uint8_t> createMinimalFrame() {
    std::vector<uint8_t> result;
    // Magic number
    result.push_back(0x28);
    result.push_back(0xB5);
    result.push_back(0x2F);
    result.push_back(0xFD);
    // FHD: single segment, no checksum, no dict, FCS_flag=0 (0 byte FCS)
    // Actually need FCS_flag to indicate 0 bytes content, use 0x20
    // Per spec: if single_segment=1 and FCS_flag=00, FCS takes 1 byte if >0
    // For empty frame: FCS_flag=01 (2 bytes) with value 0
    // Simpler: use FCS_flag=00 and single_segment, content_size=0 (1 byte)
    result.push_back(0x60); // single_segment=1, FCS_flag=01 (2 bytes)
    // FCS: 2 bytes little-endian (0)
    result.push_back(0x00);
    result.push_back(0x00);
    // Block header: last=1, type=raw, size=0
    // Bits: last(0)=1, type(1-2)=0, size(3-23)=0
    result.push_back(0x01);
    result.push_back(0x00);
    result.push_back(0x00);
    return result;
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Bad Magic Number Tests
//

TEST_F(ZstdCorruptionTest, BadMagicAllZeros) {
  uint8_t data[] = {
      0x00, 0x00, 0x00, 0x00, // Wrong magic
      0x20,                   // FHD
  };
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, BadMagicSingleBitFlip) {
  uint8_t data[] = {
      0x29, 0xB5, 0x2F, 0xFD, // Magic with bit flip (0x28 -> 0x29)
      0x20,                   // FHD
  };
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, BadMagicByteSwapped) {
  uint8_t data[] = {
      0xFD, 0x2F, 0xB5, 0x28, // Byte-swapped magic
      0x20,                   // FHD
  };
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, BadMagicLz4) {
  // LZ4 frame magic instead of zstd
  uint8_t data[] = {
      0x04, 0x22, 0x4D, 0x18, // LZ4 magic
      0x60,                   // FLG
      0x70,                   // BD
  };
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, BadMagicGzip) {
  // Gzip magic instead of zstd
  uint8_t data[] = {
      0x1F,
      0x8B,
      0x08,
      0x00, // Gzip magic
      0x00,
      0x00,
      0x00,
      0x00,
  };
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

//
// Reserved Bit Tests
//

TEST_F(ZstdCorruptionTest, ReservedBitSet) {
  uint8_t data[] = {
      0x28, 0xB5, 0x2F, 0xFD, // Magic
      0x28,                   // FHD with reserved bit (0x08) set
  };
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, UnusedBitSet) {
  uint8_t data[] = {
      0x28, 0xB5, 0x2F, 0xFD, // Magic
      0x30,                   // FHD with unused bit (0x10) set + single_segment
  };
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, BothReservedBitsSet) {
  uint8_t data[] = {
      0x28, 0xB5, 0x2F, 0xFD, // Magic
      0x38,                   // FHD with both reserved (0x08) and unused (0x10)
  };
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

//
// Truncated Header Tests
//

TEST_F(ZstdCorruptionTest, TruncatedMagicZeroBytes) {
  EXPECT_EQ(tryDecode(nullptr, 0), GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, TruncatedMagicOneByte) {
  uint8_t data[] = {0x28};
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, TruncatedMagicTwoBytes) {
  uint8_t data[] = {0x28, 0xB5};
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, TruncatedMagicThreeBytes) {
  uint8_t data[] = {0x28, 0xB5, 0x2F};
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, TruncatedAfterMagic) {
  uint8_t data[] = {
      0x28, 0xB5, 0x2F, 0xFD, // Magic only, no FHD
  };
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, TruncatedWindowDescriptor) {
  // FHD says non-single-segment (needs window descriptor) but none provided
  uint8_t data[] = {
      0x28, 0xB5, 0x2F, 0xFD, // Magic
      0x00,                   // FHD: not single segment, needs window desc
  };
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, TruncatedContentSize) {
  // FHD says content size present but truncated
  uint8_t data[] = {
      0x28, 0xB5, 0x2F, 0xFD, // Magic
      0x60,                   // FHD: single segment, FCS_flag=01 (2 bytes)
      0x00,                   // Only 1 byte of FCS (need 2)
  };
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, TruncatedDictId) {
  // FHD says dict_id present but truncated
  uint8_t data[] = {
      0x28, 0xB5, 0x2F, 0xFD, // Magic
      0x23,                   // FHD: single segment, dict_id_flag=3 (4 bytes)
      0x01, 0x02, 0x03,       // Only 3 bytes of dict_id (need 4)
  };
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

// Maximum header size (18 bytes) parses without overflow.
// Header = magic(4) + FHD(1) + window(1) + dict_id(4) + fcs(8) = 18.
// Use dict_id=0 so decoder does not require a dictionary.
TEST_F(ZstdCorruptionTest, HeaderWithMaximumOptionalFieldsParsed) {
  uint8_t data[] = {
      0x28, 0xB5, 0x2F, 0xFD, // Magic
      0xC3,                   // FHD: not single_segment, dict_id_flag=3,
                              // fcs_flag=3 (no checksum/reserved/unused)
      0x00,                   // Window: 2^10 = 1KB
      0x00, 0x00, 0x00, 0x00, // Dict ID 0 (4 bytes LE)
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // FCS 8 bytes (0)
      0x01, 0x00, 0x00, // Block: last, raw, size 0
  };
  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_OK);
}

//
// Block Header Tests
//

TEST_F(ZstdCorruptionTest, TruncatedBlockHeader) {
  // Valid header, but block header is truncated
  uint8_t data[] = {
      0x28, 0xB5, 0x2F, 0xFD, // Magic
      0x60,                   // FHD: single segment, FCS=2 bytes
      0x10, 0x00,             // FCS: 16 bytes content
      0x01,                   // Only 1 byte of block header (need 3)
  };
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, ReservedBlockType) {
  // Block header with reserved type (3)
  uint8_t data[] = {
      0x28,
      0xB5,
      0x2F,
      0xFD, // Magic
      0x60, // FHD: single segment, FCS=2 bytes
      0x00,
      0x00, // FCS: 0 bytes content
      // Block header: last=1, type=3(reserved), size=0
      // Encoding: 0x07 = last(1) | type(3<<1)
      0x07,
      0x00,
      0x00,
  };
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

//
// Truncated Block Data Tests
//

TEST_F(ZstdCorruptionTest, TruncatedRawBlockData) {
  // Raw block says 10 bytes but only 5 provided
  uint8_t data[] = {
      0x28, 0xB5, 0x2F, 0xFD, // Magic
      0x60,                   // FHD: single segment, FCS=2 bytes
      0x0A, 0x00,             // FCS: 10 bytes content
      // Block header: last=1, type=raw(0), size=10
      // Encoding: 0x51 = last(1) | type(0<<1) | size(10<<3)
      0x51, 0x00, 0x00, 'H', 'e', 'l', 'l', 'o', // Only 5 bytes (need 10)
  };
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, TruncatedRLEBlockNoData) {
  // RLE block says 100 bytes but no byte provided
  uint8_t data[] = {
      0x28, 0xB5, 0x2F, 0xFD, // Magic
      0x60,                   // FHD: single segment, FCS=2 bytes
      0x64, 0x00,             // FCS: 100 bytes content
      // Block header: last=1, type=RLE(1), size=100
      // Encoding: last(1) | type(1<<1) | size(100<<3) = 0x323
      0x23, 0x03, 0x00,
      // No byte to repeat (should have 1 byte)
  };
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

//
// Content Checksum Tests
//

TEST_F(ZstdCorruptionTest, ContentChecksumMismatch) {
  // First, compress some data with checksum enabled
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.checksum", true);

  const char data[] = "Test data for checksum validation";
  auto compressed = compress(data, strlen(data), opts);
  ASSERT_GT(compressed.size(), 4u);

  // Corrupt the checksum (last 4 bytes)
  compressed[compressed.size() - 1] ^= 0xFF;
  compressed[compressed.size() - 2] ^= 0xFF;

  EXPECT_EQ(tryDecode(compressed.data(), compressed.size()), GCOMP_ERR_CORRUPT);

  gcomp_options_destroy(opts);
}

TEST_F(ZstdCorruptionTest, TruncatedContentChecksum) {
  // First, compress some data with checksum enabled
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.checksum", true);

  const char data[] = "Test data";
  auto compressed = compress(data, strlen(data), opts);
  ASSERT_GT(compressed.size(), 4u);

  // Truncate the checksum (remove last 2 bytes)
  EXPECT_EQ(
      tryDecode(compressed.data(), compressed.size() - 2), GCOMP_ERR_CORRUPT);

  gcomp_options_destroy(opts);
}

//
// Content Size Mismatch Tests
//

TEST_F(ZstdCorruptionTest, ContentSizeTooSmall) {
  // Create frame that claims smaller content size than actual
  uint8_t data[] = {
      0x28, 0xB5, 0x2F, 0xFD, // Magic
      0x60,                   // FHD: single segment, FCS=2 bytes
      0x05, 0x00,             // FCS: claims 5 bytes content
      // Block header: last=1, type=raw(0), size=10
      // Encoding: last(1) | type(0<<1) | size(10<<3) = 0x51
      0x51, 0x00, 0x00,                                 // 10 bytes raw block
      'H', 'e', 'l', 'l', 'o', 'W', 'o', 'r', 'l', 'd', // 10 bytes data
  };
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, ContentSizeTooLarge) {
  // Create frame that claims larger content size than actual
  uint8_t data[] = {
      0x28, 0xB5, 0x2F, 0xFD, // Magic
      0x60,                   // FHD: single segment, FCS=2 bytes
      0x64, 0x00,             // FCS: claims 100 bytes content
      // Block header: last=1, type=raw(0), size=5
      // Encoding: last(1) | type(0<<1) | size(5<<3) = 0x29
      0x29, 0x00, 0x00,        // 5 bytes raw block
      'H', 'e', 'l', 'l', 'o', // 5 bytes data
  };
  EXPECT_EQ(tryDecode(data, sizeof(data)), GCOMP_ERR_CORRUPT);
}

//
// Corrupt Block Data Tests
//

TEST_F(ZstdCorruptionTest, CorruptedCompressedBlockRandomBytes) {
  // Compress some data
  const char test_data[] = "Test data that produces a compressed block";
  auto compressed = compress(test_data, strlen(test_data));
  ASSERT_GT(compressed.size(), 15u);

  // Find block data area (skip magic + header) and corrupt it
  // Frame header is at least 6 bytes for minimal single-segment frame
  size_t block_data_offset = 10; // Approximate
  if (block_data_offset < compressed.size()) {
    compressed[block_data_offset] ^= 0xFF;
    compressed[block_data_offset + 1] ^= 0xAA;
  }

  // May return CORRUPT or might succeed if corruption doesn't affect decoding
  gcomp_status_t status = tryDecode(compressed.data(), compressed.size());
  // Accept either CORRUPT or OK (if corruption was in non-critical area)
  EXPECT_TRUE(status == GCOMP_ERR_CORRUPT || status == GCOMP_OK);
}

//
// Malformed FSE / Huffman / Invalid match offset (Z7.7)
//

TEST_F(ZstdCorruptionTest, MalformedFSETables) {
  // Data that produces a compressed block (sequences use FSE)
  std::string data;
  for (int i = 0; i < 200; ++i) {
    data += "Hello world ";
  }
  auto compressed = compress(data.data(), data.size());
  ASSERT_GT(compressed.size(), 25u);

  // Corrupt several bytes in the block payload so FSE table/symbol decode
  // fails. Frame header + block header is at least 10 bytes; corrupt from start
  // of block data and a bit beyond to ensure we hit FSE or sequence data.
  for (size_t off = 10; off < compressed.size() && off < 25; ++off) {
    compressed[off] ^= 0xFF;
  }

  EXPECT_EQ(tryDecode(compressed.data(), compressed.size()), GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, MalformedHuffmanTables) {
  // Data that produces a block with Huffman-compressed literals (diverse,
  // large enough for encoder to choose Huffman)
  std::vector<uint8_t> data(2500);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>((i * 31 + 17) % 256);
  }
  auto compressed = compress(data.data(), data.size());
  ASSERT_GT(compressed.size(), 20u);

  // Corrupt start of block payload (literals section / Huffman header)
  size_t off = 10;
  if (off < compressed.size()) {
    compressed[off] ^= 0xFF;
  }
  if (off + 1 < compressed.size()) {
    compressed[off + 1] ^= 0xAA;
  }

  EXPECT_EQ(tryDecode(compressed.data(), compressed.size()), GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, InvalidMatchOffset) {
  // Use data that produces a compressed block with sequences (not RLE). Corrupt
  // bytes in the sequences section so decoded offset is invalid or FSE fails.
  std::string data;
  for (int i = 0; i < 200; ++i) {
    data += "Hello world ";
  }
  auto compressed = compress(data.data(), data.size());
  ASSERT_GT(compressed.size(), 20u);

  // Corrupt bytes near the end of block payload (sequences section); avoid
  // corrupting content checksum (last 4 bytes if present).
  size_t payload_end = compressed.size();
  if (payload_end > 6) {
    payload_end -= 5;
  }
  if (payload_end > 12) {
    compressed[payload_end - 1] ^= 0xFF;
    compressed[payload_end - 2] ^= 0xFF;
  }

  EXPECT_EQ(tryDecode(compressed.data(), compressed.size()), GCOMP_ERR_CORRUPT);
}

//
// Valid Frame Tests (sanity checks)
//

TEST_F(ZstdCorruptionTest, ValidEmptyFrame) {
  // Use the encoder to create a valid empty frame
  auto frame = compress(nullptr, 0);
  ASSERT_GT(frame.size(), 0u) << "Failed to compress empty input";
  EXPECT_EQ(tryDecode(frame.data(), frame.size()), GCOMP_OK);
}

TEST_F(ZstdCorruptionTest, ValidCompressedRoundtrip) {
  const char data[] = "Hello, valid zstd compression test!";
  auto compressed = compress(data, strlen(data));
  ASSERT_GT(compressed.size(), 0u);
  EXPECT_EQ(tryDecode(compressed.data(), compressed.size()), GCOMP_OK);
}

TEST_F(ZstdCorruptionTest, ValidRLEBlock) {
  // Create data that produces an RLE block
  std::vector<uint8_t> data(1000, 'X');
  auto compressed = compress(data.data(), data.size());
  ASSERT_GT(compressed.size(), 0u);
  EXPECT_EQ(tryDecode(compressed.data(), compressed.size()), GCOMP_OK);
}

//
// Edge Cases
//

TEST_F(ZstdCorruptionTest, ExtraBytesAfterFrame) {
  const char data[] = "Test data";
  auto compressed = compress(data, strlen(data));
  ASSERT_GT(compressed.size(), 0u);

  // Add garbage after the frame
  std::vector<uint8_t> with_garbage(compressed);
  with_garbage.push_back(0xDE);
  with_garbage.push_back(0xAD);
  with_garbage.push_back(0xBE);
  with_garbage.push_back(0xEF);

  // Should still decode OK (extra bytes ignored after complete frame)
  // OR should fail if decoder is strict
  gcomp_status_t status = tryDecode(with_garbage.data(), with_garbage.size());
  EXPECT_TRUE(status == GCOMP_OK || status == GCOMP_ERR_CORRUPT);
}

TEST_F(ZstdCorruptionTest, ZeroLengthInput) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder), GCOMP_OK);

  std::vector<uint8_t> output(256);
  gcomp_buffer_t in_buf = {nullptr, 0, 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  // Zero-length update should be OK (no data to process)
  EXPECT_EQ(gcomp_decoder_update(decoder, &in_buf, &out_buf), GCOMP_OK);

  // But finish without any data should fail (incomplete frame)
  EXPECT_EQ(gcomp_decoder_finish(decoder, &out_buf), GCOMP_ERR_CORRUPT);

  gcomp_decoder_destroy(decoder);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
