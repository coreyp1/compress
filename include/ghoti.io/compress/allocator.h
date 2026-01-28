/**
 * @file allocator.h
 *
 * Allocator abstraction for the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_ALLOCATOR_H
#define GHOTI_IO_GCOMP_ALLOCATOR_H

#include <ghoti.io/compress/macros.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocator interface used by the library.
 *
 * All function pointers must be non-NULL.
 */
typedef struct gcomp_allocator_s {
  void * ctx;
  void * (*malloc_fn)(void * ctx, size_t size);
  void * (*calloc_fn)(void * ctx, size_t nitems, size_t size);
  void * (*realloc_fn)(void * ctx, void * ptr, size_t size);
  void (*free_fn)(void * ctx, void * ptr);
} gcomp_allocator_t;

/**
 * @brief Get the default allocator (stdlib-backed).
 *
 * @return Pointer to a process-global allocator instance.
 */
GCOMP_API const gcomp_allocator_t * gcomp_allocator_default(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_ALLOCATOR_H
