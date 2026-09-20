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
 * - Parallel encoding on a thread pool (see below)
 *
 * ## Parallel encoding
 *
 * Set `threads.count` above 1 and blocks are compressed on worker threads.
 * What comes out is **one ordinary LZ4 frame, byte-for-byte identical to
 * what a single thread would have produced** -- the same input and options
 * give the same bytes at any thread count, so nothing downstream can tell
 * the difference or needs to.
 *
 * ```c
 * gcomp_options_set_uint64(options, "threads.count", 4);
 * ```
 *
 * One job is one block, so `lz4.block_size` is also the parallel
 * granularity; an input of only a few blocks has correspondingly little to
 * divide up.
 *
 * Parallel encoding requires independent blocks, which is the default.  With
 * `lz4.independent_blocks=false` a block may reference the one before it, so
 * there is nothing to compress in parallel; `threads.count` is then ignored
 * and the encoder compresses in the calling thread.  This is not an error
 * and does not change the output.
 *
 * Decoding is always single-threaded.
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
 * @brief The largest expansion the LZ4 block format permits, output over input.
 *
 * A sequence costs a one-byte token and a two-byte offset, and extends its
 * match length by one byte of input per 255 bytes of output.  Lengthening the
 * match is therefore worth 255:1 at the margin, and the fixed three bytes keep
 * the whole sequence below that, so 255 is approached from beneath and never
 * reached.
 *
 * Measured: 32 MiB of zeros compresses to 254.8:1.
 */
#define GCOMP_LZ4_MAX_EXPANSION_RATIO 255ULL

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
 * @brief What a frame's header says about it.
 *
 * Filled by gcomp_lz4_peek_frame_info(). Every field is read straight out of
 * the frame descriptor; nothing is inferred and nothing is decoded.
 */
typedef struct {
  /** Non-zero for a skippable frame. The fields below it describe one; the
   *  data-frame fields are all zero. */
  int is_skippable;
  /** Skippable frames: the magic number's low nibble, 0 through 15. */
  unsigned magic_variant;
  /** Skippable frames: the payload size the frame declares. */
  uint64_t skippable_payload_size;

  /** Data frames: blocks do not reference each other (B.Indep). */
  int block_independent;
  /** Data frames: each block carries an xxHash32 (B.Checksum). */
  int block_checksum;
  /** Data frames: the frame ends with an xxHash32 of the content. */
  int content_checksum;
  /** Data frames: largest block the frame may hold -- 65536, 262144, 1048576
   *  or 4194304. */
  uint32_t block_max_size;
  /** Data frames: the header states the uncompressed size. */
  int content_size_present;
  /** Data frames: that size, or 0 when it is absent. */
  uint64_t content_size;
  /** Data frames: the header names a dictionary. */
  int dict_id_present;
  /** Data frames: which one, or 0 when absent.  This is how a reader learns
   *  what to pass as `lz4.dictionary` before decoding. */
  uint32_t dict_id;

  /** Bytes the header occupies, so the caller can step to the first block. */
  size_t frame_header_size;
  /** The whole frame's size, for a skippable frame.  Zero for a data frame:
   *  its length is not knowable from the header, since blocks are only sized
   *  as they are met. */
  size_t frame_size;
} gcomp_lz4_frame_info_t;

/**
 * @brief Read a frame's header without decoding it.
 *
 * Answers the questions a reader has before it commits: how large the content
 * will be, whether the frame is checksummed, and above all **which dictionary
 * it needs**, which is otherwise unknowable -- the decoder needs the
 * dictionary to start, and the only place its identity appears is the header.
 *
 * Nothing is allocated and nothing is decoded; the bytes are parsed in place.
 * The validation is the decoder's own, so a header this accepts is one the
 * decoder accepts and a header this refuses the decoder refuses too.
 *
 * Skippable frames are reported rather than refused: a reader walking a
 * stream needs to know how far to step over one.
 *
 * @param input Buffer positioned at the start of a frame
 * @param input_size Bytes available from that position
 * @param info_out Filled in on success; zeroed first
 * @param needed_out On GCOMP_ERR_LIMIT, set to how many bytes are needed
 *        before asking again; may be NULL
 * @return GCOMP_OK on success; GCOMP_ERR_LIMIT if @p input is too short, with
 *         @p needed_out saying how short; GCOMP_ERR_CORRUPT if the bytes are
 *         not an LZ4 frame header, carry an unknown version, set a reserved
 *         bit, name an invalid block size, or fail the header checksum;
 *         GCOMP_ERR_INVALID_ARG if @p input or @p info_out is NULL
 */
GCOMP_API gcomp_status_t gcomp_lz4_peek_frame_info(const void * input,
    size_t input_size, gcomp_lz4_frame_info_t * info_out, size_t * needed_out);

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
