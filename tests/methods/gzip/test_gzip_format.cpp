/**
 * @file test_gzip_format.cpp
 *
 * Unit tests for gzip format helpers (RFC 1952 header/trailer).
 *
 * These tests verify:
 * - Header writer with various optional fields
 * - Header parser with streaming input
 * - Trailer writer and validator
 * - Error handling for malformed headers
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/crc32.h>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/gzip.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <vector>

// Access to internal header format functions for direct testing
extern "C" {
// These are declared in gzip_internal.h but we include the prototypes here
// since we can't easily include gzip_internal.h from tests

// RFC 1952 Constants
#define GZIP_ID1 0x1F
#define GZIP_ID2 0x8B
#define GZIP_CM_DEFLATE 8
#define GZIP_HEADER_MIN_SIZE 10
#define GZIP_TRAILER_SIZE 8
#define GZIP_OS_UNKNOWN 255

// FLG bit masks
#define GZIP_FLG_FTEXT 0x01
#define GZIP_FLG_FHCRC 0x02
#define GZIP_FLG_FEXTRA 0x04
#define GZIP_FLG_FNAME 0x08
#define GZIP_FLG_FCOMMENT 0x10
#define GZIP_FLG_RESERVED 0xE0
}

//
// Test fixture
//

class GzipFormatTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Get default registry (with deflate registered)
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Header Writer Tests
//

TEST_F(GzipFormatTest, MinimalHeader) {
  // Create encoder with no options - minimal header
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(encoder, nullptr);

  // Encode empty data to get header
  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // Verify minimal header structure (10 bytes minimum)
  ASSERT_GE(out_buf.used, 10u);
  EXPECT_EQ(output[0], GZIP_ID1);
  EXPECT_EQ(output[1], GZIP_ID2);
  EXPECT_EQ(output[2], GZIP_CM_DEFLATE);
  // FLG byte at index 3 - should be 0 for minimal header
  EXPECT_EQ(output[3], 0x00);
  // OS byte at index 9 should be 255 (unknown)
  EXPECT_EQ(output[9], GZIP_OS_UNKNOWN);

  gcomp_encoder_destroy(encoder);
}

TEST_F(GzipFormatTest, HeaderWithFNAME) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_string(opts, "gzip.name", "test.txt");
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "gzip", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // Verify header
  ASSERT_GE(out_buf.used, 10u);
  EXPECT_EQ(output[0], GZIP_ID1);
  EXPECT_EQ(output[1], GZIP_ID2);
  // FLG should have FNAME bit set
  EXPECT_TRUE(output[3] & GZIP_FLG_FNAME);

  // Find the filename in header (after fixed 10-byte header)
  const char * expected_name = "test.txt";
  size_t name_len = strlen(expected_name) + 1; // +1 for null terminator
  ASSERT_GE(out_buf.used, 10u + name_len);
  EXPECT_EQ(memcmp(output + 10, expected_name, name_len), 0);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

TEST_F(GzipFormatTest, HeaderWithFCOMMENT) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_string(opts, "gzip.comment", "Test comment");
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "gzip", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // FLG should have FCOMMENT bit set
  EXPECT_TRUE(output[3] & GZIP_FLG_FCOMMENT);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

TEST_F(GzipFormatTest, HeaderWithFNAMEAndFCOMMENT) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_string(opts, "gzip.name", "myfile.dat");
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_string(opts, "gzip.comment", "My comment");
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "gzip", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // FLG should have both FNAME and FCOMMENT bits set
  EXPECT_TRUE(output[3] & GZIP_FLG_FNAME);
  EXPECT_TRUE(output[3] & GZIP_FLG_FCOMMENT);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

TEST_F(GzipFormatTest, HeaderWithFEXTRA) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t extra_data[] = {0x41, 0x42, 0x01, 0x00, 0x55}; // "AB" subfield
  status = gcomp_options_set_bytes(
      opts, "gzip.extra", extra_data, sizeof(extra_data));
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "gzip", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // FLG should have FEXTRA bit set
  EXPECT_TRUE(output[3] & GZIP_FLG_FEXTRA);

  // Extra field length at bytes 10-11 (little-endian)
  uint16_t xlen = (uint16_t)output[10] | ((uint16_t)output[11] << 8);
  EXPECT_EQ(xlen, sizeof(extra_data));

  // Extra field data
  EXPECT_EQ(memcmp(output + 12, extra_data, sizeof(extra_data)), 0);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

TEST_F(GzipFormatTest, HeaderWithFHCRC) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_bool(opts, "gzip.header_crc", 1);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "gzip", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // FLG should have FHCRC bit set
  EXPECT_TRUE(output[3] & GZIP_FLG_FHCRC);

  // Header should be 12 bytes (10 + 2 for CRC16)
  ASSERT_GE(out_buf.used, 12u);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

TEST_F(GzipFormatTest, HeaderWithAllOptionalFields) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t extra_data[] = {0x00, 0x01};
  status = gcomp_options_set_bytes(opts, "gzip.extra", extra_data, 2);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_string(opts, "gzip.name", "all.txt");
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_string(opts, "gzip.comment", "All fields");
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "gzip.header_crc", 1);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "gzip", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // FLG should have all optional bits set (except FTEXT)
  EXPECT_TRUE(output[3] & GZIP_FLG_FEXTRA);
  EXPECT_TRUE(output[3] & GZIP_FLG_FNAME);
  EXPECT_TRUE(output[3] & GZIP_FLG_FCOMMENT);
  EXPECT_TRUE(output[3] & GZIP_FLG_FHCRC);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

TEST_F(GzipFormatTest, XFLAutoCalculationFastest) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Level 1 should set XFL=4 (fastest)
  status = gcomp_options_set_int64(opts, "deflate.level", 1);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "gzip", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // XFL at byte 8 should be 4 for fastest
  EXPECT_EQ(output[8], 4);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

TEST_F(GzipFormatTest, XFLAutoCalculationMaximum) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Level 9 should set XFL=2 (maximum compression)
  status = gcomp_options_set_int64(opts, "deflate.level", 9);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "gzip", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // XFL at byte 8 should be 2 for maximum compression
  EXPECT_EQ(output[8], 2);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

TEST_F(GzipFormatTest, CustomMTIMEAndOS) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Set custom MTIME (Unix timestamp)
  uint64_t mtime = 0x12345678;
  status = gcomp_options_set_uint64(opts, "gzip.mtime", mtime);
  ASSERT_EQ(status, GCOMP_OK);

  // Set custom OS (Unix = 3)
  status = gcomp_options_set_uint64(opts, "gzip.os", 3);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "gzip", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // MTIME at bytes 4-7 (little-endian)
  uint32_t actual_mtime = (uint32_t)output[4] | ((uint32_t)output[5] << 8) |
      ((uint32_t)output[6] << 16) | ((uint32_t)output[7] << 24);
  EXPECT_EQ(actual_mtime, mtime);

  // OS at byte 9
  EXPECT_EQ(output[9], 3);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

//
// Header Parser Tests (via decoder)
//

TEST_F(GzipFormatTest, DecodeMinimalHeader) {
  // First encode to get valid minimal gzip data
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t compressed[256];
  gcomp_buffer_t out_buf = {compressed, sizeof(compressed), 0};

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  gcomp_encoder_destroy(encoder);

  // Now decode it
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_buffer_t in_buf = {compressed, out_buf.used, 0};
  uint8_t decompressed[256];
  gcomp_buffer_t dec_out = {decompressed, sizeof(decompressed), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &dec_out);
  EXPECT_EQ(status, GCOMP_OK);

  status = gcomp_decoder_finish(decoder, &dec_out);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_decoder_destroy(decoder);
}

TEST_F(GzipFormatTest, DecodeStreamingByteByByte) {
  // Encode some data
  const char * test_data = "Hello, gzip!";
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t compressed[512];
  gcomp_buffer_t enc_in = {(void *)test_data, strlen(test_data), 0};
  gcomp_buffer_t enc_out = {compressed, sizeof(compressed), 0};

  status = gcomp_encoder_update(encoder, &enc_in, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  gcomp_encoder_destroy(encoder);

  // Decode byte-by-byte
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t decompressed[512];
  size_t dec_pos = 0;

  for (size_t i = 0; i < enc_out.used; i++) {
    gcomp_buffer_t in_buf = {compressed + i, 1, 0};
    gcomp_buffer_t out_buf = {
        decompressed + dec_pos, sizeof(decompressed) - dec_pos, 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    EXPECT_EQ(status, GCOMP_OK);
    dec_pos += out_buf.used;
  }

  gcomp_buffer_t final_out = {
      decompressed + dec_pos, sizeof(decompressed) - dec_pos, 0};
  status = gcomp_decoder_finish(decoder, &final_out);
  EXPECT_EQ(status, GCOMP_OK);
  dec_pos += final_out.used;

  // Verify decompressed data
  EXPECT_EQ(dec_pos, strlen(test_data));
  EXPECT_EQ(memcmp(decompressed, test_data, dec_pos), 0);

  gcomp_decoder_destroy(decoder);
}

TEST_F(GzipFormatTest, DecodeWithRandomChunkSizes) {
  // Encode some data
  const char * test_data = "The quick brown fox jumps over the lazy dog.";
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t compressed[512];
  gcomp_buffer_t enc_in = {(void *)test_data, strlen(test_data), 0};
  gcomp_buffer_t enc_out = {compressed, sizeof(compressed), 0};

  status = gcomp_encoder_update(encoder, &enc_in, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  gcomp_encoder_destroy(encoder);

  // Decode with random-ish chunk sizes
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t decompressed[512];
  size_t dec_pos = 0;
  size_t in_pos = 0;
  size_t chunk_sizes[] = {1, 3, 7, 2, 11, 5, 13, 17, 23, 100};
  size_t chunk_idx = 0;

  while (in_pos < enc_out.used) {
    size_t chunk = std::min(chunk_sizes[chunk_idx % 10], enc_out.used - in_pos);
    chunk_idx++;

    gcomp_buffer_t in_buf = {compressed + in_pos, chunk, 0};
    gcomp_buffer_t out_buf = {
        decompressed + dec_pos, sizeof(decompressed) - dec_pos, 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    EXPECT_EQ(status, GCOMP_OK);
    if (status != GCOMP_OK) {
      break; // Don't loop forever on error
    }
    in_pos += in_buf.used;
    dec_pos += out_buf.used;
  }

  gcomp_buffer_t final_out = {
      decompressed + dec_pos, sizeof(decompressed) - dec_pos, 0};
  status = gcomp_decoder_finish(decoder, &final_out);
  EXPECT_EQ(status, GCOMP_OK);
  if (status != GCOMP_OK) {
    gcomp_decoder_destroy(decoder);
    return; // Exit test on error
  }
  dec_pos += final_out.used;

  // Verify
  EXPECT_EQ(dec_pos, strlen(test_data));
  EXPECT_EQ(memcmp(decompressed, test_data, dec_pos), 0);

  gcomp_decoder_destroy(decoder);
}

//
// Error Cases - Header Parser
//

TEST_F(GzipFormatTest, ErrorWrongMagicBytes) {
  // Invalid magic bytes
  uint8_t bad_data[] = {
      0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x03, 0x00};

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_buffer_t in_buf = {bad_data, sizeof(bad_data), 0};
  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);

  gcomp_decoder_destroy(decoder);
}

TEST_F(GzipFormatTest, ErrorUnsupportedCM) {
  // Valid magic, but wrong CM (not 8)
  uint8_t bad_data[] = {
      GZIP_ID1, GZIP_ID2, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF};

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_buffer_t in_buf = {bad_data, sizeof(bad_data), 0};
  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);

  gcomp_decoder_destroy(decoder);
}

TEST_F(GzipFormatTest, ErrorReservedFLGBitsSet) {
  // Valid magic and CM, but reserved FLG bits set
  uint8_t bad_data[] = {GZIP_ID1, GZIP_ID2, GZIP_CM_DEFLATE, 0xE0, 0x00, 0x00,
      0x00, 0x00, 0x00, 0xFF};

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_buffer_t in_buf = {bad_data, sizeof(bad_data), 0};
  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);

  gcomp_decoder_destroy(decoder);
}

TEST_F(GzipFormatTest, ErrorFEXTRAExceedsLimit) {
  // Create options with very small FEXTRA limit
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_uint64(opts, "gzip.max_extra_bytes", 2);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", opts, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Header with FEXTRA flag set and length > 2
  uint8_t bad_data[] = {
      GZIP_ID1, GZIP_ID2, GZIP_CM_DEFLATE,
      GZIP_FLG_FEXTRA,        // FLG with FEXTRA
      0x00, 0x00, 0x00, 0x00, // MTIME
      0x00, 0xFF,             // XFL, OS
      0x10, 0x00,             // XLEN = 16 (exceeds limit of 2)
  };

  gcomp_buffer_t in_buf = {bad_data, sizeof(bad_data), 0};
  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_decoder_destroy(decoder);
  gcomp_options_destroy(opts);
}

TEST_F(GzipFormatTest, ErrorFNAMEExceedsLimit) {
  // Create options with very small FNAME limit
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_uint64(opts, "gzip.max_name_bytes", 5);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", opts, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Header with FNAME that exceeds limit
  uint8_t bad_data[] = {
      GZIP_ID1, GZIP_ID2, GZIP_CM_DEFLATE,
      GZIP_FLG_FNAME,                         // FLG with FNAME
      0x00, 0x00, 0x00, 0x00,                 // MTIME
      0x00, 0xFF,                             // XFL, OS
      'l', 'o', 'n', 'g', 'n', 'a', 'm', 'e', // Long filename (> 5 bytes)
  };

  gcomp_buffer_t in_buf = {bad_data, sizeof(bad_data), 0};
  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_decoder_destroy(decoder);
  gcomp_options_destroy(opts);
}

TEST_F(GzipFormatTest, ErrorTruncatedHeader) {
  // Just the magic bytes - truncated in MTIME
  uint8_t truncated[] = {GZIP_ID1, GZIP_ID2, GZIP_CM_DEFLATE, 0x00, 0x12};

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_buffer_t in_buf = {truncated, sizeof(truncated), 0};
  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  // Update consumes what it can
  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // Finish should fail - header incomplete
  status = gcomp_decoder_finish(decoder, &out_buf);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);

  gcomp_decoder_destroy(decoder);
}

//
// Trailer Tests
//

TEST_F(GzipFormatTest, TrailerCRCMismatch) {
  // Encode some data first
  const char * test_data = "Test data for CRC check";
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t compressed[512];
  gcomp_buffer_t enc_in = {(void *)test_data, strlen(test_data), 0};
  gcomp_buffer_t enc_out = {compressed, sizeof(compressed), 0};

  status = gcomp_encoder_update(encoder, &enc_in, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  gcomp_encoder_destroy(encoder);

  // Corrupt the CRC32 in trailer (last 8 bytes, first 4 are CRC)
  size_t crc_pos = enc_out.used - 8;
  compressed[crc_pos] ^= 0xFF;

  // Try to decode - should fail with CRC mismatch
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_buffer_t in_buf = {compressed, enc_out.used, 0};
  uint8_t output[512];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);

  gcomp_decoder_destroy(decoder);
}

TEST_F(GzipFormatTest, TrailerISIZEMismatch) {
  // Encode some data first
  const char * test_data = "Test data for ISIZE check";
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t compressed[512];
  gcomp_buffer_t enc_in = {(void *)test_data, strlen(test_data), 0};
  gcomp_buffer_t enc_out = {compressed, sizeof(compressed), 0};

  status = gcomp_encoder_update(encoder, &enc_in, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  gcomp_encoder_destroy(encoder);

  // Corrupt the ISIZE in trailer (last 4 bytes)
  size_t isize_pos = enc_out.used - 4;
  compressed[isize_pos] ^= 0xFF;

  // Try to decode - should fail with ISIZE mismatch
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_buffer_t in_buf = {compressed, enc_out.used, 0};
  uint8_t output[512];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);

  gcomp_decoder_destroy(decoder);
}

TEST_F(GzipFormatTest, TrailerPartialRead) {
  // Encode some data
  const char * test_data = "Short test";
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t compressed[512];
  gcomp_buffer_t enc_in = {(void *)test_data, strlen(test_data), 0};
  gcomp_buffer_t enc_out = {compressed, sizeof(compressed), 0};

  status = gcomp_encoder_update(encoder, &enc_in, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  gcomp_encoder_destroy(encoder);

  // Truncate the trailer - remove last 4 bytes (partial ISIZE)
  size_t truncated_len = enc_out.used - 4;

  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_buffer_t in_buf = {compressed, truncated_len, 0};
  uint8_t output[512];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  // May succeed with partial read
  if (status == GCOMP_OK) {
    // Finish should fail - trailer incomplete
    status = gcomp_decoder_finish(decoder, &out_buf);
    EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
  }

  gcomp_decoder_destroy(decoder);
}

//
// FHCRC Validation Test
//

TEST_F(GzipFormatTest, FHCRCValidation) {
  // Encode with FHCRC
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_bool(opts, "gzip.header_crc", 1);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "gzip", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  const char * test_data = "FHCRC test";
  uint8_t compressed[512];
  gcomp_buffer_t enc_in = {(void *)test_data, strlen(test_data), 0};
  gcomp_buffer_t enc_out = {compressed, sizeof(compressed), 0};

  status = gcomp_encoder_update(encoder, &enc_in, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  gcomp_encoder_destroy(encoder);

  // First verify it decodes correctly
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_buffer_t in_buf = {compressed, enc_out.used, 0};
  uint8_t output[512];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_OK);
  gcomp_decoder_destroy(decoder);

  // Now corrupt the header CRC (bytes 10-11 for minimal header with FHCRC)
  // Header with FHCRC is: 10 bytes fixed + 2 bytes CRC = 12 bytes
  compressed[10] ^= 0xFF;

  // Try to decode - should fail with header CRC mismatch
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_buffer_t in_buf2 = {compressed, enc_out.used, 0};
  gcomp_buffer_t out_buf2 = {output, sizeof(output), 0};

  status = gcomp_decoder_update(decoder, &in_buf2, &out_buf2);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);

  gcomp_decoder_destroy(decoder);
  gcomp_options_destroy(opts);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
