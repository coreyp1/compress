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
 * @file
 *
 * Main implementation for the Ghoti.io Compress library.
 *
 * This file contains version information accessors and any shared
 * utilities used across the library modules.
 */

#include <stdio.h>
#include <stdlib.h>

#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/compress.h>

GCOMP_API uint32_t gcomp_version_major(void) {
  return GCOMP_VERSION_MAJOR;
}

GCOMP_API uint32_t gcomp_version_minor(void) {
  return GCOMP_VERSION_MINOR;
}

GCOMP_API uint32_t gcomp_version_patch(void) {
  return GCOMP_VERSION_PATCH;
}

GCOMP_API const char *gcomp_version_string(void) {
  static char version_string[32];
  static int initialized = 0;

  if (!initialized) {
    snprintf(version_string, sizeof(version_string), "%u.%u.%u",
        GCOMP_VERSION_MAJOR, GCOMP_VERSION_MINOR,
        GCOMP_VERSION_PATCH);
    initialized = 1;
  }

  return version_string;
}
