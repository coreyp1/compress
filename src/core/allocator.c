/**
 * @file allocator.c
 *
 * Default allocator implementation for the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/allocator.h>
#include <stdlib.h>

static void * gcomp_stdlib_malloc(void * ctx, size_t size) {
  (void)ctx;
  return malloc(size);
}

static void * gcomp_stdlib_calloc(void * ctx, size_t nitems, size_t size) {
  (void)ctx;
  return calloc(nitems, size);
}

static void * gcomp_stdlib_realloc(void * ctx, void * ptr, size_t size) {
  (void)ctx;
  return realloc(ptr, size);
}

static void gcomp_stdlib_free(void * ctx, void * ptr) {
  (void)ctx;
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
