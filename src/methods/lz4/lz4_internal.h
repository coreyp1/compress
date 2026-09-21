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

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_LZ4_LZ4_INTERNAL_H
#define GHOTI_IO_GCOMP_SRC_METHODS_LZ4_LZ4_INTERNAL_H

#include <ghoti.io/compress/macros.h>

#include "../../core/stepdown.h"

#include "../../core/alloc_internal.h"
#include "../../core/endian.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/lz4.h>
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

/**
 * @brief Lowest skippable-frame magic number.
 *
 * LZ4 Frame Format, "Skippable Frames": the magic is 0x184D2A50 through
 * 0x184D2A5F -- the low nibble is the writer's to choose, so that an
 * application can tag its own kind of embedded data.  A decoder must skip
 * these frames whatever the nibble says.  The frame is the 4-byte magic, a
 * 4-byte little-endian size, and that many bytes of user data.
 */
#define LZ4_SKIPPABLE_MAGIC 0x184D2A50U

/** @brief Bits of a magic number that identify it as skippable. */
#define LZ4_SKIPPABLE_MAGIC_MASK 0xFFFFFFF0U

/** @brief True if @p magic names a skippable frame. */
#define LZ4_IS_SKIPPABLE_MAGIC(magic) \
  (((magic) & LZ4_SKIPPABLE_MAGIC_MASK) == LZ4_SKIPPABLE_MAGIC)
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

/**
 * Bytes moved by one wide copy in the block decoder.
 *
 * Sixteen is one SSE2 register, so the group is a load and a store on every
 * x86-64 target without asking for a newer instruction set, and compilers
 * turn a 16-byte `memcpy` of a constant size into exactly that rather than
 * calling libc.
 */
#define LZ4_WIDE_GROUP 16u

/**
 * Bytes a wide copy may write past the last one actually wanted.
 *
 * The decoder keeps this much room between its write cursor and the end of
 * the caller's buffer while it uses the wide path, and finishes the block
 * with exact copies, so the slack never leaves the buffer.  Two groups,
 * because the longest match the wide path takes is two groups.
 */
#define LZ4_WIDE_SLACK 32u
#define LZ4_LAST_LITERALS 5    ///< Minimum literals in last sequence

/**
 * @brief Furthest a match may reach back.
 *
 * The LZ4 block format writes the match offset as two little-endian bytes, so
 * the reach is 65535 regardless of how much of the frame precedes the block.
 */
#define LZ4_MAX_OFFSET 65535U

/**
 * @brief Bytes of the preceding frame kept in front of a linked block.
 *
 * One more than LZ4_MAX_OFFSET, so that every byte a match may legally reach
 * is present.  Held only when the Block Independence flag is 0.
 */
#define LZ4_WINDOW_SIZE 65536U
#define LZ4_HISTORY_SIZE 65536 ///< History window size (64KB)

// Limit defaults
#define LZ4_DEFAULT_MAX_OUTPUT_BYTES (512ULL * 1024 * 1024) ///< 512 MiB
#define LZ4_DEFAULT_MAX_MEMORY_BYTES (256ULL * 1024 * 1024) ///< 256 MiB
/// The format's own ceiling; see ::GCOMP_LZ4_MAX_EXPANSION_RATIO.
#define LZ4_DEFAULT_MAX_EXPANSION_RATIO GCOMP_LZ4_MAX_EXPANSION_RATIO

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
  LZ4_DEC_STAGE_SKIPPABLE_SIZE,   ///< Reading a skippable frame's 4-byte size
  LZ4_DEC_STAGE_SKIPPABLE_DATA,   ///< Discarding a skippable frame's payload
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

/**
 * Parallel encoding types, declared here so the encoder state can hold
 * pointers to them without pulling lz4_parallel.h into every translation
 * unit that only needs the state.  See lz4_parallel.h for what they are.
 */
typedef struct lz4_parallel_ctx_s lz4_parallel_ctx_t;
typedef struct lz4_parallel_job_s lz4_parallel_job_t;

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
  uint8_t * block_buffer;   ///< Window: prefix, then the current block
  size_t block_buffer_size; ///< Capacity for the block itself
  size_t block_buffer_pos;  ///< Bytes of the current block buffered so far
  /**
   * Bytes of the preceding frame held in front of the current block, so a
   * linked block can match into it.  Always 0 when blocks are independent;
   * otherwise it grows to LZ4_WINDOW_SIZE and stays there.  The block being
   * filled begins at `block_buffer + prefix_len`.
   */
  size_t prefix_len;
  size_t prefix_capacity; ///< LZ4_WINDOW_SIZE when linked or seeded, else 0
  /**
   * Dictionary the frame is compressed against, from `lz4.dictionary`; only
   * its last LZ4_WINDOW_SIZE bytes, since the match offset is two bytes.
   * With linked blocks it seeds the window once, before the first block; with
   * independent blocks it is the window every block starts from, because such
   * a block may reference the dictionary but not the blocks before it.
   */
  uint8_t * dictionary;
  size_t dictionary_size;

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
  uint32_t * dict_hash_table; ///< The table as the dictionary alone leaves it
  size_t hash_table_size; ///< Hash table size in entries

  // Content size tracking (for header if known)
  uint64_t total_input_bytes; ///< Total uncompressed bytes

  // Memory tracking
  gcomp_memory_tracker_t mem_tracker;
  uint64_t max_memory_bytes;

  /**
   * @brief Times this encoder settled for a weaker encoding, and why.
   *
   * Read by tests, which assert that nothing was forced.  See
   * src/core/stepdown.h.
   */
  gcomp_stepdown_tally_t stepdowns;

  /**
   * @name Parallel encoding
   *
   * Set up only when `threads.count > 1` and blocks are independent; see
   * lz4_parallel.h.  When @ref parallel_ctx is NULL the encoder compresses in
   * the calling thread and none of the rest of this group is used -- that is
   * the single test the update and finish paths branch on.
   *
   * In parallel mode a job's buffers replace @ref block_buffer and
   * @ref hash_table, which are not allocated at all; @ref compressed_buffer
   * survives as the place a collected block's tail waits when the caller's
   * output buffer filled part-way through it.
   * @{
   */
  uint32_t num_threads;              ///< `threads.count`, as asked for
  lz4_parallel_ctx_t * parallel_ctx; ///< NULL when compressing inline
  lz4_parallel_job_t * parallel_job; ///< The block currently being filled
  /** @} */

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

  // Skippable frames
  uint32_t skippable_remaining; ///< Payload bytes left to discard
  unsigned skippable_variant;   ///< Low nibble of the magic being skipped
  uint64_t skippable_size;      ///< That frame's whole payload size
  uint64_t skippable_delivered; ///< Payload bytes handed to the callback
  /**
   * Reported to the caller as the payload goes past, rather than buffered: a
   * skippable frame may declare up to 4 GB and the size comes from the
   * stream, so holding one whole would let the input choose an allocation.
   * Survives a reset -- it describes how the caller is using the decoder, not
   * the stream being read.
   */
  gcomp_lz4_skippable_cb skippable_cb;
  void * skippable_ctx;
  /**
   * Frames finished so far, skippable ones included.  finish() uses it to
   * tell "the stream ended cleanly after a frame" from "the stream was cut
   * off before one started" -- both leave the parser waiting on a magic
   * number with nothing accumulated.
   */
  uint64_t frames_completed;

  /**
   * Dictionary the stream was compressed against, as supplied through
   * `lz4.dictionary`.  Only the last LZ4_HISTORY_SIZE bytes are kept: the
   * match offset is two bytes, so nothing earlier is reachable.
   *
   * LZ4 Frame Format: with linked blocks the dictionary is what precedes the
   * first block, and the frame's own history takes over from there.  With
   * independent blocks every block starts from the dictionary again -- which
   * was settled against liblz4's own output, not inferred.
   */
  uint8_t * dictionary;
  size_t dictionary_size;

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
/**
 * @brief Close out the block being filled, without ending the frame.
 *
 * See gcomp_encoder_flush().  A flushed block is an ordinary LZ4 block, so
 * nothing about the frame changes except where the block boundaries fall.
 * With independent blocks -- the default, and what parallel encoding requires
 * -- a block already carries no history, so the two flush modes do the same
 * thing.  With linked blocks GCOMP_FLUSH_FULL additionally drops the 64 KB
 * window, so nothing after the flush can match into anything before it.
 */
gcomp_status_t lz4_encoder_flush(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output, gcomp_flush_t mode);

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
/**
 * @brief Parse and validate a frame header.
 *
 * The single place the frame descriptor is interpreted, so that
 * gcomp_lz4_peek_frame_info() and the decoder cannot come to different
 * conclusions about the same bytes.
 *
 * @param buf Buffer positioned at the frame's magic number
 * @param len Bytes available
 * @param header_out Filled in on success
 * @param header_size_out On success, the header's length; on GCOMP_ERR_LIMIT,
 *        how many bytes are needed before asking again
 * @return GCOMP_OK, GCOMP_ERR_LIMIT if @p len is too short, or
 *         GCOMP_ERR_CORRUPT if the header is not valid
 */
gcomp_status_t lz4_parse_frame_header(const uint8_t * buf, size_t len,
    lz4_frame_header_t * header_out, size_t * header_size_out);

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
 * @brief Compress one block, letting matches reach back into the frame.
 *
 * LZ4 Frame Format, "Blocks": when the Block Independence flag is 0, a block
 * may reference data from the blocks that precede it in the same frame.  The
 * offset field is two bytes, so the reach is at most LZ4_MAX_OFFSET however
 * long the frame is.
 *
 * Unlike lz4_block_compress(), this does NOT clear the hash table: entries
 * left by the previous call are what make a cross-block match findable. The
 * caller owns the window and the table, and must rebase both together -- see
 * lz4_encoder_slide_window().
 *
 * @param window Search window: `prefix_len` bytes of already emitted frame
 *        data followed by the block to compress
 * @param prefix_len Bytes of preceding frame data in `window`; 0 compresses
 *        the block independently
 * @param block_len Length of the block beginning at `window + prefix_len`
 * @param output Output buffer for compressed data
 * @param output_cap Output buffer capacity
 * @param output_len_out Output: actual compressed length
 * @param hash_table Hash table for match finding, holding offsets from
 *        `window`
 * @param hash_table_size Hash table size in entries
 * @return GCOMP_OK on success, GCOMP_ERR_LIMIT if compression expands data
 */
/**
 * @brief Index a window's bytes into a hash table for match finding.
 *
 * Used to make a dictionary findable before any of the frame's own data has
 * been seen.  Writes the same entries lz4_block_compress_linked() would have
 * written had it scanned those bytes itself.
 *
 * @param window Bytes to index
 * @param len How many bytes @p window holds
 * @param hash_table Hash table to fill; not cleared first
 * @param hash_table_size Hash table size in entries (a power of two)
 */
void lz4_block_index_window(const uint8_t * window, size_t len,
    uint32_t * hash_table, size_t hash_table_size);

gcomp_status_t lz4_block_compress_linked(const uint8_t * window,
    size_t prefix_len, size_t block_len, uint8_t * output, size_t output_cap,
    size_t * output_len_out, uint32_t * hash_table, size_t hash_table_size);

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
/**
 * @brief Decode a whole LZ4 stream with several threads, or decline.
 *
 * The ::gcomp_method_s::decode_parallel hook. See lz4_decode_parallel.c for
 * which streams it declines and why declining is the common answer.
 */
gcomp_status_t lz4_decode_parallel(gcomp_registry_t * registry,
    gcomp_options_t * options, const void * input, size_t input_size,
    void * output, size_t output_capacity, size_t * output_size_out);

gcomp_status_t lz4_block_decompress(const uint8_t * input, size_t input_len,
    uint8_t * output, size_t output_cap, size_t * output_len_out,
    const uint8_t * history, size_t history_len);


/**
 * @brief The encoder's record of when it settled for a weaker encoding.
 *
 * Exists so that a test can assert nothing was forced - see
 * src/core/stepdown.h for why that needs counting.  The tally accumulates
 * over the encoder's life and is cleared by a reset.
 *
 * @param encoder Encoder to read; NULL returns NULL.
 * @return The tally, owned by the encoder, or NULL if there is no state.
 */
/**
 * @brief How many threads this encoder is actually compressing blocks with.
 *
 * 1 when compressing inline, whether because `threads.count` said so or
 * because the frame uses linked blocks and parallelism is not available.
 * Read by tests, and by anyone who would rather check than assume.
 *
 * @param encoder Encoder to ask; NULL reads as 1.
 * @return Worker thread count.
 */
uint32_t gcomp_lz4_encoder_worker_count(const gcomp_encoder_t * encoder);

const gcomp_stepdown_tally_t * gcomp_lz4_encoder_stepdowns(
    const gcomp_encoder_t * encoder);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_METHODS_LZ4_LZ4_INTERNAL_H
