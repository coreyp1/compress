/**
 * @file test_limits.cpp
 *
 * Unit tests for the limits API in the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstdint>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/options.h>
#include <gtest/gtest.h>

class LimitsTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create a fresh options object for each test
    gcomp_options_create(&options_);
  }

  void TearDown() override {
    if (options_ != nullptr) {
      gcomp_options_destroy(options_);
      options_ = nullptr;
    }
  }

  gcomp_options_t * options_ = nullptr;
};

// Test gcomp_limits_read_output_max()
TEST_F(LimitsTest, ReadOutputMaxWithDefault) {
  uint64_t default_val = 1024;
  uint64_t result = gcomp_limits_read_output_max(options_, default_val);
  EXPECT_EQ(result, default_val);
}

TEST_F(LimitsTest, ReadOutputMaxWithNullOptions) {
  uint64_t default_val = 1024;
  uint64_t result = gcomp_limits_read_output_max(nullptr, default_val);
  EXPECT_EQ(result, default_val);
}

TEST_F(LimitsTest, ReadOutputMaxFromOptions) {
  uint64_t set_value = 2048;
  ASSERT_EQ(
      gcomp_options_set_uint64(options_, "limits.max_output_bytes", set_value),
      GCOMP_OK);

  uint64_t default_val = 1024;
  uint64_t result = gcomp_limits_read_output_max(options_, default_val);
  EXPECT_EQ(result, set_value);
}

TEST_F(LimitsTest, ReadOutputMaxZeroUnlimited) {
  uint64_t set_value = 0; // 0 means unlimited
  ASSERT_EQ(
      gcomp_options_set_uint64(options_, "limits.max_output_bytes", set_value),
      GCOMP_OK);

  uint64_t default_val = 1024;
  uint64_t result = gcomp_limits_read_output_max(options_, default_val);
  EXPECT_EQ(result, 0);
}

// Test gcomp_limits_read_memory_max()
TEST_F(LimitsTest, ReadMemoryMaxWithDefault) {
  uint64_t default_val = 512;
  uint64_t result = gcomp_limits_read_memory_max(options_, default_val);
  EXPECT_EQ(result, default_val);
}

TEST_F(LimitsTest, ReadMemoryMaxWithNullOptions) {
  uint64_t default_val = 512;
  uint64_t result = gcomp_limits_read_memory_max(nullptr, default_val);
  EXPECT_EQ(result, default_val);
}

TEST_F(LimitsTest, ReadMemoryMaxFromOptions) {
  uint64_t set_value = 1024;
  ASSERT_EQ(
      gcomp_options_set_uint64(options_, "limits.max_memory_bytes", set_value),
      GCOMP_OK);

  uint64_t default_val = 512;
  uint64_t result = gcomp_limits_read_memory_max(options_, default_val);
  EXPECT_EQ(result, set_value);
}

// Test gcomp_limits_read_window_max()
TEST_F(LimitsTest, ReadWindowMaxWithDefault) {
  uint64_t default_val = 32768;
  uint64_t result = gcomp_limits_read_window_max(options_, default_val);
  EXPECT_EQ(result, default_val);
}

TEST_F(LimitsTest, ReadWindowMaxWithNullOptions) {
  uint64_t default_val = 32768;
  uint64_t result = gcomp_limits_read_window_max(nullptr, default_val);
  EXPECT_EQ(result, default_val);
}

TEST_F(LimitsTest, ReadWindowMaxFromOptions) {
  uint64_t set_value = 65536;
  ASSERT_EQ(
      gcomp_options_set_uint64(options_, "limits.max_window_bytes", set_value),
      GCOMP_OK);

  uint64_t default_val = 32768;
  uint64_t result = gcomp_limits_read_window_max(options_, default_val);
  EXPECT_EQ(result, set_value);
}

// Test gcomp_limits_check_output()
TEST_F(LimitsTest, CheckOutputWithinLimit) {
  size_t current = 100;
  uint64_t limit = 200;
  gcomp_status_t status = gcomp_limits_check_output(current, limit);
  EXPECT_EQ(status, GCOMP_OK);
}

TEST_F(LimitsTest, CheckOutputAtLimit) {
  size_t current = 200;
  uint64_t limit = 200;
  gcomp_status_t status = gcomp_limits_check_output(current, limit);
  EXPECT_EQ(status, GCOMP_OK);
}

TEST_F(LimitsTest, CheckOutputOverLimit) {
  size_t current = 300;
  uint64_t limit = 200;
  gcomp_status_t status = gcomp_limits_check_output(current, limit);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);
}

TEST_F(LimitsTest, CheckOutputUnlimited) {
  size_t current = SIZE_MAX;
  uint64_t limit = 0; // 0 means unlimited
  gcomp_status_t status = gcomp_limits_check_output(current, limit);
  EXPECT_EQ(status, GCOMP_OK);
}

TEST_F(LimitsTest, CheckOutputZeroLimit) {
  size_t current = 0;
  uint64_t limit = 0; // 0 means unlimited
  gcomp_status_t status = gcomp_limits_check_output(current, limit);
  EXPECT_EQ(status, GCOMP_OK);
}

// Test gcomp_limits_check_memory()
TEST_F(LimitsTest, CheckMemoryWithinLimit) {
  size_t current = 100;
  uint64_t limit = 200;
  gcomp_status_t status = gcomp_limits_check_memory(current, limit);
  EXPECT_EQ(status, GCOMP_OK);
}

TEST_F(LimitsTest, CheckMemoryAtLimit) {
  size_t current = 200;
  uint64_t limit = 200;
  gcomp_status_t status = gcomp_limits_check_memory(current, limit);
  EXPECT_EQ(status, GCOMP_OK);
}

TEST_F(LimitsTest, CheckMemoryOverLimit) {
  size_t current = 300;
  uint64_t limit = 200;
  gcomp_status_t status = gcomp_limits_check_memory(current, limit);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);
}

TEST_F(LimitsTest, CheckMemoryUnlimited) {
  size_t current = SIZE_MAX;
  uint64_t limit = 0; // 0 means unlimited
  gcomp_status_t status = gcomp_limits_check_memory(current, limit);
  EXPECT_EQ(status, GCOMP_OK);
}

// Test gcomp_memory_track_alloc()
TEST_F(LimitsTest, MemoryTrackAllocBasic) {
  gcomp_memory_tracker_t tracker = {0};
  gcomp_memory_track_alloc(&tracker, 100);
  EXPECT_EQ(tracker.current_bytes, 100);
}

TEST_F(LimitsTest, MemoryTrackAllocMultiple) {
  gcomp_memory_tracker_t tracker = {0};
  gcomp_memory_track_alloc(&tracker, 100);
  gcomp_memory_track_alloc(&tracker, 50);
  gcomp_memory_track_alloc(&tracker, 25);
  EXPECT_EQ(tracker.current_bytes, 175);
}

TEST_F(LimitsTest, MemoryTrackAllocNullPointer) {
  // Should not crash
  gcomp_memory_track_alloc(nullptr, 100);
}

TEST_F(LimitsTest, MemoryTrackAllocOverflow) {
  gcomp_memory_tracker_t tracker = {0};
  tracker.current_bytes = SIZE_MAX - 50;
  gcomp_memory_track_alloc(&tracker, 100); // Would overflow
  EXPECT_EQ(tracker.current_bytes, SIZE_MAX);
}

// Test gcomp_memory_track_free()
TEST_F(LimitsTest, MemoryTrackFreeBasic) {
  gcomp_memory_tracker_t tracker = {0};
  tracker.current_bytes = 100;
  gcomp_memory_track_free(&tracker, 50);
  EXPECT_EQ(tracker.current_bytes, 50);
}

TEST_F(LimitsTest, MemoryTrackFreeMultiple) {
  gcomp_memory_tracker_t tracker = {0};
  tracker.current_bytes = 200;
  gcomp_memory_track_free(&tracker, 50);
  gcomp_memory_track_free(&tracker, 75);
  EXPECT_EQ(tracker.current_bytes, 75);
}

TEST_F(LimitsTest, MemoryTrackFreeNullPointer) {
  // Should not crash
  gcomp_memory_track_free(nullptr, 100);
}

TEST_F(LimitsTest, MemoryTrackFreeUnderflow) {
  gcomp_memory_tracker_t tracker = {0};
  tracker.current_bytes = 50;
  gcomp_memory_track_free(&tracker, 100); // Would underflow
  EXPECT_EQ(tracker.current_bytes, 0);
}

TEST_F(LimitsTest, MemoryTrackFreeToZero) {
  gcomp_memory_tracker_t tracker = {0};
  tracker.current_bytes = 100;
  gcomp_memory_track_free(&tracker, 100);
  EXPECT_EQ(tracker.current_bytes, 0);
}

// Test gcomp_memory_check_limit()
TEST_F(LimitsTest, MemoryCheckLimitWithinLimit) {
  gcomp_memory_tracker_t tracker = {0};
  tracker.current_bytes = 100;
  uint64_t limit = 200;
  gcomp_status_t status = gcomp_memory_check_limit(&tracker, limit);
  EXPECT_EQ(status, GCOMP_OK);
}

TEST_F(LimitsTest, MemoryCheckLimitAtLimit) {
  gcomp_memory_tracker_t tracker = {0};
  tracker.current_bytes = 200;
  uint64_t limit = 200;
  gcomp_status_t status = gcomp_memory_check_limit(&tracker, limit);
  EXPECT_EQ(status, GCOMP_OK);
}

TEST_F(LimitsTest, MemoryCheckLimitOverLimit) {
  gcomp_memory_tracker_t tracker = {0};
  tracker.current_bytes = 300;
  uint64_t limit = 200;
  gcomp_status_t status = gcomp_memory_check_limit(&tracker, limit);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);
}

TEST_F(LimitsTest, MemoryCheckLimitUnlimited) {
  gcomp_memory_tracker_t tracker = {0};
  tracker.current_bytes = SIZE_MAX;
  uint64_t limit = 0; // 0 means unlimited
  gcomp_status_t status = gcomp_memory_check_limit(&tracker, limit);
  EXPECT_EQ(status, GCOMP_OK);
}

TEST_F(LimitsTest, MemoryCheckLimitNullPointer) {
  gcomp_status_t status = gcomp_memory_check_limit(nullptr, 100);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

// Test round-trip: set limit, read it, check it
TEST_F(LimitsTest, RoundTripOutputLimit) {
  uint64_t set_value = 4096;
  ASSERT_EQ(
      gcomp_options_set_uint64(options_, "limits.max_output_bytes", set_value),
      GCOMP_OK);

  uint64_t read_value =
      gcomp_limits_read_output_max(options_, GCOMP_DEFAULT_MAX_OUTPUT_BYTES);
  EXPECT_EQ(read_value, set_value);

  // Check within limit
  EXPECT_EQ(gcomp_limits_check_output(1000, read_value), GCOMP_OK);
  // Check over limit
  EXPECT_EQ(gcomp_limits_check_output(5000, read_value), GCOMP_ERR_LIMIT);
}

// Test memory tracking with allocations and deallocations
TEST_F(LimitsTest, MemoryTrackingRoundTrip) {
  gcomp_memory_tracker_t tracker = {0};
  uint64_t limit = 1000;

  // Allocate some memory
  gcomp_memory_track_alloc(&tracker, 300);
  EXPECT_EQ(gcomp_memory_check_limit(&tracker, limit), GCOMP_OK);

  gcomp_memory_track_alloc(&tracker, 400);
  EXPECT_EQ(gcomp_memory_check_limit(&tracker, limit), GCOMP_OK);

  // This should exceed the limit (300 + 400 + 400 = 1100 > 1000)
  gcomp_memory_track_alloc(&tracker, 400);
  EXPECT_EQ(gcomp_memory_check_limit(&tracker, limit), GCOMP_ERR_LIMIT);

  // Free some memory (1100 - 200 = 900, now under limit)
  gcomp_memory_track_free(&tracker, 200);
  EXPECT_EQ(gcomp_memory_check_limit(&tracker, limit), GCOMP_OK);

  // Free more to get even further under limit
  gcomp_memory_track_free(&tracker, 500);
  EXPECT_EQ(gcomp_memory_check_limit(&tracker, limit), GCOMP_OK);
}

// Test edge cases: maximum values
TEST_F(LimitsTest, EdgeCaseMaximumValues) {
  uint64_t max_uint64 = UINT64_MAX;
  ASSERT_EQ(
      gcomp_options_set_uint64(options_, "limits.max_output_bytes", max_uint64),
      GCOMP_OK);

  uint64_t result = gcomp_limits_read_output_max(options_, 0);
  EXPECT_EQ(result, max_uint64);
}

// Test edge cases: very large current values
TEST_F(LimitsTest, EdgeCaseLargeCurrent) {
  size_t large_current = SIZE_MAX;
  uint64_t limit = SIZE_MAX;
  gcomp_status_t status = gcomp_limits_check_output(large_current, limit);
  EXPECT_EQ(status, GCOMP_OK);
}

//
// Tests for expansion ratio limit (decompression bomb protection)
//

// Test gcomp_limits_read_expansion_ratio_max()
TEST_F(LimitsTest, ReadExpansionRatioMaxWithDefault) {
  uint64_t default_val = 1000;
  uint64_t result =
      gcomp_limits_read_expansion_ratio_max(options_, default_val);
  EXPECT_EQ(result, default_val);
}

TEST_F(LimitsTest, ReadExpansionRatioMaxWithNullOptions) {
  uint64_t default_val = 1000;
  uint64_t result = gcomp_limits_read_expansion_ratio_max(nullptr, default_val);
  EXPECT_EQ(result, default_val);
}

TEST_F(LimitsTest, ReadExpansionRatioMaxFromOptions) {
  uint64_t set_value = 500;
  ASSERT_EQ(gcomp_options_set_uint64(
                options_, "limits.max_expansion_ratio", set_value),
      GCOMP_OK);

  uint64_t default_val = 1000;
  uint64_t result =
      gcomp_limits_read_expansion_ratio_max(options_, default_val);
  EXPECT_EQ(result, set_value);
}

TEST_F(LimitsTest, ReadExpansionRatioMaxZeroUnlimited) {
  uint64_t set_value = 0; // 0 means unlimited
  ASSERT_EQ(gcomp_options_set_uint64(
                options_, "limits.max_expansion_ratio", set_value),
      GCOMP_OK);

  uint64_t default_val = 1000;
  uint64_t result =
      gcomp_limits_read_expansion_ratio_max(options_, default_val);
  EXPECT_EQ(result, 0);
}

// Test gcomp_limits_check_expansion_ratio()
TEST_F(LimitsTest, CheckExpansionRatioWithinLimit) {
  // 100 input, 1000 output, ratio = 10x, limit = 100x
  uint64_t input_bytes = 100;
  uint64_t output_bytes = 1000;
  uint64_t ratio_limit = 100;
  gcomp_status_t status =
      gcomp_limits_check_expansion_ratio(input_bytes, output_bytes, ratio_limit);
  EXPECT_EQ(status, GCOMP_OK);
}

TEST_F(LimitsTest, CheckExpansionRatioAtLimit) {
  // 100 input, 10000 output, ratio = 100x, limit = 100x
  uint64_t input_bytes = 100;
  uint64_t output_bytes = 10000;
  uint64_t ratio_limit = 100;
  gcomp_status_t status =
      gcomp_limits_check_expansion_ratio(input_bytes, output_bytes, ratio_limit);
  EXPECT_EQ(status, GCOMP_OK);
}

TEST_F(LimitsTest, CheckExpansionRatioOverLimit) {
  // 100 input, 10001 output, ratio > 100x, limit = 100x
  uint64_t input_bytes = 100;
  uint64_t output_bytes = 10001;
  uint64_t ratio_limit = 100;
  gcomp_status_t status =
      gcomp_limits_check_expansion_ratio(input_bytes, output_bytes, ratio_limit);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);
}

TEST_F(LimitsTest, CheckExpansionRatioUnlimited) {
  // With ratio_limit = 0, any expansion should be allowed
  uint64_t input_bytes = 1;
  uint64_t output_bytes = UINT64_MAX;
  uint64_t ratio_limit = 0; // unlimited
  gcomp_status_t status =
      gcomp_limits_check_expansion_ratio(input_bytes, output_bytes, ratio_limit);
  EXPECT_EQ(status, GCOMP_OK);
}

TEST_F(LimitsTest, CheckExpansionRatioZeroInput) {
  // With 0 input bytes, ratio cannot be computed, should allow
  uint64_t input_bytes = 0;
  uint64_t output_bytes = 1000;
  uint64_t ratio_limit = 100;
  gcomp_status_t status =
      gcomp_limits_check_expansion_ratio(input_bytes, output_bytes, ratio_limit);
  EXPECT_EQ(status, GCOMP_OK);
}

TEST_F(LimitsTest, CheckExpansionRatioZeroOutput) {
  // With 0 output bytes, ratio is 0, always within limit
  uint64_t input_bytes = 100;
  uint64_t output_bytes = 0;
  uint64_t ratio_limit = 100;
  gcomp_status_t status =
      gcomp_limits_check_expansion_ratio(input_bytes, output_bytes, ratio_limit);
  EXPECT_EQ(status, GCOMP_OK);
}

TEST_F(LimitsTest, CheckExpansionRatioDefaultValue) {
  // Test with default ratio (1000x)
  // 1 KB input, 1000 KB output = 1000x ratio, should be exactly at limit
  uint64_t input_bytes = 1024;
  uint64_t output_bytes = 1024 * 1000; // Exactly 1000x
  uint64_t ratio_limit = GCOMP_DEFAULT_MAX_EXPANSION_RATIO;
  gcomp_status_t status =
      gcomp_limits_check_expansion_ratio(input_bytes, output_bytes, ratio_limit);
  EXPECT_EQ(status, GCOMP_OK);

  // 1 KB input, slightly more than 1000 KB output, should exceed
  output_bytes = 1024 * 1000 + 1;
  status =
      gcomp_limits_check_expansion_ratio(input_bytes, output_bytes, ratio_limit);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);
}

TEST_F(LimitsTest, CheckExpansionRatioOverflowProtection) {
  // Test that the multiplication doesn't overflow
  // With very large ratio_limit * input_bytes that would overflow
  uint64_t input_bytes = UINT64_MAX / 100;
  uint64_t output_bytes = UINT64_MAX / 2;
  uint64_t ratio_limit = 200; // ratio_limit * input_bytes would overflow
  gcomp_status_t status =
      gcomp_limits_check_expansion_ratio(input_bytes, output_bytes, ratio_limit);
  // Since overflow would occur, the effective limit is infinite, so allow
  EXPECT_EQ(status, GCOMP_OK);
}

TEST_F(LimitsTest, CheckExpansionRatioTypicalBombScenario) {
  // Typical decompression bomb: 1 KB compressed -> 1 GB decompressed
  // Ratio = 1,000,000x, should exceed default limit of 1000x
  uint64_t input_bytes = 1024;               // 1 KB
  uint64_t output_bytes = 1024ULL * 1024 * 1024; // 1 GB
  uint64_t ratio_limit = GCOMP_DEFAULT_MAX_EXPANSION_RATIO;
  gcomp_status_t status =
      gcomp_limits_check_expansion_ratio(input_bytes, output_bytes, ratio_limit);
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);
}

TEST_F(LimitsTest, CheckExpansionRatioLegitimateHighRatio) {
  // Legitimate high-ratio data like all zeros
  // 1 KB compressed -> 900 KB decompressed (ratio ~900x)
  // Should be allowed with default 1000x limit
  uint64_t input_bytes = 1024;
  uint64_t output_bytes = 900 * 1024;
  uint64_t ratio_limit = GCOMP_DEFAULT_MAX_EXPANSION_RATIO;
  gcomp_status_t status =
      gcomp_limits_check_expansion_ratio(input_bytes, output_bytes, ratio_limit);
  EXPECT_EQ(status, GCOMP_OK);
}

// Test round-trip: set expansion ratio limit, read it, check it
TEST_F(LimitsTest, RoundTripExpansionRatioLimit) {
  uint64_t set_value = 50;
  ASSERT_EQ(gcomp_options_set_uint64(
                options_, "limits.max_expansion_ratio", set_value),
      GCOMP_OK);

  uint64_t read_value = gcomp_limits_read_expansion_ratio_max(
      options_, GCOMP_DEFAULT_MAX_EXPANSION_RATIO);
  EXPECT_EQ(read_value, set_value);

  // 100 input, 5000 output = 50x ratio, exactly at limit
  EXPECT_EQ(gcomp_limits_check_expansion_ratio(100, 5000, read_value), GCOMP_OK);
  // 100 input, 5001 output > 50x ratio, over limit
  EXPECT_EQ(
      gcomp_limits_check_expansion_ratio(100, 5001, read_value), GCOMP_ERR_LIMIT);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
