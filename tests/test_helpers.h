/**
 * @file test_helpers.h
 *
 * Test helper utilities for the Ghoti.io Compress library tests.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_TEST_HELPERS_H
#define GHOTI_IO_GCOMP_TEST_HELPERS_H

#include <gtest/gtest.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Compare two buffers for equality
 *
 * @param expected Expected buffer
 * @param expected_len Length of expected buffer
 * @param actual Actual buffer
 * @param actual_len Length of actual buffer
 * @return true if buffers are equal, false otherwise
 */
bool test_helpers_buffers_equal(const uint8_t * expected, size_t expected_len,
    const uint8_t * actual, size_t actual_len);

/**
 * @brief Generate random test data
 *
 * @param buffer Output buffer
 * @param len Length of buffer to generate
 * @param seed Random seed (0 = use time-based seed)
 */
void test_helpers_generate_random(uint8_t * buffer, size_t len, uint32_t seed);

/**
 * @brief Generate pattern test data (repeating pattern)
 *
 * @param buffer Output buffer
 * @param len Length of buffer to generate
 * @param pattern Pattern to repeat
 * @param pattern_len Length of pattern
 */
void test_helpers_generate_pattern(
    uint8_t * buffer, size_t len, const uint8_t * pattern, size_t pattern_len);

/**
 * @brief Generate sequential test data (0, 1, 2, ...)
 *
 * @param buffer Output buffer
 * @param len Length of buffer to generate
 */
void test_helpers_generate_sequential(uint8_t * buffer, size_t len);

/**
 * @brief Generate all-zeros test data
 *
 * @param buffer Output buffer
 * @param len Length of buffer to generate
 */
void test_helpers_generate_zeros(uint8_t * buffer, size_t len);

/**
 * @brief Generate all-ones test data
 *
 * @param buffer Output buffer
 * @param len Length of buffer to generate
 */
void test_helpers_generate_ones(uint8_t * buffer, size_t len);

/**
 * @brief Load test data from file
 *
 * @param filepath Path to test data file
 * @param buffer_out Output buffer (caller must free)
 * @param len_out Output length
 * @return true on success, false on failure
 */
bool test_helpers_load_file(
    const char * filepath, uint8_t ** buffer_out, size_t * len_out);

#ifdef __cplusplus
}
#endif

// C++ helper macros and utilities

#ifdef __cplusplus

/**
 * @brief Google Test assertion for buffer equality
 */
#define EXPECT_BUFFERS_EQ(expected, expected_len, actual, actual_len)          \
  do {                                                                         \
    EXPECT_TRUE(test_helpers_buffers_equal(                                    \
        reinterpret_cast<const uint8_t *>(expected), expected_len,             \
        reinterpret_cast<const uint8_t *>(actual), actual_len))                \
        << "Buffers differ at position "                                       \
        << test_helpers_find_first_diff(                                       \
               reinterpret_cast<const uint8_t *>(expected), expected_len,      \
               reinterpret_cast<const uint8_t *>(actual), actual_len);         \
  } while (0)

/**
 * @brief Find first differing byte position between two buffers
 *
 * @param expected Expected buffer
 * @param expected_len Length of expected buffer
 * @param actual Actual buffer
 * @param actual_len Length of actual buffer
 * @return Position of first difference, or expected_len if buffers are equal
 */
size_t test_helpers_find_first_diff(const uint8_t * expected,
    size_t expected_len, const uint8_t * actual, size_t actual_len);

#endif /* __cplusplus */

#endif /* GHOTI_IO_GCOMP_TEST_HELPERS_H */
