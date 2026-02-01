/**
 * @file allocator.c
 *
 * Default allocator implementation for the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/macros.h>
#include <stdlib.h>

static void * gcomp_stdlib_malloc(GCOMP_MAYBE_UNUSED(void * ctx), size_t size) {
  return malloc(size);
}

static void * gcomp_stdlib_calloc(
    GCOMP_MAYBE_UNUSED(void * ctx), size_t nitems, size_t size) {
  return calloc(nitems, size);
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
