/**
 * @file lzw_core.c
 *
 * LZW core implementation: dictionary build/lookup, decode stack,
 * KwKwK case, reset behavior. Uses safe math for size calculations.
 *
 * CORE RESPONSIBILITIES
 * =====================
 *
 * This file is intentionally *container-agnostic* and *profile-agnostic*.
 * It implements the algorithmic mechanics that are shared by GIF/TIFF variants:
 *
 * - Dictionary storage using parallel arrays:
 *   - `prefix_code[code]` : previous code in the string (or sentinel)
 *   - `append_char[code]`: final byte appended to the prefix
 * - Decoder reconstruction using a reverse stack (walk prefixes then reverse)
 * - KwKwK handling (code equals next unassigned code)
 *
 * The profile layer controls:
 * - bit packing order (LSB vs MSB)
 * - when code widths increase
 * - when CLEAR/EOI appear in the bitstream
 *
 * INVARIANTS / LAYOUT
 * ===================
 *
 * - Codes 0..255 are always literal bytes (single-character strings).
 * - CLEAR and EOI codes are handled by the caller; do not pass them to
 *   `lzw_core_decoder_decode()`.
 * - `next_code` is the next available dictionary entry (first is EOI+1).
 * - `prefix_code[code] == LZW_SENTINEL` means “no prefix” (literal root).
 *
 * PERFORMANCE NOTE
 * ================
 *
 * Encoder lookup is currently O(n) via a linear scan of the table. This is
 * acceptable for correctness and test workloads. For production throughput,
 * replace `lzw_core_encoder_find()` with a hash table (prefix, byte) → code.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "lzw_core.h"
#include "../../core/alloc_internal.h"
#include "../../core/safe_math.h"

#define LZW_SENTINEL 0xFFFFu

static uint32_t first_sequence_code(uint32_t clear_code, uint32_t eoi_code) {
  (void)clear_code;
  return eoi_code + 1;
}

gcomp_status_t lzw_core_decoder_init(lzw_core_decoder_t * core,
    const gcomp_allocator_t * allocator, unsigned max_code_bits,
    uint32_t clear_code, uint32_t eoi_code) {
  if (!core || !allocator) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (max_code_bits == 0 || max_code_bits > LZW_CORE_MAX_CODE_BITS) {
    return GCOMP_ERR_INVALID_ARG;
  }

  uint32_t capacity = 1u << max_code_bits;
  size_t prefix_size, append_size, stack_size;
  if (!gcomp_safe_mul_size((size_t)capacity, sizeof(uint16_t), &prefix_size) ||
      !gcomp_safe_mul_size((size_t)capacity, sizeof(uint8_t), &append_size) ||
      !gcomp_safe_mul_size((size_t)capacity, sizeof(uint8_t), &stack_size)) {
    return GCOMP_ERR_CORRUPT;
  }

  core->prefix_code = (uint16_t *)gcomp_malloc(allocator, prefix_size);
  if (!core->prefix_code) {
    return GCOMP_ERR_MEMORY;
  }
  core->append_char = (uint8_t *)gcomp_malloc(allocator, append_size);
  if (!core->append_char) {
    gcomp_free(allocator, core->prefix_code);
    core->prefix_code = NULL;
    return GCOMP_ERR_MEMORY;
  }
  core->stack = (uint8_t *)gcomp_malloc(allocator, stack_size);
  if (!core->stack) {
    gcomp_free(allocator, core->append_char);
    gcomp_free(allocator, core->prefix_code);
    core->prefix_code = NULL;
    core->append_char = NULL;
    return GCOMP_ERR_MEMORY;
  }

  core->allocator = allocator;
  core->capacity = capacity;
  core->next_code = first_sequence_code(clear_code, eoi_code);
  core->prev_code = 0;
  core->prev_first_byte = 0;
  core->has_prev = 0;

  for (uint32_t i = 0; i < 256u; i++) {
    core->prefix_code[i] = (uint16_t)LZW_SENTINEL;
    core->append_char[i] = (uint8_t)i;
  }
  for (uint32_t i = 256; i < capacity; i++) {
    core->prefix_code[i] = (uint16_t)LZW_SENTINEL;
    core->append_char[i] = 0;
  }

  return GCOMP_OK;
}

void lzw_core_decoder_reset(
    lzw_core_decoder_t * core, uint32_t clear_code, uint32_t eoi_code) {
  if (!core || !core->prefix_code) {
    return;
  }
  core->next_code = first_sequence_code(clear_code, eoi_code);
  core->prev_code = 0;
  core->prev_first_byte = 0;
  core->has_prev = 0;
  for (uint32_t i = 256; i < core->capacity; i++) {
    core->prefix_code[i] = (uint16_t)LZW_SENTINEL;
    core->append_char[i] = 0;
  }
}

void lzw_core_decoder_destroy(lzw_core_decoder_t * core) {
  if (!core || !core->allocator) {
    return;
  }
  const gcomp_allocator_t * a = core->allocator;
  if (core->prefix_code) {
    gcomp_free(a, core->prefix_code);
    core->prefix_code = NULL;
  }
  if (core->append_char) {
    gcomp_free(a, core->append_char);
    core->append_char = NULL;
  }
  if (core->stack) {
    gcomp_free(a, core->stack);
    core->stack = NULL;
  }
  core->capacity = 0;
}

// Decode code into stack (reverse order: stack[0] is last byte output).
// Returns number of bytes pushed. Code must be valid (literal or in table).
static size_t decode_to_stack(const lzw_core_decoder_t * core, uint32_t code,
    uint8_t * stack, uint32_t capacity, uint8_t * first_byte_out) {
  size_t n = 0;
  uint32_t c = code;

  while (c > 257u && c < capacity && core->prefix_code[c] != LZW_SENTINEL) {
    if (n >= capacity) {
      return 0;
    }
    stack[n++] = core->append_char[c];
    c = core->prefix_code[c];
  }
  if (n >= capacity) {
    return 0;
  }
  stack[n++] = (uint8_t)(c & 0xFFu);
  *first_byte_out = (uint8_t)(c & 0xFFu);

  return n;
}

gcomp_status_t lzw_core_decoder_decode(lzw_core_decoder_t * core, uint32_t code,
    uint8_t * output_data, size_t output_size, size_t * output_used) {
  if (!core || !core->prefix_code || !output_data || !output_used) {
    return GCOMP_ERR_INVALID_ARG;
  }

  uint32_t next = core->next_code;
  if (code > next) {
    return GCOMP_ERR_CORRUPT;
  }

  size_t used = *output_used;
  uint8_t first_byte;
  size_t n;

  if (code == next) {
    // KwKwK: string is prev_string + first_byte(prev_string)
    if (!core->has_prev) {
      return GCOMP_ERR_CORRUPT;
    }
    {
      uint32_t prev = core->prev_code;
      uint8_t prev_first = core->prev_first_byte;
      n = decode_to_stack(core, prev, core->stack, core->capacity, &first_byte);
      if (n == 0) {
        return GCOMP_ERR_CORRUPT;
      }
      first_byte = prev_first;
    }
  }
  else {
    n = decode_to_stack(core, code, core->stack, core->capacity, &first_byte);
    if (n == 0) {
      return GCOMP_ERR_CORRUPT;
    }
  }

  size_t to_write = (code == next) ? n + 1 : n;
  size_t new_used;
  if (!gcomp_safe_add_size(used, to_write, &new_used) ||
      new_used > output_size) {
    return GCOMP_ERR_LIMIT;
  }

  for (size_t i = n; i > 0; i--) {
    output_data[used++] = core->stack[i - 1];
  }
  if (code == next) {
    output_data[used++] = core->prev_first_byte;
  }

  // Add new table entry: (prev_code, first_byte)
  if (core->next_code < core->capacity) {
    core->prefix_code[core->next_code] = (uint16_t)(core->prev_code & 0xFFFFu);
    core->append_char[core->next_code] = first_byte;
    core->next_code++;
  }

  core->prev_code = code;
  core->prev_first_byte = first_byte;
  core->has_prev = 1;
  *output_used = used;
  return GCOMP_OK;
}

gcomp_status_t lzw_core_encoder_init(lzw_core_encoder_t * core,
    const gcomp_allocator_t * allocator, unsigned max_code_bits,
    uint32_t clear_code, uint32_t eoi_code) {
  if (!core || !allocator) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (max_code_bits == 0 || max_code_bits > LZW_CORE_MAX_CODE_BITS) {
    return GCOMP_ERR_INVALID_ARG;
  }

  uint32_t capacity = 1u << max_code_bits;
  size_t prefix_size, append_size;
  if (!gcomp_safe_mul_size((size_t)capacity, sizeof(uint16_t), &prefix_size) ||
      !gcomp_safe_mul_size((size_t)capacity, sizeof(uint8_t), &append_size)) {
    return GCOMP_ERR_CORRUPT;
  }

  core->prefix_code = (uint16_t *)gcomp_malloc(allocator, prefix_size);
  if (!core->prefix_code) {
    return GCOMP_ERR_MEMORY;
  }
  core->append_char = (uint8_t *)gcomp_malloc(allocator, append_size);
  if (!core->append_char) {
    gcomp_free(allocator, core->prefix_code);
    core->prefix_code = NULL;
    return GCOMP_ERR_MEMORY;
  }

  core->allocator = allocator;
  core->capacity = capacity;
  core->next_code = first_sequence_code(clear_code, eoi_code);

  for (uint32_t i = 0; i < 256u; i++) {
    core->prefix_code[i] = (uint16_t)LZW_SENTINEL;
    core->append_char[i] = (uint8_t)i;
  }
  for (uint32_t i = 256; i < capacity; i++) {
    core->prefix_code[i] = (uint16_t)LZW_SENTINEL;
    core->append_char[i] = 0;
  }

  return GCOMP_OK;
}

void lzw_core_encoder_reset(
    lzw_core_encoder_t * core, uint32_t clear_code, uint32_t eoi_code) {
  if (!core || !core->prefix_code) {
    return;
  }
  core->next_code = first_sequence_code(clear_code, eoi_code);
  for (uint32_t i = 256; i < core->capacity; i++) {
    core->prefix_code[i] = (uint16_t)LZW_SENTINEL;
    core->append_char[i] = 0;
  }
}

void lzw_core_encoder_destroy(lzw_core_encoder_t * core) {
  if (!core || !core->allocator) {
    return;
  }
  const gcomp_allocator_t * a = core->allocator;
  if (core->prefix_code) {
    gcomp_free(a, core->prefix_code);
    core->prefix_code = NULL;
  }
  if (core->append_char) {
    gcomp_free(a, core->append_char);
    core->append_char = NULL;
  }
  core->capacity = 0;
}

int lzw_core_encoder_find(const lzw_core_encoder_t * core, uint32_t prefix,
    uint8_t byte, uint32_t * code_out) {
  if (!core || !core->prefix_code || !code_out) {
    return 0;
  }
  uint32_t next = core->next_code;
  for (uint32_t i = 258; i < next; i++) {
    if (core->prefix_code[i] == (uint16_t)(prefix & 0xFFFFu) &&
        core->append_char[i] == byte) {
      *code_out = i;
      return 1;
    }
  }
  return 0;
}

uint32_t lzw_core_encoder_add(
    lzw_core_encoder_t * core, uint32_t prefix, uint8_t byte) {
  if (!core || core->next_code >= core->capacity) {
    return 0;
  }
  uint32_t code = core->next_code++;
  core->prefix_code[code] = (uint16_t)(prefix & 0xFFFFu);
  core->append_char[code] = byte;
  return code;
}

int lzw_core_encoder_is_full(const lzw_core_encoder_t * core) {
  return core ? (core->next_code >= core->capacity) : 1;
}
