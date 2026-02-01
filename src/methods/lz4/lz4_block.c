/**
 * @file lz4_block.c
 *
 * LZ4 block format compression and decompression.
 *
 * This file implements the LZ4 block compression algorithm as specified in:
 * https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md
 *
 * ## Block Format Overview
 *
 * LZ4 block format uses a sequence-based structure optimized for fast
 * decompression. Each sequence represents either literal bytes or a
 * back-reference to previously seen data.
 *
 * ```
 * ┌─────────┬───────────────┬──────────┬────────┬───────────────┐
 * │  Token  │  Lit Length   │ Literals │ Offset │ Match Length  │
 * │ (1 byte)│   (0+ bytes)  │(variable)│(2 bytes│  (0+ bytes)   │
 * └─────────┴───────────────┴──────────┴────────┴───────────────┘
 *      │                                    │
 *      └─ High 4 bits: literal length       │
 *         Low 4 bits: match length - 4      │
 *                                           └─ Little-endian, 1-65535
 * ```
 *
 * ## Token Byte Encoding
 *
 * The token byte encodes two values:
 * - **High nibble (4 bits)**: Literal length (0-14, or 15 = extended)
 * - **Low nibble (4 bits)**: Match length minus 4 (0-14, or 15 = extended)
 *
 * When either nibble is 15, additional extension bytes follow:
 * - Each 0xFF byte adds 255 to the length
 * - The first non-0xFF byte adds its value and terminates the extension
 *
 * Example: Literal length 300 = 15 + 255 + 30 → token high nibble 15,
 * followed by bytes 0xFF (255) and 0x1E (30).
 *
 * ## Match Mechanics
 *
 * - **Minimum match**: 4 bytes (encoded length 0 in token)
 * - **Offset range**: 1-65535 bytes back (offset 0 is invalid)
 * - **Overlapping matches**: When match length > offset, the pattern repeats.
 *   For example, offset=1, length=10 copies the previous byte 10 times.
 *
 * ## Compression Algorithm (lz4_block_compress)
 *
 * The compressor uses a simple, fast LZ77-style algorithm:
 *
 * 1. **Hash table**: Maps 4-byte sequences to their last occurrence position
 *    - Hash function: multiply first 4 bytes by prime 2654435761, shift >> 16
 *    - Table size: 64K entries (covers 64KB window)
 *    - Single entry per hash (no chaining)
 *
 * 2. **Greedy matching**: At each position, check if hash table has a match
 *    - Match must be within 64KB window
 *    - Match must be at least 4 bytes
 *    - Extend match as far as possible (but not past `match_limit`)
 *
 * 3. **Literal accumulation**: Non-matching bytes accumulate as literals
 *
 * 4. **Last literals requirement** (critical for spec compliance):
 *    - The LZ4 spec mandates: "The last sequence is incomplete, and stops
 *      right after literals field."
 *    - This means the final sequence MUST be literals-only (no match).
 *    - We enforce this by:
 *      a. Not starting matches in the last 12 bytes (MFLIMIT - for safe reads)
 *      b. Not extending matches past `src_end - 5` (LZ4_LAST_LITERALS)
 *      c. Always emitting a final literals-only sequence with >= 5 bytes
 *    - Violating this produces output that reference decoders reject.
 *
 * The algorithm prioritizes speed over compression ratio. For better
 * compression, consider using higher-level strategies like HC (hash chains)
 * which are not implemented here.
 *
 * ## Decompression Algorithm (lz4_block_decompress)
 *
 * The decompressor is straightforward and very fast:
 *
 * 1. Read token byte
 * 2. Decode literal length (with optional extension)
 * 3. Copy literal bytes from input to output
 * 4. If not at end of block:
 *    a. Read 2-byte offset (little-endian)
 *    b. Decode match length (with optional extension, plus 4)
 *    c. Copy match bytes from (output - offset) to output
 * 5. Repeat until input exhausted
 *
 * ## History Buffer Support
 *
 * For dependent blocks (block independence = false), matches can reference
 * data from previous blocks. The `history` and `history_len` parameters
 * provide this context. When a match offset exceeds the current output
 * position, the decompressor looks in the history buffer.
 *
 * ## Error Cases
 *
 * The decompressor returns GCOMP_ERR_CORRUPT for:
 * - Offset == 0 (invalid back-reference)
 * - Offset exceeds (output position + history length)
 * - Input exhausted before block complete
 * - Literal length exceeds remaining input
 *
 * Returns GCOMP_ERR_LIMIT if output buffer is too small.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../core/safe_math.h"
#include "lz4_internal.h"
#include <string.h>

//
// Hash function for match finding
//

static inline uint32_t lz4_hash_position(const uint8_t * p) {
  // Read 4 bytes and compute hash
  uint32_t v = gcomp_read_le32(p);
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
  // The last 5 bytes MUST be literals (per LZ4 spec), and we reserve some
  // buffer for safe 4-byte reads during match finding.
  // MFLIMIT ensures we don't start a match too close to the end.
  const uint8_t * src_limit = src_end - 12; // Don't start matches in last 12 bytes
  // Match limit: matches must end before the last 5 bytes (last literals requirement)
  const uint8_t * match_limit = src_end - 5;
  const uint8_t * anchor = src;             // Start of literal run

  uint8_t * dst = output;
  uint8_t * dst_end = output + output_cap;

  // Handle small inputs: if < 5 bytes, just write as literals
  if (input_len < LZ4_LAST_LITERALS) {
    // Token for last literals (no match)
    size_t needed = 1 + (input_len >= 15 ? (input_len - 15) / 255 + 1 : 0) + input_len;
    if (needed > output_cap) {
      return GCOMP_ERR_LIMIT;
    }
    uint8_t token = (input_len >= 15) ? 0xF0 : (uint8_t)(input_len << 4);
    *dst++ = token;
    if (input_len >= 15) {
      size_t remaining = input_len - 15;
      while (remaining >= 255) {
        *dst++ = 255;
        remaining -= 255;
      }
      *dst++ = (uint8_t)remaining;
    }
    memcpy(dst, input, input_len);
    dst += input_len;
    *output_len_out = (size_t)(dst - output);
    return GCOMP_OK;
  }

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
        gcomp_read_le32(match_ref) == gcomp_read_le32(src)) {
      // Found a match! Extend it, but not past match_limit
      size_t match_len = 4;
      while (
          src + match_len < match_limit && match_ref[match_len] == src[match_len]) {
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

    // Read literal length extension (with overflow protection)
    if (lit_len == 15) {
      while (src < src_end) {
        uint8_t b = *src++;
        if (!gcomp_safe_add_size(lit_len, b, &lit_len)) {
          return GCOMP_ERR_CORRUPT; // Overflow in literal length
        }
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
    uint16_t offset = gcomp_read_le16(src);
    src += 2;

    if (offset == 0) {
      return GCOMP_ERR_CORRUPT; // Invalid offset
    }

    // Read match length extension (with overflow protection)
    if ((token & 0x0F) == 15) {
      while (src < src_end) {
        uint8_t b = *src++;
        if (!gcomp_safe_add_size(match_len, b, &match_len)) {
          return GCOMP_ERR_CORRUPT; // Overflow in match length
        }
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
      if (history && match_src >= history && match_src < history + history_len) {
        *dst++ = *match_src++;
        // Check boundary AFTER increment for next iteration
        if (match_src >= history + history_len) {
          match_src = output; // Continue from start of output
        }
      }
      else if (match_src >= output && match_src < dst) {
        // Match is in already-decompressed output - valid for overlapping matches
        *dst++ = *match_src++;
      }
      else {
        // Pointer is outside valid ranges - corrupt data
        return GCOMP_ERR_CORRUPT;
      }
    }
  }

  *output_len_out = (size_t)(dst - output);
  return GCOMP_OK;
}
