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
 * @file allocator.c
 *
 * The library's default allocator, which is cutil's.
 *
 * The stdlib-backed implementation this file used to carry was identical to
 * cutil's, down to treating calloc overflow as an allocation failure, so it
 * forwards rather than repeating it.
 */

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/allocator.h>

const gcomp_allocator_t * gcomp_allocator_default(void) {
  return gcu_allocator_default();
}
