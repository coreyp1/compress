/**
 * @file
 *
 * Main implementation for the Ghoti.io Compress library.
 *
 * This file contains version information accessors and any shared
 * utilities used across the library modules.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <stdio.h>
#include <stdlib.h>

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
