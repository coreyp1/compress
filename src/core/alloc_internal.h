/**
 * @file alloc_internal.h
 *
 * Internal allocator helpers for the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GCOMP_ALLOC_INTERNAL_H
#define GCOMP_ALLOC_INTERNAL_H

#include <ghoti.io/compress/allocator.h>
#include <stddef.h>
#include <stdint.h>

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
  char * out = gcomp_malloc(allocator, len + 1);
  if (!out) {
    return NULL;
  }
  for (size_t i = 0; i <= len; i++) {
    out[i] = str[i];
  }
  return out;
}

#endif // GCOMP_ALLOC_INTERNAL_H
