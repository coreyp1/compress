/**
 * @file lz4.h
 *
 * LZ4 Frame Format compression for the Ghoti.io Compress library.
 *
 * This module provides LZ4 Frame Format (not raw block) compression and
 * decompression. The LZ4 frame format is specified at:
 * https://github.com/lz4/lz4/blob/dev/doc/lz4_Frame_format.md
 *
 * ## Features
 *
 * - Full LZ4 frame format support (header + blocks + trailer)
 * - Block checksum support (xxHash32)
 * - Content checksum support (xxHash32)
 * - Content size field support
 * - Independent and dependent block modes
 * - Concatenated frame support
 * - Streaming with arbitrary input/output buffer sizes
 * - Memory tracking and limits
 *
 * ## Usage
 *
 * The LZ4 method is registered with name "lz4" and can be used through the
 * standard compress API:
 *
 * ```c
 * // Encode
 * gcomp_encoder_t *encoder = NULL;
 * gcomp_encoder_create(registry, "lz4", options, &encoder);
 * gcomp_encoder_update(encoder, &input, &output);
 * gcomp_encoder_finish(encoder, &output);
 * gcomp_encoder_destroy(encoder);
 *
 * // Decode
 * gcomp_decoder_t *decoder = NULL;
 * gcomp_decoder_create(registry, "lz4", options, &decoder);
 * gcomp_decoder_update(decoder, &input, &output);
 * gcomp_decoder_finish(decoder, &output);
 * gcomp_decoder_destroy(decoder);
 * ```
 *
 * Or using the buffer convenience functions:
 *
 * ```c
 * gcomp_encode_buffer(NULL, "lz4", NULL, input, in_len, output, out_cap,
 * &out_len); gcomp_decode_buffer(NULL, "lz4", NULL, input, in_len, output,
 * out_cap, &out_len);
 * ```
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_LZ4_H
#define GHOTI_IO_GCOMP_LZ4_H

#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <ghoti.io/compress/registry.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the LZ4 method with a registry.
 *
 * This function registers the "lz4" compression method with the given
 * registry. The method provides LZ4 frame format compression and
 * decompression.
 *
 * @param registry The registry to register with (must not be NULL)
 * @return GCOMP_OK on success, error code on failure
 *
 * @note This function is called automatically during library initialization
 *       for the default registry via the auto-registration mechanism.
 *       Manual registration is only needed for custom registries.
 */
GCOMP_API gcomp_status_t gcomp_method_lz4_register(gcomp_registry_t * registry);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_LZ4_H
