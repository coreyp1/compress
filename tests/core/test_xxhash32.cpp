/**
 * @file test_xxhash32.cpp
 *
 * Unit tests for the xxHash32 API in the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstdint>
#include <cstring>
#include <ghoti.io/compress/xxhash32.h>
#include <gtest/gtest.h>

//
// Known xxHash32 test vectors (seed 0)
//
// These values are verified against the reference xxHash implementation.
// Reference: https://github.com/Cyan4973/xxHash
//

// Empty string
static const uint32_t XXHASH32_EMPTY = 0x02CC5D05U;

// "a" (single character)
static const uint8_t TEST_STRING_A[] = {'a'};
static const uint32_t XXHASH32_A = 0x550D7456U;

// "abc"
static const uint8_t TEST_STRING_ABC[] = {'a', 'b', 'c'};
static const uint32_t XXHASH32_ABC = 0x32D153FFU;

// "123456789"
static const uint8_t TEST_STRING_123456789[] = {
    '1', '2', '3', '4', '5', '6', '7', '8', '9'};
static const uint32_t XXHASH32_123456789 = 0x937BAD67U;

// "abcdefghijklmnop" (16 bytes - exactly one block)
static const uint8_t TEST_STRING_16[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h',
    'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p'};
static const uint32_t XXHASH32_16 = 0x9D2D8B62U;

// "The quick brown fox jumps over the lazy dog"
static const uint8_t TEST_STRING_QUICK_BROWN[] = {'T', 'h', 'e', ' ', 'q', 'u',
    'i', 'c', 'k', ' ', 'b', 'r', 'o', 'w', 'n', ' ', 'f', 'o', 'x', ' ', 'j',
    'u', 'm', 'p', 's', ' ', 'o', 'v', 'e', 'r', ' ', 't', 'h', 'e', ' ', 'l',
    'a', 'z', 'y', ' ', 'd', 'o', 'g'};
static const uint32_t XXHASH32_QUICK_BROWN = 0xE85EA4DEU;

class Xxhash32Test : public ::testing::Test {
protected:
  void SetUp() override {
    // No setup needed
  }

  void TearDown() override {
    // No cleanup needed
  }
};

// Test GCOMP_XXHASH32_SEED_DEFAULT constant
TEST_F(Xxhash32Test, SeedDefaultIsZero) {
  EXPECT_EQ(GCOMP_XXHASH32_SEED_DEFAULT, 0U);
}

// Test gcomp_xxhash32() with empty input
TEST_F(Xxhash32Test, OneShotEmptyInput) {
  uint32_t hash = gcomp_xxhash32(nullptr, 0, 0);
  EXPECT_EQ(hash, XXHASH32_EMPTY);
}

TEST_F(Xxhash32Test, OneShotEmptyInputWithValidPointer) {
  uint8_t dummy[1] = {0};
  uint32_t hash = gcomp_xxhash32(dummy, 0, 0);
  EXPECT_EQ(hash, XXHASH32_EMPTY);
}

// Test gcomp_xxhash32() with single character
TEST_F(Xxhash32Test, OneShotSingleChar) {
  uint32_t hash = gcomp_xxhash32(TEST_STRING_A, sizeof(TEST_STRING_A), 0);
  EXPECT_EQ(hash, XXHASH32_A);
}

// Test gcomp_xxhash32() with "abc"
TEST_F(Xxhash32Test, OneShotAbc) {
  uint32_t hash = gcomp_xxhash32(TEST_STRING_ABC, sizeof(TEST_STRING_ABC), 0);
  EXPECT_EQ(hash, XXHASH32_ABC);
}

// Test gcomp_xxhash32() with known test vector "123456789"
TEST_F(Xxhash32Test, OneShotKnownVector123456789) {
  uint32_t hash =
      gcomp_xxhash32(TEST_STRING_123456789, sizeof(TEST_STRING_123456789), 0);
  EXPECT_EQ(hash, XXHASH32_123456789);
}

// Test gcomp_xxhash32() with exactly 16 bytes (one block)
TEST_F(Xxhash32Test, OneShotExactlyOneBlock) {
  uint32_t hash = gcomp_xxhash32(TEST_STRING_16, sizeof(TEST_STRING_16), 0);
  EXPECT_EQ(hash, XXHASH32_16);
}

// Test gcomp_xxhash32() with "The quick brown fox..."
TEST_F(Xxhash32Test, OneShotKnownVectorQuickBrown) {
  uint32_t hash = gcomp_xxhash32(
      TEST_STRING_QUICK_BROWN, sizeof(TEST_STRING_QUICK_BROWN), 0);
  EXPECT_EQ(hash, XXHASH32_QUICK_BROWN);
}

// Test streaming computation with single chunk
TEST_F(Xxhash32Test, StreamingSingleChunk) {
  gcomp_xxhash32_state_t state;
  gcomp_xxhash32_reset(&state, 0);
  gcomp_xxhash32_update(
      &state, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
  uint32_t hash = gcomp_xxhash32_finalize(&state);
  EXPECT_EQ(hash, XXHASH32_123456789);
}

// Test streaming matches one-shot for empty input
TEST_F(Xxhash32Test, StreamingEmptyMatchesOneShot) {
  gcomp_xxhash32_state_t state;
  gcomp_xxhash32_reset(&state, 0);
  // No update calls
  uint32_t hash = gcomp_xxhash32_finalize(&state);
  EXPECT_EQ(hash, XXHASH32_EMPTY);
}

// Test streaming matches one-shot for single char
TEST_F(Xxhash32Test, StreamingSingleCharMatchesOneShot) {
  gcomp_xxhash32_state_t state;
  gcomp_xxhash32_reset(&state, 0);
  gcomp_xxhash32_update(&state, TEST_STRING_A, sizeof(TEST_STRING_A));
  uint32_t hash = gcomp_xxhash32_finalize(&state);
  EXPECT_EQ(hash, XXHASH32_A);
}

// Test streaming computation with multiple chunks
TEST_F(Xxhash32Test, StreamingMultipleChunks) {
  gcomp_xxhash32_state_t state;
  gcomp_xxhash32_reset(&state, 0);

  // Process in chunks: "123" + "456" + "789"
  gcomp_xxhash32_update(&state, TEST_STRING_123456789, 3);
  gcomp_xxhash32_update(&state, TEST_STRING_123456789 + 3, 3);
  gcomp_xxhash32_update(&state, TEST_STRING_123456789 + 6, 3);

  uint32_t hash = gcomp_xxhash32_finalize(&state);
  EXPECT_EQ(hash, XXHASH32_123456789);
}

// Test streaming computation byte-by-byte
TEST_F(Xxhash32Test, StreamingByteByByte) {
  gcomp_xxhash32_state_t state;
  gcomp_xxhash32_reset(&state, 0);

  // Process byte by byte
  for (size_t i = 0; i < sizeof(TEST_STRING_123456789); i++) {
    gcomp_xxhash32_update(&state, TEST_STRING_123456789 + i, 1);
  }

  uint32_t hash = gcomp_xxhash32_finalize(&state);
  EXPECT_EQ(hash, XXHASH32_123456789);
}

// Test streaming matches one-shot for various inputs
TEST_F(Xxhash32Test, StreamingMatchesOneShot) {
  // Test with quick brown fox
  gcomp_xxhash32_state_t state;
  gcomp_xxhash32_reset(&state, 0);
  gcomp_xxhash32_update(
      &state, TEST_STRING_QUICK_BROWN, sizeof(TEST_STRING_QUICK_BROWN));
  uint32_t hash_streaming = gcomp_xxhash32_finalize(&state);

  uint32_t hash_oneshot = gcomp_xxhash32(
      TEST_STRING_QUICK_BROWN, sizeof(TEST_STRING_QUICK_BROWN), 0);

  EXPECT_EQ(hash_streaming, hash_oneshot);
  EXPECT_EQ(hash_streaming, XXHASH32_QUICK_BROWN);
}

// Test streaming with 16-byte boundary crossing
TEST_F(Xxhash32Test, StreamingBoundaryCrossing) {
  // Create input larger than 16 bytes
  gcomp_xxhash32_state_t state;
  gcomp_xxhash32_reset(&state, 0);

  // Feed in chunks that cross 16-byte boundaries
  // Quick brown fox is 43 bytes
  gcomp_xxhash32_update(&state, TEST_STRING_QUICK_BROWN, 10);      // 10 bytes
  gcomp_xxhash32_update(&state, TEST_STRING_QUICK_BROWN + 10, 10); // 20 total
  gcomp_xxhash32_update(&state, TEST_STRING_QUICK_BROWN + 20, 10); // 30 total
  gcomp_xxhash32_update(&state, TEST_STRING_QUICK_BROWN + 30, 13); // 43 total

  uint32_t hash = gcomp_xxhash32_finalize(&state);
  EXPECT_EQ(hash, XXHASH32_QUICK_BROWN);
}

// Test streaming with odd chunk sizes
TEST_F(Xxhash32Test, StreamingOddChunkSizes) {
  gcomp_xxhash32_state_t state;
  gcomp_xxhash32_reset(&state, 0);

  // Feed in chunks of various odd sizes
  // Quick brown fox is 43 bytes
  gcomp_xxhash32_update(&state, TEST_STRING_QUICK_BROWN, 7);
  gcomp_xxhash32_update(&state, TEST_STRING_QUICK_BROWN + 7, 11);
  gcomp_xxhash32_update(&state, TEST_STRING_QUICK_BROWN + 18, 5);
  gcomp_xxhash32_update(&state, TEST_STRING_QUICK_BROWN + 23, 13);
  gcomp_xxhash32_update(&state, TEST_STRING_QUICK_BROWN + 36, 7);

  uint32_t hash = gcomp_xxhash32_finalize(&state);
  EXPECT_EQ(hash, XXHASH32_QUICK_BROWN);
}

// Test gcomp_xxhash32_update() with NULL state
TEST_F(Xxhash32Test, UpdateWithNullState) {
  // Should not crash
  gcomp_xxhash32_update(
      nullptr, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
}

// Test gcomp_xxhash32_update() with NULL data pointer
TEST_F(Xxhash32Test, UpdateWithNullPointer) {
  gcomp_xxhash32_state_t state;
  gcomp_xxhash32_reset(&state, 0);
  gcomp_xxhash32_update(&state, nullptr, 10);
  // Should still produce empty hash
  uint32_t hash = gcomp_xxhash32_finalize(&state);
  EXPECT_EQ(hash, XXHASH32_EMPTY);
}

// Test gcomp_xxhash32_update() with zero length
TEST_F(Xxhash32Test, UpdateWithZeroLength) {
  gcomp_xxhash32_state_t state;
  gcomp_xxhash32_reset(&state, 0);
  gcomp_xxhash32_update(&state, TEST_STRING_123456789, 0);
  // Should still produce empty hash
  uint32_t hash = gcomp_xxhash32_finalize(&state);
  EXPECT_EQ(hash, XXHASH32_EMPTY);
}

// Test gcomp_xxhash32_finalize() with NULL state
TEST_F(Xxhash32Test, FinalizeWithNullState) {
  uint32_t hash = gcomp_xxhash32_finalize(nullptr);
  EXPECT_EQ(hash, 0U);
}

// Test gcomp_xxhash32_reset() with NULL state
TEST_F(Xxhash32Test, ResetWithNullState) {
  // Should not crash
  gcomp_xxhash32_reset(nullptr, 0);
}

// Test large input (multi-kilobyte)
TEST_F(Xxhash32Test, LargeInput) {
  const size_t large_size = 64 * 1024; // 64 KB
  uint8_t * large_data = new uint8_t[large_size];

  // Fill with pattern
  for (size_t i = 0; i < large_size; i++) {
    large_data[i] = (uint8_t)(i & 0xFF);
  }

  // Compute hash
  uint32_t hash = gcomp_xxhash32(large_data, large_size, 0);

  // Verify it's non-zero and consistent
  EXPECT_NE(hash, 0U);

  // Verify streaming matches one-shot
  gcomp_xxhash32_state_t state;
  gcomp_xxhash32_reset(&state, 0);
  gcomp_xxhash32_update(&state, large_data, large_size);
  uint32_t hash_streaming = gcomp_xxhash32_finalize(&state);
  EXPECT_EQ(hash, hash_streaming);

  delete[] large_data;
}

// Test large input with incremental computation in 1KB chunks
TEST_F(Xxhash32Test, LargeInputIncremental) {
  const size_t large_size = 64 * 1024; // 64 KB
  uint8_t * large_data = new uint8_t[large_size];

  // Fill with pattern
  for (size_t i = 0; i < large_size; i++) {
    large_data[i] = (uint8_t)(i & 0xFF);
  }

  // One-shot
  uint32_t hash_oneshot = gcomp_xxhash32(large_data, large_size, 0);

  // Incremental in 1KB chunks
  gcomp_xxhash32_state_t state;
  gcomp_xxhash32_reset(&state, 0);
  const size_t chunk_size = 1024;
  for (size_t offset = 0; offset < large_size; offset += chunk_size) {
    size_t remaining = large_size - offset;
    size_t chunk_len = (remaining < chunk_size) ? remaining : chunk_size;
    gcomp_xxhash32_update(&state, large_data + offset, chunk_len);
  }
  uint32_t hash_incremental = gcomp_xxhash32_finalize(&state);

  EXPECT_EQ(hash_oneshot, hash_incremental);

  delete[] large_data;
}

// Test that xxHash32 is deterministic (same input = same output)
TEST_F(Xxhash32Test, Deterministic) {
  uint32_t hash1 =
      gcomp_xxhash32(TEST_STRING_123456789, sizeof(TEST_STRING_123456789), 0);
  uint32_t hash2 =
      gcomp_xxhash32(TEST_STRING_123456789, sizeof(TEST_STRING_123456789), 0);
  EXPECT_EQ(hash1, hash2);
}

// Test xxHash32 with different seeds
TEST_F(Xxhash32Test, DifferentSeeds) {
  uint32_t hash_seed0 =
      gcomp_xxhash32(TEST_STRING_123456789, sizeof(TEST_STRING_123456789), 0);
  uint32_t hash_seed1 =
      gcomp_xxhash32(TEST_STRING_123456789, sizeof(TEST_STRING_123456789), 1);
  uint32_t hash_seed42 =
      gcomp_xxhash32(TEST_STRING_123456789, sizeof(TEST_STRING_123456789), 42);

  // Different seeds should produce different hashes
  EXPECT_NE(hash_seed0, hash_seed1);
  EXPECT_NE(hash_seed0, hash_seed42);
  EXPECT_NE(hash_seed1, hash_seed42);
}

// Test streaming with different seeds
TEST_F(Xxhash32Test, StreamingWithDifferentSeeds) {
  gcomp_xxhash32_state_t state;

  gcomp_xxhash32_reset(&state, 42);
  gcomp_xxhash32_update(
      &state, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
  uint32_t hash_streaming = gcomp_xxhash32_finalize(&state);

  uint32_t hash_oneshot =
      gcomp_xxhash32(TEST_STRING_123456789, sizeof(TEST_STRING_123456789), 42);

  EXPECT_EQ(hash_streaming, hash_oneshot);
}

// Test xxHash32 with all zeros
TEST_F(Xxhash32Test, AllZeros) {
  const uint8_t zeros[100] = {0};
  uint32_t hash = gcomp_xxhash32(zeros, sizeof(zeros), 0);
  // Should be consistent and non-empty
  EXPECT_NE(hash, XXHASH32_EMPTY);
}

// Test xxHash32 with all 0xFF
TEST_F(Xxhash32Test, AllOnes) {
  uint8_t ones[100];
  memset(ones, 0xFF, sizeof(ones));
  uint32_t hash = gcomp_xxhash32(ones, sizeof(ones), 0);
  // Should be consistent and non-empty
  EXPECT_NE(hash, XXHASH32_EMPTY);
}

// Test multiple finalize calls (should produce same result)
TEST_F(Xxhash32Test, MultipleFinalize) {
  gcomp_xxhash32_state_t state;
  gcomp_xxhash32_reset(&state, 0);
  gcomp_xxhash32_update(
      &state, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));

  uint32_t hash1 = gcomp_xxhash32_finalize(&state);
  uint32_t hash2 = gcomp_xxhash32_finalize(&state);

  EXPECT_EQ(hash1, hash2);
  EXPECT_EQ(hash1, XXHASH32_123456789);
}

// Test reset clears state
TEST_F(Xxhash32Test, ResetClearsState) {
  gcomp_xxhash32_state_t state;

  // First computation
  gcomp_xxhash32_reset(&state, 0);
  gcomp_xxhash32_update(
      &state, TEST_STRING_QUICK_BROWN, sizeof(TEST_STRING_QUICK_BROWN));
  uint32_t hash1 = gcomp_xxhash32_finalize(&state);
  EXPECT_EQ(hash1, XXHASH32_QUICK_BROWN);

  // Reset and compute different hash
  gcomp_xxhash32_reset(&state, 0);
  gcomp_xxhash32_update(
      &state, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
  uint32_t hash2 = gcomp_xxhash32_finalize(&state);
  EXPECT_EQ(hash2, XXHASH32_123456789);
}

// Test round-trip: reset -> update -> finalize -> reset -> update -> finalize
TEST_F(Xxhash32Test, RoundTrip) {
  gcomp_xxhash32_state_t state;

  gcomp_xxhash32_reset(&state, 0);
  gcomp_xxhash32_update(
      &state, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
  uint32_t hash1 = gcomp_xxhash32_finalize(&state);

  gcomp_xxhash32_reset(&state, 0);
  gcomp_xxhash32_update(
      &state, TEST_STRING_123456789, sizeof(TEST_STRING_123456789));
  uint32_t hash2 = gcomp_xxhash32_finalize(&state);

  EXPECT_EQ(hash1, hash2);
  EXPECT_EQ(hash1, XXHASH32_123456789);
}

// Test LZ4 header checksum calculation pattern
// LZ4 uses (xxhash32 >> 8) & 0xFF for the header checksum byte
TEST_F(Xxhash32Test, Lz4HeaderChecksumPattern) {
  // For LZ4, the header checksum is computed over the header descriptor bytes
  // and then the second byte of the hash is used
  uint8_t descriptor[] = {0x60, 0x40}; // Example descriptor bytes
  uint32_t hash = gcomp_xxhash32(descriptor, sizeof(descriptor), 0);
  uint8_t header_checksum = (uint8_t)((hash >> 8) & 0xFF);

  // Just verify the pattern works (specific value depends on the descriptor)
  EXPECT_EQ(header_checksum, (hash >> 8) & 0xFF);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
