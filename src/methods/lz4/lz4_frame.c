/**
 * @file lz4_frame.c
 *
 * LZ4 frame format helpers.
 *
 * This file provides functions for building and parsing LZ4 frame headers
 * and block metadata according to the LZ4 Frame Format specification:
 * https://github.com/lz4/lz4/blob/dev/doc/lz4_Frame_format.md
 *
 * ## Frame Header Structure
 *
 * ```
 * ┌────────────────┬─────┬─────┬────────────────┬──────────┬─────┐
 * │  Magic Number  │ FLG │ BD  │ [Content Size] │ [DictID] │ HC  │
 * │   (4 bytes)    │(1B) │(1B) │   (8 bytes)    │(4 bytes) │(1B) │
 * └────────────────┴─────┴─────┴────────────────┴──────────┴─────┘
 *   0x184D2204       │      │         │              │        │
 *   (little-endian)  │      │         │              │        └─ Header Checksum
 *                    │      │         │              └─ Dictionary ID (optional)
 *                    │      │         └─ Original size (optional)
 *                    │      └─ Block Descriptor
 *                    └─ Flags byte
 * ```
 *
 * ## FLG Byte (Flags)
 *
 * | Bit | Mask | Name | Description |
 * |-----|------|------|-------------|
 * | 7-6 | 0xC0 | Version | Must be 01 (value 0x40) |
 * | 5 | 0x20 | B.Indep | Block independence (1 = independent) |
 * | 4 | 0x10 | B.Checksum | Block checksum present |
 * | 3 | 0x08 | C.Size | Content size field present |
 * | 2 | 0x04 | C.Checksum | Content checksum in trailer |
 * | 1 | 0x02 | Reserved | Must be 0 |
 * | 0 | 0x01 | DictID | Dictionary ID field present |
 *
 * ## BD Byte (Block Descriptor)
 *
 * | Bit | Mask | Name | Description |
 * |-----|------|------|-------------|
 * | 7 | 0x80 | Reserved | Must be 0 |
 * | 6-4 | 0x70 | Block MaxSize | Block size code (4-7) |
 * | 3-0 | 0x0F | Reserved | Must be 0 |
 *
 * Block size codes:
 * - 4 → 64 KB (65536 bytes)
 * - 5 → 256 KB (262144 bytes)
 * - 6 → 1 MB (1048576 bytes)
 * - 7 → 4 MB (4194304 bytes)
 *
 * ## Header Checksum (HC)
 *
 * The header checksum is computed as:
 * ```
 * HC = (xxHash32(FLG || BD || [Content Size] || [DictID], seed=0) >> 8) & 0xFF
 * ```
 *
 * Only the second-lowest byte of the hash is used. This provides basic
 * integrity checking with minimal overhead (1 byte).
 *
 * ## Block Size Field
 *
 * Each data block is preceded by a 4-byte size field:
 * - Bit 31: Uncompressed flag (1 = data stored uncompressed)
 * - Bits 30-0: Block size in bytes
 * - Value 0x00000000: End of frame marker
 *
 * ## xxHash32 Usage
 *
 * LZ4 uses xxHash32 for all checksums:
 * - Header checksum: second byte of hash
 * - Block checksums: full 32-bit hash of compressed block data
 * - Content checksum: full 32-bit hash of all uncompressed data
 *
 * All checksums use seed value 0.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "lz4_internal.h"
#include <ghoti.io/compress/xxhash32.h>
#include <string.h>

//
// Block Size Conversion
//

uint32_t lz4_block_code_to_size(uint8_t code) {
  switch (code) {
  case LZ4_BLOCK_MAX_64KB:
    return LZ4_BLOCK_SIZE_64KB;
  case LZ4_BLOCK_MAX_256KB:
    return LZ4_BLOCK_SIZE_256KB;
  case LZ4_BLOCK_MAX_1MB:
    return LZ4_BLOCK_SIZE_1MB;
  case LZ4_BLOCK_MAX_4MB:
    return LZ4_BLOCK_SIZE_4MB;
  default:
    return 0;
  }
}

uint8_t lz4_size_to_block_code(uint32_t size) {
  switch (size) {
  case LZ4_BLOCK_SIZE_64KB:
    return LZ4_BLOCK_MAX_64KB;
  case LZ4_BLOCK_SIZE_256KB:
    return LZ4_BLOCK_MAX_256KB;
  case LZ4_BLOCK_SIZE_1MB:
    return LZ4_BLOCK_MAX_1MB;
  case LZ4_BLOCK_SIZE_4MB:
    return LZ4_BLOCK_MAX_4MB;
  default:
    return 0;
  }
}

//
// Frame Header Building
//

gcomp_status_t lz4_write_frame_header(const lz4_frame_header_t * header,
    uint8_t * buf, size_t buf_size, size_t * header_len_out) {
  if (!header || !buf || !header_len_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  size_t pos = 0;

  // Calculate required size
  size_t required = 4 + 2 + 1; // Magic + FLG + BD + HC
  if (header->content_size_present) {
    required += 8;
  }
  if (header->dict_id_present) {
    required += 4;
  }

  if (buf_size < required) {
    return GCOMP_ERR_LIMIT;
  }

  // Magic number
  lz4_write_le32(buf + pos, LZ4_MAGIC);
  pos += 4;

  // FLG byte
  buf[pos++] = header->flg;

  // BD byte
  buf[pos++] = header->bd;

  // Content size (if present)
  if (header->content_size_present) {
    lz4_write_le64(buf + pos, header->content_size);
    pos += 8;
  }

  // Dictionary ID (if present)
  if (header->dict_id_present) {
    lz4_write_le32(buf + pos, header->dict_id);
    pos += 4;
  }

  // Header checksum: (xxHash32(FLG..end) >> 8) & 0xFF
  // The checksum covers bytes from FLG to just before HC (exclusive of magic)
  uint32_t hash = gcomp_xxhash32(buf + 4, pos - 4, 0);
  buf[pos++] = (uint8_t)((hash >> 8) & 0xFF);

  *header_len_out = pos;
  return GCOMP_OK;
}

//
// Block Size Parsing
//

gcomp_status_t lz4_parse_block_size(
    const uint8_t * buf, uint32_t * size_out, bool * uncompressed_out) {
  if (!buf || !size_out || !uncompressed_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  uint32_t raw = lz4_read_le32(buf);

  *uncompressed_out = (raw & LZ4_BLOCK_UNCOMPRESSED_FLAG) != 0;
  *size_out = raw & LZ4_BLOCK_SIZE_MASK;

  return GCOMP_OK;
}
