/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Compress.
 *
 * Ghoti.io Compress is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Compress is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file strutil_internal.h
 *
 * Internal string utilities for the Ghoti.io Compress library.
 */

#ifndef GHOTI_IO_GCOMP_SRC_CORE_STRUTIL_INTERNAL_H
#define GHOTI_IO_GCOMP_SRC_CORE_STRUTIL_INTERNAL_H

#include <ghoti.io/compress/macros.h>

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

#endif // GHOTI_IO_GCOMP_SRC_CORE_STRUTIL_INTERNAL_H
