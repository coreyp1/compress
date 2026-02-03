/**
 * @file allocator.c
 *
 * Default allocator implementation for the Ghoti.io Compress library.
 *
 * The default allocator treats overflow in calloc(nitems, size) as allocation
 * failure: if nitems * size would overflow size_t, gcomp_stdlib_calloc returns
 * NULL rather than calling calloc with undefined behavior.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "safe_math.h"
#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/macros.h>
#include <stdlib.h>

static void * gcomp_stdlib_malloc(GCOMP_MAYBE_UNUSED(void * ctx), size_t size) {
  return malloc(size);
}

static void * gcomp_stdlib_calloc(
    GCOMP_MAYBE_UNUSED(void * ctx), size_t nitems, size_t size) {
  size_t total;
  if (!gcomp_safe_mul_size(nitems, size, &total)) {
    return NULL; // overflow: treat as allocation failure
  }
  return calloc(1, total);
}

static void * gcomp_stdlib_realloc(
    GCOMP_MAYBE_UNUSED(void * ctx), void * ptr, size_t size) {
  return realloc(ptr, size);
}

static void gcomp_stdlib_free(GCOMP_MAYBE_UNUSED(void * ctx), void * ptr) {
  free(ptr);
}

const gcomp_allocator_t * gcomp_allocator_default(void) {
  static const gcomp_allocator_t allocator = {
      .ctx = NULL,
      .malloc_fn = gcomp_stdlib_malloc,
      .calloc_fn = gcomp_stdlib_calloc,
      .realloc_fn = gcomp_stdlib_realloc,
      .free_fn = gcomp_stdlib_free,
  };
  return &allocator;
}
