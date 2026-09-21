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
 * @file registry_internal.h
 *
 * Internal registry definitions for the Ghoti.io Compress library.
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
