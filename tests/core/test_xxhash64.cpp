/**
 * @file test_xxhash64.cpp
 *
 * Unit tests for the xxHash64 API in the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
#include <cstdint>
#include <cstring>
#include <ghoti.io/compress/xxhash64.h>
#include <gtest/gtest.h>

//
// Known xxHash64 test vectors (seed 0)
//
// These values are verified against the reference xxHash implementation.
// Reference: https://github.com/Cyan4973/xxHash
//

// Empty string
static const uint64_t XXHASH64_EMPTY = 0xEF46DB3751D8E999ULL;

// "a" (single character)
static const uint8_t TEST_STRING_A[] = {'a'};
static const uint64_t XXHASH64_A = 0xD24EC4F1A98C6E5BULL;

// "abc"
static const uint8_t TEST_STRING_ABC[] = {'a', 'b', 'c'};
static const uint64_t XXHASH64_ABC = 0x44BC2CF5AD770999ULL;

// "123456789"
static const uint8_t TEST_STRING_123456789[] = {
    '1', '2', '3', '4', '5', '6', '7', '8', '9'};
static const uint64_t XXHASH64_123456789 = 0x8CB841DB40E6AE83ULL;

// "abcdefghijklmnopqrstuvwxyz012345" (32 bytes - exactly one block)
static const uint8_t TEST_STRING_32[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h',
    'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w',
    'x', 'y', 'z', '0', '1', '2', '3', '4', '5'};
static const uint64_t XXHASH64_32 = 0xBF2CD639B4143B80ULL;

// "The quick brown fox jumps over the lazy dog"
static const uint8_t TEST_STRING_QUICK_BROWN[] = {'T', 'h', 'e', ' ', 'q', 'u',
    'i', 'c', 'k', ' ', 'b', 'r', 'o', 'w', 'n', ' ', 'f', 'o', 'x', ' ', 'j',
    'u', 'm', 'p', 's', ' ', 'o', 'v', 'e', 'r', ' ', 't', 'h', 'e', ' ', 'l',
    'a', 'z', 'y', ' ', 'd', 'o', 'g'};
static const uint64_t XXHASH64_QUICK_BROWN = 0x0B242D361FDA71BCULL;

class Xxhash64Test : public ::testing::Test {
protected:
  void SetUp() override {
    // No setup needed
  }

  void TearDown() override {
    // No cleanup needed
  }
};

// Test GCOMP_XXHASH64_SEED_DEFAULT constant
TEST_F(Xxhash64Test, SeedDefaultIsZero) {
  EXPECT_EQ(GCOMP_XXHASH64_SEED_DEFAULT, 0U);
}

// Test gcomp_xxhash64() with empty input
TEST_F(Xxhash64Test, OneShotEmptyInput) {
  uint64_t hash = gcomp_xxhash64(nullptr, 0, 0);
  EXPECT_EQ(hash, XXHASH64_EMPTY);
}

TEST_F(Xxhash64Test, OneShotEmptyInputWithValidPointer) {
  uint8_t dummy[1] = {0};
  uint64_t hash = gcomp_xxhash64(dummy, 0, 0);
  EXPECT_EQ(hash, XXHASH64_EMPTY);
}

// Test gcomp_xxhash64() with single character
TEST_F(Xxhash64Test, OneShotSingleChar) {
  uint64_t hash = gcomp_xxhash64(TEST_STRING_A, sizeof(TEST_STRING_A), 0);
  EXPECT_EQ(hash, XXHASH64_A);
}

// Test gcomp_xxhash64() with "abc"
TEST_F(Xxhash64Test, OneShotAbc) {
  uint64_t hash = gcomp_xxhash64(TEST_STRING_ABC, sizeof(TEST_STRING_ABC), 0);
  EXPECT_EQ(hash, XXHASH64_ABC);
}

// Test gcomp_xxhash64() with known test vector "123456789"
TEST_F(Xxhash64Test, OneShotKnownVector123456789) {
  uint64_t hash =
      gcomp_xxhash64(TEST_STRING_123456789, sizeof(TEST_STRING_123456789), 0);
  EXPECT_EQ(hash, XXHASH64_123456789);
}

// Test gcomp_xxhash64() with exactly 32 bytes (one block)
TEST_F(Xxhash64Test, OneShotExactlyOneBlock) {
  uint64_t hash = gcomp_xxhash64(TEST_STRING_32, sizeof(TEST_STRING_32), 0);
  EXPECT_EQ(hash, XXHASH64_32);
}

// Test gcomp_xxhash64() with "The quick brown fox..."
TEST_F(Xxhash64Test, OneShotKnownVectorQuickBrown) {
  uint64_t hash = gcomp_xxhash64(
      TEST_STRING_QUICK_BROWN, sizeof(TEST_STRING_QUICK_BROWN), 0);
  EXPECT_EQ(hash, XXHASH64_QUICK_BROWN);
}

// Test streaming computation with single chunk
TEST_F(Xxhash64Test, StreamingSingleChunk) {
  gcomp_xxhash64_state_t state;
  gcomp_xxhash64_reset(&state, 0);
  gcomp_xxhash64_update(
      &state, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
  uint64_t hash = gcomp_xxhash64_finalize(&state);
  EXPECT_EQ(hash, XXHASH64_123456789);
}

// Test streaming matches one-shot for empty input
TEST_F(Xxhash64Test, StreamingEmptyMatchesOneShot) {
  gcomp_xxhash64_state_t state;
  gcomp_xxhash64_reset(&state, 0);
  // No update calls
  uint64_t hash = gcomp_xxhash64_finalize(&state);
  EXPECT_EQ(hash, XXHASH64_EMPTY);
}

// Test streaming matches one-shot for single char
TEST_F(Xxhash64Test, StreamingSingleCharMatchesOneShot) {
  gcomp_xxhash64_state_t state;
  gcomp_xxhash64_reset(&state, 0);
  gcomp_xxhash64_update(&state, TEST_STRING_A, sizeof(TEST_STRING_A));
  uint64_t hash = gcomp_xxhash64_finalize(&state);
  EXPECT_EQ(hash, XXHASH64_A);
}

// Test streaming computation with multiple chunks
TEST_F(Xxhash64Test, StreamingMultipleChunks) {
  gcomp_xxhash64_state_t state;
  gcomp_xxhash64_reset(&state, 0);

  // Process in chunks: "123" + "456" + "789"
  gcomp_xxhash64_update(&state, TEST_STRING_123456789, 3);
  gcomp_xxhash64_update(&state, TEST_STRING_123456789 + 3, 3);
  gcomp_xxhash64_update(&state, TEST_STRING_123456789 + 6, 3);

  uint64_t hash = gcomp_xxhash64_finalize(&state);
  EXPECT_EQ(hash, XXHASH64_123456789);
}

// Test streaming computation byte-by-byte
TEST_F(Xxhash64Test, StreamingByteByByte) {
  gcomp_xxhash64_state_t state;
  gcomp_xxhash64_reset(&state, 0);

  // Process byte by byte
  for (size_t i = 0; i < sizeof(TEST_STRING_123456789); i++) {
    gcomp_xxhash64_update(&state, TEST_STRING_123456789 + i, 1);
  }

  uint64_t hash = gcomp_xxhash64_finalize(&state);
  EXPECT_EQ(hash, XXHASH64_123456789);
}

// Test streaming matches one-shot for various inputs
TEST_F(Xxhash64Test, StreamingMatchesOneShot) {
  // Test with quick brown fox
  gcomp_xxhash64_state_t state;
  gcomp_xxhash64_reset(&state, 0);
  gcomp_xxhash64_update(
      &state, TEST_STRING_QUICK_BROWN, sizeof(TEST_STRING_QUICK_BROWN));
  uint64_t hash_streaming = gcomp_xxhash64_finalize(&state);

  uint64_t hash_oneshot = gcomp_xxhash64(
      TEST_STRING_QUICK_BROWN, sizeof(TEST_STRING_QUICK_BROWN), 0);

  EXPECT_EQ(hash_streaming, hash_oneshot);
  EXPECT_EQ(hash_streaming, XXHASH64_QUICK_BROWN);
}

// Test streaming with 32-byte boundary crossing
TEST_F(Xxhash64Test, StreamingBoundaryCrossing) {
  // Create input larger than 32 bytes
  gcomp_xxhash64_state_t state;
  gcomp_xxhash64_reset(&state, 0);

  // Feed in chunks that cross 32-byte boundaries
  // Quick brown fox is 43 bytes
  gcomp_xxhash64_update(&state, TEST_STRING_QUICK_BROWN, 10);      // 10 bytes
  gcomp_xxhash64_update(&state, TEST_STRING_QUICK_BROWN + 10, 10); // 20 total
  gcomp_xxhash64_update(&state, TEST_STRING_QUICK_BROWN + 20, 10); // 30 total
  gcomp_xxhash64_update(&state, TEST_STRING_QUICK_BROWN + 30, 13); // 43 total

  uint64_t hash = gcomp_xxhash64_finalize(&state);
  EXPECT_EQ(hash, XXHASH64_QUICK_BROWN);
}

// Test streaming with odd chunk sizes
TEST_F(Xxhash64Test, StreamingOddChunkSizes) {
  gcomp_xxhash64_state_t state;
  gcomp_xxhash64_reset(&state, 0);

  // Feed in chunks of various odd sizes
  // Quick brown fox is 43 bytes
  gcomp_xxhash64_update(&state, TEST_STRING_QUICK_BROWN, 7);
  gcomp_xxhash64_update(&state, TEST_STRING_QUICK_BROWN + 7, 11);
  gcomp_xxhash64_update(&state, TEST_STRING_QUICK_BROWN + 18, 5);
  gcomp_xxhash64_update(&state, TEST_STRING_QUICK_BROWN + 23, 13);
  gcomp_xxhash64_update(&state, TEST_STRING_QUICK_BROWN + 36, 7);

  uint64_t hash = gcomp_xxhash64_finalize(&state);
  EXPECT_EQ(hash, XXHASH64_QUICK_BROWN);
}

// Test gcomp_xxhash64_update() with NULL state
TEST_F(Xxhash64Test, UpdateWithNullState) {
  // Should not crash
  gcomp_xxhash64_update(
      nullptr, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
}

// Test gcomp_xxhash64_update() with NULL data pointer
TEST_F(Xxhash64Test, UpdateWithNullPointer) {
  gcomp_xxhash64_state_t state;
  gcomp_xxhash64_reset(&state, 0);
  gcomp_xxhash64_update(&state, nullptr, 10);
  // Should still produce empty hash
  uint64_t hash = gcomp_xxhash64_finalize(&state);
  EXPECT_EQ(hash, XXHASH64_EMPTY);
}

// Test gcomp_xxhash64_update() with zero length
TEST_F(Xxhash64Test, UpdateWithZeroLength) {
  gcomp_xxhash64_state_t state;
  gcomp_xxhash64_reset(&state, 0);
  gcomp_xxhash64_update(&state, TEST_STRING_123456789, 0);
  // Should still produce empty hash
  uint64_t hash = gcomp_xxhash64_finalize(&state);
  EXPECT_EQ(hash, XXHASH64_EMPTY);
}

// Test gcomp_xxhash64_finalize() with NULL state
TEST_F(Xxhash64Test, FinalizeWithNullState) {
  uint64_t hash = gcomp_xxhash64_finalize(nullptr);
  EXPECT_EQ(hash, 0U);
}

// Test gcomp_xxhash64_reset() with NULL state
TEST_F(Xxhash64Test, ResetWithNullState) {
  // Should not crash
  gcomp_xxhash64_reset(nullptr, 0);
}

// Test large input (multi-kilobyte)
TEST_F(Xxhash64Test, LargeInput) {
  const size_t large_size = 64 * 1024; // 64 KB
  uint8_t * large_data = new uint8_t[large_size];

  // Fill with pattern
  for (size_t i = 0; i < large_size; i++) {
    large_data[i] = (uint8_t)(i & 0xFF);
  }

  // Compute hash
  uint64_t hash = gcomp_xxhash64(large_data, large_size, 0);

  // Verify it's non-zero and consistent
  EXPECT_NE(hash, 0U);

  // Verify streaming matches one-shot
  gcomp_xxhash64_state_t state;
  gcomp_xxhash64_reset(&state, 0);
  gcomp_xxhash64_update(&state, large_data, large_size);
  uint64_t hash_streaming = gcomp_xxhash64_finalize(&state);
  EXPECT_EQ(hash, hash_streaming);

  delete[] large_data;
}

// Test large input with incremental computation in 1KB chunks
TEST_F(Xxhash64Test, LargeInputIncremental) {
  const size_t large_size = 64 * 1024; // 64 KB
  uint8_t * large_data = new uint8_t[large_size];

  // Fill with pattern
  for (size_t i = 0; i < large_size; i++) {
    large_data[i] = (uint8_t)(i & 0xFF);
  }

  // One-shot
  uint64_t hash_oneshot = gcomp_xxhash64(large_data, large_size, 0);

  // Incremental in 1KB chunks
  gcomp_xxhash64_state_t state;
  gcomp_xxhash64_reset(&state, 0);
  const size_t chunk_size = 1024;
  for (size_t offset = 0; offset < large_size; offset += chunk_size) {
    size_t remaining = large_size - offset;
    size_t chunk_len = (remaining < chunk_size) ? remaining : chunk_size;
    gcomp_xxhash64_update(&state, large_data + offset, chunk_len);
  }
  uint64_t hash_incremental = gcomp_xxhash64_finalize(&state);

  EXPECT_EQ(hash_oneshot, hash_incremental);

  delete[] large_data;
}

// Test that xxHash64 is deterministic (same input = same output)
TEST_F(Xxhash64Test, Deterministic) {
  uint64_t hash1 =
      gcomp_xxhash64(TEST_STRING_123456789, sizeof(TEST_STRING_123456789), 0);
  uint64_t hash2 =
      gcomp_xxhash64(TEST_STRING_123456789, sizeof(TEST_STRING_123456789), 0);
  EXPECT_EQ(hash1, hash2);
}

// Test xxHash64 with different seeds
TEST_F(Xxhash64Test, DifferentSeeds) {
  uint64_t hash_seed0 =
      gcomp_xxhash64(TEST_STRING_123456789, sizeof(TEST_STRING_123456789), 0);
  uint64_t hash_seed1 =
      gcomp_xxhash64(TEST_STRING_123456789, sizeof(TEST_STRING_123456789), 1);
  uint64_t hash_seed42 =
      gcomp_xxhash64(TEST_STRING_123456789, sizeof(TEST_STRING_123456789), 42);

  // Different seeds should produce different hashes
  EXPECT_NE(hash_seed0, hash_seed1);
  EXPECT_NE(hash_seed0, hash_seed42);
  EXPECT_NE(hash_seed1, hash_seed42);
}

// Test streaming with different seeds
TEST_F(Xxhash64Test, StreamingWithDifferentSeeds) {
  gcomp_xxhash64_state_t state;

  gcomp_xxhash64_reset(&state, 42);
  gcomp_xxhash64_update(
      &state, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
  uint64_t hash_streaming = gcomp_xxhash64_finalize(&state);

  uint64_t hash_oneshot =
      gcomp_xxhash64(TEST_STRING_123456789, sizeof(TEST_STRING_123456789), 42);

  EXPECT_EQ(hash_streaming, hash_oneshot);
}

// Test xxHash64 with all zeros
TEST_F(Xxhash64Test, AllZeros) {
  const uint8_t zeros[100] = {0};
  uint64_t hash = gcomp_xxhash64(zeros, sizeof(zeros), 0);
  // Should be consistent and non-empty
  EXPECT_NE(hash, XXHASH64_EMPTY);
}

// Test xxHash64 with all 0xFF
TEST_F(Xxhash64Test, AllOnes) {
  uint8_t ones[100];
  memset(ones, 0xFF, sizeof(ones));
  uint64_t hash = gcomp_xxhash64(ones, sizeof(ones), 0);
  // Should be consistent and non-empty
  EXPECT_NE(hash, XXHASH64_EMPTY);
}

// Test multiple finalize calls (should produce same result)
TEST_F(Xxhash64Test, MultipleFinalize) {
  gcomp_xxhash64_state_t state;
  gcomp_xxhash64_reset(&state, 0);
  gcomp_xxhash64_update(
      &state, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));

  uint64_t hash1 = gcomp_xxhash64_finalize(&state);
  uint64_t hash2 = gcomp_xxhash64_finalize(&state);

  EXPECT_EQ(hash1, hash2);
  EXPECT_EQ(hash1, XXHASH64_123456789);
}

// Test reset clears state
TEST_F(Xxhash64Test, ResetClearsState) {
  gcomp_xxhash64_state_t state;

  // First computation
  gcomp_xxhash64_reset(&state, 0);
  gcomp_xxhash64_update(
      &state, TEST_STRING_QUICK_BROWN, sizeof(TEST_STRING_QUICK_BROWN));
  uint64_t hash1 = gcomp_xxhash64_finalize(&state);
  EXPECT_EQ(hash1, XXHASH64_QUICK_BROWN);

  // Reset and compute different hash
  gcomp_xxhash64_reset(&state, 0);
  gcomp_xxhash64_update(
      &state, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
  uint64_t hash2 = gcomp_xxhash64_finalize(&state);
  EXPECT_EQ(hash2, XXHASH64_123456789);
}

// Test round-trip: reset -> update -> finalize -> reset -> update -> finalize
TEST_F(Xxhash64Test, RoundTrip) {
  gcomp_xxhash64_state_t state;

  gcomp_xxhash64_reset(&state, 0);
  gcomp_xxhash64_update(
      &state, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
  uint64_t hash1 = gcomp_xxhash64_finalize(&state);

  gcomp_xxhash64_reset(&state, 0);
  gcomp_xxhash64_update(
      &state, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
  uint64_t hash2 = gcomp_xxhash64_finalize(&state);

  EXPECT_EQ(hash1, hash2);
  EXPECT_EQ(hash1, XXHASH64_123456789);
}

// Test Zstd content checksum calculation pattern
// Zstd uses the low 32 bits of xxHash64 for the content checksum
TEST_F(Xxhash64Test, ZstdContentChecksumPattern) {
  // For Zstd, the content checksum is the low 32 bits of the xxHash64
  uint64_t full_hash =
      gcomp_xxhash64(TEST_STRING_123456789, sizeof(TEST_STRING_123456789), 0);
  uint32_t content_checksum = (uint32_t)(full_hash & 0xFFFFFFFF);

  // Just verify the pattern works
  EXPECT_EQ(content_checksum, (uint32_t)(XXHASH64_123456789 & 0xFFFFFFFF));
}

// Test streaming 32-byte input
TEST_F(Xxhash64Test, Streaming32ByteInput) {
  gcomp_xxhash64_state_t state;
  gcomp_xxhash64_reset(&state, 0);
  gcomp_xxhash64_update(&state, TEST_STRING_32, sizeof(TEST_STRING_32));
  uint64_t hash = gcomp_xxhash64_finalize(&state);
  EXPECT_EQ(hash, XXHASH64_32);
}

// Test streaming 32-byte input in two chunks
TEST_F(Xxhash64Test, Streaming32ByteInTwoChunks) {
  gcomp_xxhash64_state_t state;
  gcomp_xxhash64_reset(&state, 0);
  gcomp_xxhash64_update(&state, TEST_STRING_32, 16);
  gcomp_xxhash64_update(&state, TEST_STRING_32 + 16, 16);
  uint64_t hash = gcomp_xxhash64_finalize(&state);
  EXPECT_EQ(hash, XXHASH64_32);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
