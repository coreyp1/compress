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

  /* Step 1: count codes at each length. */
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

  /* Step 2 (RFC 1951): smallest code for each length. */
  code = 0;
  bl_count[0] = 0;
  for (bits = 1; bits <= max_bits; bits++) {
    code = (code + bl_count[bits - 1]) << 1;
    next_code[bits] = code;
  }

  /* Over-subscribed: at length L we have at most 2^L code values. */
  for (bits = 1; bits <= max_bits; bits++) {
    if (next_code[bits] + bl_count[bits] > (1u << bits)) {
      return GCOMP_ERR_CORRUPT;
    }
  }

  /* Incomplete trees allowed in DEFLATE; only over-subscribed is rejected. */
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
// Two passes: first pass counts long_table size and sets
// long_base/long_extra_bits; then we allocate long_table and fill it in a
// second pass.
//

gcomp_status_t gcomp_deflate_huffman_build_decode_table(const uint8_t * lengths,
    size_t num_symbols, unsigned max_bits,
    gcomp_deflate_huffman_decode_table_t * table) {
  uint16_t codes[288]; /* DEFLATE literal/length max 286 + slack */
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

  table->long_table = NULL;
  table->long_table_count = 0;
  memset(table->long_base, 0, sizeof(table->long_base));
  memset(table->long_extra_bits, 0, sizeof(table->long_extra_bits));

  {
    gcomp_status_t st = gcomp_deflate_huffman_build_codes(
        lengths, num_symbols, max_bits, codes, code_lens);
    if (st != GCOMP_OK) {
      return st;
    }
  }

  /* Initialize fast table: nbits=0 means "use long table" or no code. */
  for (i = 0; i < GCOMP_DEFLATE_HUFFMAN_FAST_SIZE; i++) {
    table->fast_table[i].symbol = 0;
    table->fast_table[i].nbits = 0;
  }

  long_offset = 0;

  /* First pass: fill fast table for short codes; for long codes, compute
   * long_base/long_extra_bits and total long_table size. */
  for (i = 0; i < num_symbols; i++) {
    unsigned len = code_lens[i];
    uint16_t code = codes[i];
    unsigned j;

    if (len == 0) {
      continue;
    }

    if (len <= GCOMP_DEFLATE_HUFFMAN_FAST_BITS) {
      /* Short code: index = (code << (FAST_BITS - len)) + low; fill step
       * consecutive entries with (symbol i, nbits len). */
      unsigned step = 1u << (GCOMP_DEFLATE_HUFFMAN_FAST_BITS - len);
      unsigned start = code << (GCOMP_DEFLATE_HUFFMAN_FAST_BITS - len);
      for (j = 0; j < step; j++) {
        table->fast_table[start + j].symbol = (uint16_t)i;
        table->fast_table[start + j].nbits = (uint8_t)len;
      }
    }
    else {
      /* Long code: high = first FAST_BITS bits; we need 2^(len - FAST_BITS)
       * entries in long_table for this high. */
      unsigned extra = len - GCOMP_DEFLATE_HUFFMAN_FAST_BITS;
      unsigned high = code >> extra;

      if (table->long_extra_bits[high] == 0) {
        table->long_extra_bits[high] = (uint8_t)extra;
        table->long_base[high] = (uint16_t)long_offset;
        long_offset += (1u << extra);
      }
    }
  }

  /* Allocate long_table and fill it in second pass. */
  if (long_offset > 0) {
    alloc = gcomp_allocator_default();
    table->long_table = (gcomp_deflate_huffman_fast_entry_t *)gcomp_calloc(
        alloc, (size_t)long_offset, sizeof(gcomp_deflate_huffman_fast_entry_t));
    if (!table->long_table) {
      return GCOMP_ERR_MEMORY;
    }
    table->long_table_count = (size_t)long_offset;

    /* Second pass: re-establish long_base/long_extra_bits and fill
     * long_table. Entry for (high, low) is at long_base[high] + low. */
    memset(table->long_base, 0, sizeof(table->long_base));
    memset(table->long_extra_bits, 0, sizeof(table->long_extra_bits));
    long_offset = 0;

    for (i = 0; i < num_symbols; i++) {
      unsigned len = code_lens[i];
      uint16_t code = codes[i];

      if (len == 0) {
        continue;
      }

      if (len > GCOMP_DEFLATE_HUFFMAN_FAST_BITS) {
        unsigned extra = len - GCOMP_DEFLATE_HUFFMAN_FAST_BITS;
        unsigned high = code >> extra;
        unsigned low_mask = (1u << extra) - 1;
        unsigned low = code & low_mask;
        size_t idx;

        if (table->long_extra_bits[high] == 0) {
          table->long_extra_bits[high] = (uint8_t)extra;
          table->long_base[high] = (uint16_t)long_offset;
          long_offset += (1u << extra);
        }

        idx = (size_t)table->long_base[high] + (size_t)low;
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
  const gcomp_allocator_t * alloc;

  if (!table) {
    return;
  }

  if (table->long_table) {
    alloc = gcomp_allocator_default();
    gcomp_free(alloc, table->long_table);
    table->long_table = NULL;
    table->long_table_count = 0;
  }
}
