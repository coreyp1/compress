/**
 * @file test_helpers.cpp
 *
 * Test helper utilities implementation for the Ghoti.io Compress library tests.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <vector>

extern "C" {

bool test_helpers_buffers_equal(const uint8_t * expected, size_t expected_len,
    const uint8_t * actual, size_t actual_len) {
  if (expected_len != actual_len) {
    return false;
  }
  if (expected == nullptr && actual == nullptr) {
    return true;
  }
  if (expected == nullptr || actual == nullptr) {
    return false;
  }
  return std::memcmp(expected, actual, expected_len) == 0;
}

void test_helpers_generate_random(uint8_t * buffer, size_t len, uint32_t seed) {
  if (buffer == nullptr || len == 0) {
    return;
  }

  static bool seeded = false;
  if (seed == 0 && !seeded) {
    std::srand(static_cast<unsigned int>(std::time(nullptr)));
    seeded = true;
  }
  else if (seed != 0) {
    std::srand(seed);
  }

  for (size_t i = 0; i < len; ++i) {
    buffer[i] = static_cast<uint8_t>(std::rand() & 0xFF);
  }
}

void test_helpers_generate_pattern(
    uint8_t * buffer, size_t len, const uint8_t * pattern, size_t pattern_len) {
  if (buffer == nullptr || len == 0 || pattern == nullptr || pattern_len == 0) {
    return;
  }

  for (size_t i = 0; i < len; ++i) {
    buffer[i] = pattern[i % pattern_len];
  }
}

void test_helpers_generate_sequential(uint8_t * buffer, size_t len) {
  if (buffer == nullptr || len == 0) {
    return;
  }

  for (size_t i = 0; i < len; ++i) {
    buffer[i] = static_cast<uint8_t>(i & 0xFF);
  }
}

void test_helpers_generate_zeros(uint8_t * buffer, size_t len) {
  if (buffer == nullptr || len == 0) {
    return;
  }
  std::memset(buffer, 0, len);
}

void test_helpers_generate_ones(uint8_t * buffer, size_t len) {
  if (buffer == nullptr || len == 0) {
    return;
  }
  std::memset(buffer, 0xFF, len);
}

bool test_helpers_load_file(
    const char * filepath, uint8_t ** buffer_out, size_t * len_out) {
  if (filepath == nullptr || buffer_out == nullptr || len_out == nullptr) {
    return false;
  }

  std::ifstream file(filepath, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    return false;
  }

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  std::vector<uint8_t> buffer(static_cast<size_t>(size));
  if (!file.read(reinterpret_cast<char *>(buffer.data()), size)) {
    return false;
  }

  *buffer_out = static_cast<uint8_t *>(std::malloc(buffer.size()));
  if (*buffer_out == nullptr) {
    return false;
  }

  std::memcpy(*buffer_out, buffer.data(), buffer.size());
  *len_out = buffer.size();
  return true;
}

} // extern "C"

#ifdef __cplusplus

size_t test_helpers_find_first_diff(const uint8_t * expected,
    size_t expected_len, const uint8_t * actual, size_t actual_len) {
  if (expected == nullptr && actual == nullptr) {
    return 0;
  }
  if (expected == nullptr || actual == nullptr) {
    return 0;
  }

  size_t min_len = (expected_len < actual_len) ? expected_len : actual_len;
  for (size_t i = 0; i < min_len; ++i) {
    if (expected[i] != actual[i]) {
      return i;
    }
  }

  if (expected_len != actual_len) {
    return min_len;
  }

  return expected_len; // Buffers are equal
}

#endif // __cplusplus
