/**
 * @file registry_internal.h
 *
 * Internal registry definitions for the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GCOMP_REGISTRY_INTERNAL_H
#define GCOMP_REGISTRY_INTERNAL_H

#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/registry.h>

// Internal helper to fetch registry allocator (never NULL).
const gcomp_allocator_t * gcomp_registry_get_allocator(
    const gcomp_registry_t * registry);

#endif // GCOMP_REGISTRY_INTERNAL_H
