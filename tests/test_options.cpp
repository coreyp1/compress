/**
 * @file test_options.cpp
 *
 * Unit tests for the options API in the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstdlib>
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <gtest/gtest.h>

class OptionsTest : public ::testing::Test {
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

// Test gcomp_options_create()
TEST_F(OptionsTest, CreateSuccess) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(opts, nullptr);
  gcomp_options_destroy(opts);
}

TEST_F(OptionsTest, CreateNullPointer) {
  gcomp_status_t status = gcomp_options_create(nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

// Test gcomp_options_destroy()
TEST_F(OptionsTest, DestroyNullPointer) {
  // Should not crash
  gcomp_options_destroy(nullptr);
}

TEST_F(OptionsTest, DestroyCleanup) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  // Set some values
  ASSERT_EQ(gcomp_options_set_int64(opts, "test.int", 42), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_string(opts, "test.str", "hello"), GCOMP_OK);

  // Destroy should clean up
  gcomp_options_destroy(opts);
  // If we get here without crashing, cleanup worked
}

// Test gcomp_options_set_int64() / gcomp_options_get_int64()
TEST_F(OptionsTest, SetGetInt64) {
  int64_t value = 0;

  // Set a value
  EXPECT_EQ(gcomp_options_set_int64(options_, "test.int", 42), GCOMP_OK);

  // Get it back
  EXPECT_EQ(gcomp_options_get_int64(options_, "test.int", &value), GCOMP_OK);
  EXPECT_EQ(value, 42);
}

TEST_F(OptionsTest, SetInt64Overwrite) {
  int64_t value = 0;

  // Set initial value
  EXPECT_EQ(gcomp_options_set_int64(options_, "test.int", 42), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_int64(options_, "test.int", &value), GCOMP_OK);
  EXPECT_EQ(value, 42);

  // Overwrite with new value
  EXPECT_EQ(gcomp_options_set_int64(options_, "test.int", 100), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_int64(options_, "test.int", &value), GCOMP_OK);
  EXPECT_EQ(value, 100);
}

TEST_F(OptionsTest, GetInt64NotFound) {
  int64_t value = 0;
  EXPECT_EQ(gcomp_options_get_int64(options_, "nonexistent", &value),
      GCOMP_ERR_INVALID_ARG);
}

TEST_F(OptionsTest, SetInt64NullPointer) {
  EXPECT_EQ(
      gcomp_options_set_int64(nullptr, "test.int", 42), GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(
      gcomp_options_set_int64(options_, nullptr, 42), GCOMP_ERR_INVALID_ARG);
}

TEST_F(OptionsTest, GetInt64NullPointer) {
  int64_t value = 0;
  EXPECT_EQ(gcomp_options_get_int64(nullptr, "test.int", &value),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_options_get_int64(options_, nullptr, &value),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_options_get_int64(options_, "test.int", nullptr),
      GCOMP_ERR_INVALID_ARG);
}

TEST_F(OptionsTest, SetInt64TypeMismatch) {
  // Set as int64
  EXPECT_EQ(gcomp_options_set_int64(options_, "test", 42), GCOMP_OK);

  // Try to get as uint64 (should fail or return error)
  uint64_t uvalue = 0;
  EXPECT_NE(gcomp_options_get_uint64(options_, "test", &uvalue), GCOMP_OK);
}

TEST_F(OptionsTest, SetInt64MaxValues) {
  int64_t value = 0;

  // Test maximum positive value
  EXPECT_EQ(gcomp_options_set_int64(options_, "max", INT64_MAX), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_int64(options_, "max", &value), GCOMP_OK);
  EXPECT_EQ(value, INT64_MAX);

  // Test minimum negative value
  EXPECT_EQ(gcomp_options_set_int64(options_, "min", INT64_MIN), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_int64(options_, "min", &value), GCOMP_OK);
  EXPECT_EQ(value, INT64_MIN);
}

// Test gcomp_options_set_uint64() / gcomp_options_get_uint64()
TEST_F(OptionsTest, SetGetUint64) {
  uint64_t value = 0;

  EXPECT_EQ(gcomp_options_set_uint64(options_, "test.uint", 100), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_uint64(options_, "test.uint", &value), GCOMP_OK);
  EXPECT_EQ(value, 100U);
}

TEST_F(OptionsTest, SetUint64Overwrite) {
  uint64_t value = 0;

  EXPECT_EQ(gcomp_options_set_uint64(options_, "test.uint", 42), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_uint64(options_, "test.uint", &value), GCOMP_OK);
  EXPECT_EQ(value, 42U);

  EXPECT_EQ(gcomp_options_set_uint64(options_, "test.uint", 200), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_uint64(options_, "test.uint", &value), GCOMP_OK);
  EXPECT_EQ(value, 200U);
}

TEST_F(OptionsTest, GetUint64NotFound) {
  uint64_t value = 0;
  EXPECT_EQ(gcomp_options_get_uint64(options_, "nonexistent", &value),
      GCOMP_ERR_INVALID_ARG);
}

TEST_F(OptionsTest, SetUint64MaxValue) {
  uint64_t value = 0;
  EXPECT_EQ(gcomp_options_set_uint64(options_, "max", UINT64_MAX), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_uint64(options_, "max", &value), GCOMP_OK);
  EXPECT_EQ(value, UINT64_MAX);
}

// Test gcomp_options_set_bool() / gcomp_options_get_bool()
TEST_F(OptionsTest, SetGetBool) {
  int value = -1;

  EXPECT_EQ(gcomp_options_set_bool(options_, "test.bool", 1), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_bool(options_, "test.bool", &value), GCOMP_OK);
  EXPECT_EQ(value, 1);

  EXPECT_EQ(gcomp_options_set_bool(options_, "test.bool2", 0), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_bool(options_, "test.bool2", &value), GCOMP_OK);
  EXPECT_EQ(value, 0);
}

TEST_F(OptionsTest, SetBoolOverwrite) {
  int value = -1;

  EXPECT_EQ(gcomp_options_set_bool(options_, "test.bool", 1), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_bool(options_, "test.bool", &value), GCOMP_OK);
  EXPECT_EQ(value, 1);

  EXPECT_EQ(gcomp_options_set_bool(options_, "test.bool", 0), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_bool(options_, "test.bool", &value), GCOMP_OK);
  EXPECT_EQ(value, 0);
}

TEST_F(OptionsTest, GetBoolNotFound) {
  int value = -1;
  EXPECT_EQ(gcomp_options_get_bool(options_, "nonexistent", &value),
      GCOMP_ERR_INVALID_ARG);
}

// Test gcomp_options_set_string() / gcomp_options_get_string()
TEST_F(OptionsTest, SetGetString) {
  const char * value = nullptr;

  EXPECT_EQ(gcomp_options_set_string(options_, "test.str", "hello"), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_string(options_, "test.str", &value), GCOMP_OK);
  EXPECT_NE(value, nullptr);
  EXPECT_STREQ(value, "hello");
}

TEST_F(OptionsTest, SetStringOverwrite) {
  const char * value = nullptr;

  EXPECT_EQ(gcomp_options_set_string(options_, "test.str", "hello"), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_string(options_, "test.str", &value), GCOMP_OK);
  EXPECT_STREQ(value, "hello");

  EXPECT_EQ(gcomp_options_set_string(options_, "test.str", "world"), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_string(options_, "test.str", &value), GCOMP_OK);
  EXPECT_STREQ(value, "world");
}

TEST_F(OptionsTest, SetStringEmpty) {
  const char * value = nullptr;

  EXPECT_EQ(gcomp_options_set_string(options_, "test.str", ""), GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_string(options_, "test.str", &value), GCOMP_OK);
  EXPECT_NE(value, nullptr);
  EXPECT_STREQ(value, "");
}

TEST_F(OptionsTest, SetStringNullPointer) {
  EXPECT_EQ(gcomp_options_set_string(nullptr, "test.str", "hello"),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_options_set_string(options_, nullptr, "hello"),
      GCOMP_ERR_INVALID_ARG);
  // NULL value might be allowed or not - test both behaviors
  // For now, assume NULL value is invalid
  EXPECT_EQ(gcomp_options_set_string(options_, "test.str", nullptr),
      GCOMP_ERR_INVALID_ARG);
}

TEST_F(OptionsTest, GetStringNotFound) {
  const char * value = nullptr;
  EXPECT_EQ(gcomp_options_get_string(options_, "nonexistent", &value),
      GCOMP_ERR_INVALID_ARG);
}

TEST_F(OptionsTest, SetStringLong) {
  const char * value = nullptr;
  std::string long_str(1000, 'a');

  EXPECT_EQ(gcomp_options_set_string(options_, "test.str", long_str.c_str()),
      GCOMP_OK);
  EXPECT_EQ(gcomp_options_get_string(options_, "test.str", &value), GCOMP_OK);
  EXPECT_NE(value, nullptr);
  EXPECT_STREQ(value, long_str.c_str());
}

// Test gcomp_options_set_bytes() / gcomp_options_get_bytes()
TEST_F(OptionsTest, SetGetBytes) {
  const void * data_out = nullptr;
  size_t size_out = 0;
  uint8_t test_data[] = {0x01, 0x02, 0x03, 0x04};

  EXPECT_EQ(gcomp_options_set_bytes(
                options_, "test.bytes", test_data, sizeof(test_data)),
      GCOMP_OK);
  EXPECT_EQ(
      gcomp_options_get_bytes(options_, "test.bytes", &data_out, &size_out),
      GCOMP_OK);
  EXPECT_NE(data_out, nullptr);
  EXPECT_EQ(size_out, sizeof(test_data));
  EXPECT_BUFFERS_EQ(test_data, sizeof(test_data), data_out, size_out);
}

TEST_F(OptionsTest, SetBytesOverwrite) {
  const void * data_out = nullptr;
  size_t size_out = 0;
  uint8_t data1[] = {0x01, 0x02};
  uint8_t data2[] = {0x03, 0x04, 0x05};

  EXPECT_EQ(
      gcomp_options_set_bytes(options_, "test.bytes", data1, sizeof(data1)),
      GCOMP_OK);
  EXPECT_EQ(
      gcomp_options_set_bytes(options_, "test.bytes", data2, sizeof(data2)),
      GCOMP_OK);
  EXPECT_EQ(
      gcomp_options_get_bytes(options_, "test.bytes", &data_out, &size_out),
      GCOMP_OK);
  EXPECT_EQ(size_out, sizeof(data2));
  EXPECT_BUFFERS_EQ(data2, sizeof(data2), data_out, size_out);
}

TEST_F(OptionsTest, SetBytesEmpty) {
  const void * data_out = nullptr;
  size_t size_out = 0;

  EXPECT_EQ(
      gcomp_options_set_bytes(options_, "test.bytes", nullptr, 0), GCOMP_OK);
  EXPECT_EQ(
      gcomp_options_get_bytes(options_, "test.bytes", &data_out, &size_out),
      GCOMP_OK);
  EXPECT_EQ(size_out, 0U);
}

TEST_F(OptionsTest, SetBytesNullPointer) {
  uint8_t data[] = {0x01, 0x02};
  EXPECT_EQ(gcomp_options_set_bytes(nullptr, "test.bytes", data, sizeof(data)),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_options_set_bytes(options_, nullptr, data, sizeof(data)),
      GCOMP_ERR_INVALID_ARG);
}

TEST_F(OptionsTest, GetBytesNotFound) {
  const void * data_out = nullptr;
  size_t size_out = 0;
  EXPECT_EQ(
      gcomp_options_get_bytes(options_, "nonexistent", &data_out, &size_out),
      GCOMP_ERR_INVALID_ARG);
}

TEST_F(OptionsTest, SetBytesLarge) {
  const void * data_out = nullptr;
  size_t size_out = 0;
  std::vector<uint8_t> large_data(10000);
  for (size_t i = 0; i < large_data.size(); ++i) {
    large_data[i] = static_cast<uint8_t>(i & 0xFF);
  }

  EXPECT_EQ(gcomp_options_set_bytes(
                options_, "test.bytes", large_data.data(), large_data.size()),
      GCOMP_OK);
  EXPECT_EQ(
      gcomp_options_get_bytes(options_, "test.bytes", &data_out, &size_out),
      GCOMP_OK);
  EXPECT_EQ(size_out, large_data.size());
  EXPECT_BUFFERS_EQ(large_data.data(), large_data.size(), data_out, size_out);
}

// Test gcomp_options_clone()
TEST_F(OptionsTest, Clone) {
  // Set various types of values
  EXPECT_EQ(gcomp_options_set_int64(options_, "test.int", 42), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_uint64(options_, "test.uint", 100U), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_bool(options_, "test.bool", 1), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_string(options_, "test.str", "hello"), GCOMP_OK);
  uint8_t bytes[] = {0x01, 0x02, 0x03};
  EXPECT_EQ(
      gcomp_options_set_bytes(options_, "test.bytes", bytes, sizeof(bytes)),
      GCOMP_OK);

  // Clone
  gcomp_options_t * cloned = nullptr;
  EXPECT_EQ(gcomp_options_clone(options_, &cloned), GCOMP_OK);
  EXPECT_NE(cloned, nullptr);

  // Verify all values are cloned correctly
  int64_t int_val = 0;
  EXPECT_EQ(gcomp_options_get_int64(cloned, "test.int", &int_val), GCOMP_OK);
  EXPECT_EQ(int_val, 42);

  uint64_t uint_val = 0;
  EXPECT_EQ(gcomp_options_get_uint64(cloned, "test.uint", &uint_val), GCOMP_OK);
  EXPECT_EQ(uint_val, 100U);

  int bool_val = 0;
  EXPECT_EQ(gcomp_options_get_bool(cloned, "test.bool", &bool_val), GCOMP_OK);
  EXPECT_EQ(bool_val, 1);

  const char * str_val = nullptr;
  EXPECT_EQ(gcomp_options_get_string(cloned, "test.str", &str_val), GCOMP_OK);
  EXPECT_STREQ(str_val, "hello");

  const void * bytes_data = nullptr;
  size_t bytes_size = 0;
  EXPECT_EQ(
      gcomp_options_get_bytes(cloned, "test.bytes", &bytes_data, &bytes_size),
      GCOMP_OK);
  EXPECT_EQ(bytes_size, sizeof(bytes));
  EXPECT_BUFFERS_EQ(bytes, sizeof(bytes), bytes_data, bytes_size);

  // Cleanup
  gcomp_options_destroy(cloned);
}

TEST_F(OptionsTest, CloneNullPointer) {
  gcomp_options_t * cloned = nullptr;
  EXPECT_EQ(gcomp_options_clone(nullptr, &cloned), GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_options_clone(options_, nullptr), GCOMP_ERR_INVALID_ARG);
}

// Test gcomp_options_freeze()
TEST_F(OptionsTest, Freeze) {
  // Set a value before freezing
  EXPECT_EQ(gcomp_options_set_int64(options_, "test.int", 42), GCOMP_OK);

  // Freeze
  EXPECT_EQ(gcomp_options_freeze(options_), GCOMP_OK);

  // Should still be able to read
  int64_t value = 0;
  EXPECT_EQ(gcomp_options_get_int64(options_, "test.int", &value), GCOMP_OK);
  EXPECT_EQ(value, 42);

  // Should not be able to set new values
  EXPECT_NE(gcomp_options_set_int64(options_, "test.int", 100), GCOMP_OK);
  EXPECT_NE(gcomp_options_set_int64(options_, "test.new", 200), GCOMP_OK);

  // Verify original value unchanged
  value = 0;
  EXPECT_EQ(gcomp_options_get_int64(options_, "test.int", &value), GCOMP_OK);
  EXPECT_EQ(value, 42);
}

TEST_F(OptionsTest, FreezeNullPointer) {
  EXPECT_EQ(gcomp_options_freeze(nullptr), GCOMP_ERR_INVALID_ARG);
}

// Test memory cleanup with many values
TEST_F(OptionsTest, MemoryCleanupManyValues) {
  // Set many values
  for (int i = 0; i < 100; ++i) {
    char key[32];
    std::snprintf(key, sizeof(key), "key.%d", i);
    EXPECT_EQ(gcomp_options_set_int64(options_, key, i), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_string(options_, key, "test string"), GCOMP_OK);
  }

  // Destroy should clean up all memory
  gcomp_options_destroy(options_);
  options_ = nullptr;
  // If we get here without crashing, cleanup worked
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
