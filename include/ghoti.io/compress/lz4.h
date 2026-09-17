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
#include <ghoti.io/compress/stream.h>
#include <stddef.h>
#include <stdint.h>


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

/**
 * @brief Receives a skippable frame's payload while a stream is decoded.
 *
 * The decoder discards skippable frames, as the LZ4 frame format requires, so
 * without this a payload embedded in a stream is only recoverable by parsing
 * the bytes yourself -- which a caller decoding a pipe or a socket cannot do.
 *
 * ## Delivery
 *
 * The payload arrives in pieces, in order, exactly covering
 * `[0, payload_size)`. It is **not** buffered: a skippable frame may declare
 * up to 4 GB, and the size comes from the stream, so holding one whole would
 * let the input choose an allocation. A caller that wants the whole payload
 * decides its own ceiling, checks @p payload_size against it on the first
 * piece, and reassembles; a caller writing it elsewhere appends each piece.
 *
 * The callback runs at least once per skippable frame, so a frame with no
 * payload is still reported -- once, with @p chunk_size 0 -- because its
 * variant may be the whole message.
 *
 * ## Rules
 *
 * @p chunk points into the buffer you handed to gcomp_decoder_update(), and
 * is valid only for the duration of the call. Copy anything you need to keep.
 *
 * Returning anything but GCOMP_OK stops the decode, and that status is what
 * gcomp_decoder_update() returns. The decoder is left in its error state and
 * will not decode further; reset it to reuse it.
 *
 * The callback is invoked synchronously, on the thread calling
 * gcomp_decoder_update(). It must not call back into the same decoder.
 *
 * @param ctx The context pointer registered alongside the callback
 * @param magic_variant The magic number's low nibble, 0 through 15
 * @param payload_size The whole payload's size, known from the first call
 * @param payload_offset Where this piece starts within the payload
 * @param chunk The payload bytes; NULL is possible only when chunk_size is 0
 * @param chunk_size How many bytes @p chunk holds
 * @return GCOMP_OK to continue decoding, or any error to stop
 */
typedef gcomp_status_t (*gcomp_lz4_skippable_cb)(void * ctx,
    unsigned magic_variant, uint64_t payload_size, uint64_t payload_offset,
    const uint8_t * chunk, size_t chunk_size);

/**
 * @brief Report skippable frames to a callback as they are decoded.
 *
 * Registers @p callback on an LZ4 decoder created with
 * gcomp_decoder_create(). Pass NULL to stop reporting them; the frames are
 * still skipped either way, so removing the callback changes what you are
 * told, never what the stream decodes to.
 *
 * May be called at any point, including between updates. It survives
 * gcomp_decoder_reset(), because it is a property of how you are using the
 * decoder rather than of the stream it is reading.
 *
 * The convenience wrappers -- gcomp_decode_buffer(), gcomp_decode_stream_cb()
 * -- create and destroy a decoder internally, so there is none to register on.
 * Use gcomp_decoder_create() directly when you need this.
 *
 * @param decoder An LZ4 decoder (must not be NULL)
 * @param callback The callback, or NULL to stop reporting
 * @param ctx Passed to the callback unchanged; may be NULL
 * @return GCOMP_OK on success; GCOMP_ERR_INVALID_ARG if @p decoder is NULL or
 *         is not an LZ4 decoder
 */
GCOMP_API gcomp_status_t gcomp_lz4_decoder_on_skippable_frame(
    gcomp_decoder_t * decoder, gcomp_lz4_skippable_cb callback, void * ctx);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_LZ4_H
