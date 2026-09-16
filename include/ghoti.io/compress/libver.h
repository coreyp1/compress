/**
 * @file libver.h
 *
 * Version numbering and the symbol namespace for the Ghoti.io Compress
 * library.
 *
 * Every exported symbol carries a per-version token so that two versions of
 * this library can be loaded into one process without the dynamic linker
 * binding one caller to the other version's implementation.  The programmer
 * writes `gcomp_crc32()`; the linker sees
 * `ghotiio_compress_dev_gcomp_crc32`.
 *
 * See CONVENTIONS.md section 4.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_LIBVER_H
#define GHOTI_IO_GCOMP_LIBVER_H

/**
 * GHOTIIO_COMPRESS_NAME and GHOTIIO_COMPRESS_VERSION come from here.  They are
 * generated at build time from the Makefile's BRANCH, so that the token inside
 * every exported symbol is the same one that names the .pc file, the install
 * directory and the shared library.
 */
#include <ghoti.io/compress/libver_gen.h>


/**
 * Produce the namespaced form of an identifier.
 *
 * @param NAME The identifier to prefix with GHOTIIO_COMPRESS_NAME.
 */
#define GHOTIIO_COMPRESS(NAME) \
  GHOTIIO_COMPRESS_RENAME(GHOTIIO_COMPRESS_NAME, _##NAME)

/** Helper.  Concatenation needs two levels of expansion. */
#define GHOTIIO_COMPRESS_RENAME_INNER(a, b) a##b

/** Helper.  Concatenation needs two levels of expansion. */
#define GHOTIIO_COMPRESS_RENAME(a, b) GHOTIIO_COMPRESS_RENAME_INNER(a, b)


//-----------------------------------------------------------------------------
// Version
//-----------------------------------------------------------------------------
//
// The numbers come from libver_gen.h, which the Makefile writes from
// MAJOR_VERSION and MINOR_VERSION. Writing them out here instead is correct
// only until someone bumps the Makefile, at which point the soname, the .pc
// Version: and the install directory all move and these do not.

/** This build's major version. */
#define GCOMP_VERSION_MAJOR GHOTIIO_COMPRESS_VERSION_MAJOR
/** This build's minor version. */
#define GCOMP_VERSION_MINOR GHOTIIO_COMPRESS_VERSION_MINOR
/** This build's patch version. */
#define GCOMP_VERSION_PATCH GHOTIIO_COMPRESS_VERSION_PATCH
/** This build's version as a string, e.g. "1.2.3" or "1.2.3-dev". */
#define GCOMP_VERSION_STRING GHOTIIO_COMPRESS_VERSION

/**
 * Pack a version into one comparable integer, one byte per component.
 *
 * This is libcurl's LIBCURL_VERSION_NUM layout, which is the common spelling
 * across C libraries: 1.2.3 becomes 0x010203, and a plain `<` compares two
 * versions correctly. Every library in the suite uses it, so a consumer
 * checking one checks them all the same way.
 */
#define GCOMP_MAKE_VERSION(major, minor, patch)                                  \
  ((((unsigned)(major)) << 16) | (((unsigned)(minor)) << 8) |                  \
      ((unsigned)(patch)))

/** This build's version, packed. Compare against GCOMP_MAKE_VERSION(1, 2, 3). */
#define GCOMP_VERSION_NUMBER                                                     \
  GCOMP_MAKE_VERSION(GCOMP_VERSION_MAJOR, GCOMP_VERSION_MINOR, GCOMP_VERSION_PATCH)

#endif // GHOTI_IO_GCOMP_LIBVER_H
