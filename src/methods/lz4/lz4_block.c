/**
 * @file lz4_block.c
 *
 * LZ4 block format compression and decompression.
 *
 * This file implements the LZ4 block compression algorithm as specified in:
 * https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md
 *
 * ## Block Format
 *
 * A block consists of sequences, each containing:
 * 1. Token byte: high nibble = literal length, low nibble = match length
 * 2. Optional literal length extension bytes (if literal length == 15)
 * 3. Literal bytes
 * 4. Match offset (2 bytes, little-endian)
 * 5. Optional match length extension bytes (if match length nibble == 15)
 *
 * The last sequence may omit the match section.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "lz4_internal.h"
#include <string.h>

//
// Hash function for match finding
//

static inline uint32_t lz4_hash_position(const uint8_t * p) {
  // Read 4 bytes and compute hash
  uint32_t v = lz4_read_le32(p);
  return (v * 2654435761U) >> 16;
}

//
// Block Compression
//

gcomp_status_t lz4_block_compress(const uint8_t * input, size_t input_len,
    uint8_t * output, size_t output_cap, size_t * output_len_out,
    uint32_t * hash_table, size_t hash_table_size) {
  if (!input || !output || !output_len_out || !hash_table) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (input_len == 0) {
    *output_len_out = 0;
    return GCOMP_OK;
  }

  // Clear hash table
  memset(hash_table, 0, hash_table_size * sizeof(uint32_t));

  const uint8_t * src = input;
  const uint8_t * src_end = input + input_len;
  const uint8_t * src_limit = src_end - 12; // Leave room for last literals
  const uint8_t * anchor = src;             // Start of literal run

  uint8_t * dst = output;
  uint8_t * dst_end = output + output_cap;

  // Skip first byte (no match possible)
  src++;

  while (src < src_limit) {
    // Find match in hash table
    uint32_t hash = lz4_hash_position(src) & (hash_table_size - 1);
    uint32_t match_pos = hash_table[hash];
    const uint8_t * match_ref = input + match_pos;

    // Update hash table
    hash_table[hash] = (uint32_t)(src - input);

    // Check for match (at least 4 bytes, within 64KB window)
    if (match_pos > 0 && src - match_ref <= 65535 &&
        lz4_read_le32(match_ref) == lz4_read_le32(src)) {
      // Found a match! Extend it
      size_t match_len = 4;
      while (
          src + match_len < src_end && match_ref[match_len] == src[match_len]) {
        match_len++;
      }

      // Encode literal length
      size_t lit_len = src - anchor;
      uint8_t token;

      if (lit_len >= 15) {
        token = 0xF0;
      }
      else {
        token = (uint8_t)(lit_len << 4);
      }

      // Encode match length (minus 4 for minmatch)
      size_t match_len_adj = match_len - LZ4_MIN_MATCH;
      if (match_len_adj >= 15) {
        token |= 0x0F;
      }
      else {
        token |= (uint8_t)match_len_adj;
      }

      // Check output space
      size_t needed = 1 + (lit_len >= 15 ? (lit_len - 15) / 255 + 1 : 0) +
          lit_len + 2 +
          (match_len_adj >= 15 ? (match_len_adj - 15) / 255 + 1 : 0);
      if (dst + needed > dst_end) {
        return GCOMP_ERR_LIMIT; // Compression expanded
      }

      // Write token
      *dst++ = token;

      // Write literal length extension
      if (lit_len >= 15) {
        size_t remaining = lit_len - 15;
        while (remaining >= 255) {
          *dst++ = 255;
          remaining -= 255;
        }
        *dst++ = (uint8_t)remaining;
      }

      // Write literals
      memcpy(dst, anchor, lit_len);
      dst += lit_len;

      // Write match offset (little-endian)
      uint16_t offset = (uint16_t)(src - match_ref);
      *dst++ = (uint8_t)offset;
      *dst++ = (uint8_t)(offset >> 8);

      // Write match length extension
      if (match_len_adj >= 15) {
        size_t remaining = match_len_adj - 15;
        while (remaining >= 255) {
          *dst++ = 255;
          remaining -= 255;
        }
        *dst++ = (uint8_t)remaining;
      }

      // Advance past match
      src += match_len;
      anchor = src;
    }
    else {
      src++;
    }
  }

  // Write remaining literals (last sequence has no match)
  size_t last_lit_len = src_end - anchor;
  if (last_lit_len > 0) {
    // Check output space
    size_t needed = 1 +
        (last_lit_len >= 15 ? (last_lit_len - 15) / 255 + 1 : 0) + last_lit_len;
    if (dst + needed > dst_end) {
      return GCOMP_ERR_LIMIT;
    }

    // Token for last literals (no match)
    uint8_t token;
    if (last_lit_len >= 15) {
      token = 0xF0;
    }
    else {
      token = (uint8_t)(last_lit_len << 4);
    }
    *dst++ = token;

    // Literal length extension
    if (last_lit_len >= 15) {
      size_t remaining = last_lit_len - 15;
      while (remaining >= 255) {
        *dst++ = 255;
        remaining -= 255;
      }
      *dst++ = (uint8_t)remaining;
    }

    // Literals
    memcpy(dst, anchor, last_lit_len);
    dst += last_lit_len;
  }

  *output_len_out = (size_t)(dst - output);
  return GCOMP_OK;
}

//
// Block Decompression
//

gcomp_status_t lz4_block_decompress(const uint8_t * input, size_t input_len,
    uint8_t * output, size_t output_cap, size_t * output_len_out,
    const uint8_t * history, size_t history_len) {
  if (!input || !output || !output_len_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (input_len == 0) {
    *output_len_out = 0;
    return GCOMP_OK;
  }

  const uint8_t * src = input;
  const uint8_t * src_end = input + input_len;
  uint8_t * dst = output;
  uint8_t * dst_end = output + output_cap;

  while (src < src_end) {
    // Read token
    uint8_t token = *src++;
    size_t lit_len = (token >> 4) & 0x0F;
    size_t match_len = (token & 0x0F) + LZ4_MIN_MATCH;

    // Read literal length extension
    if (lit_len == 15) {
      while (src < src_end) {
        uint8_t b = *src++;
        lit_len += b;
        if (b != 255) {
          break;
        }
      }
    }

    // Check bounds
    if (src + lit_len > src_end) {
      return GCOMP_ERR_CORRUPT;
    }
    if (dst + lit_len > dst_end) {
      return GCOMP_ERR_LIMIT;
    }

    // Copy literals
    memcpy(dst, src, lit_len);
    src += lit_len;
    dst += lit_len;

    // Check if this is the last sequence (no match section)
    if (src >= src_end) {
      break;
    }

    // Read match offset
    if (src + 2 > src_end) {
      return GCOMP_ERR_CORRUPT;
    }
    uint16_t offset = lz4_read_le16(src);
    src += 2;

    if (offset == 0) {
      return GCOMP_ERR_CORRUPT; // Invalid offset
    }

    // Read match length extension
    if ((token & 0x0F) == 15) {
      while (src < src_end) {
        uint8_t b = *src++;
        match_len += b;
        if (b != 255) {
          break;
        }
      }
    }

    // Find match source
    const uint8_t * match_src;
    size_t dst_offset = dst - output;

    if (offset <= dst_offset) {
      // Match is within output buffer
      match_src = dst - offset;
    }
    else if (history && offset <= dst_offset + history_len) {
      // Match is in history buffer
      size_t history_offset = offset - dst_offset;
      match_src = history + history_len - history_offset;
    }
    else {
      return GCOMP_ERR_CORRUPT; // Invalid back-reference
    }

    // Check output bounds
    if (dst + match_len > dst_end) {
      return GCOMP_ERR_LIMIT;
    }

    // Copy match (byte-by-byte for overlapping matches)
    for (size_t i = 0; i < match_len; i++) {
      // Handle case where match_src is in history and crosses into output
      if (match_src >= history && match_src < history + history_len) {
        *dst++ = *match_src++;
        if (match_src >= history + history_len) {
          match_src = output; // Continue from start of output
        }
      }
      else {
        *dst++ = *match_src++;
      }
    }
  }

  *output_len_out = (size_t)(dst - output);
  return GCOMP_OK;
}
