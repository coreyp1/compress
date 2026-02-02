/**
 * @file zstd_internal.h
 *
 * Internal declarations for the Zstandard method implementation.
 *
 * This header is intended for use only by the Zstd method sources. It
 * exposes internal helpers used by the method registration vtable and
 * defines structures shared between encoder and decoder.
 *
 * ## Zstd Frame Format Overview
 *
 * A Zstandard frame consists of:
 * 1. Magic number (4 bytes): 0xFD2FB528
 * 2. Frame header (2-14 bytes):
 *    - Frame Header Descriptor (1 byte)
 *    - Window Descriptor (0 or 1 byte)
 *    - Dictionary ID (0, 1, 2, or 4 bytes)
 *    - Frame Content Size (0, 1, 2, 4, or 8 bytes)
 * 3. Data blocks (variable):
 *    - Block header (3 bytes)
 *    - Block data (variable)
 * 4. Content checksum (0 or 4 bytes, if flag set)
 *
 * Reference:
 * https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_ZSTD_INTERNAL_H
#define GHOTI_IO_GCOMP_ZSTD_INTERNAL_H

#include "../../core/alloc_internal.h"
#include "../../core/endian.h"
#include "../../core/registry_internal.h"
#include "../../core/safe_math.h"
#include "../../core/stream_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/xxhash64.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//
// Zstd Frame Format Constants
//

#define ZSTD_MAGIC 0xFD2FB528U               ///< Zstd frame magic number
#define ZSTD_MAGIC_SKIPPABLE_MIN 0x184D2A50U ///< Skippable frame magic (min)
#define ZSTD_MAGIC_SKIPPABLE_MAX 0x184D2A5FU ///< Skippable frame magic (max)
#define ZSTD_HEADER_MIN_SIZE 2               ///< FHD(1) + min window/size
#define ZSTD_HEADER_MAX_SIZE 14      ///< FHD(1) + window(1) + dict(4) + fcs(8)
#define ZSTD_BLOCK_HEADER_SIZE 3     ///< Block header size
#define ZSTD_CONTENT_CHECKSUM_SIZE 4 ///< Content checksum (xxHash64 low 32)

// Frame Header Descriptor bits
#define ZSTD_FHD_DICT_ID_FLAG_MASK 0x03   ///< Dictionary ID flag (2 bits)
#define ZSTD_FHD_CHECKSUM_FLAG 0x04       ///< Content checksum flag
#define ZSTD_FHD_RESERVED_BIT 0x08        ///< Reserved (must be 0)
#define ZSTD_FHD_UNUSED_BIT 0x10          ///< Unused (must be 0)
#define ZSTD_FHD_SINGLE_SEGMENT_FLAG 0x20 ///< Single segment flag
#define ZSTD_FHD_FCS_FLAG_MASK 0xC0       ///< Frame content size flag (2 bits)
#define ZSTD_FHD_FCS_FLAG_SHIFT 6

// Block header bits (per RFC 8878 Section 3.1.1.2)
// Bit 0: Last_Block flag
// Bits 1-2: Block_Type (0=raw, 1=RLE, 2=compressed, 3=reserved)
// Bits 3-23: Block_Size (21 bits)
#define ZSTD_BLOCK_LAST_FLAG 0x01 ///< Last block flag (bit 0)
#define ZSTD_BLOCK_TYPE_MASK 0x06 ///< Block type (bits 1-2)
#define ZSTD_BLOCK_TYPE_SHIFT 1
#define ZSTD_BLOCK_SIZE_MASK 0xFFFFF8 ///< Block size (bits 3-23)
#define ZSTD_BLOCK_SIZE_SHIFT 3

// Block types
#define ZSTD_BLOCK_TYPE_RAW 0        ///< Raw uncompressed block
#define ZSTD_BLOCK_TYPE_RLE 1        ///< RLE block (single repeated byte)
#define ZSTD_BLOCK_TYPE_COMPRESSED 2 ///< Compressed block
#define ZSTD_BLOCK_TYPE_RESERVED 3   ///< Reserved (invalid)

// Window size constraints
#define ZSTD_WINDOW_LOG_MIN 10     ///< Minimum window log (1 KB)
#define ZSTD_WINDOW_LOG_MAX 31     ///< Maximum window log (2 GB)
#define ZSTD_WINDOW_LOG_DEFAULT 22 ///< Default window log (4 MB)

// Block size constraints
#define ZSTD_BLOCK_SIZE_MAX 131072 ///< Maximum block size (128 KB)

// Compression level constraints
#define ZSTD_LEVEL_MIN 1     ///< Minimum compression level
#define ZSTD_LEVEL_MAX 22    ///< Maximum compression level
#define ZSTD_LEVEL_DEFAULT 3 ///< Default compression level

// Default limits
#define ZSTD_DEFAULT_MAX_OUTPUT_BYTES (512ULL * 1024 * 1024) ///< 512 MiB
#define ZSTD_DEFAULT_MAX_MEMORY_BYTES (256ULL * 1024 * 1024) ///< 256 MiB
#define ZSTD_DEFAULT_MAX_EXPANSION_RATIO 1000                ///< 1000x
#define ZSTD_DEFAULT_MAX_WINDOW_BYTES (1ULL << 27)           ///< 128 MiB

// Parallel compression constants
#define ZSTD_DEFAULT_JOB_SIZE (512 * 1024)   ///< Default job size: 512 KB
#define ZSTD_MIN_JOB_SIZE (64 * 1024)        ///< Minimum job size: 64 KB
#define ZSTD_MAX_JOB_SIZE (16 * 1024 * 1024) ///< Maximum job size: 16 MB
#define ZSTD_DEFAULT_MAX_IN_FLIGHT_MULT 2 ///< In-flight multiplier per thread

// Initial repeat offsets per specification
#define ZSTD_REP_OFFSET_1_INIT 1
#define ZSTD_REP_OFFSET_2_INIT 4
#define ZSTD_REP_OFFSET_3_INIT 8

//
// Encoder State Machine
//

typedef enum {
  ZSTD_ENC_STAGE_HEADER = 0, ///< Writing frame header
  ZSTD_ENC_STAGE_BLOCKS,     ///< Writing data blocks
  ZSTD_ENC_STAGE_CHECKSUM,   ///< Writing content checksum (if enabled)
  ZSTD_ENC_STAGE_DONE,       ///< Frame complete
  ZSTD_ENC_STAGE_ERROR,      ///< Unrecoverable error
} zstd_encoder_stage_t;

//
// Decoder State Machine
//

typedef enum {
  ZSTD_DEC_STAGE_HEADER = 0,       ///< Parsing frame header
  ZSTD_DEC_STAGE_BLOCK_HEADER,     ///< Reading 3-byte block header
  ZSTD_DEC_STAGE_BLOCK_DATA,       ///< Decompressing block content
  ZSTD_DEC_STAGE_CONTENT_CHECKSUM, ///< Reading content checksum (4 bytes)
  ZSTD_DEC_STAGE_DONE,             ///< Frame complete
  ZSTD_DEC_STAGE_ERROR,            ///< Unrecoverable error
} zstd_decoder_stage_t;

//
// Header Parser State Machine (for streaming header parse)
//

typedef enum {
  ZSTD_HEADER_MAGIC = 0,    ///< Reading 4-byte magic number
  ZSTD_HEADER_DESCRIPTOR,   ///< Reading frame header descriptor byte
  ZSTD_HEADER_WINDOW,       ///< Reading window descriptor (if present)
  ZSTD_HEADER_DICT_ID,      ///< Reading dictionary ID (if present)
  ZSTD_HEADER_CONTENT_SIZE, ///< Reading content size (if present)
  ZSTD_HEADER_DONE,         ///< Header complete
} zstd_header_parse_stage_t;

//
// Frame Header Info Structure
//

typedef struct {
  uint8_t descriptor;        ///< Frame header descriptor byte
  bool single_segment;       ///< Single segment flag
  bool content_checksum;     ///< Content checksum flag
  bool content_size_present; ///< Frame content size present
  uint8_t dict_id_flag;      ///< Dictionary ID flag (0-3)
  uint8_t fcs_flag;          ///< Frame content size flag (0-3)
  uint8_t window_log;        ///< Window log (10-31)
  uint32_t window_size;      ///< Computed window size
  uint32_t dict_id;          ///< Dictionary ID (0 if not present)
  uint64_t content_size;     ///< Frame content size (0 if not present)
} zstd_frame_header_t;

//
// FSE Table Entry
//

typedef struct {
  uint16_t new_state; ///< New state after transition
  uint8_t nb_bits;    ///< Number of bits to read
  uint8_t symbol;     ///< Output symbol
} zstd_fse_entry_t;

//
// FSE Decoder State
//

typedef struct {
  uint16_t state;                 ///< Current FSE state
  const zstd_fse_entry_t * table; ///< Pointer to decoding table
  unsigned table_log;             ///< Log2 of table size
} zstd_fse_state_t;

//
// Huffman Decoding Table Entry
//

typedef struct {
  uint8_t symbol;  ///< Output symbol
  uint8_t nb_bits; ///< Number of bits consumed
} zstd_huf_entry_t;

//
// Match Finder Context (for encoder)
//

typedef struct {
  uint32_t * hash_table;  ///< Hash table: hash -> position
  uint32_t * chain_table; ///< Chain table: position -> previous position
  unsigned hash_log;      ///< Log2 of hash table size
  size_t hash_size;       ///< Hash table size
  size_t chain_size;      ///< Chain table size (= window size)
  unsigned search_depth;  ///< Maximum chain search depth
  size_t window_size;     ///< Window size for match offsets
} zstd_match_finder_t;

//
// Sequence Structure (for encoder)
//

typedef struct {
  uint32_t lit_length;   ///< Literal length (bytes to copy from input)
  uint32_t match_offset; ///< Match offset (encoded, +3 for new offsets)
  uint32_t match_length; ///< Match length
} zstd_sequence_t;

//
// Encoder State Structure
//

typedef struct {
  // Allocator for memory management
  const gcomp_allocator_t * allocator;

  // Stage tracking
  zstd_encoder_stage_t stage;

  // Frame header configuration
  zstd_frame_header_t header;
  int compression_level;
  bool checksum_enabled;

  // Running content checksum (if enabled)
  gcomp_xxhash64_state_t content_hash;

  // Block buffer for collecting input
  uint8_t * block_buffer;       ///< Input buffer for current block
  size_t block_buffer_capacity; ///< Block buffer capacity
  size_t block_buffer_pos;      ///< Bytes buffered so far

  // Output staging buffers
  uint8_t header_buf[ZSTD_HEADER_MAX_SIZE + 4]; ///< Magic + header
  size_t header_len;                            ///< Total header length
  size_t header_pos;                            ///< Bytes written so far

  uint8_t checksum_buf[ZSTD_CONTENT_CHECKSUM_SIZE];
  size_t checksum_pos; ///< Bytes written so far

  // Compressed block output buffer
  uint8_t * compressed_buffer; ///< Compressed block output
  size_t compressed_buffer_capacity;
  size_t compressed_buffer_pos; ///< Read position for output
  size_t compressed_buffer_len; ///< Valid bytes in buffer

  // Match finder for compression
  zstd_match_finder_t * match_finder; ///< Match finder context

  // Sequence buffer for compression
  zstd_sequence_t * seq_buffer; ///< Sequence buffer
  size_t seq_buffer_capacity;   ///< Sequence buffer capacity

  // Literals buffer for compression
  uint8_t * literals_buffer;       ///< Literals buffer
  size_t literals_buffer_capacity; ///< Literals buffer capacity

  // Content tracking
  uint64_t total_input_bytes; ///< Total uncompressed bytes

  // Repeat offsets
  uint32_t rep_offset_1;
  uint32_t rep_offset_2;
  uint32_t rep_offset_3;

  // Memory tracking
  gcomp_memory_tracker_t mem_tracker;
  uint64_t max_memory_bytes;

  // Finish state
  bool finish_called;
  bool blocks_finished;

  // ─────────────────────────────────────────────────────────────────────
  // Parallel Compression State (when threads.count > 1)
  // ─────────────────────────────────────────────────────────────────────
  //
  // When parallel mode is active (parallel_ctx != NULL):
  // - Input is accumulated in parallel_job's buffer until job_size
  // - Full jobs are submitted to parallel_ctx for compression
  // - Each job produces an independent zstd frame
  // - Results are collected in order via parallel_output_buf
  // - Output is valid concatenated zstd frames
  //
  // When single-threaded (parallel_ctx == NULL):
  // - Uses block_buffer, match_finder for streaming compression
  // - Produces a single zstd frame
  //
  struct zstd_parallel_ctx_s *
      parallel_ctx; ///< Parallel context (NULL = single-threaded)
  struct zstd_parallel_job_s *
      parallel_job;     ///< Current job being filled with input
  uint32_t num_threads; ///< Thread count from options (0/1 = single-threaded)
  uint64_t job_size;    ///< Target bytes per parallel job
  uint8_t *
      parallel_output_buf; ///< Buffer for compressed frames awaiting output
  size_t parallel_output_buf_cap; ///< Allocated capacity of parallel_output_buf
  size_t
      parallel_output_buf_pos; ///< Current read position (bytes already output)
  size_t parallel_output_buf_len; ///< Valid bytes in buffer (write position)
} zstd_encoder_state_t;

//
// Decoder State Structure
//

typedef struct {
  // Allocator for memory management
  const gcomp_allocator_t * allocator;

  // Stage tracking
  zstd_decoder_stage_t stage;
  zstd_header_parse_stage_t header_stage;

  // Parsed frame header
  zstd_frame_header_t header;

  // Header parsing state
  uint8_t header_accum[ZSTD_HEADER_MAX_SIZE + 4]; ///< Magic + header
  size_t header_accum_pos;
  size_t header_expected_len; ///< Expected total header length

  // Running content checksum (if enabled)
  gcomp_xxhash64_state_t content_hash;

  // Block state
  uint8_t block_header_buf[ZSTD_BLOCK_HEADER_SIZE];
  size_t block_header_buf_pos;
  uint32_t current_block_size;
  uint8_t current_block_type;
  bool current_block_last;
  size_t block_bytes_remaining;

  // Block data buffer (for compressed blocks)
  uint8_t * block_buffer;
  size_t block_buffer_capacity;
  size_t block_buffer_pos;

  // Decompressed block output buffer
  uint8_t * output_buffer;
  size_t output_buffer_capacity;
  size_t output_buffer_pos; ///< Read position
  size_t output_buffer_len; ///< Valid bytes

  // Content checksum accumulator
  uint8_t content_checksum_buf[ZSTD_CONTENT_CHECKSUM_SIZE];
  size_t content_checksum_buf_pos;

  // Window buffer for back-references
  uint8_t * window_buffer;
  size_t window_size;
  size_t window_capacity;
  size_t window_pos; ///< Write position (circular)

  // Repeat offsets
  uint32_t rep_offset_1;
  uint32_t rep_offset_2;
  uint32_t rep_offset_3;

  // FSE decoding tables
  zstd_fse_entry_t * fse_lit_table;
  zstd_fse_entry_t * fse_match_table;
  zstd_fse_entry_t * fse_offset_table;
  size_t fse_lit_table_size;
  size_t fse_match_table_size;
  size_t fse_offset_table_size;
  unsigned fse_ll_log; ///< Literal length FSE table log
  unsigned fse_ml_log; ///< Match length FSE table log
  unsigned fse_of_log; ///< Offset FSE table log

  // Huffman decoding table
  zstd_huf_entry_t * huf_table;
  size_t huf_table_size;
  unsigned huf_max_bits; ///< Max bits for current Huffman table
  bool huf_table_valid;  ///< For treeless literals

  // Options
  bool concat_enabled;

  // Limit configuration
  uint64_t max_output_bytes;
  uint64_t max_expansion_ratio;
  uint64_t max_window_bytes;

  // Limit tracking
  uint64_t total_input_bytes;
  uint64_t total_output_bytes;
  uint64_t frame_output_bytes; ///< Output bytes for current frame (for content
                               ///< size check)

  // Memory tracking
  gcomp_memory_tracker_t mem_tracker;
  uint64_t max_memory_bytes;
} zstd_decoder_state_t;

//
// Internal API: Encoder
//

/**
 * @brief Initialize Zstd encoder state.
 */
gcomp_status_t zstd_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder);

/**
 * @brief Destroy Zstd encoder state.
 */
void zstd_encoder_destroy(gcomp_encoder_t * encoder);

/**
 * @brief Zstd encoder update implementation.
 */
gcomp_status_t zstd_encoder_update(
    gcomp_encoder_t * encoder, gcomp_buffer_t * input, gcomp_buffer_t * output);

/**
 * @brief Zstd encoder finish implementation.
 */
gcomp_status_t zstd_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output);

/**
 * @brief Reset Zstd encoder to initial state.
 */
gcomp_status_t zstd_encoder_reset(gcomp_encoder_t * encoder);

//
// Internal API: Decoder
//

/**
 * @brief Initialize Zstd decoder state.
 */
gcomp_status_t zstd_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder);

/**
 * @brief Destroy Zstd decoder state.
 */
void zstd_decoder_destroy(gcomp_decoder_t * decoder);

/**
 * @brief Zstd decoder update implementation.
 */
gcomp_status_t zstd_decoder_update(
    gcomp_decoder_t * decoder, gcomp_buffer_t * input, gcomp_buffer_t * output);

/**
 * @brief Zstd decoder finish implementation.
 */
gcomp_status_t zstd_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output);

/**
 * @brief Reset Zstd decoder to initial state.
 */
gcomp_status_t zstd_decoder_reset(gcomp_decoder_t * decoder);

//
// Internal API: Frame Format Helpers
//

/**
 * @brief Build Zstd frame header into buffer.
 *
 * @param header Frame header configuration
 * @param buf Output buffer
 * @param buf_size Buffer capacity
 * @param header_len_out Output: actual header length written
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_write_frame_header(const zstd_frame_header_t * header,
    uint8_t * buf, size_t buf_size, size_t * header_len_out);

/**
 * @brief Parse block header.
 *
 * @param buf 3-byte buffer containing block header
 * @param last_out Output: true if last block
 * @param type_out Output: block type (0-3)
 * @param size_out Output: block data size
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_parse_block_header(const uint8_t * buf, bool * last_out,
    uint8_t * type_out, uint32_t * size_out);

/**
 * @brief Write block header.
 *
 * @param buf Output buffer (must be at least 3 bytes)
 * @param last True if last block
 * @param type Block type (0-3)
 * @param size Block data size
 */
void zstd_write_block_header(
    uint8_t * buf, bool last, uint8_t type, uint32_t size);

/**
 * @brief Compute window size from window log.
 *
 * @param window_log Window log (10-31)
 * @return Window size in bytes
 */
uint32_t zstd_window_log_to_size(uint8_t window_log);

/**
 * @brief Get window log from compression level.
 *
 * @param level Compression level (1-22)
 * @return Appropriate window log
 */
uint8_t zstd_level_to_window_log(int level);

//
// Internal API: Block Compression/Decompression
//

/**
 * @brief Decompress a raw block (copy).
 */
gcomp_status_t zstd_block_decompress_raw(const uint8_t * input,
    size_t input_len, uint8_t * output, size_t output_cap,
    size_t * output_len_out);

/**
 * @brief Decompress an RLE block.
 */
gcomp_status_t zstd_block_decompress_rle(const uint8_t * input,
    size_t input_len, uint8_t * output, size_t output_cap,
    size_t * output_len_out, uint32_t regenerated_size);

/**
 * @brief Decompress a compressed block.
 */
gcomp_status_t zstd_block_decompress_compressed(zstd_decoder_state_t * state,
    const uint8_t * input, size_t input_len, uint8_t * output,
    size_t output_cap, size_t * output_len_out);

/**
 * @brief Compress a block.
 */
gcomp_status_t zstd_block_compress(zstd_encoder_state_t * state,
    const uint8_t * input, size_t input_len, uint8_t * output,
    size_t output_cap, size_t * output_len_out, uint8_t * type_out);

//
// Internal API: FSE (Finite State Entropy)
//

/**
 * @brief Read FSE table header and decode normalized counts.
 *
 * @param src Source data
 * @param src_size Source size
 * @param norm_counts Output: normalized counts for each symbol
 * @param max_symbol_out Output: maximum symbol value
 * @param table_log_out Output: log2 of table size
 * @param bytes_read_out Output: bytes consumed from source
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_fse_read_table_header(const uint8_t * src, size_t src_size,
    int16_t * norm_counts, unsigned * max_symbol_out, unsigned * table_log_out,
    size_t * bytes_read_out);

/**
 * @brief Build complete FSE decoding table from bitstream.
 *
 * @param src Source data containing table header
 * @param src_size Source size
 * @param table Output: decoding table
 * @param table_capacity Table capacity (must be >= 1 << table_log)
 * @param table_log_out Output: log2 of table size
 * @param max_symbol_out Output: maximum symbol value
 * @param bytes_read_out Output: bytes consumed from source
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_fse_build_decoding_table(const uint8_t * src,
    size_t src_size, zstd_fse_entry_t * table, size_t table_capacity,
    unsigned * table_log_out, unsigned * max_symbol_out,
    size_t * bytes_read_out);

/**
 * @brief Build predefined literal length FSE table.
 */
gcomp_status_t zstd_fse_build_predefined_ll_table(
    zstd_fse_entry_t * table, size_t table_capacity);

/**
 * @brief Build predefined match length FSE table.
 */
gcomp_status_t zstd_fse_build_predefined_ml_table(
    zstd_fse_entry_t * table, size_t table_capacity);

/**
 * @brief Build predefined offset FSE table.
 */
gcomp_status_t zstd_fse_build_predefined_of_table(
    zstd_fse_entry_t * table, size_t table_capacity);

//
// Internal API: Huffman Decoding
//

/**
 * @brief Read Huffman table from bitstream.
 *
 * Parses the Huffman tree description and builds a decoding table.
 *
 * @param src Source data containing Huffman tree description
 * @param src_size Source size
 * @param table Output: decoding table (must be at least 2^11 entries)
 * @param table_capacity Table capacity
 * @param max_bits_out Output: maximum code length (table log)
 * @param bytes_read_out Output: bytes consumed from source
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_huf_read_table(const uint8_t * src, size_t src_size,
    zstd_huf_entry_t * table, size_t table_capacity, unsigned * max_bits_out,
    size_t * bytes_read_out);

//
// Internal API: Huffman Encoding
//
// The Huffman encoder compresses literal bytes by assigning shorter codes
// to more frequent symbols. It is used by zstd_literals_encode_compressed()
// when compression provides sufficient benefit over raw literals.
//
// Format requirements (RFC 8878):
//   - Maximum code length: 11 bits
//   - Weights encoded as 4-bit values (direct) or FSE-compressed
//   - Bitstream written backwards with marker bit
//
// Implementation choices (tunable heuristics):
//   - When to use Huffman vs raw (see zstd_literals.c)
//   - Tree building algorithm (two-queue, O(n log n))
//   - Code length limiting strategy (greedy redistribution)
//
// Typical usage:
//   1. Count symbol frequencies in input literals
//   2. Call zstd_huf_build_enc_table() to build encoding table
//   3. Call zstd_huf_write_weights() to write table description
//   4. Call zstd_huf_encode_1stream() to write compressed bitstream
//

/**
 * @brief Huffman encoding table entry.
 *
 * Maps a symbol (byte value) to its Huffman code.
 */
typedef struct {
  uint16_t code;   ///< Huffman code bits (canonical, right-aligned)
  uint8_t nb_bits; ///< Number of bits in code (1-11, or 0 if unused)
} zstd_huf_enc_entry_t;

/**
 * @brief Huffman encoder context.
 *
 * Holds the encoding table and metadata needed for Huffman compression.
 * Built from symbol frequencies via zstd_huf_build_enc_table().
 */
typedef struct {
  zstd_huf_enc_entry_t symbols[256]; ///< Encoding table: symbol -> code
  uint8_t weights[256];              ///< Weights for each symbol (for header)
  unsigned max_bits;                 ///< Maximum code length (1-11)
  unsigned num_symbols;              ///< Number of symbols with non-zero weight
} zstd_huf_enc_table_t;

/**
 * @brief Build Huffman encoding table from symbol frequencies.
 *
 * Builds an optimal Huffman tree from the given frequencies, limits code
 * lengths to 11 bits (Zstd maximum), and generates canonical codes.
 *
 * The resulting table can be used for encoding via zstd_huf_encode_1stream()
 * and the weights can be written via zstd_huf_write_weights().
 *
 * @param freq Symbol frequency array (256 entries, one per byte value).
 *             Symbols with freq[i] == 0 are not included in the tree.
 * @param table Output: encoding table with codes, weights, and metadata.
 * @return GCOMP_OK on success, GCOMP_ERR_INVALID_ARG if freq or table is NULL.
 */
gcomp_status_t zstd_huf_build_enc_table(
    const uint32_t * freq, zstd_huf_enc_table_t * table);

/**
 * @brief Write Huffman table description (weights) to output.
 *
 * Writes the Huffman tree in Zstd's weight format. Uses direct 4-bit
 * representation (header byte < 128) where weights are packed as nibbles.
 *
 * Format: [header_byte][weight_pairs...]
 *   - header_byte = num_symbols - 1 (must be < 128)
 *   - Each byte contains two 4-bit weights: (w[i] << 4) | w[i+1]
 *
 * @param table Encoding table with weights (from zstd_huf_build_enc_table).
 * @param output Output buffer.
 * @param output_cap Output buffer capacity.
 * @param output_len_out Output: number of bytes written.
 * @return GCOMP_OK on success,
 *         GCOMP_ERR_LIMIT if output buffer too small,
 *         GCOMP_ERR_UNSUPPORTED if num_symbols > 128 (would need FSE weights).
 */
gcomp_status_t zstd_huf_write_weights(const zstd_huf_enc_table_t * table,
    uint8_t * output, size_t output_cap, size_t * output_len_out);

/**
 * @brief Encode literals using Huffman coding (single stream).
 *
 * Encodes literal bytes as a Huffman bitstream. The stream is written
 * forward, then a marker bit (1) is appended, and finally the bytes are
 * reversed. This matches Zstd's backward reading convention.
 *
 * The decoder reads the stream backwards, finding the marker bit in the
 * last byte to determine where data starts.
 *
 * @param table Encoding table (from zstd_huf_build_enc_table).
 * @param literals Input literal bytes to encode.
 * @param literals_size Number of literals (0 for empty stream).
 * @param output Output buffer.
 * @param output_cap Output buffer capacity.
 * @param output_len_out Output: number of bytes written.
 * @return GCOMP_OK on success,
 *         GCOMP_ERR_LIMIT if output buffer too small,
 *         GCOMP_ERR_CORRUPT if a literal has no code in the table.
 */
gcomp_status_t zstd_huf_encode_1stream(const zstd_huf_enc_table_t * table,
    const uint8_t * literals, size_t literals_size, uint8_t * output,
    size_t output_cap, size_t * output_len_out);

/**
 * @brief Decode a single Huffman stream.
 *
 * @param table Huffman decoding table
 * @param max_bits Table log (maximum code length)
 * @param src Source compressed data
 * @param src_size Source size
 * @param dst Destination buffer
 * @param dst_size Destination capacity
 * @param decoded_size_out Output: actual bytes decoded
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_huf_decode_1stream(const zstd_huf_entry_t * table,
    unsigned max_bits, const uint8_t * src, size_t src_size, uint8_t * dst,
    size_t dst_size, size_t * decoded_size_out);

/**
 * @brief Decode 4 interleaved Huffman streams.
 *
 * Used for large literal sections (> 1024 bytes typically).
 *
 * @param table Huffman decoding table
 * @param max_bits Table log (maximum code length)
 * @param src Source compressed data
 * @param src_size Source size
 * @param jump_table Array of 3 offsets for streams 2, 3, 4
 * @param dst Destination buffer
 * @param dst_size Destination capacity
 * @param decoded_size_out Output: actual bytes decoded
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_huf_decode_4streams(const zstd_huf_entry_t * table,
    unsigned max_bits, const uint8_t * src, size_t src_size,
    const uint32_t * jump_table, uint8_t * dst, size_t dst_size,
    size_t * decoded_size_out);

//
// Internal API: Literals Section Decoding
//

// Maximum Huffman table size
#define HUF_MAX_TABLE_SIZE 2048

/**
 * @brief Decode literals section from compressed block.
 *
 * Handles all four literals types: raw, RLE, compressed, treeless.
 *
 * @param state Decoder state (for Huffman table storage)
 * @param src Source data (literals section)
 * @param src_size Source size
 * @param dst Destination buffer for decoded literals
 * @param dst_capacity Destination capacity
 * @param regenerated_size_out Output: actual decoded size
 * @param bytes_read_out Output: bytes consumed from source
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_literals_decode(zstd_decoder_state_t * state,
    const uint8_t * src, size_t src_size, uint8_t * dst, size_t dst_capacity,
    size_t * regenerated_size_out, size_t * bytes_read_out);

/**
 * @brief Get size of literals section header.
 *
 * @param src Source data
 * @param src_size Source size
 * @return Header size in bytes, or 0 on error
 */
size_t zstd_literals_header_size(const uint8_t * src, size_t src_size);

//
// Internal API: Sequences Section Decoding
//

/**
 * @brief Decode sequences section and execute to produce output.
 *
 * @param state Decoder state (for FSE tables and repeat offsets)
 * @param src Source data (sequences section)
 * @param src_size Source size
 * @param literals Decoded literals from literals section
 * @param literals_size Size of literals buffer
 * @param dst Destination buffer for output
 * @param dst_capacity Destination capacity
 * @param output_size_out Output: actual output size
 * @param bytes_read_out Output: bytes consumed from source
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_sequences_decode(zstd_decoder_state_t * state,
    const uint8_t * src, size_t src_size, const uint8_t * literals,
    size_t literals_size, uint8_t * dst, size_t dst_capacity,
    size_t * output_size_out, size_t * bytes_read_out);

//
// Internal API: Encoder - Match Finding and Compression
//

/**
 * @brief Initialize match finder.
 */
gcomp_status_t zstd_mf_init(zstd_match_finder_t * mf,
    const gcomp_allocator_t * alloc, int level, size_t window_size,
    gcomp_memory_tracker_t * mem_tracker);

/**
 * @brief Destroy match finder.
 */
void zstd_mf_destroy(zstd_match_finder_t * mf, const gcomp_allocator_t * alloc,
    gcomp_memory_tracker_t * mem_tracker);

/**
 * @brief Reset match finder for new block.
 */
void zstd_mf_reset(zstd_match_finder_t * mf);

/**
 * @brief Generate sequences from input data.
 */
gcomp_status_t zstd_mf_generate_sequences(zstd_match_finder_t * mf,
    const uint8_t * data, size_t data_size, zstd_sequence_t * sequences,
    size_t max_sequences, size_t * num_sequences_out, uint8_t * literals_out,
    size_t * literals_size_out, uint32_t * rep_offset_1,
    uint32_t * rep_offset_2, uint32_t * rep_offset_3);

/**
 * @brief Encode literals section (raw mode).
 *
 * @param literals Literals data
 * @param literals_size Literals size
 * @param output Output buffer
 * @param output_cap Output capacity
 * @param output_len_out Output: bytes written
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_literals_encode_raw(const uint8_t * literals,
    size_t literals_size, uint8_t * output, size_t output_cap,
    size_t * output_len_out);

/**
 * @brief Encode literals section using Huffman compression.
 *
 * Builds a Huffman table from symbol frequencies and encodes
 * the literals as a compressed bitstream. Falls back to raw
 * encoding if compression doesn't provide benefit.
 *
 * @param literals Literals data
 * @param literals_size Literals size
 * @param output Output buffer
 * @param output_cap Output capacity
 * @param output_len_out Output: bytes written
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_literals_encode_compressed(const uint8_t * literals,
    size_t literals_size, uint8_t * output, size_t output_cap,
    size_t * output_len_out);

/**
 * @brief Encode sequences section using predefined FSE tables.
 *
 * @param sequences Sequence array
 * @param num_sequences Number of sequences
 * @param output Output buffer
 * @param output_cap Output capacity
 * @param output_len_out Output: bytes written
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_sequences_encode_predefined(
    const zstd_sequence_t * sequences, size_t num_sequences, uint8_t * output,
    size_t output_cap, size_t * output_len_out);

/**
 * @brief Compress a block using full LZ77 + entropy encoding.
 *
 * @param state Encoder state
 * @param input Input data
 * @param input_len Input size
 * @param output Output buffer
 * @param output_cap Output capacity
 * @param output_len_out Output: compressed size
 * @param type_out Output: block type
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_compress_block_full(zstd_encoder_state_t * state,
    const uint8_t * input, size_t input_len, uint8_t * output,
    size_t output_cap, size_t * output_len_out, uint8_t * type_out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_ZSTD_INTERNAL_H
