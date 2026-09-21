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
 * @file alloc_internal.h
 *
 * Internal allocator helpers for the Ghoti.io Compress library.
 */

#ifndef GHOTI_IO_GCOMP_SRC_CORE_ALLOC_INTERNAL_H
#define GHOTI_IO_GCOMP_SRC_CORE_ALLOC_INTERNAL_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/allocator.h>
#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/cutil/safemath.h>

static inline const gcomp_allocator_t * gcomp_alloc_or_default(
    const gcomp_allocator_t * allocator) {
  return allocator ? allocator : gcomp_allocator_default();
}

static inline void * gcomp_malloc(
    const gcomp_allocator_t * allocator, size_t size) {
  allocator = gcomp_alloc_or_default(allocator);
  return allocator->malloc_fn(allocator->ctx, size);
}

static inline void * gcomp_calloc(
    const gcomp_allocator_t * allocator, size_t nitems, size_t size) {
  allocator = gcomp_alloc_or_default(allocator);
  return allocator->calloc_fn(allocator->ctx, nitems, size);
}

static inline void * gcomp_realloc(
    const gcomp_allocator_t * allocator, void * ptr, size_t size) {
  allocator = gcomp_alloc_or_default(allocator);
  return allocator->realloc_fn(allocator->ctx, ptr, size);
}

static inline void gcomp_free(const gcomp_allocator_t * allocator, void * ptr) {
  allocator = gcomp_alloc_or_default(allocator);
  allocator->free_fn(allocator->ctx, ptr);
}

static inline char * gcomp_strdup(
    const gcomp_allocator_t * allocator, const char * str) {
  if (!str) {
    return NULL;
  }
  size_t len = 0;
  while (str[len] != '\0') {
    len++;
  }
  size_t alloc_size;
  if (!gcu_safe_add_size(len, 1, &alloc_size)) {
    return NULL; // len + 1 would overflow (e.g. len == SIZE_MAX)
  }
  char * out = (char *)gcomp_malloc(allocator, alloc_size);
  if (!out) {
    return NULL;
  }
  for (size_t i = 0; i <= len; i++) {
    out[i] = str[i];
  }
  return out;
}

#endif // GHOTI_IO_GCOMP_SRC_CORE_ALLOC_INTERNAL_H
