/**
 * @file compress.h
 *
 * Main header for the Ghoti.io Compress library.
 *
 * Cross-platform C library implementing streaming compression with no
 * external dependencies.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_H
#define GHOTI_IO_GCOMP_H

#include <ghoti.io/compress/macros.h>
#include <stdint.h>

/**
 * @brief Library version information
 */
#define GCOMP_VERSION_MAJOR 0
#define GCOMP_VERSION_MINOR 0
#define GCOMP_VERSION_PATCH 0

/**
 * @brief Get the major version number
 * @return The major version number
 */
GCOMP_API uint32_t gcomp_version_major(void);

/**
 * @brief Get the minor version number
 * @return The minor version number
 */
GCOMP_API uint32_t gcomp_version_minor(void);

/**
 * @brief Get the patch version number
 * @return The patch version number
 */
GCOMP_API uint32_t gcomp_version_patch(void);

/**
 * @brief Get the version string
 * @return A string representation of the version (e.g., "0.0.0")
 */
GCOMP_API const char *gcomp_version_string(void);

#endif /* GHOTI_IO_GCOMP_H */
