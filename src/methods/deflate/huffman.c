/**
 * @file huffman.c
 *
 * Canonical Huffman table builder for DEFLATE (RFC 1951). Builds codes from
 * code lengths, validates over-subscribed/incomplete trees, and builds
 * two-level fast decode tables. See huffman.h for creation/usage overview.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "huffman.h"
#include "../../core/alloc_internal.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

//
// Validation algorithm (RFC 1951 Section 3.2.2 style)
//
// We reject over-subscribed trees: at each bit length L, we can assign at
// most 2^L codes. Steps:
// 1. Count how many symbols have each code length -> bl_count[L].
// 2. Compute the smallest code value for each length (next_code[L]) using the
//    recurrence: next_code[L] = (next_code[L-1] + bl_count[L-1]) << 1.
// 3. Check that next_code[L] + bl_count[L] <= 2^L for all L (otherwise we
//    would assign a code that doesn't fit in L bits).
// Incomplete trees (Kraft sum < 1) are allowed in DEFLATE and not rejected.
//

gcomp_status_t gcomp_deflate_huffman_validate(
    const uint8_t * lengths, size_t num_symbols, unsigned max_bits) {
  uint32_t bl_count[GCOMP_DEFLATE_HUFFMAN_MAX_BITS + 1];
  uint32_t next_code[GCOMP_DEFLATE_HUFFMAN_MAX_BITS + 1];
  uint32_t code;
  size_t i;
  unsigned bits;

  if (!lengths) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (max_bits == 0 || max_bits > GCOMP_DEFLATE_HUFFMAN_MAX_BITS) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Step 1: count codes at each length.
  memset(bl_count, 0, sizeof(bl_count));
  for (i = 0; i < num_symbols; i++) {
    uint32_t len = lengths[i];
    if (len > max_bits) {
      return GCOMP_ERR_CORRUPT;
    }
    if (len > 0) {
      bl_count[len]++;
    }
  }

  // Step 2 (RFC 1951): smallest code for each length.
  code = 0;
  bl_count[0] = 0;
  for (bits = 1; bits <= max_bits; bits++) {
    code = (code + bl_count[bits - 1]) << 1;
    next_code[bits] = code;
  }

  // Over-subscribed: at length L we have at most 2^L code values.
  for (bits = 1; bits <= max_bits; bits++) {
    if (next_code[bits] + bl_count[bits] > (1u << bits)) {
      return GCOMP_ERR_CORRUPT;
    }
  }

  // Incomplete trees allowed in DEFLATE; only over-subscribed is rejected.
  return GCOMP_OK;
}

//
// Canonical code assignment (RFC 1951 Section 3.2.2, steps 1-3)
//
// Given code lengths per symbol, assign integer code values so that:
// - Shorter codes have smaller values.
// - Same-length codes get consecutive values (lexicographic order).
// Algorithm:
// 1. bl_count[L] = number of symbols with code length L.
// 2. next_code[L] = smallest code value for length L (recurrence as above).
// 3. For each symbol i with length len > 0: codes[i] = next_code[len]; then
//    next_code[len]++. Symbols with length 0 get no code.
//

gcomp_status_t gcomp_deflate_huffman_build_codes(const uint8_t * lengths,
    size_t num_symbols, unsigned max_bits, uint16_t * codes,
    uint8_t * code_lens) {
  uint32_t bl_count[GCOMP_DEFLATE_HUFFMAN_MAX_BITS + 1];
  uint32_t next_code[GCOMP_DEFLATE_HUFFMAN_MAX_BITS + 1];
  uint32_t code;
  size_t i;
  unsigned bits;

  if (!lengths || !codes) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (max_bits == 0 || max_bits > GCOMP_DEFLATE_HUFFMAN_MAX_BITS) {
    return GCOMP_ERR_INVALID_ARG;
  }

  {
    gcomp_status_t st =
        gcomp_deflate_huffman_validate(lengths, num_symbols, max_bits);
    if (st != GCOMP_OK) {
      return st;
    }
  }

  memset(bl_count, 0, sizeof(bl_count));

  for (i = 0; i < num_symbols; i++) {
    uint32_t len = lengths[i];
    if (len > 0) {
      bl_count[len]++;
    }
  }

  code = 0;
  bl_count[0] = 0;
  for (bits = 1; bits <= max_bits; bits++) {
    code = (code + bl_count[bits - 1]) << 1;
    next_code[bits] = code;
  }

  for (i = 0; i < num_symbols; i++) {
    unsigned len = lengths[i];
    if (len != 0) {
      codes[i] = (uint16_t)next_code[len];
      next_code[len]++;
      if (code_lens) {
        code_lens[i] = (uint8_t)len;
      }
    }
    else {
      // Ensure zero-length symbols have zero codes/code_lens to avoid using
      // uninitialized stack values.
      codes[i] = 0;
      if (code_lens) {
        code_lens[i] = 0;
      }
    }
  }

  return GCOMP_OK;
}

//
// Two-level decode table construction
//
// Goal: decode one symbol by peeking at most FAST_BITS bits, then either
// resolve immediately (short codes) or read a few more bits and index into
// long_table (long codes).
//
// Short codes (length L <= FAST_BITS):
//   Code value C occupies L bits. When the decoder peeks FAST_BITS bits, the
//   index is (C << (FAST_BITS - L)) + (low bits from stream). So we fill
//   fast_table[start .. start+step-1] with (symbol, L) where start = C <<
//   (FAST_BITS - L) and step = 2^(FAST_BITS - L).
//
// Long codes (length L > FAST_BITS):
//   The first FAST_BITS bits of the code give index "high". We set
//   fast_table[high].nbits = 0 so the decoder reads more bits. The remaining
//   (L - FAST_BITS) bits form "low". We store (symbol, L) in long_table at
//   long_base[high] + low. So we need 2^(L - FAST_BITS) entries per distinct
//   "high" that has long codes.
//
// IMPORTANT: Mixed-length codes sharing the same prefix
// -----------------------------------------------------
// Multiple codes with DIFFERENT lengths can share the same FAST_BITS prefix.
// For example, with FAST_BITS=9, codes at lengths 11, 12, and 13 might all
// share prefix 511:
//   - 11-bit code 0x7FC: high = 0x7FC >> 2 = 511, extra = 2
//   - 12-bit code 0xFFE: high = 0xFFE >> 3 = 511, extra = 3
//   - 13-bit code 0x1FFF: high = 0x1FFF >> 4 = 511, extra = 4
//
// The long_table must accommodate ALL these codes. We allocate based on the
// MAXIMUM extra bits for each prefix (4 in this example = 16 entries).
//
// Shorter codes must be REPLICATED to fill all matching bit patterns. When
// the decoder reads max_extra bits from the stream, shorter codes have their
// actual low bits in the HIGH part of the extended value, with trailing bits
// (from the next symbol's code) in the LOW part.
//
// Example: An 11-bit code with extra=2 (actual low bits = 2 bits) in a table
// using max_extra=4. The decoder reads 4 bits as "low":
//   extended_low = (actual_low << 2) | trailing_bits
// where trailing_bits can be 0-3. So we fill indices:
//   base + (actual_low << 2) + 0
//   base + (actual_low << 2) + 1
//   base + (actual_low << 2) + 2
//   base + (actual_low << 2) + 3
// All with the same (symbol, actual_length=11).
//
// Algorithm (two passes):
//   Pass 1: For each long code, track maximum extra bits per prefix.
//           Then compute long_base[] and total long_table size.
//   Pass 2: Allocate long_table. For each long code:
//           - If extra == max_extra: fill single entry
//           - If extra < max_extra: replicate to 2^(max_extra - extra) entries
//

gcomp_status_t gcomp_deflate_huffman_build_decode_table(
    const gcomp_allocator_t * allocator, const uint8_t * lengths,
    size_t num_symbols, unsigned max_bits,
    gcomp_deflate_huffman_decode_table_t * table) {
  uint16_t codes[288]; // DEFLATE literal/length max 286 + slack
  uint8_t code_lens[288];
  size_t i;
  uint16_t long_offset;
  const gcomp_allocator_t * alloc;

  if (!lengths || !table) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (num_symbols > 288) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (max_bits == 0 || max_bits > GCOMP_DEFLATE_HUFFMAN_MAX_BITS) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Use provided allocator or default
  alloc = gcomp_alloc_or_default(allocator);

  table->long_table = NULL;
  table->long_table_count = 0;
  table->allocator = alloc;
  memset(table->long_base, 0, sizeof(table->long_base));
  memset(table->long_extra_bits, 0, sizeof(table->long_extra_bits));

  {
    gcomp_status_t st = gcomp_deflate_huffman_build_codes(
        lengths, num_symbols, max_bits, codes, code_lens);
    if (st != GCOMP_OK) {
      return st;
    }
  }

  // Initialize fast table: nbits=0 means "use long table" or no code.
  for (i = 0; i < GCOMP_DEFLATE_HUFFMAN_FAST_SIZE; i++) {
    table->fast_table[i].symbol = 0;
    table->fast_table[i].nbits = 0;
  }

  long_offset = 0;

  // First pass: fill fast table for short codes; for long codes, compute
  // long_base/long_extra_bits and total long_table size.
  for (i = 0; i < num_symbols; i++) {
    unsigned len = code_lens[i];
    uint16_t code = codes[i];
    unsigned j;

    if (len == 0) {
      continue;
    }

    if (len <= GCOMP_DEFLATE_HUFFMAN_FAST_BITS) {
      // Short code: index = (code << (FAST_BITS - len)) + low; fill step
      // consecutive entries with (symbol i, nbits len).
      unsigned step = 1u << (GCOMP_DEFLATE_HUFFMAN_FAST_BITS - len);
      unsigned start = code << (GCOMP_DEFLATE_HUFFMAN_FAST_BITS - len);
      if (start + step > GCOMP_DEFLATE_HUFFMAN_FAST_SIZE) {
        return GCOMP_ERR_CORRUPT;
      }
      for (j = 0; j < step; j++) {
        table->fast_table[start + j].symbol = (uint16_t)i;
        table->fast_table[start + j].nbits = (uint8_t)len;
      }
    }
    else {
      // Long code: high = first FAST_BITS bits; we need 2^(len - FAST_BITS)
      // entries in long_table for this high.
      //
      // IMPORTANT: Multiple codes can share the same high prefix but have
      // different lengths. We must track the MAXIMUM extra bits needed for
      // each prefix to allocate enough space for all codes.
      unsigned extra = len - GCOMP_DEFLATE_HUFFMAN_FAST_BITS;
      unsigned high = code >> extra;

      // Update to maximum extra bits for this prefix
      if (table->long_extra_bits[high] < extra) {
        table->long_extra_bits[high] = (uint8_t)extra;
      }
    }
  }

  // Calculate long_offset based on maximum extra bits for each prefix
  for (i = 0; i < GCOMP_DEFLATE_HUFFMAN_FAST_SIZE; i++) {
    if (table->long_extra_bits[i] > 0) {
      table->long_base[i] = (uint16_t)long_offset;
      long_offset += (1u << table->long_extra_bits[i]);
    }
  }

  // Allocate long_table and fill it in second pass.
  if (long_offset > 0) {
    table->long_table = (gcomp_deflate_huffman_fast_entry_t *)gcomp_calloc(
        alloc, (size_t)long_offset, sizeof(gcomp_deflate_huffman_fast_entry_t));
    if (!table->long_table) {
      return GCOMP_ERR_MEMORY;
    }
    table->long_table_count = (size_t)long_offset;

    // Second pass: fill long_table. Entry for (high, low) is at
    // long_base[high] + low, where low uses the maximum extra bits for that
    // prefix. Shorter codes must be replicated to fill all matching patterns.
    for (i = 0; i < num_symbols; i++) {
      unsigned len = code_lens[i];
      uint16_t code = codes[i];

      if (len == 0 || len <= GCOMP_DEFLATE_HUFFMAN_FAST_BITS) {
        continue;
      }

      unsigned extra = len - GCOMP_DEFLATE_HUFFMAN_FAST_BITS;
      unsigned high = code >> extra;
      unsigned max_extra = table->long_extra_bits[high];
      unsigned low_bits = code & ((1u << extra) - 1);

      // If this code has fewer extra bits than the max for this prefix,
      // we need to replicate it to all matching patterns in the larger table.
      //
      // When the decoder reads max_extra bits and reverses them, shorter codes
      // have their actual low bits in the HIGH part of the extended low value,
      // with trailing bits (from the next code) in the LOW part.
      //
      // So extended_low = (actual_low << diff) | trailing_bits
      // We need to fill all combinations of trailing_bits (0 to 2^diff - 1).
      if (extra < max_extra) {
        unsigned diff = max_extra - extra;
        unsigned step = 1u << diff;
        for (unsigned j = 0; j < step; j++) {
          unsigned low = (low_bits << diff) | j;
          size_t idx = (size_t)table->long_base[high] + (size_t)low;
          table->long_table[idx].symbol = (uint16_t)i;
          table->long_table[idx].nbits = (uint8_t)len;
        }
      }
      else {
        // Code uses maximum extra bits, single entry
        size_t idx = (size_t)table->long_base[high] + (size_t)low_bits;
        table->long_table[idx].symbol = (uint16_t)i;
        table->long_table[idx].nbits = (uint8_t)len;
      }
    }
  }

  return GCOMP_OK;
}

//
// Release heap memory for long_table only (fast_table is inline in the struct).
//

void gcomp_deflate_huffman_decode_table_cleanup(
    gcomp_deflate_huffman_decode_table_t * table) {
  if (!table) {
    return;
  }

  if (table->long_table) {
    // Use the allocator stored during build, or default if not set
    const gcomp_allocator_t * alloc =
        gcomp_alloc_or_default(table->allocator);
    gcomp_free(alloc, table->long_table);
    table->long_table = NULL;
    table->long_table_count = 0;
  }
}
