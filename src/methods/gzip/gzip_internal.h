/**
 * @file gzip_internal.h
 *
 * Internal declarations for the gzip (RFC 1952) method implementation.
 *
 * This header is intended for use only by the gzip method sources. It
 * exposes internal helpers used by the method registration vtable and
 * defines structures shared between encoder and decoder.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_GZIP_INTERNAL_H
#define GHOTI_IO_GCOMP_GZIP_INTERNAL_H

#include "../../core/stream_internal.h"
#include <ghoti.io/compress/crc32.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//
// RFC 1952 Constants
//

#define GZIP_ID1 0x1F               ///< First magic byte
#define GZIP_ID2 0x8B               ///< Second magic byte
#define GZIP_CM_DEFLATE 8           ///< Compression method: deflate
#define GZIP_HEADER_MIN_SIZE 10     ///< Minimum gzip header size
#define GZIP_TRAILER_SIZE 8         ///< CRC32 (4) + ISIZE (4)
#define GZIP_OS_UNKNOWN 255         ///< Operating system: unknown
#define GZIP_MAX_HEADER_BUFFER 1024 ///< Buffer size for header generation

// Default limits for header field sizes
#define GZIP_MAX_NAME_BYTES_DEFAULT (1024 * 1024)    ///< 1 MiB
#define GZIP_MAX_COMMENT_BYTES_DEFAULT (1024 * 1024) ///< 1 MiB
#define GZIP_MAX_EXTRA_BYTES_DEFAULT (64 * 1024)     ///< 64 KiB

// FLG bit masks
#define GZIP_FLG_FTEXT 0x01    ///< Text file hint (not used)
#define GZIP_FLG_FHCRC 0x02    ///< Header CRC present
#define GZIP_FLG_FEXTRA 0x04   ///< Extra field present
#define GZIP_FLG_FNAME 0x08    ///< Original filename present
#define GZIP_FLG_FCOMMENT 0x10 ///< Comment present
#define GZIP_FLG_RESERVED 0xE0 ///< Reserved bits (must be zero)

//
// Little-Endian I/O Helpers
//
// Gzip uses little-endian byte order for multi-byte integers (RFC 1952).
// These inline helpers provide consistent, readable access patterns.
//

/**
 * @brief Read a 16-bit little-endian value from a byte buffer.
 */
static inline uint16_t gzip_read_le16(const uint8_t * buf) {
  return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

/**
 * @brief Read a 32-bit little-endian value from a byte buffer.
 */
static inline uint32_t gzip_read_le32(const uint8_t * buf) {
  return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) |
      ((uint32_t)buf[3] << 24);
}

/**
 * @brief Write a 16-bit value to a buffer in little-endian order.
 */
static inline void gzip_write_le16(uint8_t * buf, uint16_t val) {
  buf[0] = (uint8_t)(val);
  buf[1] = (uint8_t)(val >> 8);
}

/**
 * @brief Write a 32-bit value to a buffer in little-endian order.
 */
static inline void gzip_write_le32(uint8_t * buf, uint32_t val) {
  buf[0] = (uint8_t)(val);
  buf[1] = (uint8_t)(val >> 8);
  buf[2] = (uint8_t)(val >> 16);
  buf[3] = (uint8_t)(val >> 24);
}

//
// Encoder State Machine
//

typedef enum {
  GZIP_ENC_STAGE_HEADER = 0, ///< Writing gzip header
  GZIP_ENC_STAGE_BODY,       ///< Streaming through deflate encoder
  GZIP_ENC_STAGE_TRAILER,    ///< Writing trailer (CRC32 + ISIZE)
  GZIP_ENC_STAGE_DONE,       ///< Stream complete
} gzip_encoder_stage_t;

//
// Decoder State Machine
//

typedef enum {
  GZIP_DEC_STAGE_HEADER = 0, ///< Parsing gzip header
  GZIP_DEC_STAGE_BODY,       ///< Streaming through deflate decoder
  GZIP_DEC_STAGE_TRAILER,    ///< Parsing/validating trailer
  GZIP_DEC_STAGE_DONE,       ///< Member complete
  GZIP_DEC_STAGE_ERROR,      ///< Unrecoverable error
} gzip_decoder_stage_t;

//
// Header Parser State Machine (for streaming header parse)
//

typedef enum {
  GZIP_HEADER_MAGIC = 0,   ///< Reading ID1, ID2
  GZIP_HEADER_CM_FLG,      ///< Reading CM, FLG
  GZIP_HEADER_MTIME,       ///< Reading 4-byte MTIME
  GZIP_HEADER_XFL_OS,      ///< Reading XFL, OS
  GZIP_HEADER_FEXTRA_LEN,  ///< Reading 2-byte FEXTRA length
  GZIP_HEADER_FEXTRA_DATA, ///< Reading FEXTRA bytes
  GZIP_HEADER_FNAME,       ///< Reading null-terminated name
  GZIP_HEADER_FCOMMENT,    ///< Reading null-terminated comment
  GZIP_HEADER_FHCRC,       ///< Reading 2-byte header CRC
  GZIP_HEADER_DONE,        ///< Header complete
} gzip_header_parse_stage_t;

//
// Header Info Structure
//
// Holds parsed header information (decoder) or header to write (encoder).
//

typedef struct {
  uint32_t mtime; ///< Modification time (Unix timestamp)
  uint8_t xfl;    ///< Extra flags
  uint8_t os;     ///< Operating system
  uint8_t flg;    ///< Flags byte

  // Optional fields (dynamically allocated when present)
  uint8_t * extra;     ///< FEXTRA data (NULL if not present)
  size_t extra_len;    ///< FEXTRA length
  char * name;         ///< FNAME (NULL if not present, null-terminated)
  char * comment;      ///< FCOMMENT (NULL if not present, null-terminated)
  uint16_t header_crc; ///< FHCRC value (valid if FHCRC flag set)
} gzip_header_info_t;

//
// Encoder State Structure
//

typedef struct {
  // Inner deflate encoder (owned)
  gcomp_encoder_t * inner_encoder;

  // Running checksums
  uint32_t crc32; ///< Running CRC32 of uncompressed input
  uint32_t isize; ///< Running size counter (mod 2^32)

  // Stage tracking
  gzip_encoder_stage_t stage;

  // Header buffer and position
  uint8_t header_buf[GZIP_MAX_HEADER_BUFFER];
  size_t header_len; ///< Total header length
  size_t header_pos; ///< Bytes written so far

  // Trailer buffer and position
  uint8_t trailer_buf[GZIP_TRAILER_SIZE];
  size_t trailer_pos; ///< Bytes written so far

  // Header info (configuration from options)
  gzip_header_info_t header_info;

  // Memory tracking
  gcomp_memory_tracker_t mem_tracker; ///< Tracks wrapper memory usage
  uint64_t max_memory_bytes;          ///< Memory limit (0 = unlimited)
} gzip_encoder_state_t;

//
// Decoder State Structure
//

typedef struct {
  // Inner deflate decoder (owned)
  gcomp_decoder_t * inner_decoder;

  // Running checksums
  uint32_t crc32; ///< Running CRC32 of decompressed output
  uint32_t isize; ///< Running size counter (mod 2^32)

  // Stage tracking
  gzip_decoder_stage_t stage;

  // Header parsing state
  gzip_header_parse_stage_t header_stage;
  uint8_t
      header_accum[GZIP_MAX_HEADER_BUFFER]; ///< Accumulator for partial reads
  size_t header_accum_pos;
  size_t header_field_target; ///< Target size for current field
  uint32_t header_crc_accum;  ///< Running CRC for FHCRC validation

  // Parsed header info
  gzip_header_info_t header_info;

  // Trailer accumulator
  uint8_t trailer_buf[GZIP_TRAILER_SIZE];
  size_t trailer_pos;

  // Options
  int concat_enabled; ///< Support concatenated members
  uint64_t max_name_bytes;
  uint64_t max_comment_bytes;
  uint64_t max_extra_bytes;

  // Limit configuration
  uint64_t max_output_bytes;    ///< Maximum total output bytes (0 = unlimited)
  uint64_t max_expansion_ratio; ///< Maximum output/input ratio (0 = unlimited)

  // Limit tracking
  uint64_t total_input_bytes;
  uint64_t total_output_bytes;

  // Memory tracking
  gcomp_memory_tracker_t mem_tracker; ///< Tracks wrapper memory usage
  uint64_t max_memory_bytes;          ///< Memory limit (0 = unlimited)
} gzip_decoder_state_t;

//
// Internal API: Encoder
//

/**
 * @brief Create and attach gzip encoder state to an encoder.
 *
 * On success, sets @c encoder->method_state and function pointers.
 */
gcomp_status_t gzip_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder);

/**
 * @brief Destroy and detach gzip encoder state.
 */
void gzip_encoder_destroy(gcomp_encoder_t * encoder);

/**
 * @brief Gzip encoder update implementation.
 */
gcomp_status_t gzip_encoder_update(
    gcomp_encoder_t * encoder, gcomp_buffer_t * input, gcomp_buffer_t * output);

/**
 * @brief Gzip encoder finish implementation.
 */
gcomp_status_t gzip_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output);

/**
 * @brief Reset gzip encoder to initial state.
 */
gcomp_status_t gzip_encoder_reset(gcomp_encoder_t * encoder);

//
// Internal API: Decoder
//

/**
 * @brief Create and attach gzip decoder state to a decoder.
 *
 * On success, sets @c decoder->method_state and function pointers.
 */
gcomp_status_t gzip_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder);

/**
 * @brief Destroy and detach gzip decoder state.
 */
void gzip_decoder_destroy(gcomp_decoder_t * decoder);

/**
 * @brief Gzip decoder update implementation.
 */
gcomp_status_t gzip_decoder_update(
    gcomp_decoder_t * decoder, gcomp_buffer_t * input, gcomp_buffer_t * output);

/**
 * @brief Gzip decoder finish implementation.
 */
gcomp_status_t gzip_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output);

/**
 * @brief Reset gzip decoder to initial state.
 */
gcomp_status_t gzip_decoder_reset(gcomp_decoder_t * decoder);

//
// Internal API: Format Helpers
//

/**
 * @brief Build gzip header into buffer.
 *
 * @param info Header info structure with values to encode
 * @param buf Output buffer
 * @param buf_size Buffer size
 * @param header_len_out Output: actual header length written
 * @return GCOMP_OK on success
 */
gcomp_status_t gzip_write_header(const gzip_header_info_t * info, uint8_t * buf,
    size_t buf_size, size_t * header_len_out);

/**
 * @brief Build gzip trailer (CRC32 + ISIZE).
 *
 * @param crc32 Finalized CRC32 value
 * @param isize Uncompressed size mod 2^32
 * @param buf Output buffer (must be at least GZIP_TRAILER_SIZE bytes)
 */
void gzip_write_trailer(uint32_t crc32, uint32_t isize, uint8_t * buf);

/**
 * @brief Free dynamically allocated fields in header_info.
 *
 * @param info Header info structure to clean up
 */
void gzip_header_info_free(gzip_header_info_t * info);

/**
 * @brief Extract options to pass through to the inner deflate encoder/decoder.
 *
 * Creates a clone of the source options for pass-through to deflate. The
 * deflate method will ignore unknown keys via its schema validation.
 *
 * @param src Source options (may be NULL)
 * @param dst_out Receives cloned options (set to NULL if src is NULL)
 * @return GCOMP_OK on success
 */
gcomp_status_t gzip_extract_passthrough_options(
    const gcomp_options_t * src, gcomp_options_t ** dst_out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_GZIP_INTERNAL_H
