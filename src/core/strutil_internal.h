/**
 * @file strutil_internal.h
 *
 * Internal string utilities for the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GCOMP_STRUTIL_INTERNAL_H
#define GCOMP_STRUTIL_INTERNAL_H

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Copy a C string into a fixed-size buffer with safe truncation.
 *
 * - If dst_size is 0, returns without writing.
 * - If src is NULL, writes a single '\0' to dst (if dst_size > 0).
 * - Otherwise copies up to dst_size - 1 characters and always NUL-terminates.
 *
 * @param dst      Destination buffer.
 * @param dst_size Size of dst (capacity including NUL).
 * @param src      Source string (may be NULL).
 */
static inline void gcomp_copy_cstr(
    char * dst, size_t dst_size, const char * src) {
  if (!dst || dst_size == 0) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t len = strlen(src);
  if (len >= dst_size) {
    len = dst_size - 1;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

#ifdef __cplusplus
}
#endif

#endif // GCOMP_STRUTIL_INTERNAL_H
