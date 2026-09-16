/**
 * @file registry_internal.h
 *
 * Internal registry definitions for the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_SRC_CORE_REGISTRY_INTERNAL_H
#define GHOTI_IO_GCOMP_SRC_CORE_REGISTRY_INTERNAL_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/registry.h>

// Internal helper to fetch registry allocator (never NULL).
const gcomp_allocator_t * gcomp_registry_get_allocator(
    const gcomp_registry_t * registry);

#endif // GHOTI_IO_GCOMP_SRC_CORE_REGISTRY_INTERNAL_H
