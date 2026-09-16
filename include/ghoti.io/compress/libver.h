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

#endif // GHOTI_IO_GCOMP_LIBVER_H
