/**
 * @file lzw_hash.c
 *
 * LZW encoder hash table: open-addressing (prefix, byte) → code.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>
#include "lzw_hash.h"
#include "../../core/alloc_internal.h"
#include <ghoti.io/cutil/safemath.h>
#include <string.h>

#define LZW_HASH_EMPTY_PREFIX 0xFFFFu

struct lzw_encoder_hash_s {
  uint32_t size;     ///< Number of slots (power of 2).
  uint32_t mask;     ///< size - 1, for wrapping without a divide.
  uint16_t * prefix; ///< Slot key prefix [size].
  uint8_t * byte;    ///< Slot key byte [size].
  uint16_t * code;   ///< Slot value code [size].
};

// The slot count is a power of two, but it is a runtime value, so the
// compiler cannot prove that and emits a 64-bit hardware divide for `%` --
// around 30-40 cycles, and not pipelined.  Masking is the same operation on a
// power-of-two size and costs one instruction.
//
// This is worth having but it is not where the encoder's time went: replacing
// the divides here moved a 42-file corpus from 2.5 to 2.6 MB/s.  The cost was
// somewhere else entirely -- the hash table was not being used at all.  See
// the note on state->use_hash in lzw_encoder.c.
static uint32_t hash_key(uint32_t prefix, uint8_t byte, uint32_t mask) {
  uint32_t k = (prefix & 0xFFFFu) * 257u + (uint32_t)byte;
  return k & mask;
}

gcomp_status_t lzw_encoder_hash_init(lzw_encoder_hash_t ** out,
    const gcomp_allocator_t * allocator, uint32_t capacity,
    gcomp_memory_tracker_t * mem_tracker, size_t * bytes_tracked) {
  if (!out || !allocator || capacity == 0) {
    return GCOMP_ERR_INVALID_ARG;
  }

  uint32_t size = capacity * 2u;
  if (size < capacity) {
    return GCOMP_ERR_CORRUPT; /* overflow */
  }

  // Round up so the power-of-two assumption the mask depends on holds for any
  // capacity, not just the powers of two the callers happen to pass today.
  {
    uint32_t pow2 = 1u;
    while (pow2 < size) {
      pow2 <<= 1;
      if (pow2 == 0u) {
        return GCOMP_ERR_CORRUPT; /* overflow */
      }
    }
    size = pow2;
  }

  size_t prefix_bytes, byte_bytes, code_bytes, struct_bytes, total;
  if (!gcu_safe_mul_size((size_t)size, sizeof(uint16_t), &prefix_bytes) ||
      !gcu_safe_mul_size((size_t)size, sizeof(uint8_t), &byte_bytes) ||
      !gcu_safe_mul_size((size_t)size, sizeof(uint16_t), &code_bytes) ||
      !gcu_safe_add_size(prefix_bytes, byte_bytes, &total) ||
      !gcu_safe_add_size(total, code_bytes, &total) ||
      !gcu_safe_add_size(
          total, sizeof(struct lzw_encoder_hash_s), &struct_bytes)) {
    return GCOMP_ERR_CORRUPT;
  }
  total = prefix_bytes + byte_bytes + code_bytes;

  lzw_encoder_hash_t * h =
      (lzw_encoder_hash_t *)gcomp_malloc(allocator, struct_bytes);
  if (!h) {
    return GCOMP_ERR_MEMORY;
  }

  h->size = size;
  h->mask = size - 1u;
  h->prefix = (uint16_t *)(h + 1);
  h->byte = (uint8_t *)(h->prefix + size);
  h->code = (uint16_t *)(h->byte + size);

  // LZW_HASH_EMPTY_PREFIX is 0xFFFF, so a byte fill sets every slot empty.
  memset(h->prefix, 0xFF, (size_t)size * sizeof(uint16_t));

  if (mem_tracker && bytes_tracked) {
    *bytes_tracked = total + sizeof(struct lzw_encoder_hash_s);
    gcomp_memory_track_alloc(mem_tracker, *bytes_tracked);
  }

  *out = h;
  return GCOMP_OK;
}

void lzw_encoder_hash_destroy(lzw_encoder_hash_t ** h,
    const gcomp_allocator_t * allocator, gcomp_memory_tracker_t * mem_tracker,
    size_t bytes_tracked) {
  if (!h || !*h) {
    return;
  }
  if (mem_tracker && bytes_tracked != 0) {
    gcomp_memory_track_free(mem_tracker, bytes_tracked);
  }
  gcomp_free(allocator, *h);
  *h = NULL;
}

void lzw_encoder_hash_reset(lzw_encoder_hash_t * h) {
  if (!h || !h->prefix) {
    return;
  }
  memset(h->prefix, 0xFF, (size_t)h->size * sizeof(uint16_t));
}

int lzw_encoder_hash_find(lzw_encoder_hash_t * h,
    const lzw_core_encoder_t * core, uint32_t prefix, uint8_t byte,
    uint32_t * code_out) {
  if (!h || !h->prefix || !core || !code_out) {
    return 0;
  }
  uint32_t idx = hash_key(prefix, byte, h->mask);
  uint16_t p16 = (uint16_t)(prefix & 0xFFFFu);

  for (uint32_t n = 0; n < h->size; n++) {
    if (h->prefix[idx] == (uint16_t)LZW_HASH_EMPTY_PREFIX) {
      return 0;
    }
    if (h->prefix[idx] == p16 && h->byte[idx] == byte) {
      *code_out = (uint32_t)h->code[idx];
      return 1;
    }
    idx = (idx + 1u) & h->mask;
  }
  return 0;
}

void lzw_encoder_hash_insert(
    lzw_encoder_hash_t * h, uint32_t prefix, uint8_t byte, uint32_t code) {
  if (!h || !h->prefix || code > 0xFFFFu) {
    return;
  }
  uint32_t idx = hash_key(prefix, byte, h->mask);
  uint16_t p16 = (uint16_t)(prefix & 0xFFFFu);

  for (uint32_t n = 0; n < h->size; n++) {
    if (h->prefix[idx] == (uint16_t)LZW_HASH_EMPTY_PREFIX) {
      h->prefix[idx] = p16;
      h->byte[idx] = byte;
      h->code[idx] = (uint16_t)code;
      return;
    }
    idx = (idx + 1u) & h->mask;
  }
  /* Table full (should not happen if we only insert up to capacity) */
}
