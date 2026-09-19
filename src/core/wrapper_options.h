/**
 * @file wrapper_options.h
 *
 * Cloning a caller's options for the method a wrapper wraps.
 *
 * gzip and zlib are both a header, a deflate stream and a checksum trailer.
 * Each creates an inner deflate encoder and decoder, and each has to decide
 * what of the caller's options that inner method should see.
 *
 * The answer is not "all of them".  Deflate's schema policy is
 * GCOMP_UNKNOWN_KEY_ERROR, so handing it a `gzip.name` or a `zlib.dictionary`
 * fails the call outright.  Nor is it a hand-written list: that is a second
 * copy of deflate's schema, kept up to date by hope.
 *
 * So the schema itself is the authority.  This walks the inner method's
 * declared options and copies across whichever of them the caller actually
 * set, which cannot drift as that schema changes.
 *
 * Internal only -- not part of the public API.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_SRC_CORE_WRAPPER_OPTIONS_H
#define GHOTI_IO_GCOMP_SRC_CORE_WRAPPER_OPTIONS_H

#include <ghoti.io/compress/macros.h>

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Copy the options @p inner_method declares, from @p src into a new set.
 *
 * @param registry Registry to look @p inner_method up in.
 * @param inner_method Name of the method being wrapped, e.g. "deflate".
 * @param src Caller's options; NULL yields NULL.
 * @param dst_out Receives the new options, or NULL when @p src was NULL.
 *        The caller owns them and must destroy them.
 * @return GCOMP_OK on success, GCOMP_ERR_UNSUPPORTED when @p inner_method is
 *         not registered, or an error from the options or schema layer.
 */
gcomp_status_t gcomp_clone_options_for_method(gcomp_registry_t * registry,
    const char * inner_method, const gcomp_options_t * src,
    gcomp_options_t ** dst_out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_CORE_WRAPPER_OPTIONS_H
