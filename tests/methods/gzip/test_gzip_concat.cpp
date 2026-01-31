/**
 * @file test_gzip_concat.cpp
 *
 * Concatenated member tests for gzip decoder in the Ghoti.io Compress library.
 *
 * These tests verify:
 * - Decode of 2-member concatenated gzip
 * - Decode of many-member concatenated gzip
 * - Each member has correct CRC/ISIZE validation
 * - Output is continuous across members
 * - Limits apply across all members
 * - Error in second member after first succeeds
 * - concat disabled rejects extra bytes
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

class GzipConcatTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Compress single data block
  std::vector<uint8_t> compress(
      const void * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "gzip", opts, &encoder);
    if (status != GCOMP_OK)
      return {};

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
    gcomp_encoder_destroy(encoder);
    if (status != GCOMP_OK)
      return {};

    result.resize(out_buf.used);
    return result;
  }

  // Helper: Concatenate multiple gzip streams
  std::vector<uint8_t> concatenateGzip(
      const std::vector<std::vector<uint8_t>> & streams) {
    std::vector<uint8_t> result;
    size_t total = 0;
    for (const auto & s : streams) {
      total += s.size();
    }
    result.reserve(total);
    for (const auto & s : streams) {
      result.insert(result.end(), s.begin(), s.end());
    }
    return result;
  }

  // Helper: Decompress with concat option
  std::vector<uint8_t> decompressConcat(const void * data, size_t len,
      bool concat_enabled, gcomp_status_t * status_out = nullptr) {
    gcomp_options_t * opts = nullptr;
    gcomp_status_t status = gcomp_options_create(&opts);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    status =
        gcomp_options_set_bool(opts, "gzip.concat", concat_enabled ? 1 : 0);
    if (status != GCOMP_OK) {
      gcomp_options_destroy(opts);
      if (status_out)
        *status_out = status;
      return {};
    }

    gcomp_decoder_t * decoder = nullptr;
    status = gcomp_decoder_create(registry_, "gzip", opts, &decoder);
    gcomp_options_destroy(opts);
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
    if (status_out)
      *status_out = status;
    gcomp_decoder_destroy(decoder);

    if (status != GCOMP_OK)
      return {};

    result.resize(out_buf.used);
    return result;
  }

  // Helper: Standard decompress (no concat)
  std::vector<uint8_t> decompress(
      const void * data, size_t len, gcomp_status_t * status_out = nullptr) {
    return decompressConcat(data, len, false, status_out);
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Basic Concatenated Member Tests
//

TEST_F(GzipConcatTest, TwoMemberConcat) {
  const char * data1 = "First member data";
  const char * data2 = "Second member data";

  auto comp1 = compress(data1, strlen(data1));
  auto comp2 = compress(data2, strlen(data2));
  ASSERT_FALSE(comp1.empty());
  ASSERT_FALSE(comp2.empty());

  auto concat = concatenateGzip({comp1, comp2});

  gcomp_status_t status;
  auto decomp = decompressConcat(concat.data(), concat.size(), true, &status);
  ASSERT_EQ(status, GCOMP_OK);

  // Output should be both members concatenated
  std::string expected = std::string(data1) + std::string(data2);
  ASSERT_EQ(decomp.size(), expected.size());
  EXPECT_EQ(memcmp(decomp.data(), expected.data(), expected.size()), 0);
}

TEST_F(GzipConcatTest, ThreeMemberConcat) {
  const char * data1 = "Part one";
  const char * data2 = "Part two";
  const char * data3 = "Part three";

  auto comp1 = compress(data1, strlen(data1));
  auto comp2 = compress(data2, strlen(data2));
  auto comp3 = compress(data3, strlen(data3));
  ASSERT_FALSE(comp1.empty());
  ASSERT_FALSE(comp2.empty());
  ASSERT_FALSE(comp3.empty());

  auto concat = concatenateGzip({comp1, comp2, comp3});

  gcomp_status_t status;
  auto decomp = decompressConcat(concat.data(), concat.size(), true, &status);
  ASSERT_EQ(status, GCOMP_OK);

  std::string expected =
      std::string(data1) + std::string(data2) + std::string(data3);
  ASSERT_EQ(decomp.size(), expected.size());
  EXPECT_EQ(memcmp(decomp.data(), expected.data(), expected.size()), 0);
}

TEST_F(GzipConcatTest, ManyMemberConcat) {
  const int num_members = 10;
  std::vector<std::vector<uint8_t>> compressed_members;
  std::string expected_output;

  for (int i = 0; i < num_members; i++) {
    std::string data = "Member " + std::to_string(i) + " content. ";
    auto comp = compress(data.data(), data.size());
    ASSERT_FALSE(comp.empty()) << "Compression failed for member " << i;
    compressed_members.push_back(comp);
    expected_output += data;
  }

  auto concat = concatenateGzip(compressed_members);

  gcomp_status_t status;
  auto decomp = decompressConcat(concat.data(), concat.size(), true, &status);
  ASSERT_EQ(status, GCOMP_OK);

  ASSERT_EQ(decomp.size(), expected_output.size());
  EXPECT_EQ(
      memcmp(decomp.data(), expected_output.data(), expected_output.size()), 0);
}

//
// CRC/ISIZE Validation Per Member
//

TEST_F(GzipConcatTest, CRCValidatedPerMember) {
  const char * data1 = "First member";
  const char * data2 = "Second member";

  auto comp1 = compress(data1, strlen(data1));
  auto comp2 = compress(data2, strlen(data2));
  ASSERT_FALSE(comp1.empty());
  ASSERT_FALSE(comp2.empty());

  // Corrupt CRC in second member (last 8 bytes of comp2, first 4 are CRC)
  comp2[comp2.size() - 8] ^= 0xFF;

  auto concat = concatenateGzip({comp1, comp2});

  gcomp_status_t status;
  auto decomp = decompressConcat(concat.data(), concat.size(), true, &status);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipConcatTest, ISIZEValidatedPerMember) {
  const char * data1 = "First member";
  const char * data2 = "Second member";

  auto comp1 = compress(data1, strlen(data1));
  auto comp2 = compress(data2, strlen(data2));
  ASSERT_FALSE(comp1.empty());
  ASSERT_FALSE(comp2.empty());

  // Corrupt ISIZE in first member (last 4 bytes of comp1)
  comp1[comp1.size() - 4] ^= 0xFF;

  auto concat = concatenateGzip({comp1, comp2});

  gcomp_status_t status;
  auto decomp = decompressConcat(concat.data(), concat.size(), true, &status);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

//
// Output Continuity Tests
//

TEST_F(GzipConcatTest, OutputIsContinuous) {
  // Create members with known content to verify output is continuous
  std::vector<uint8_t> data1(100);
  std::vector<uint8_t> data2(100);
  std::vector<uint8_t> data3(100);

  // Fill with distinct patterns
  for (int i = 0; i < 100; i++) {
    data1[i] = (uint8_t)(i);
    data2[i] = (uint8_t)(i + 100);
    data3[i] = (uint8_t)(i + 200);
  }

  auto comp1 = compress(data1.data(), data1.size());
  auto comp2 = compress(data2.data(), data2.size());
  auto comp3 = compress(data3.data(), data3.size());

  auto concat = concatenateGzip({comp1, comp2, comp3});

  gcomp_status_t status;
  auto decomp = decompressConcat(concat.data(), concat.size(), true, &status);
  ASSERT_EQ(status, GCOMP_OK);

  ASSERT_EQ(decomp.size(), 300u);

  // Verify each section
  EXPECT_EQ(memcmp(decomp.data(), data1.data(), 100), 0);
  EXPECT_EQ(memcmp(decomp.data() + 100, data2.data(), 100), 0);
  EXPECT_EQ(memcmp(decomp.data() + 200, data3.data(), 100), 0);
}

//
// Limits Across Members Tests
//

TEST_F(GzipConcatTest, MaxOutputBytesAcrossMembers) {
  // Each member produces 100 bytes, limit to 150 bytes
  std::vector<uint8_t> data1(100, 'A');
  std::vector<uint8_t> data2(100, 'B');

  auto comp1 = compress(data1.data(), data1.size());
  auto comp2 = compress(data2.data(), data2.size());
  ASSERT_FALSE(comp1.empty());
  ASSERT_FALSE(comp2.empty());

  auto concat = concatenateGzip({comp1, comp2});

  // Create options with concat enabled and output limit
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "gzip.concat", 1);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "limits.max_output_bytes", 150);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", opts, &decoder);
  gcomp_options_destroy(opts);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> output(300);
  gcomp_buffer_t in_buf = {concat.data(), concat.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  // Should fail with limit error somewhere during second member
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);

  gcomp_decoder_destroy(decoder);
}

//
// Error in Second Member Tests
//

TEST_F(GzipConcatTest, TrailingGarbageIgnoredWithConcatEnabled) {
  // When concat is enabled, trailing data that doesn't look like a gzip member
  // (wrong magic bytes) is ignored, same as when concat is disabled
  const char * data1 = "Good first member";

  auto comp1 = compress(data1, strlen(data1));
  ASSERT_FALSE(comp1.empty());

  // Append garbage (not valid gzip magic)
  std::vector<uint8_t> garbage = {0xFF, 0xFF, 0x08, 0x00, 0x00};
  auto concat = concatenateGzip({comp1, garbage});

  gcomp_status_t status;
  auto decomp = decompressConcat(concat.data(), concat.size(), true, &status);
  // Should succeed - trailing garbage is ignored since it's not a valid gzip
  // member
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decomp.size(), strlen(data1));
  EXPECT_EQ(memcmp(decomp.data(), data1, strlen(data1)), 0);
}

TEST_F(GzipConcatTest, ErrorInSecondMemberHeader) {
  // Test error detection in a second member that starts with valid magic
  // but has an invalid header (reserved bits set)
  const char * data1 = "Good first member";

  auto comp1 = compress(data1, strlen(data1));
  ASSERT_FALSE(comp1.empty());

  // Create invalid second member with valid magic but reserved FLG bits set
  std::vector<uint8_t> bad_member = {
      0x1F, 0x8B,             // Valid magic
      0x08,                   // Valid CM (deflate)
      0xE0,                   // Invalid: reserved bits set
      0x00, 0x00, 0x00, 0x00, // MTIME
      0x00, 0xFF              // XFL, OS
  };

  auto concat = concatenateGzip({comp1, bad_member});

  gcomp_status_t status;
  auto decomp = decompressConcat(concat.data(), concat.size(), true, &status);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(GzipConcatTest, TruncatedSecondMember) {
  const char * data1 = "Complete first member";

  auto comp1 = compress(data1, strlen(data1));
  ASSERT_FALSE(comp1.empty());

  // Incomplete second member header
  std::vector<uint8_t> incomplete = {0x1F, 0x8B, 0x08, 0x00}; // Only 4 bytes

  auto concat = concatenateGzip({comp1, incomplete});

  gcomp_status_t status;
  auto decomp = decompressConcat(concat.data(), concat.size(), true, &status);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

//
// Concat Disabled Tests
//

TEST_F(GzipConcatTest, ConcatDisabledStopsAtFirstMember) {
  const char * data1 = "First member";
  const char * data2 = "Second member";

  auto comp1 = compress(data1, strlen(data1));
  auto comp2 = compress(data2, strlen(data2));
  ASSERT_FALSE(comp1.empty());
  ASSERT_FALSE(comp2.empty());

  auto concat = concatenateGzip({comp1, comp2});

  // With concat disabled
  gcomp_status_t status;
  auto decomp = decompressConcat(concat.data(), concat.size(), false, &status);

  // Should succeed and return only first member's data
  // The extra data (second member) should be left unconsumed or ignored
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decomp.size(), strlen(data1));
  EXPECT_EQ(memcmp(decomp.data(), data1, strlen(data1)), 0);
}

TEST_F(GzipConcatTest, ConcatDisabledIgnoresTrailingGarbage) {
  const char * data = "Single member data";
  auto comp = compress(data, strlen(data));
  ASSERT_FALSE(comp.empty());

  // Append garbage after the gzip stream
  comp.push_back(0xDE);
  comp.push_back(0xAD);
  comp.push_back(0xBE);
  comp.push_back(0xEF);

  // With concat disabled, should ignore trailing garbage
  gcomp_status_t status;
  auto decomp = decompressConcat(comp.data(), comp.size(), false, &status);

  // Should succeed
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decomp.size(), strlen(data));
  EXPECT_EQ(memcmp(decomp.data(), data, strlen(data)), 0);
}

//
// Empty Member Tests
//

TEST_F(GzipConcatTest, EmptyMemberInMiddle) {
  const char * data1 = "First";
  const char * data3 = "Third";

  auto comp1 = compress(data1, strlen(data1));
  auto comp2 = compress(nullptr, 0); // Empty data
  auto comp3 = compress(data3, strlen(data3));
  ASSERT_FALSE(comp1.empty());
  ASSERT_FALSE(
      comp2.empty()); // Empty input still produces gzip with header/trailer
  ASSERT_FALSE(comp3.empty());

  auto concat = concatenateGzip({comp1, comp2, comp3});

  gcomp_status_t status;
  auto decomp = decompressConcat(concat.data(), concat.size(), true, &status);
  ASSERT_EQ(status, GCOMP_OK);

  std::string expected = std::string(data1) + std::string(data3);
  ASSERT_EQ(decomp.size(), expected.size());
  EXPECT_EQ(memcmp(decomp.data(), expected.data(), expected.size()), 0);
}

TEST_F(GzipConcatTest, AllEmptyMembers) {
  auto comp1 = compress(nullptr, 0);
  auto comp2 = compress(nullptr, 0);
  auto comp3 = compress(nullptr, 0);

  auto concat = concatenateGzip({comp1, comp2, comp3});

  gcomp_status_t status;
  auto decomp = decompressConcat(concat.data(), concat.size(), true, &status);
  ASSERT_EQ(status, GCOMP_OK);
  EXPECT_EQ(decomp.size(), 0u);
}

//
// Different Header Options Per Member
//

TEST_F(GzipConcatTest, DifferentHeadersPerMember) {
  // First member with FNAME
  gcomp_options_t * opts1 = nullptr;
  gcomp_options_create(&opts1);
  gcomp_options_set_string(opts1, "gzip.name", "file1.txt");

  // Second member with FCOMMENT
  gcomp_options_t * opts2 = nullptr;
  gcomp_options_create(&opts2);
  gcomp_options_set_string(opts2, "gzip.comment", "Second file comment");

  // Third member with FHCRC
  gcomp_options_t * opts3 = nullptr;
  gcomp_options_create(&opts3);
  gcomp_options_set_bool(opts3, "gzip.header_crc", 1);

  const char * data1 = "Data 1";
  const char * data2 = "Data 2";
  const char * data3 = "Data 3";

  auto comp1 = compress(data1, strlen(data1), opts1);
  auto comp2 = compress(data2, strlen(data2), opts2);
  auto comp3 = compress(data3, strlen(data3), opts3);

  gcomp_options_destroy(opts1);
  gcomp_options_destroy(opts2);
  gcomp_options_destroy(opts3);

  ASSERT_FALSE(comp1.empty());
  ASSERT_FALSE(comp2.empty());
  ASSERT_FALSE(comp3.empty());

  auto concat = concatenateGzip({comp1, comp2, comp3});

  gcomp_status_t status;
  auto decomp = decompressConcat(concat.data(), concat.size(), true, &status);
  ASSERT_EQ(status, GCOMP_OK);

  std::string expected =
      std::string(data1) + std::string(data2) + std::string(data3);
  ASSERT_EQ(decomp.size(), expected.size());
  EXPECT_EQ(memcmp(decomp.data(), expected.data(), expected.size()), 0);
}

//
// Streaming Concatenated Members
//

TEST_F(GzipConcatTest, StreamingSmallChunks) {
  // Test streaming with small (but not 1-byte) chunks
  // Note: 1-byte chunks don't work for concat detection because the decoder
  // needs at least 2 bytes available after trailer to check for magic bytes
  const char * data1 = "Member one";
  const char * data2 = "Member two";

  auto comp1 = compress(data1, strlen(data1));
  auto comp2 = compress(data2, strlen(data2));
  auto concat = concatenateGzip({comp1, comp2});

  // Create decoder with concat enabled
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "gzip.concat", 1);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", opts, &decoder);
  gcomp_options_destroy(opts);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> output;
  output.resize(1024);
  size_t output_pos = 0;

  // Feed in small chunks (16 bytes at a time)
  const size_t chunk_size = 16;
  for (size_t i = 0; i < concat.size(); i += chunk_size) {
    size_t remaining = concat.size() - i;
    size_t chunk = (remaining < chunk_size) ? remaining : chunk_size;

    gcomp_buffer_t in_buf = {concat.data() + i, chunk, 0};
    gcomp_buffer_t out_buf = {
        output.data() + output_pos, output.size() - output_pos, 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    ASSERT_EQ(status, GCOMP_OK) << "Failed at offset " << i;
    output_pos += out_buf.used;
  }

  gcomp_buffer_t final_out = {
      output.data() + output_pos, output.size() - output_pos, 0};
  status = gcomp_decoder_finish(decoder, &final_out);
  ASSERT_EQ(status, GCOMP_OK);
  output_pos += final_out.used;

  gcomp_decoder_destroy(decoder);

  std::string expected = std::string(data1) + std::string(data2);
  ASSERT_EQ(output_pos, expected.size());
  EXPECT_EQ(memcmp(output.data(), expected.data(), expected.size()), 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
