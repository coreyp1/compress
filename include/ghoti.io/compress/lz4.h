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
 * - Skippable frames: skipped on decode wherever they appear, and writable
 *   and parseable through the functions below
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
#include <stddef.h>


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

/**
 * @brief Bytes a skippable frame adds to its payload.
 *
 * The 4-byte magic number and the 4-byte size field. A buffer of
 * `GCOMP_LZ4_SKIPPABLE_OVERHEAD + payload_size` always holds the frame.
 */
#define GCOMP_LZ4_SKIPPABLE_OVERHEAD 8

/**
 * @brief Largest payload a skippable frame can carry.
 *
 * The size field is 32 bits.
 */
#define GCOMP_LZ4_SKIPPABLE_MAX_PAYLOAD 0xFFFFFFFFu

/**
 * @brief Write a skippable frame.
 *
 * A skippable frame carries bytes of the caller's choosing through an LZ4
 * stream: every conforming decoder steps over it, so it is where an
 * application puts its own metadata without disturbing the data. It is a
 * complete frame and may be placed before, between or after data frames, or
 * stand alone.
 *
 * The frame is built directly rather than through an encoder, because none of
 * it is compressed -- it is a magic number, a length, and the payload verbatim.
 *
 * @param magic_variant Which of the 16 skippable magic numbers to use, 0
 *        through 15. The LZ4 frame format leaves the low nibble to the writer
 *        so an application can tell its own kinds of embedded data apart;
 *        decoders skip the frame whatever it says. Called the magic variant
 *        here to match the name the wider LZ4 ecosystem uses for it.
 * @param payload The bytes to embed; may be NULL only when payload_size is 0
 * @param payload_size Size of the payload, at most
 *        GCOMP_LZ4_SKIPPABLE_MAX_PAYLOAD
 * @param output Output buffer (must not be NULL)
 * @param output_capacity Capacity of the output buffer; the frame needs
 *        GCOMP_LZ4_SKIPPABLE_OVERHEAD + payload_size bytes
 * @param output_size_out Set to the number of bytes written, or to 0 on error
 * @return GCOMP_OK on success; GCOMP_ERR_INVALID_ARG if magic_variant exceeds
 *         15, the payload is too large, or a required pointer is NULL;
 *         GCOMP_ERR_LIMIT if the output buffer is too small
 */
GCOMP_API gcomp_status_t gcomp_lz4_write_skippable_frame(
    unsigned magic_variant, const void * payload, size_t payload_size,
    void * output, size_t output_capacity, size_t * output_size_out);

/**
 * @brief Parse a skippable frame without copying its payload.
 *
 * The decoder discards skippable frames, as the format requires, so this is
 * how an application reads back what it embedded: locate the frame in its own
 * buffer, then read the payload in place.
 *
 * Nothing is copied and nothing is allocated; the payload is reported as an
 * offset into @p input, which stays valid as long as that buffer does.
 *
 * @param input Buffer positioned at the start of a skippable frame
 * @param input_size Bytes available from that position
 * @param magic_variant_out Set to the low nibble of the magic number, 0
 *        through 15; may be NULL
 * @param payload_offset_out Set to the payload's offset within @p input,
 *        which is always GCOMP_LZ4_SKIPPABLE_OVERHEAD; may be NULL
 * @param payload_size_out Set to the payload size; may be NULL
 * @param frame_size_out Set to the whole frame's size, so the caller can step
 *        to whatever follows it; may be NULL
 * @return GCOMP_OK on success; GCOMP_ERR_INVALID_ARG if @p input is NULL;
 *         GCOMP_ERR_CORRUPT if the magic number is not a skippable one, or
 *         the frame is cut short of the size it declares
 */
GCOMP_API gcomp_status_t gcomp_lz4_read_skippable_frame(const void * input,
    size_t input_size, unsigned * magic_variant_out,
    size_t * payload_offset_out, size_t * payload_size_out,
    size_t * frame_size_out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_LZ4_H
