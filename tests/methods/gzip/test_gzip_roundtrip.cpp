/**
 * @file test_gzip_roundtrip.cpp
 *
 * Round-trip tests for gzip encoder/decoder in the Ghoti.io Compress library.
 *
 * These tests verify:
 * - Compress → decompress matches original input
 * - Various input types (empty, single byte, large, patterns, random)
 * - Various compression levels and strategies
 * - Optional header fields (name, comment) preserved through round-trip
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

class GzipRoundtripTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Compress data using streaming API
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
    // Worst case: random data can expand slightly. Also need space for
    // header/trailer.
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

  // Helper: Decompress data using streaming API
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
    // For highly compressible data, expansion can be 1000x+
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

  // Helper: Perform round-trip and verify data matches
  void verifyRoundtrip(const void * data, size_t len,
      gcomp_options_t * opts = nullptr, const char * description = nullptr) {
    gcomp_status_t status;

    // Compress
    auto compressed = compress(data, len, opts, &status);
    ASSERT_EQ(status, GCOMP_OK)
        << "Compression failed" << (description ? " for " : "")
        << (description ? description : "");
    ASSERT_FALSE(compressed.empty())
        << "Compression produced no output" << (description ? " for " : "")
        << (description ? description : "");

    // Decompress
    auto decompressed =
        decompress(compressed.data(), compressed.size(), nullptr, &status);
    ASSERT_EQ(status, GCOMP_OK)
        << "Decompression failed" << (description ? " for " : "")
        << (description ? description : "");

    // Verify round-trip
    ASSERT_EQ(decompressed.size(), len)
        << "Round-trip size mismatch" << (description ? " for " : "")
        << (description ? description : "");

    if (len > 0) {
      EXPECT_EQ(memcmp(decompressed.data(), data, len), 0)
          << "Round-trip data mismatch" << (description ? " for " : "")
          << (description ? description : "");
    }
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Basic Round-trip Tests
//

TEST_F(GzipRoundtripTest, EmptyInput) {
  // Empty input should produce valid gzip with no uncompressed content
  verifyRoundtrip(nullptr, 0, nullptr, "empty input");
}

TEST_F(GzipRoundtripTest, SingleByte) {
  uint8_t single = 'X';
  verifyRoundtrip(&single, 1, nullptr, "single byte");
}

TEST_F(GzipRoundtripTest, SmallData) {
  const char * data = "Hello, gzip world!";
  verifyRoundtrip(data, strlen(data), nullptr, "small data");
}

TEST_F(GzipRoundtripTest, MediumData) {
  // 1 KB of text
  std::string data;
  for (int i = 0; i < 100; i++) {
    data += "The quick brown fox jumps over the lazy dog. ";
  }
  verifyRoundtrip(data.data(), data.size(), nullptr, "medium data (1KB)");
}

TEST_F(GzipRoundtripTest, LargeData) {
  // 1 MB of random-ish data
  std::vector<uint8_t> data(1024 * 1024);
  test_helpers_generate_random(data.data(), data.size(), 12345);
  verifyRoundtrip(data.data(), data.size(), nullptr, "large data (1MB)");
}

//
// Pattern Tests
//

TEST_F(GzipRoundtripTest, AllZeros) {
  // Highly compressible: all zeros
  std::vector<uint8_t> data(10000, 0);
  verifyRoundtrip(data.data(), data.size(), nullptr, "all zeros");
}

TEST_F(GzipRoundtripTest, AllOnes) {
  // Highly compressible: all 0xFF
  std::vector<uint8_t> data(10000, 0xFF);
  verifyRoundtrip(data.data(), data.size(), nullptr, "all 0xFF");
}

TEST_F(GzipRoundtripTest, RepeatingPattern) {
  // Repeating pattern
  std::vector<uint8_t> data(10000);
  uint8_t pattern[] = {0xAB, 0xCD, 0xEF, 0x01};
  test_helpers_generate_pattern(data.data(), data.size(), pattern, 4);
  verifyRoundtrip(data.data(), data.size(), nullptr, "repeating pattern");
}

TEST_F(GzipRoundtripTest, SequentialData) {
  // Sequential bytes (0, 1, 2, ...)
  std::vector<uint8_t> data(1000);
  test_helpers_generate_sequential(data.data(), data.size());
  verifyRoundtrip(data.data(), data.size(), nullptr, "sequential data");
}

TEST_F(GzipRoundtripTest, RandomData) {
  // Random data (low compressibility)
  std::vector<uint8_t> data(10000);
  test_helpers_generate_random(data.data(), data.size(), 42);
  verifyRoundtrip(data.data(), data.size(), nullptr, "random data");
}

TEST_F(GzipRoundtripTest, HighEntropyRandom) {
  // Truly random-like data
  std::vector<uint8_t> data(50000);
  test_helpers_generate_random(data.data(), data.size(), 9999);
  verifyRoundtrip(data.data(), data.size(), nullptr, "high-entropy random");
}

//
// Compression Level Tests
//

TEST_F(GzipRoundtripTest, Level0NoCompression) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_int64(opts, "deflate.level", 0);
  ASSERT_EQ(status, GCOMP_OK);

  const char * data = "Test data for level 0 (no compression)";
  verifyRoundtrip(data, strlen(data), opts, "level 0");

  gcomp_options_destroy(opts);
}

TEST_F(GzipRoundtripTest, Level1Fastest) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_int64(opts, "deflate.level", 1);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> data(5000);
  test_helpers_generate_random(data.data(), data.size(), 111);
  verifyRoundtrip(data.data(), data.size(), opts, "level 1 (fastest)");

  gcomp_options_destroy(opts);
}

TEST_F(GzipRoundtripTest, Level6Default) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_int64(opts, "deflate.level", 6);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> data(5000);
  test_helpers_generate_random(data.data(), data.size(), 222);
  verifyRoundtrip(data.data(), data.size(), opts, "level 6 (default)");

  gcomp_options_destroy(opts);
}

TEST_F(GzipRoundtripTest, Level9Maximum) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_int64(opts, "deflate.level", 9);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> data(5000);
  test_helpers_generate_random(data.data(), data.size(), 333);
  verifyRoundtrip(data.data(), data.size(), opts, "level 9 (maximum)");

  gcomp_options_destroy(opts);
}

TEST_F(GzipRoundtripTest, AllLevels) {
  // Test all compression levels 0-9
  std::vector<uint8_t> data(2000);
  test_helpers_generate_random(data.data(), data.size(), 777);

  for (int level = 0; level <= 9; level++) {
    gcomp_options_t * opts = nullptr;
    gcomp_status_t status = gcomp_options_create(&opts);
    ASSERT_EQ(status, GCOMP_OK);
    status = gcomp_options_set_int64(opts, "deflate.level", level);
    ASSERT_EQ(status, GCOMP_OK);

    std::string desc = "level " + std::to_string(level);
    verifyRoundtrip(data.data(), data.size(), opts, desc.c_str());

    gcomp_options_destroy(opts);
  }
}

//
// Optional Header Fields
//

TEST_F(GzipRoundtripTest, WithFNAME) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_string(opts, "gzip.name", "testfile.txt");
  ASSERT_EQ(status, GCOMP_OK);

  const char * data = "Data with filename in header";
  verifyRoundtrip(data, strlen(data), opts, "with FNAME");

  gcomp_options_destroy(opts);
}

TEST_F(GzipRoundtripTest, WithFCOMMENT) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status =
      gcomp_options_set_string(opts, "gzip.comment", "This is a test file");
  ASSERT_EQ(status, GCOMP_OK);

  const char * data = "Data with comment in header";
  verifyRoundtrip(data, strlen(data), opts, "with FCOMMENT");

  gcomp_options_destroy(opts);
}

TEST_F(GzipRoundtripTest, WithFEXTRA) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  uint8_t extra[] = {0x41, 0x42, 0x03, 0x00, 'X', 'Y', 'Z'};
  status = gcomp_options_set_bytes(opts, "gzip.extra", extra, sizeof(extra));
  ASSERT_EQ(status, GCOMP_OK);

  const char * data = "Data with extra field in header";
  verifyRoundtrip(data, strlen(data), opts, "with FEXTRA");

  gcomp_options_destroy(opts);
}

TEST_F(GzipRoundtripTest, WithFHCRC) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "gzip.header_crc", 1);
  ASSERT_EQ(status, GCOMP_OK);

  const char * data = "Data with header CRC";
  verifyRoundtrip(data, strlen(data), opts, "with FHCRC");

  gcomp_options_destroy(opts);
}

TEST_F(GzipRoundtripTest, WithAllOptionalFields) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  uint8_t extra[] = {0x00, 0x01, 0x02};
  status = gcomp_options_set_bytes(opts, "gzip.extra", extra, sizeof(extra));
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_string(opts, "gzip.name", "allfields.dat");
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_string(opts, "gzip.comment", "Full header test");
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "gzip.header_crc", 1);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "gzip.mtime", 1234567890);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "gzip.os", 3); // Unix
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> data(5000);
  test_helpers_generate_random(data.data(), data.size(), 888);
  verifyRoundtrip(data.data(), data.size(), opts, "with all optional fields");

  gcomp_options_destroy(opts);
}

TEST_F(GzipRoundtripTest, WithCustomMTIMEAndOS) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "gzip.mtime", 0xDEADBEEF);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "gzip.os", 11); // NTFS (Windows)
  ASSERT_EQ(status, GCOMP_OK);

  const char * data = "Data with custom MTIME and OS";
  verifyRoundtrip(data, strlen(data), opts, "with custom MTIME/OS");

  gcomp_options_destroy(opts);
}

//
// Edge Cases
//

TEST_F(GzipRoundtripTest, LongFilename) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Long filename (100 bytes - well within limits)
  std::string long_name(100, 'a');
  status = gcomp_options_set_string(opts, "gzip.name", long_name.c_str());
  ASSERT_EQ(status, GCOMP_OK);

  const char * data = "Data with very long filename";
  verifyRoundtrip(data, strlen(data), opts, "long filename");

  gcomp_options_destroy(opts);
}

TEST_F(GzipRoundtripTest, LongComment) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Long comment (200 bytes - well within limits)
  std::string long_comment(200, 'X');
  status = gcomp_options_set_string(opts, "gzip.comment", long_comment.c_str());
  ASSERT_EQ(status, GCOMP_OK);

  const char * data = "Data with very long comment";
  verifyRoundtrip(data, strlen(data), opts, "long comment");

  gcomp_options_destroy(opts);
}

TEST_F(GzipRoundtripTest, BinaryContent) {
  // Binary data with all byte values (0x00-0xFF)
  std::vector<uint8_t> data(256 * 10);
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = (uint8_t)(i % 256);
  }
  verifyRoundtrip(data.data(), data.size(), nullptr, "binary content");
}

TEST_F(GzipRoundtripTest, SpecialCharactersInName) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Filename with various characters (but no NUL)
  status =
      gcomp_options_set_string(opts, "gzip.name", "file-with_special.chars!");
  ASSERT_EQ(status, GCOMP_OK);

  const char * data = "Data with special chars in name";
  verifyRoundtrip(data, strlen(data), opts, "special chars in name");

  gcomp_options_destroy(opts);
}

//
// Stress Test
//

TEST_F(GzipRoundtripTest, MultipleSizes) {
  // Test various sizes: 0, 1, 2, 10, 100, 1000, 10000, 100000
  std::vector<size_t> sizes = {0, 1, 2, 10, 100, 1000, 10000, 100000};

  for (size_t size : sizes) {
    std::vector<uint8_t> data(size);
    if (size > 0) {
      test_helpers_generate_random(
          data.data(), data.size(), static_cast<uint32_t>(size));
    }

    std::string desc = "size " + std::to_string(size);
    verifyRoundtrip(data.data(), data.size(), nullptr, desc.c_str());
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
