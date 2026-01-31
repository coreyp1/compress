/**
 * @file test_gzip_corruption.cpp
 *
 * Corruption tests for gzip decoder in the Ghoti.io Compress library.
 *
 * These tests verify that the decoder correctly detects and reports
 * various forms of corrupted or malformed gzip data:
 * - Invalid magic bytes
 * - Wrong compression method
 * - Reserved flag bits set
 * - Truncated data (header, body, trailer)
 * - CRC/ISIZE mismatches
 * - Invalid header field lengths
 * - Malformed optional fields
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

// RFC 1952 constants
#define GZIP_ID1 0x1F
#define GZIP_ID2 0x8B
#define GZIP_CM_DEFLATE 8
#define GZIP_FLG_FTEXT 0x01
#define GZIP_FLG_FHCRC 0x02
#define GZIP_FLG_FEXTRA 0x04
#define GZIP_FLG_FNAME 0x08
#define GZIP_FLG_FCOMMENT 0x10
#define GZIP_FLG_RESERVED 0xE0

//
// Test fixture
//

class GzipCorruptionTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Create valid gzip data for corruption testing
  std::vector<uint8_t> createValidGzip(
      const void * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "gzip", opts, &encoder);
    if (status != GCOMP_OK) {
      return {};
    }

    std::vector<uint8_t> result;
    result.resize(len + len / 10 + 256);

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

  // Helper: Try to decode data and return the status
  gcomp_status_t tryDecode(
      const void * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "gzip", opts, &decoder);
    if (status != GCOMP_OK) {
      return status;
    }

    std::vector<uint8_t> output(len * 1000 + 65536);

    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};
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

  gcomp_registry_t * registry_ = nullptr;
};

//
// Magic Byte Tests
//

TEST_F(GzipCorruptionTest, WrongID1) {
  uint8_t data[] = {
      0x00,                   // Wrong ID1 (should be 0x1F)
      GZIP_ID2,               // ID2
      GZIP_CM_DEFLATE,        // CM
      0x00,                   // FLG
      0x00, 0x00, 0x00, 0x00, // MTIME
      0x00, 0xFF,             // XFL, OS
  };

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, WrongID2) {
  uint8_t data[] = {
      GZIP_ID1,               // ID1
      0x00,                   // Wrong ID2 (should be 0x8B)
      GZIP_CM_DEFLATE,        // CM
      0x00,                   // FLG
      0x00, 0x00, 0x00, 0x00, // MTIME
      0x00, 0xFF,             // XFL, OS
  };

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, BothMagicBytesWrong) {
  uint8_t data[] = {
      0xFF, 0xFF,             // Both magic bytes wrong
      GZIP_CM_DEFLATE,        // CM
      0x00,                   // FLG
      0x00, 0x00, 0x00, 0x00, // MTIME
      0x00, 0xFF,             // XFL, OS
  };

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

//
// Compression Method Tests
//

TEST_F(GzipCorruptionTest, WrongCM_Zero) {
  uint8_t data[] = {
      GZIP_ID1, GZIP_ID2,     // Magic
      0x00,                   // Wrong CM (should be 8)
      0x00,                   // FLG
      0x00, 0x00, 0x00, 0x00, // MTIME
      0x00, 0xFF,             // XFL, OS
  };

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
}

TEST_F(GzipCorruptionTest, WrongCM_Seven) {
  uint8_t data[] = {
      GZIP_ID1, GZIP_ID2,     // Magic
      7,                      // Wrong CM (should be 8)
      0x00,                   // FLG
      0x00, 0x00, 0x00, 0x00, // MTIME
      0x00, 0xFF,             // XFL, OS
  };

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
}

TEST_F(GzipCorruptionTest, WrongCM_Nine) {
  uint8_t data[] = {
      GZIP_ID1, GZIP_ID2,     // Magic
      9,                      // Wrong CM (should be 8)
      0x00,                   // FLG
      0x00, 0x00, 0x00, 0x00, // MTIME
      0x00, 0xFF,             // XFL, OS
  };

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
}

//
// Reserved Flag Bits Tests
//

TEST_F(GzipCorruptionTest, ReservedBit5Set) {
  uint8_t data[] = {
      GZIP_ID1, GZIP_ID2, GZIP_CM_DEFLATE,
      0x20,                   // Reserved bit 5 set
      0x00, 0x00, 0x00, 0x00, // MTIME
      0x00, 0xFF,             // XFL, OS
  };

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, ReservedBit6Set) {
  uint8_t data[] = {
      GZIP_ID1, GZIP_ID2, GZIP_CM_DEFLATE,
      0x40,                   // Reserved bit 6 set
      0x00, 0x00, 0x00, 0x00, // MTIME
      0x00, 0xFF,             // XFL, OS
  };

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, ReservedBit7Set) {
  uint8_t data[] = {
      GZIP_ID1, GZIP_ID2, GZIP_CM_DEFLATE,
      0x80,                   // Reserved bit 7 set
      0x00, 0x00, 0x00, 0x00, // MTIME
      0x00, 0xFF,             // XFL, OS
  };

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, AllReservedBitsSet) {
  uint8_t data[] = {
      GZIP_ID1, GZIP_ID2, GZIP_CM_DEFLATE,
      0xE0,                   // All reserved bits set
      0x00, 0x00, 0x00, 0x00, // MTIME
      0x00, 0xFF,             // XFL, OS
  };

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

//
// Truncated Header Tests
//

TEST_F(GzipCorruptionTest, TruncatedAtMagic1) {
  uint8_t data[] = {GZIP_ID1}; // Only first magic byte

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, TruncatedAtMagic2) {
  uint8_t data[] = {GZIP_ID1, GZIP_ID2}; // Only magic bytes

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, TruncatedInMTIME) {
  uint8_t data[] = {
      GZIP_ID1, GZIP_ID2, GZIP_CM_DEFLATE, 0x00, // Header up to FLG
      0x12, 0x34,                                // Partial MTIME (2/4 bytes)
  };

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, TruncatedAtXFL) {
  uint8_t data[] = {
      GZIP_ID1, GZIP_ID2, GZIP_CM_DEFLATE, 0x00, 0x00, 0x00, 0x00,
      0x00, // MTIME complete
      0x00, // XFL but no OS
  };

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, TruncatedFEXTRALength) {
  uint8_t data[] = {
      GZIP_ID1, GZIP_ID2, GZIP_CM_DEFLATE,
      GZIP_FLG_FEXTRA,        // FLG with FEXTRA
      0x00, 0x00, 0x00, 0x00, // MTIME
      0x00, 0xFF,             // XFL, OS
      0x10,                   // XLEN low byte only (missing high byte)
  };

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, TruncatedFEXTRAData) {
  uint8_t data[] = {
      GZIP_ID1, GZIP_ID2, GZIP_CM_DEFLATE,
      GZIP_FLG_FEXTRA,        // FLG with FEXTRA
      0x00, 0x00, 0x00, 0x00, // MTIME
      0x00, 0xFF,             // XFL, OS
      0x10, 0x00,             // XLEN = 16
      0x01, 0x02, 0x03,       // Only 3 bytes of 16
  };

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, TruncatedFNAME) {
  uint8_t data[] = {
      GZIP_ID1, GZIP_ID2, GZIP_CM_DEFLATE,
      GZIP_FLG_FNAME,         // FLG with FNAME
      0x00, 0x00, 0x00, 0x00, // MTIME
      0x00, 0xFF,             // XFL, OS
      't', 'e', 's', 't',     // FNAME without null terminator
  };

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, TruncatedFCOMMENT) {
  uint8_t data[] = {
      GZIP_ID1, GZIP_ID2, GZIP_CM_DEFLATE,
      GZIP_FLG_FCOMMENT,      // FLG with FCOMMENT
      0x00, 0x00, 0x00, 0x00, // MTIME
      0x00, 0xFF,             // XFL, OS
      'c', 'o', 'm', 'm',     // FCOMMENT without null terminator
  };

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, TruncatedFHCRC) {
  uint8_t data[] = {
      GZIP_ID1, GZIP_ID2, GZIP_CM_DEFLATE,
      GZIP_FLG_FHCRC,         // FLG with FHCRC
      0x00, 0x00, 0x00, 0x00, // MTIME
      0x00, 0xFF,             // XFL, OS
      0x12,                   // Only 1 byte of 2-byte CRC
  };

  gcomp_status_t status = tryDecode(data, sizeof(data));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

//
// Truncated Body Tests
//

TEST_F(GzipCorruptionTest, TruncatedBody) {
  // Create valid gzip data, then truncate it
  const char * test_data = "Test data for truncation";
  auto valid = createValidGzip(test_data, strlen(test_data));
  ASSERT_FALSE(valid.empty());
  ASSERT_GT(valid.size(), 20u);

  // Keep only header + partial body (remove trailer and some body)
  size_t truncated_len = valid.size() - 12;
  gcomp_status_t status = tryDecode(valid.data(), truncated_len);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

//
// Truncated Trailer Tests
//

TEST_F(GzipCorruptionTest, TruncatedTrailer_NoCRC) {
  // Create valid gzip data, then remove entire trailer
  const char * test_data = "Test data";
  auto valid = createValidGzip(test_data, strlen(test_data));
  ASSERT_FALSE(valid.empty());
  ASSERT_GT(valid.size(), 8u);

  // Remove entire 8-byte trailer
  size_t truncated_len = valid.size() - 8;
  gcomp_status_t status = tryDecode(valid.data(), truncated_len);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, TruncatedTrailer_PartialCRC) {
  const char * test_data = "Test data";
  auto valid = createValidGzip(test_data, strlen(test_data));
  ASSERT_FALSE(valid.empty());
  ASSERT_GT(valid.size(), 8u);

  // Remove 6 bytes (partial CRC, no ISIZE)
  size_t truncated_len = valid.size() - 6;
  gcomp_status_t status = tryDecode(valid.data(), truncated_len);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, TruncatedTrailer_NoISIZE) {
  const char * test_data = "Test data";
  auto valid = createValidGzip(test_data, strlen(test_data));
  ASSERT_FALSE(valid.empty());
  ASSERT_GT(valid.size(), 8u);

  // Remove 4 bytes (ISIZE)
  size_t truncated_len = valid.size() - 4;
  gcomp_status_t status = tryDecode(valid.data(), truncated_len);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, TruncatedTrailer_PartialISIZE) {
  const char * test_data = "Test data";
  auto valid = createValidGzip(test_data, strlen(test_data));
  ASSERT_FALSE(valid.empty());
  ASSERT_GT(valid.size(), 8u);

  // Remove 2 bytes (partial ISIZE)
  size_t truncated_len = valid.size() - 2;
  gcomp_status_t status = tryDecode(valid.data(), truncated_len);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

//
// CRC/ISIZE Mismatch Tests
//

TEST_F(GzipCorruptionTest, CRCMismatch_FirstByte) {
  const char * test_data = "Test data for CRC check";
  auto valid = createValidGzip(test_data, strlen(test_data));
  ASSERT_FALSE(valid.empty());
  ASSERT_GT(valid.size(), 8u);

  // Corrupt first byte of CRC (4th from end)
  valid[valid.size() - 8] ^= 0xFF;

  gcomp_status_t status = tryDecode(valid.data(), valid.size());
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, CRCMismatch_LastByte) {
  const char * test_data = "Test data for CRC check";
  auto valid = createValidGzip(test_data, strlen(test_data));
  ASSERT_FALSE(valid.empty());
  ASSERT_GT(valid.size(), 8u);

  // Corrupt last byte of CRC (5th from end)
  valid[valid.size() - 5] ^= 0xFF;

  gcomp_status_t status = tryDecode(valid.data(), valid.size());
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, ISIZEMismatch_FirstByte) {
  const char * test_data = "Test data for ISIZE check";
  auto valid = createValidGzip(test_data, strlen(test_data));
  ASSERT_FALSE(valid.empty());
  ASSERT_GT(valid.size(), 8u);

  // Corrupt first byte of ISIZE (4th from end)
  valid[valid.size() - 4] ^= 0xFF;

  gcomp_status_t status = tryDecode(valid.data(), valid.size());
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, ISIZEMismatch_LastByte) {
  const char * test_data = "Test data for ISIZE check";
  auto valid = createValidGzip(test_data, strlen(test_data));
  ASSERT_FALSE(valid.empty());
  ASSERT_GT(valid.size(), 8u);

  // Corrupt last byte of ISIZE
  valid[valid.size() - 1] ^= 0xFF;

  gcomp_status_t status = tryDecode(valid.data(), valid.size());
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, BothCRCAndISIZECorrupt) {
  const char * test_data = "Test data";
  auto valid = createValidGzip(test_data, strlen(test_data));
  ASSERT_FALSE(valid.empty());
  ASSERT_GT(valid.size(), 8u);

  // Corrupt both CRC and ISIZE
  valid[valid.size() - 8] ^= 0xFF;
  valid[valid.size() - 4] ^= 0xFF;

  gcomp_status_t status = tryDecode(valid.data(), valid.size());
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

//
// Header Field Limit Tests
//

TEST_F(GzipCorruptionTest, FEXTRAExceedsLimit) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Set very small FEXTRA limit
  status = gcomp_options_set_uint64(opts, "gzip.max_extra_bytes", 5);
  ASSERT_EQ(status, GCOMP_OK);

  // Header with FEXTRA length > 5
  uint8_t data[] = {
      GZIP_ID1, GZIP_ID2, GZIP_CM_DEFLATE,
      GZIP_FLG_FEXTRA,        // FLG with FEXTRA
      0x00, 0x00, 0x00, 0x00, // MTIME
      0x00, 0xFF,             // XFL, OS
      0x10, 0x00,             // XLEN = 16 (exceeds limit of 5)
                              // Extra data would follow...
  };

  status = tryDecode(data, sizeof(data), opts);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_options_destroy(opts);
}

TEST_F(GzipCorruptionTest, FNAMEExceedsLimit) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Set very small FNAME limit
  status = gcomp_options_set_uint64(opts, "gzip.max_name_bytes", 5);
  ASSERT_EQ(status, GCOMP_OK);

  // Header with long FNAME
  uint8_t data[] = {
      GZIP_ID1, GZIP_ID2, GZIP_CM_DEFLATE,
      GZIP_FLG_FNAME,                              // FLG with FNAME
      0x00, 0x00, 0x00, 0x00,                      // MTIME
      0x00, 0xFF,                                  // XFL, OS
      'l', 'o', 'n', 'g', 'n', 'a', 'm', 'e', 0x00 // 8-char name (> 5)
  };

  status = tryDecode(data, sizeof(data), opts);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_options_destroy(opts);
}

TEST_F(GzipCorruptionTest, FCOMMENTExceedsLimit) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Set very small FCOMMENT limit
  status = gcomp_options_set_uint64(opts, "gzip.max_comment_bytes", 5);
  ASSERT_EQ(status, GCOMP_OK);

  // Header with long FCOMMENT
  uint8_t data[] = {
      GZIP_ID1, GZIP_ID2, GZIP_CM_DEFLATE,
      GZIP_FLG_FCOMMENT,                           // FLG with FCOMMENT
      0x00, 0x00, 0x00, 0x00,                      // MTIME
      0x00, 0xFF,                                  // XFL, OS
      'l', 'o', 'n', 'g', 'c', 'o', 'm', 'm', 0x00 // 8-char comment (> 5)
  };

  status = tryDecode(data, sizeof(data), opts);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_options_destroy(opts);
}

//
// FHCRC Validation Tests
//

TEST_F(GzipCorruptionTest, FHCRCMismatch) {
  // Create valid gzip with header CRC
  gcomp_options_t * enc_opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&enc_opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(enc_opts, "gzip.header_crc", 1);
  ASSERT_EQ(status, GCOMP_OK);

  const char * test_data = "FHCRC test data";
  auto valid = createValidGzip(test_data, strlen(test_data), enc_opts);
  gcomp_options_destroy(enc_opts);
  ASSERT_FALSE(valid.empty());

  // Find and corrupt the header CRC (at byte 10-11 for minimal header + FHCRC)
  // Header is: 10 bytes fixed + 2 bytes CRC16 = 12 bytes header
  valid[10] ^= 0xFF;

  status = tryDecode(valid.data(), valid.size());
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

//
// Body Corruption Tests
//

TEST_F(GzipCorruptionTest, CorruptedDeflateData) {
  const char * test_data = "Test data for deflate corruption";
  auto valid = createValidGzip(test_data, strlen(test_data));
  ASSERT_FALSE(valid.empty());
  ASSERT_GT(valid.size(), 20u);

  // Corrupt a byte in the middle of the deflate data
  size_t mid = valid.size() / 2;
  valid[mid] ^= 0xFF;

  gcomp_status_t status = tryDecode(valid.data(), valid.size());
  // Could be CORRUPT from deflate or from CRC mismatch
  EXPECT_TRUE(status == GCOMP_ERR_CORRUPT || status == GCOMP_ERR_CORRUPT);
}

//
// Empty and Edge Cases
//

TEST_F(GzipCorruptionTest, EmptyData) {
  gcomp_status_t status = tryDecode(nullptr, 0);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, SingleByte) {
  uint8_t single[] = {GZIP_ID1};
  gcomp_status_t status = tryDecode(single, 1);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipCorruptionTest, RandomGarbage) {
  std::vector<uint8_t> garbage(100);
  test_helpers_generate_random(garbage.data(), garbage.size(), 42);

  gcomp_status_t status = tryDecode(garbage.data(), garbage.size());
  // Should fail - either corrupt magic or other issue
  EXPECT_NE(status, GCOMP_OK);
}

TEST_F(GzipCorruptionTest, AllZeros) {
  std::vector<uint8_t> zeros(100, 0);
  gcomp_status_t status = tryDecode(zeros.data(), zeros.size());
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT); // Wrong magic bytes
}

TEST_F(GzipCorruptionTest, AllOnes) {
  std::vector<uint8_t> ones(100, 0xFF);
  gcomp_status_t status = tryDecode(ones.data(), ones.size());
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT); // Wrong magic bytes
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
