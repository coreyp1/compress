/**
 * @file lzw_hash.c
 *
 * LZW encoder hash table: open-addressing (prefix, byte) → code.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "lzw_hash.h"
#include "../../core/alloc_internal.h"
#include "../../core/safe_math.h"
#include <string.h>

#define LZW_HASH_EMPTY_PREFIX 0xFFFFu

struct lzw_encoder_hash_s {
  uint32_t size;     ///< Number of slots (power of 2).
  uint16_t * prefix; ///< Slot key prefix [size].
  uint8_t * byte;    ///< Slot key byte [size].
  uint16_t * code;   ///< Slot value code [size].
};

static uint32_t hash_key(uint32_t prefix, uint8_t byte, uint32_t size) {
  uint32_t k = (prefix & 0xFFFFu) * 257u + (uint32_t)byte;
  return k % size;
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

  size_t prefix_bytes, byte_bytes, code_bytes, struct_bytes, total;
  if (!gcomp_safe_mul_size((size_t)size, sizeof(uint16_t), &prefix_bytes) ||
      !gcomp_safe_mul_size((size_t)size, sizeof(uint8_t), &byte_bytes) ||
      !gcomp_safe_mul_size((size_t)size, sizeof(uint16_t), &code_bytes) ||
      !gcomp_safe_add_size(prefix_bytes, byte_bytes, &total) ||
      !gcomp_safe_add_size(total, code_bytes, &total) ||
      !gcomp_safe_add_size(
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
  h->prefix = (uint16_t *)(h + 1);
  h->byte = (uint8_t *)(h->prefix + size);
  h->code = (uint16_t *)(h->byte + size);

  for (uint32_t i = 0; i < size; i++) {
    h->prefix[i] = (uint16_t)LZW_HASH_EMPTY_PREFIX;
  }

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
  for (uint32_t i = 0; i < h->size; i++) {
    h->prefix[i] = (uint16_t)LZW_HASH_EMPTY_PREFIX;
  }
}

int lzw_encoder_hash_find(lzw_encoder_hash_t * h,
    const lzw_core_encoder_t * core, uint32_t prefix, uint8_t byte,
    uint32_t * code_out) {
  if (!h || !h->prefix || !core || !code_out) {
    return 0;
  }
  uint32_t idx = hash_key(prefix, byte, h->size);
  uint16_t p16 = (uint16_t)(prefix & 0xFFFFu);

  for (uint32_t n = 0; n < h->size; n++) {
    if (h->prefix[idx] == (uint16_t)LZW_HASH_EMPTY_PREFIX) {
      return 0;
    }
    if (h->prefix[idx] == p16 && h->byte[idx] == byte) {
      *code_out = (uint32_t)h->code[idx];
      return 1;
    }
    idx = (idx + 1u) % h->size;
  }
  return 0;
}

void lzw_encoder_hash_insert(
    lzw_encoder_hash_t * h, uint32_t prefix, uint8_t byte, uint32_t code) {
  if (!h || !h->prefix || code > 0xFFFFu) {
    return;
  }
  uint32_t idx = hash_key(prefix, byte, h->size);
  uint16_t p16 = (uint16_t)(prefix & 0xFFFFu);

  for (uint32_t n = 0; n < h->size; n++) {
    if (h->prefix[idx] == (uint16_t)LZW_HASH_EMPTY_PREFIX) {
      h->prefix[idx] = p16;
      h->byte[idx] = byte;
      h->code[idx] = (uint16_t)code;
      return;
    }
    idx = (idx + 1u) % h->size;
  }
  /* Table full (should not happen if we only insert up to capacity) */
}
