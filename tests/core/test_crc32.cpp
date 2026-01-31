/**
 * @file test_crc32.cpp
 *
 * Unit tests for the CRC32 API in the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstdint>
#include <cstring>
#include <ghoti.io/compress/crc32.h>
#include <gtest/gtest.h>

// Known CRC32 test vectors from RFC 1952 (standard CRC32)
// Standard CRC32: init 0xFFFFFFFF, final XOR 0xFFFFFFFF
// These values are computed using Python: zlib.crc32(data, 0xFFFFFFFF) &
// 0xFFFFFFFF

// Empty string CRC32: 0xFFFFFFFF (unfinalized), 0x00000000 (finalized)
// For empty input, gcomp_crc32() returns 0
static const uint32_t CRC32_EMPTY = 0x00000000U;

// "123456789" CRC32: 0x340BC6D9 (unfinalized), 0xCBF43926 (finalized)
// Standard check value for CRC32 (IEEE 802.3)
static const uint8_t TEST_STRING_123456789[] = {
    '1', '2', '3', '4', '5', '6', '7', '8', '9'};
static const uint32_t CRC32_123456789_UNFINALIZED = 0x340BC6D9U;
static const uint32_t CRC32_123456789_FINALIZED = 0xCBF43926U;

// "The quick brown fox jumps over the lazy dog" CRC32
static const uint8_t TEST_STRING_QUICK_BROWN[] = {'T', 'h', 'e', ' ', 'q', 'u',
    'i', 'c', 'k', ' ', 'b', 'r', 'o', 'w', 'n', ' ', 'f', 'o', 'x', ' ', 'j',
    'u', 'm', 'p', 's', ' ', 'o', 'v', 'e', 'r', ' ', 't', 'h', 'e', ' ', 'l',
    'a', 'z', 'y', ' ', 'd', 'o', 'g'};
static const uint32_t CRC32_QUICK_BROWN_UNFINALIZED = 0xBEB05CC6U;
static const uint32_t CRC32_QUICK_BROWN_FINALIZED = 0x414FA339U;

// Single byte 'A' (0x41) CRC32: 0x2C266174 (unfinalized), 0xD3D99E8B
// (finalized)
static const uint8_t TEST_SINGLE_BYTE = 0x41;
static const uint32_t CRC32_SINGLE_BYTE_UNFINALIZED = 0x2C266174U;
static const uint32_t CRC32_SINGLE_BYTE_FINALIZED = 0xD3D99E8BU;

class Crc32Test : public ::testing::Test {
protected:
  void SetUp() override {
    // No setup needed
  }

  void TearDown() override {
    // No cleanup needed
  }
};

// Test GCOMP_CRC32_INIT constant
TEST_F(Crc32Test, InitConstantIsCorrect) {
  EXPECT_EQ(GCOMP_CRC32_INIT, 0xFFFFFFFFU);
}

// Test gcomp_crc32() with empty input
TEST_F(Crc32Test, Crc32EmptyInput) {
  uint32_t crc = gcomp_crc32(nullptr, 0);
  EXPECT_EQ(crc, CRC32_EMPTY);
}

TEST_F(Crc32Test, Crc32EmptyInputWithValidPointer) {
  const uint8_t * empty = nullptr;
  uint32_t crc = gcomp_crc32(empty, 0);
  EXPECT_EQ(crc, CRC32_EMPTY);
}

// Test gcomp_crc32() with known test vector "123456789"
TEST_F(Crc32Test, Crc32KnownVector123456789) {
  uint32_t crc =
      gcomp_crc32(TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
  // gcomp_crc32() returns unfinalized standard CRC32 (init 0xFFFFFFFF)
  EXPECT_EQ(crc, CRC32_123456789_UNFINALIZED);
}

// Test gcomp_crc32() with "The quick brown fox..."
TEST_F(Crc32Test, Crc32KnownVectorQuickBrown) {
  uint32_t crc =
      gcomp_crc32(TEST_STRING_QUICK_BROWN, sizeof(TEST_STRING_QUICK_BROWN));
  // gcomp_crc32() returns unfinalized standard CRC32
  EXPECT_EQ(crc, CRC32_QUICK_BROWN_UNFINALIZED);
}

// Test gcomp_crc32() with single byte
TEST_F(Crc32Test, Crc32SingleByte) {
  uint32_t crc = gcomp_crc32(&TEST_SINGLE_BYTE, 1);
  // gcomp_crc32() returns unfinalized standard CRC32
  EXPECT_EQ(crc, CRC32_SINGLE_BYTE_UNFINALIZED);
}

// Test incremental computation with GCOMP_CRC32_INIT/update/finalize
TEST_F(Crc32Test, IncrementalComputationSingleChunk) {
  uint32_t crc = GCOMP_CRC32_INIT;
  crc = gcomp_crc32_update(
      crc, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
  // Should match unfinalized standard CRC32
  EXPECT_EQ(crc, CRC32_123456789_UNFINALIZED);
  // After finalize, should match finalized value
  crc = gcomp_crc32_finalize(crc);
  EXPECT_EQ(crc, CRC32_123456789_FINALIZED);
}

// Test incremental computation with multiple chunks
TEST_F(Crc32Test, IncrementalComputationMultipleChunks) {
  uint32_t crc = GCOMP_CRC32_INIT;

  // Process in chunks
  crc = gcomp_crc32_update(crc, TEST_STRING_123456789, 3);     // "123"
  crc = gcomp_crc32_update(crc, TEST_STRING_123456789 + 3, 3); // "456"
  crc = gcomp_crc32_update(crc, TEST_STRING_123456789 + 6, 3); // "789"

  // Should match unfinalized standard CRC32
  EXPECT_EQ(crc, CRC32_123456789_UNFINALIZED);
  // After finalize, should match finalized value
  crc = gcomp_crc32_finalize(crc);
  EXPECT_EQ(crc, CRC32_123456789_FINALIZED);
}

// Test incremental computation matches one-shot computation
TEST_F(Crc32Test, IncrementalMatchesOneShot) {
  // One-shot (doesn't finalize) - returns unfinalized standard CRC32
  uint32_t crc_one_shot =
      gcomp_crc32(TEST_STRING_123456789, sizeof(TEST_STRING_123456789));

  // Incremental (doesn't finalize) - should match one-shot
  uint32_t crc_inc = GCOMP_CRC32_INIT; // Returns 0xFFFFFFFF
  crc_inc = gcomp_crc32_update(
      crc_inc, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));

  EXPECT_EQ(crc_one_shot, crc_inc);
  EXPECT_EQ(crc_one_shot, CRC32_123456789_UNFINALIZED);
}

// Test incremental computation with many small chunks
TEST_F(Crc32Test, IncrementalComputationManySmallChunks) {
  uint32_t crc = GCOMP_CRC32_INIT;

  // Process byte by byte
  for (size_t i = 0; i < sizeof(TEST_STRING_123456789); i++) {
    crc = gcomp_crc32_update(crc, TEST_STRING_123456789 + i, 1);
  }

  // Should match the one-shot result (unfinalized standard CRC32)
  uint32_t crc_one_shot =
      gcomp_crc32(TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
  EXPECT_EQ(crc, crc_one_shot);
  EXPECT_EQ(crc, CRC32_123456789_UNFINALIZED);

  // After finalize, should match finalized value
  crc = gcomp_crc32_finalize(crc);
  EXPECT_EQ(crc, CRC32_123456789_FINALIZED);
}

// Test gcomp_crc32_update() with NULL data pointer
TEST_F(Crc32Test, UpdateWithNullPointer) {
  uint32_t crc = GCOMP_CRC32_INIT;
  uint32_t crc_before = crc;
  crc = gcomp_crc32_update(crc, nullptr, 10);
  EXPECT_EQ(crc, crc_before); // Should return unchanged CRC
}

// Test gcomp_crc32_update() with zero length
TEST_F(Crc32Test, UpdateWithZeroLength) {
  uint32_t crc = GCOMP_CRC32_INIT;
  uint32_t crc_before = crc;
  crc = gcomp_crc32_update(crc, TEST_STRING_123456789, 0);
  EXPECT_EQ(crc, crc_before); // Should return unchanged CRC
}

// Test gcomp_crc32_finalize()
TEST_F(Crc32Test, FinalizeXorCorrect) {
  uint32_t crc = 0x12345678U;
  uint32_t finalized = gcomp_crc32_finalize(crc);
  EXPECT_EQ(finalized, crc ^ 0xFFFFFFFFU);
}

// Test gcomp_crc32_finalize() with initialized value
TEST_F(Crc32Test, FinalizeInitializedValue) {
  uint32_t crc = GCOMP_CRC32_INIT;
  uint32_t finalized = gcomp_crc32_finalize(crc);
  EXPECT_EQ(finalized, 0x00000000U); // 0xFFFFFFFF ^ 0xFFFFFFFF = 0
}

// Test large input (multi-kilobyte)
TEST_F(Crc32Test, LargeInput) {
  const size_t large_size = 64 * 1024; // 64 KB
  uint8_t * large_data = new uint8_t[large_size];

  // Fill with pattern
  for (size_t i = 0; i < large_size; i++) {
    large_data[i] = (uint8_t)(i & 0xFF);
  }

  // Compute CRC32
  uint32_t crc = gcomp_crc32(large_data, large_size);

  // Verify it's a valid CRC32 (non-zero for non-empty input)
  EXPECT_NE(crc, 0U);

  delete[] large_data;
}

// Test large input with incremental computation
TEST_F(Crc32Test, LargeInputIncremental) {
  const size_t large_size = 64 * 1024; // 64 KB
  uint8_t * large_data = new uint8_t[large_size];

  // Fill with pattern
  for (size_t i = 0; i < large_size; i++) {
    large_data[i] = (uint8_t)(i & 0xFF);
  }

  // One-shot (doesn't finalize)
  uint32_t crc_one_shot = gcomp_crc32(large_data, large_size);

  // Incremental in 1KB chunks
  uint32_t crc_inc = GCOMP_CRC32_INIT;
  const size_t chunk_size = 1024;
  for (size_t offset = 0; offset < large_size; offset += chunk_size) {
    size_t remaining = large_size - offset;
    size_t chunk_len = (remaining < chunk_size) ? remaining : chunk_size;
    crc_inc = gcomp_crc32_update(crc_inc, large_data + offset, chunk_len);
  }

  // Should match one-shot result (both unfinalized standard CRC32)
  EXPECT_EQ(crc_one_shot, crc_inc);

  delete[] large_data;
}

// Test that CRC32 is deterministic (same input = same output)
TEST_F(Crc32Test, Deterministic) {
  uint32_t crc1 =
      gcomp_crc32(TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
  uint32_t crc2 =
      gcomp_crc32(TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
  EXPECT_EQ(crc1, crc2);
}

// Test CRC32 with all zeros
TEST_F(Crc32Test, AllZeros) {
  const uint8_t zeros[100] = {0};
  uint32_t crc = gcomp_crc32(zeros, sizeof(zeros));
  // CRC32 of all zeros (after finalize) should be 0xFFFFFFFF ^ 0xFFFFFFFF = 0
  // But with actual zeros, it depends on the polynomial
  // For 100 zeros, the CRC32 should be consistent
  EXPECT_NE(crc, 0U); // Non-zero for non-empty input
}

// Test CRC32 with all 0xFF
TEST_F(Crc32Test, AllOnes) {
  uint8_t ones[100];
  memset(ones, 0xFF, sizeof(ones));
  uint32_t crc = gcomp_crc32(ones, sizeof(ones));
  EXPECT_NE(crc, 0U); // Non-zero for non-empty input
}

// Test CRC32 with alternating pattern
TEST_F(Crc32Test, AlternatingPattern) {
  uint8_t pattern[100];
  for (size_t i = 0; i < sizeof(pattern); i++) {
    pattern[i] = (uint8_t)((i % 2) ? 0xAA : 0x55);
  }
  uint32_t crc = gcomp_crc32(pattern, sizeof(pattern));
  EXPECT_NE(crc, 0U);
}

// Test multiple finalize calls (should be idempotent-like)
TEST_F(Crc32Test, MultipleFinalize) {
  uint32_t crc = GCOMP_CRC32_INIT;
  crc = gcomp_crc32_update(
      crc, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));

  uint32_t finalized1 = gcomp_crc32_finalize(crc);
  uint32_t finalized2 = gcomp_crc32_finalize(crc);

  // Both should produce the same result
  EXPECT_EQ(finalized1, finalized2);
}

// Test round-trip: init -> update -> finalize -> init -> update -> finalize
TEST_F(Crc32Test, RoundTrip) {
  uint32_t crc1 = GCOMP_CRC32_INIT;
  crc1 = gcomp_crc32_update(
      crc1, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
  crc1 = gcomp_crc32_finalize(crc1);

  uint32_t crc2 = GCOMP_CRC32_INIT;
  crc2 = gcomp_crc32_update(
      crc2, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
  crc2 = gcomp_crc32_finalize(crc2);

  EXPECT_EQ(crc1, crc2);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
