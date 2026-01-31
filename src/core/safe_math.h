/**
 * @file safe_math.h
 *
 * Safe integer arithmetic helpers for the Ghoti.io Compress library.
 *
 * These functions provide overflow-safe arithmetic operations that return
 * a boolean indicating success/failure rather than silently wrapping.
 * They are intended for use in security-sensitive code paths such as
 * buffer size calculations and limit checking.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_SAFE_MATH_H
#define GHOTI_IO_GCOMP_SAFE_MATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Safely add two size_t values.
 *
 * Computes a + b and stores the result in *result if no overflow occurs.
 *
 * @param a First operand
 * @param b Second operand
 * @param result Output parameter for the result
 * @return true if operation succeeded (no overflow), false if overflow
 */
static inline bool gcomp_safe_add_size(size_t a, size_t b, size_t * result) {
  if (a > SIZE_MAX - b) {
    return false; // Overflow would occur
  }
  *result = a + b;
  return true;
}

/**
 * @brief Safely multiply two size_t values.
 *
 * Computes a * b and stores the result in *result if no overflow occurs.
 *
 * @param a First operand
 * @param b Second operand
 * @param result Output parameter for the result
 * @return true if operation succeeded (no overflow), false if overflow
 */
static inline bool gcomp_safe_mul_size(size_t a, size_t b, size_t * result) {
  if (a == 0 || b == 0) {
    *result = 0;
    return true;
  }
  if (a > SIZE_MAX / b) {
    return false; // Overflow would occur
  }
  *result = a * b;
  return true;
}

/**
 * @brief Safely add two uint64_t values.
 *
 * Computes a + b and stores the result in *result if no overflow occurs.
 *
 * @param a First operand
 * @param b Second operand
 * @param result Output parameter for the result
 * @return true if operation succeeded (no overflow), false if overflow
 */
static inline bool gcomp_safe_add_u64(
    uint64_t a, uint64_t b, uint64_t * result) {
  if (a > UINT64_MAX - b) {
    return false; // Overflow would occur
  }
  *result = a + b;
  return true;
}

/**
 * @brief Safely multiply two uint64_t values.
 *
 * Computes a * b and stores the result in *result if no overflow occurs.
 *
 * @param a First operand
 * @param b Second operand
 * @param result Output parameter for the result
 * @return true if operation succeeded (no overflow), false if overflow
 */
static inline bool gcomp_safe_mul_u64(
    uint64_t a, uint64_t b, uint64_t * result) {
  if (a == 0 || b == 0) {
    *result = 0;
    return true;
  }
  if (a > UINT64_MAX / b) {
    return false; // Overflow would occur
  }
  *result = a * b;
  return true;
}

/**
 * @brief Safely add two uint32_t values.
 *
 * Computes a + b and stores the result in *result if no overflow occurs.
 *
 * @param a First operand
 * @param b Second operand
 * @param result Output parameter for the result
 * @return true if operation succeeded (no overflow), false if overflow
 */
static inline bool gcomp_safe_add_u32(
    uint32_t a, uint32_t b, uint32_t * result) {
  if (a > UINT32_MAX - b) {
    return false; // Overflow would occur
  }
  *result = a + b;
  return true;
}

/**
 * @brief Safely multiply two uint32_t values.
 *
 * Computes a * b and stores the result in *result if no overflow occurs.
 *
 * @param a First operand
 * @param b Second operand
 * @param result Output parameter for the result
 * @return true if operation succeeded (no overflow), false if overflow
 */
static inline bool gcomp_safe_mul_u32(
    uint32_t a, uint32_t b, uint32_t * result) {
  if (a == 0 || b == 0) {
    *result = 0;
    return true;
  }
  if (a > UINT32_MAX / b) {
    return false; // Overflow would occur
  }
  *result = a * b;
  return true;
}

/**
 * @brief Safely add three size_t values.
 *
 * Computes a + b + c and stores the result in *result if no overflow occurs.
 *
 * @param a First operand
 * @param b Second operand
 * @param c Third operand
 * @param result Output parameter for the result
 * @return true if operation succeeded (no overflow), false if overflow
 */
static inline bool gcomp_safe_add3_size(
    size_t a, size_t b, size_t c, size_t * result) {
  size_t temp;
  if (!gcomp_safe_add_size(a, b, &temp)) {
    return false;
  }
  return gcomp_safe_add_size(temp, c, result);
}

/**
 * @brief Compute size with multiplicative overhead, safely.
 *
 * Computes (base * factor) + addend, useful for buffer size calculations
 * like "n items * item_size + header_size".
 *
 * @param base Base count
 * @param factor Multiplier (e.g., item size)
 * @param addend Additional constant (e.g., header size)
 * @param result Output parameter for the result
 * @return true if operation succeeded (no overflow), false if overflow
 */
static inline bool gcomp_safe_size_calc(
    size_t base, size_t factor, size_t addend, size_t * result) {
  size_t temp;
  if (!gcomp_safe_mul_size(base, factor, &temp)) {
    return false;
  }
  return gcomp_safe_add_size(temp, addend, result);
}

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SAFE_MATH_H
