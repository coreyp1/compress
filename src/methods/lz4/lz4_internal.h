/**
 * @file lz4_internal.h
 *
 * Internal declarations for the LZ4 frame format method implementation.
 *
 * This header is intended for use only by the LZ4 method sources. It
 * exposes internal helpers used by the method registration vtable and
 * defines structures shared between encoder and decoder.
 *
 * ## LZ4 Frame Format Overview
 *
 * An LZ4 frame consists of:
 * 1. Magic number (4 bytes): 0x184D2204
 * 2. Frame descriptor (3-15 bytes):
 *    - FLG byte (flags)
 *    - BD byte (block descriptor)
 *    - Content size (0 or 8 bytes, if flag set)
 *    - Dictionary ID (0 or 4 bytes, if flag set)
 *    - Header checksum (1 byte)
 * 3. Data blocks (variable):
 *    - Block size (4 bytes, high bit = uncompressed flag)
 *    - Block data (variable)
 *    - Block checksum (0 or 4 bytes, if flag set)
 * 4. End mark: block size = 0 (4 bytes)
 * 5. Content checksum (0 or 4 bytes, if flag set)
 *
 * Reference: https://github.com/lz4/lz4/blob/dev/doc/lz4_Frame_format.md
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_LZ4_INTERNAL_H
#define GHOTI_IO_GCOMP_LZ4_INTERNAL_H

#include "../../core/alloc_internal.h"
#include "../../core/endian.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/xxhash32.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//
// LZ4 Frame Format Constants
//

#define LZ4_MAGIC 0x184D2204U     ///< LZ4 frame magic number
#define LZ4_HEADER_MIN_SIZE 7     ///< Magic(4) + FLG(1) + BD(1) + HC(1)
#define LZ4_HEADER_MAX_SIZE 19    ///< Min + ContentSize(8) + DictID(4)
#define LZ4_TRAILER_MAX_SIZE 4    ///< Content checksum (optional)
#define LZ4_BLOCK_HEADER_SIZE 4   ///< Block size field
#define LZ4_BLOCK_CHECKSUM_SIZE 4 ///< Block checksum (optional)
#define LZ4_END_MARK 0x00000000U  ///< End of frame marker

// FLG byte bit positions
#define LZ4_FLG_VERSION_SHIFT 6    ///< Version bits (must be 01)
#define LZ4_FLG_VERSION_MASK 0xC0  ///< Version mask
#define LZ4_FLG_VERSION_VALUE 0x40 ///< Expected version (01)
#define LZ4_FLG_B_INDEP 0x20       ///< Block independence flag
#define LZ4_FLG_B_CHECKSUM 0x10    ///< Block checksum flag
#define LZ4_FLG_C_SIZE 0x08        ///< Content size flag
#define LZ4_FLG_C_CHECKSUM 0x04    ///< Content checksum flag
#define LZ4_FLG_DICT_ID 0x01       ///< Dictionary ID flag
#define LZ4_FLG_RESERVED 0x02      ///< Reserved bit (must be 0)

// BD byte bit positions
#define LZ4_BD_BLOCK_MAX_SHIFT 4   ///< Block max size bits
#define LZ4_BD_BLOCK_MAX_MASK 0x70 ///< Block max size mask
#define LZ4_BD_RESERVED 0x8F       ///< Reserved bits (must be 0)

// Block max size values (encoded in BD byte)
#define LZ4_BLOCK_MAX_64KB 4  ///< 64 KB (65536)
#define LZ4_BLOCK_MAX_256KB 5 ///< 256 KB (262144)
#define LZ4_BLOCK_MAX_1MB 6   ///< 1 MB (1048576)
#define LZ4_BLOCK_MAX_4MB 7   ///< 4 MB (4194304)

// Block size values
#define LZ4_BLOCK_SIZE_64KB 65536U
#define LZ4_BLOCK_SIZE_256KB 262144U
#define LZ4_BLOCK_SIZE_1MB 1048576U
#define LZ4_BLOCK_SIZE_4MB 4194304U

// Default values
#define LZ4_DEFAULT_BLOCK_SIZE LZ4_BLOCK_SIZE_4MB
#define LZ4_DEFAULT_BLOCK_CHECKSUM false
#define LZ4_DEFAULT_CONTENT_CHECKSUM false
#define LZ4_DEFAULT_INDEPENDENT_BLOCKS true
#define LZ4_DEFAULT_CONCAT false

// LZ4 block format constants
#define LZ4_BLOCK_UNCOMPRESSED_FLAG 0x80000000U ///< High bit = uncompressed
#define LZ4_BLOCK_SIZE_MASK 0x7FFFFFFFU         ///< Size without flag bit
#define LZ4_MIN_MATCH 4                         ///< Minimum match length
#define LZ4_LAST_LITERALS 5    ///< Minimum literals in last sequence
#define LZ4_HISTORY_SIZE 65536 ///< History window size (64KB)

// Limit defaults
#define LZ4_DEFAULT_MAX_OUTPUT_BYTES (512ULL * 1024 * 1024) ///< 512 MiB
#define LZ4_DEFAULT_MAX_MEMORY_BYTES (256ULL * 1024 * 1024) ///< 256 MiB
#define LZ4_DEFAULT_MAX_EXPANSION_RATIO 1000                ///< 1000x

//

//
// Encoder State Machine
//

typedef enum {
  LZ4_ENC_STAGE_HEADER = 0, ///< Writing frame header
  LZ4_ENC_STAGE_BLOCKS,     ///< Writing data blocks
  LZ4_ENC_STAGE_END_MARK,   ///< Writing end mark (4-byte zero)
  LZ4_ENC_STAGE_TRAILER,    ///< Writing content checksum (if enabled)
  LZ4_ENC_STAGE_DONE,       ///< Frame complete
  LZ4_ENC_STAGE_ERROR,      ///< Unrecoverable error
} lz4_encoder_stage_t;

//
// Decoder State Machine
//

typedef enum {
  LZ4_DEC_STAGE_HEADER = 0,       ///< Parsing frame header
  LZ4_DEC_STAGE_BLOCK_SIZE,       ///< Reading 4-byte block size
  LZ4_DEC_STAGE_BLOCK_DATA,       ///< Decompressing block content
  LZ4_DEC_STAGE_BLOCK_CHECKSUM,   ///< Reading block checksum (if enabled)
  LZ4_DEC_STAGE_CONTENT_CHECKSUM, ///< Reading content checksum (if enabled)
  LZ4_DEC_STAGE_DONE,             ///< Frame complete
  LZ4_DEC_STAGE_ERROR,            ///< Unrecoverable error
} lz4_decoder_stage_t;

//
// Header Parser State Machine (for streaming header parse)
//

typedef enum {
  LZ4_HEADER_MAGIC = 0,    ///< Reading 4-byte magic number
  LZ4_HEADER_FLG_BD,       ///< Reading FLG, BD bytes
  LZ4_HEADER_CONTENT_SIZE, ///< Reading 8-byte content size (if present)
  LZ4_HEADER_DICT_ID,      ///< Reading 4-byte dictionary ID (if present)
  LZ4_HEADER_HC,           ///< Reading 1-byte header checksum
  LZ4_HEADER_DONE,         ///< Header complete
} lz4_header_parse_stage_t;

//
// Frame Header Info Structure
//

typedef struct {
  uint8_t flg;               ///< FLG byte
  uint8_t bd;                ///< BD (block descriptor) byte
  bool block_independence;   ///< B.Indep flag
  bool block_checksum;       ///< B.Checksum flag
  bool content_size_present; ///< C.Size flag
  bool content_checksum;     ///< C.Checksum flag
  bool dict_id_present;      ///< DictID flag
  uint64_t content_size;     ///< Content size (0 if not present)
  uint32_t dict_id;          ///< Dictionary ID (0 if not present)
  uint32_t block_max_size;   ///< Maximum block size from BD byte
} lz4_frame_header_t;

//
// Encoder State Structure
//

typedef struct {
  // Allocator for memory management
  const gcomp_allocator_t * allocator;

  // Stage tracking
  lz4_encoder_stage_t stage;

  // Frame header configuration
  lz4_frame_header_t header;

  // Running content checksum (if enabled)
  gcomp_xxhash32_state_t content_hash;

  // Block buffer for collecting input until block is full
  uint8_t * block_buffer;   ///< Input buffer for current block
  size_t block_buffer_size; ///< Current block buffer capacity
  size_t block_buffer_pos;  ///< Bytes buffered so far

  // Output staging buffers
  uint8_t header_buf[LZ4_HEADER_MAX_SIZE];
  size_t header_len; ///< Total header length
  size_t header_pos; ///< Bytes written so far

  uint8_t end_mark_buf[4]; ///< End mark (4-byte zero)
  size_t end_mark_pos;     ///< Bytes written so far

  uint8_t trailer_buf[LZ4_TRAILER_MAX_SIZE];
  size_t trailer_len; ///< Total trailer length
  size_t trailer_pos; ///< Bytes written so far

  // Compressed block output buffer (for finish)
  uint8_t * compressed_buffer;   ///< Compressed block output
  size_t compressed_buffer_size; ///< Capacity
  size_t compressed_buffer_pos;  ///< Read position for output
  size_t compressed_buffer_len;  ///< Valid bytes in compressed buffer

  // Hash table for match finding
  uint32_t * hash_table;  ///< Hash table for compression
  size_t hash_table_size; ///< Hash table size in entries

  // Content size tracking (for header if known)
  uint64_t total_input_bytes; ///< Total uncompressed bytes

  // Memory tracking
  gcomp_memory_tracker_t mem_tracker;
  uint64_t max_memory_bytes;

  // Finish state
  bool finish_called;   ///< finish() has been called
  bool blocks_finished; ///< All blocks have been flushed
} lz4_encoder_state_t;

//
// Decoder State Structure
//

typedef struct {
  // Allocator for memory management
  const gcomp_allocator_t * allocator;

  // Stage tracking
  lz4_decoder_stage_t stage;
  lz4_header_parse_stage_t header_stage;

  // Parsed frame header
  lz4_frame_header_t header;

  // Header parsing state
  uint8_t header_accum[LZ4_HEADER_MAX_SIZE]; ///< Accumulator for partial reads
  size_t header_accum_pos;
  gcomp_xxhash32_state_t header_hash; ///< For header checksum

  // Running content checksum (if enabled)
  gcomp_xxhash32_state_t content_hash;

  // Block state
  uint8_t block_size_buf[4];       ///< Block size accumulator
  size_t block_size_buf_pos;       ///< Bytes accumulated
  uint32_t current_block_size;     ///< Current block size
  bool current_block_uncompressed; ///< Current block is uncompressed
  size_t block_bytes_remaining;    ///< Bytes remaining in current block

  // Block data buffer (for compressed blocks needing decompression)
  uint8_t * block_buffer;   ///< Compressed block buffer
  size_t block_buffer_size; ///< Capacity
  size_t block_buffer_pos;  ///< Bytes accumulated

  // Decompressed block output buffer
  uint8_t * output_buffer;   ///< Decompressed output buffer
  size_t output_buffer_size; ///< Capacity
  size_t output_buffer_pos;  ///< Read position for output
  size_t output_buffer_len;  ///< Valid bytes in output buffer

  // Block checksum accumulator
  uint8_t block_checksum_buf[4];
  size_t block_checksum_buf_pos;
  gcomp_xxhash32_state_t block_hash;

  // Content checksum accumulator (at end of frame)
  uint8_t content_checksum_buf[4];
  size_t content_checksum_buf_pos;

  // History buffer for dependent blocks
  uint8_t * history_buffer; ///< History for back-references
  size_t history_size;      ///< Current history size
  size_t history_capacity;  ///< History buffer capacity

  // Options
  bool concat_enabled; ///< Support concatenated frames

  // Limit configuration
  uint64_t max_output_bytes;
  uint64_t max_expansion_ratio;
  uint64_t max_block_bytes;

  // Limit tracking
  uint64_t total_input_bytes;
  uint64_t total_output_bytes;

  // Memory tracking
  gcomp_memory_tracker_t mem_tracker;
  uint64_t max_memory_bytes;
} lz4_decoder_state_t;

//
// Internal API: Encoder
//

/**
 * @brief Initialize LZ4 encoder state.
 */
gcomp_status_t lz4_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder);

/**
 * @brief Destroy LZ4 encoder state.
 */
void lz4_encoder_destroy(gcomp_encoder_t * encoder);

/**
 * @brief LZ4 encoder update implementation.
 */
gcomp_status_t lz4_encoder_update(
    gcomp_encoder_t * encoder, gcomp_buffer_t * input, gcomp_buffer_t * output);

/**
 * @brief LZ4 encoder finish implementation.
 */
gcomp_status_t lz4_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output);

/**
 * @brief Reset LZ4 encoder to initial state.
 */
gcomp_status_t lz4_encoder_reset(gcomp_encoder_t * encoder);

//
// Internal API: Decoder
//

/**
 * @brief Initialize LZ4 decoder state.
 */
gcomp_status_t lz4_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder);

/**
 * @brief Destroy LZ4 decoder state.
 */
void lz4_decoder_destroy(gcomp_decoder_t * decoder);

/**
 * @brief LZ4 decoder update implementation.
 */
gcomp_status_t lz4_decoder_update(
    gcomp_decoder_t * decoder, gcomp_buffer_t * input, gcomp_buffer_t * output);

/**
 * @brief LZ4 decoder finish implementation.
 */
gcomp_status_t lz4_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output);

/**
 * @brief Reset LZ4 decoder to initial state.
 */
gcomp_status_t lz4_decoder_reset(gcomp_decoder_t * decoder);

//
// Internal API: Frame Format Helpers
//

/**
 * @brief Build LZ4 frame header into buffer.
 *
 * @param header Frame header configuration
 * @param buf Output buffer
 * @param buf_size Buffer capacity
 * @param header_len_out Output: actual header length written
 * @return GCOMP_OK on success
 */
gcomp_status_t lz4_write_frame_header(const lz4_frame_header_t * header,
    uint8_t * buf, size_t buf_size, size_t * header_len_out);

/**
 * @brief Parse block size field.
 *
 * @param buf 4-byte buffer containing block size
 * @param size_out Output: block data size (without uncompressed flag)
 * @param uncompressed_out Output: true if uncompressed block
 * @return GCOMP_OK on success
 */
gcomp_status_t lz4_parse_block_size(
    const uint8_t * buf, uint32_t * size_out, bool * uncompressed_out);

/**
 * @brief Convert block max size code to actual size.
 *
 * @param code Block max size code from BD byte (4-7)
 * @return Block size in bytes, or 0 if invalid code
 */
uint32_t lz4_block_code_to_size(uint8_t code);

/**
 * @brief Convert actual block size to code.
 *
 * @param size Block size in bytes
 * @return Block max size code for BD byte, or 0 if invalid size
 */
uint8_t lz4_size_to_block_code(uint32_t size);

//
// Internal API: Block Compression/Decompression
//

/**
 * @brief Compress a block using LZ4 block format.
 *
 * @param input Input data
 * @param input_len Input data length
 * @param output Output buffer for compressed data
 * @param output_cap Output buffer capacity
 * @param output_len_out Output: actual compressed length
 * @param hash_table Hash table for match finding
 * @param hash_table_size Hash table size
 * @return GCOMP_OK on success, GCOMP_ERR_LIMIT if compression expands data
 */
gcomp_status_t lz4_block_compress(const uint8_t * input, size_t input_len,
    uint8_t * output, size_t output_cap, size_t * output_len_out,
    uint32_t * hash_table, size_t hash_table_size);

/**
 * @brief Decompress a block using LZ4 block format.
 *
 * @param input Compressed block data
 * @param input_len Compressed data length
 * @param output Output buffer for decompressed data
 * @param output_cap Output buffer capacity
 * @param output_len_out Output: actual decompressed length
 * @param history History buffer for back-references (NULL for independent
 * blocks)
 * @param history_len History length
 * @return GCOMP_OK on success, error code on failure
 */
gcomp_status_t lz4_block_decompress(const uint8_t * input, size_t input_len,
    uint8_t * output, size_t output_cap, size_t * output_len_out,
    const uint8_t * history, size_t history_len);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_LZ4_INTERNAL_H
