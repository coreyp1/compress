/**
 * @file zlib_internal.h
 *
 * Internal definitions for the zlib (RFC 1950) container.
 *
 * ## What this is
 *
 * The two-byte wrapper that sits between raw DEFLATE and the world:
 *
 * ```
 *   +---+---+                 +---+---+---+---+
 *   |CMF|FLG|  DEFLATE data   |    ADLER32    |
 *   +---+---+                 +---+---+---+---+
 *      (+ 4-byte DICTID when FDICT is set)
 * ```
 *
 * Six bytes of overhead, and the reason PNG's IDAT stream, HTTP's `deflate`
 * content-coding and countless embedded formats are readable at all.  This
 * library had `deflate` and `gzip` and nothing in between, which is why the
 * PNG codec in the sibling image library carries its own CMF/FLG parser and
 * its own Adler-32 -- when your own downstream reimplements a format, that is
 * the library saying what it is missing.
 *
 * ## Relationship to gzip
 *
 * Structurally the same shape: a header, an inner deflate stream, a checksum
 * trailer.  So this mirrors src/methods/gzip/ closely and shares
 * gcomp_clone_options_for_method() with it.  The differences are that the
 * header is fixed-width rather than a run of optional fields, that the
 * checksum is Adler-32 over the *uncompressed* data rather than CRC-32, and
 * that it is stored most significant byte first -- the one big-endian field in
 * a library where everything else is little-endian, and an easy thing to get
 * backwards.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_ZLIB_ZLIB_INTERNAL_H
#define GHOTI_IO_GCOMP_SRC_METHODS_ZLIB_ZLIB_INTERNAL_H

#include <ghoti.io/compress/macros.h>

#include "../../core/stream_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//
// Format constants (RFC 1950 section 2.2)
//

/// The only compression method RFC 1950 defines: deflate.
#define ZLIB_CM_DEFLATE 8u

/// CMF: low nibble is CM, high nibble is CINFO.
#define ZLIB_CMF_CM_MASK 0x0Fu
#define ZLIB_CMF_CINFO_SHIFT 4u

/// FLG: low five bits are FCHECK, bit 5 is FDICT, top two are FLEVEL.
#define ZLIB_FLG_FCHECK_MASK 0x1Fu
#define ZLIB_FLG_FDICT 0x20u
#define ZLIB_FLG_FLEVEL_SHIFT 6u

/**
 * @brief The header's two bytes, read big-endian, must be a multiple of this.
 *
 * RFC 1950 section 2.2: "CMF and FLG, when viewed as a 16-bit unsigned integer
 * stored in MSB order (CMF*256 + FLG), is a multiple of 31."  FCHECK exists
 * only to make that true, which is what makes a mangled header cheap to spot.
 */
#define ZLIB_HEADER_MODULUS 31u

/// CINFO is log2(window) - 8 and RFC 1950 caps it at 7, a 32 KiB window.
#define ZLIB_CINFO_MAX 7u

/// Smallest and largest window a CINFO can name, as deflate counts them.
#define ZLIB_WINDOW_BITS_MIN 8u
#define ZLIB_WINDOW_BITS_MAX 15u

#define ZLIB_HEADER_SIZE 2u
#define ZLIB_DICTID_SIZE 4u
#define ZLIB_TRAILER_SIZE 4u

/// Two header bytes and a four-byte trailer; the deflate data may be empty.
#define ZLIB_MIN_STREAM_BYTES (ZLIB_HEADER_SIZE + ZLIB_TRAILER_SIZE)

//
// Defaults
//

#define ZLIB_DEFAULT_MAX_OUTPUT_BYTES (512ULL * 1024 * 1024)
#define ZLIB_DEFAULT_MAX_MEMORY_BYTES (256ULL * 1024 * 1024)
#define ZLIB_DEFAULT_MAX_EXPANSION_RATIO 1000ULL

//
// Parsed header
//

typedef struct {
  uint8_t cmf;
  uint8_t flg;
  uint8_t cm;          ///< Compression method; 8 is the only legal value.
  uint8_t cinfo;       ///< log2(window) - 8.
  uint8_t flevel;      ///< Compression-level hint; informational only.
  bool fdict;          ///< A preset dictionary is required to decode.
  uint32_t dict_id;    ///< Adler-32 of that dictionary, when fdict.
  uint8_t window_bits; ///< cinfo + 8, for convenience.
} zlib_header_t;

//
// Stages
//

typedef enum {
  ZLIB_ENC_STAGE_HEADER = 0, ///< Writing CMF/FLG.
  ZLIB_ENC_STAGE_BODY,       ///< Streaming through the deflate encoder.
  ZLIB_ENC_STAGE_TRAILER,    ///< Writing the Adler-32.
  ZLIB_ENC_STAGE_DONE,
} zlib_encoder_stage_t;

typedef enum {
  ZLIB_DEC_STAGE_HEADER = 0, ///< Accumulating CMF/FLG (and DICTID).
  ZLIB_DEC_STAGE_BODY,       ///< Streaming through the deflate decoder.
  ZLIB_DEC_STAGE_TRAILER,    ///< Accumulating and checking the Adler-32.
  ZLIB_DEC_STAGE_DONE,
  ZLIB_DEC_STAGE_ERROR,
} zlib_decoder_stage_t;

//
// Encoder state
//

typedef struct {
  const gcomp_allocator_t * allocator;
  gcomp_encoder_t * inner_encoder; ///< Owned deflate encoder.

  uint32_t adler; ///< Running Adler-32 of the uncompressed input.

  zlib_encoder_stage_t stage;

  uint8_t header_buf[ZLIB_HEADER_SIZE];
  size_t header_len;
  size_t header_pos;

  uint8_t trailer_buf[ZLIB_TRAILER_SIZE];
  size_t trailer_pos;

  uint8_t window_bits; ///< What went into CINFO.
  int level;           ///< What went into FLEVEL.

  gcomp_memory_tracker_t mem_tracker;
  uint64_t max_memory_bytes;
} zlib_encoder_state_t;

//
// Decoder state
//

typedef struct {
  const gcomp_allocator_t * allocator;
  gcomp_decoder_t * inner_decoder; ///< Owned deflate decoder.

  uint32_t adler; ///< Running Adler-32 of the decompressed output.

  zlib_decoder_stage_t stage;

  /// Header bytes seen so far; up to two, or six when FDICT is set.
  uint8_t header_accum[ZLIB_HEADER_SIZE + ZLIB_DICTID_SIZE];
  size_t header_accum_pos;
  size_t header_need; ///< How many bytes this header turns out to need.

  zlib_header_t header;

  uint8_t trailer_buf[ZLIB_TRAILER_SIZE];
  size_t trailer_pos;

  uint64_t max_output_bytes;
  uint64_t max_expansion_ratio;
  uint64_t total_input_bytes;
  uint64_t total_output_bytes;

  gcomp_memory_tracker_t mem_tracker;
  uint64_t max_memory_bytes;
} zlib_decoder_state_t;

//
// Format helpers (zlib_format.c)
//

/**
 * @brief Build the two header bytes for a given window and level.
 *
 * @param window_bits Window the deflate encoder will use, 8 to 15.
 * @param level Compression level 0 to 9, for the FLEVEL hint.
 * @param out Receives ZLIB_HEADER_SIZE bytes.
 * @return GCOMP_OK, or GCOMP_ERR_INVALID_ARG for a window that has no CINFO.
 */
gcomp_status_t zlib_write_header(
    unsigned window_bits, int level, uint8_t * out);

/**
 * @brief Parse and validate the two header bytes.
 *
 * Checks CM, CINFO and the modulus, and reports FDICT so the caller can ask
 * for the four DICTID bytes that follow it.
 *
 * @param data At least ZLIB_HEADER_SIZE bytes.
 * @param out Receives the parsed header; dict_id is left zero.
 * @return GCOMP_OK, or GCOMP_ERR_CORRUPT with nothing written on failure.
 */
gcomp_status_t zlib_parse_header(const uint8_t * data, zlib_header_t * out);

/// FLEVEL for a compression level, as zlib itself assigns it.
uint8_t zlib_flevel_for_level(int level);

//
// Encoder / decoder (zlib_encoder.c, zlib_decoder.c)
//

gcomp_status_t zlib_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder);
void zlib_encoder_destroy(gcomp_encoder_t * encoder);
gcomp_status_t zlib_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output);
gcomp_status_t zlib_encoder_flush(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output, gcomp_flush_t mode);
gcomp_status_t zlib_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output);
gcomp_status_t zlib_encoder_reset(gcomp_encoder_t * encoder);

gcomp_status_t zlib_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder);
void zlib_decoder_destroy(gcomp_decoder_t * decoder);
gcomp_status_t zlib_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output);
gcomp_status_t zlib_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output);
gcomp_status_t zlib_decoder_reset(gcomp_decoder_t * decoder);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_METHODS_ZLIB_ZLIB_INTERNAL_H
