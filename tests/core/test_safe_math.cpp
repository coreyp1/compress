/**
 * @file test_safe_math.cpp
 *
 * Unit tests for safe_math.h (internal overflow-safe arithmetic).
 * Include path: -I src so that core/safe_math.h is found.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "core/safe_math.h"
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>

// gcomp_safe_add_size
TEST(SafeMath, AddSize_NoOverflow) {
  size_t result = 0;
  EXPECT_TRUE(gcomp_safe_add_size(0, 0, &result));
  EXPECT_EQ(result, 0u);

  EXPECT_TRUE(gcomp_safe_add_size(1, 2, &result));
  EXPECT_EQ(result, 3u);

  EXPECT_TRUE(gcomp_safe_add_size(SIZE_MAX / 2, SIZE_MAX / 2, &result));
  EXPECT_EQ(result, SIZE_MAX - 1);

  EXPECT_TRUE(gcomp_safe_add_size(SIZE_MAX, 0, &result));
  EXPECT_EQ(result, SIZE_MAX);
}

TEST(SafeMath, AddSize_Overflow) {
  size_t result = 99;
  EXPECT_FALSE(gcomp_safe_add_size(SIZE_MAX, 1, &result));
  EXPECT_EQ(result, 99u); // unchanged on failure

  EXPECT_FALSE(gcomp_safe_add_size(1, SIZE_MAX, &result));
  EXPECT_EQ(result, 99u);

  EXPECT_FALSE(
      gcomp_safe_add_size(SIZE_MAX / 2 + 1, SIZE_MAX / 2 + 1, &result));
  EXPECT_EQ(result, 99u);
}

// gcomp_safe_mul_size
TEST(SafeMath, MulSize_NoOverflow) {
  size_t result = 0;
  EXPECT_TRUE(gcomp_safe_mul_size(0, 100, &result));
  EXPECT_EQ(result, 0u);

  EXPECT_TRUE(gcomp_safe_mul_size(100, 0, &result));
  EXPECT_EQ(result, 0u);

  EXPECT_TRUE(gcomp_safe_mul_size(3, 7, &result));
  EXPECT_EQ(result, 21u);

  // Largest n such that n*n <= SIZE_MAX (no loop: 32-bit -> 65535, 64-bit ->
  // 2^32-1)
  size_t max_sqrt =
      (sizeof(size_t) >= 8) ? ((size_t)1 << 32) - 1 : ((size_t)1 << 16) - 1;
  EXPECT_TRUE(gcomp_safe_mul_size(max_sqrt, max_sqrt, &result));
  EXPECT_EQ(result, max_sqrt * max_sqrt);
}

TEST(SafeMath, MulSize_Overflow) {
  size_t result = 99;
  EXPECT_FALSE(gcomp_safe_mul_size(SIZE_MAX, 2, &result));
  EXPECT_EQ(result, 99u);

  EXPECT_FALSE(gcomp_safe_mul_size(2, SIZE_MAX, &result));
  EXPECT_EQ(result, 99u);

  EXPECT_FALSE(gcomp_safe_mul_size(SIZE_MAX / 100 + 1, 101, &result));
  EXPECT_EQ(result, 99u);
}

// gcomp_safe_add_u64
TEST(SafeMath, AddU64_NoOverflow) {
  uint64_t result = 0;
  EXPECT_TRUE(gcomp_safe_add_u64(0, 0, &result));
  EXPECT_EQ(result, 0u);

  EXPECT_TRUE(gcomp_safe_add_u64(UINT64_MAX / 2, UINT64_MAX / 2, &result));
  EXPECT_EQ(result, UINT64_MAX - 1);

  EXPECT_TRUE(gcomp_safe_add_u64(UINT64_MAX, 0, &result));
  EXPECT_EQ(result, UINT64_MAX);
}

TEST(SafeMath, AddU64_Overflow) {
  uint64_t result = 99;
  EXPECT_FALSE(gcomp_safe_add_u64(UINT64_MAX, 1, &result));
  EXPECT_EQ(result, 99u);

  EXPECT_FALSE(gcomp_safe_add_u64(1, UINT64_MAX, &result));
  EXPECT_EQ(result, 99u);
}

// gcomp_safe_mul_u64
TEST(SafeMath, MulU64_NoOverflow) {
  uint64_t result = 0;
  EXPECT_TRUE(gcomp_safe_mul_u64(0, 1000, &result));
  EXPECT_EQ(result, 0u);

  EXPECT_TRUE(gcomp_safe_mul_u64(1000, 0, &result));
  EXPECT_EQ(result, 0u);

  EXPECT_TRUE(gcomp_safe_mul_u64(100, 100, &result));
  EXPECT_EQ(result, 10000u);
}

TEST(SafeMath, MulU64_Overflow) {
  uint64_t result = 99;
  EXPECT_FALSE(gcomp_safe_mul_u64(UINT64_MAX, 2, &result));
  EXPECT_EQ(result, 99u);

  EXPECT_FALSE(gcomp_safe_mul_u64(2, UINT64_MAX, &result));
  EXPECT_EQ(result, 99u);
}

// gcomp_safe_add_u32 / gcomp_safe_mul_u32
TEST(SafeMath, AddU32_NoOverflow) {
  uint32_t result = 0;
  EXPECT_TRUE(gcomp_safe_add_u32(0, 0, &result));
  EXPECT_EQ(result, 0u);

  EXPECT_TRUE(gcomp_safe_add_u32(UINT32_MAX - 1, 1, &result));
  EXPECT_EQ(result, UINT32_MAX);
}

TEST(SafeMath, AddU32_Overflow) {
  uint32_t result = 99;
  EXPECT_FALSE(gcomp_safe_add_u32(UINT32_MAX, 1, &result));
  EXPECT_EQ(result, 99u);
}

TEST(SafeMath, MulU32_NoOverflow) {
  uint32_t result = 0;
  EXPECT_TRUE(gcomp_safe_mul_u32(100, 100, &result));
  EXPECT_EQ(result, 10000u);
}

TEST(SafeMath, MulU32_Overflow) {
  uint32_t result = 99;
  EXPECT_FALSE(gcomp_safe_mul_u32(UINT32_MAX, 2, &result));
  EXPECT_EQ(result, 99u);
}

// gcomp_safe_add3_size
TEST(SafeMath, Add3Size_NoOverflow) {
  size_t result = 0;
  EXPECT_TRUE(gcomp_safe_add3_size(1, 2, 3, &result));
  EXPECT_EQ(result, 6u);

  EXPECT_TRUE(gcomp_safe_add3_size(0, 0, 0, &result));
  EXPECT_EQ(result, 0u);
}

TEST(SafeMath, Add3Size_Overflow) {
  size_t result = 99;
  EXPECT_FALSE(gcomp_safe_add3_size(SIZE_MAX, 1, 0, &result));
  EXPECT_EQ(result, 99u);

  EXPECT_FALSE(gcomp_safe_add3_size(
      SIZE_MAX / 3, SIZE_MAX / 3, SIZE_MAX / 3 + 1, &result));
  EXPECT_EQ(result, 99u);
}

// gcomp_safe_size_calc
TEST(SafeMath, SizeCalc_NoOverflow) {
  size_t result = 0;
  EXPECT_TRUE(gcomp_safe_size_calc(10, 8, 4, &result)); // 10*8+4 = 84
  EXPECT_EQ(result, 84u);

  EXPECT_TRUE(gcomp_safe_size_calc(0, 100, 5, &result));
  EXPECT_EQ(result, 5u);
}

TEST(SafeMath, SizeCalc_Overflow) {
  size_t result = 99;
  EXPECT_FALSE(gcomp_safe_size_calc(SIZE_MAX, 2, 0, &result));
  EXPECT_EQ(result, 99u);

  EXPECT_FALSE(gcomp_safe_size_calc(SIZE_MAX, 1, 1, &result));
  EXPECT_EQ(result, 99u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
