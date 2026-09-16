/**
 * @file allocator.c
 *
 * The library's default allocator, which is cutil's.
 *
 * The stdlib-backed implementation this file used to carry was identical to
 * cutil's, down to treating calloc overflow as an allocation failure, so it
 * forwards rather than repeating it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/allocator.h>

const gcomp_allocator_t * gcomp_allocator_default(void) {
  return gcu_allocator_default();
}
