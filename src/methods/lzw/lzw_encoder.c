/**
 * @file lzw_encoder.c
 *
 * LZW encoder: streaming update/finish/reset. Builds dictionary during
 * encoding; emits codes via profile bit I/O; respects CLEAR/EOI and
 * code-width growth rules.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "lzw_internal.h"
#include <limits.h>
#include <string.h>

#define LZW_FORMAT_DEFAULT LZW_FORMAT_GIF
#define LZW_LIT_WIDTH_GIF 8
#define LZW_LIT_WIDTH_TIFF 9
#define LZW_MAX_CODE_BITS_DEFAULT 12
#define LZW_NO_PREFIX UINT32_MAX

static void copy_format(char * dst, size_t dst_size, const char * src) {
  if (!dst || dst_size == 0) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t len = strlen(src);
  if (len >= dst_size) {
    len = dst_size - 1;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

gcomp_status_t lzw_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  if (!registry || !encoder) {
    if (encoder) {
      return gcomp_encoder_set_error(
          encoder, GCOMP_ERR_INVALID_ARG, "registry must be non-NULL");
    }
    return GCOMP_ERR_INVALID_ARG;
  }

  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);
  lzw_encoder_state_t * state =
      gcomp_calloc(alloc, 1, sizeof(lzw_encoder_state_t));
  if (!state) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "LZW encoder allocation failed");
  }

  state->allocator = alloc;

  const char * format_str = NULL;
  if (options &&
      gcomp_options_get_string(options, "lzw.format", &format_str) ==
          GCOMP_OK &&
      format_str) {
    copy_format(state->format, sizeof(state->format), format_str);
  }
  else {
    copy_format(state->format, sizeof(state->format), LZW_FORMAT_DEFAULT);
  }

  state->profile_id = lzw_profile_from_string(state->format);
  if (state->profile_id == LZW_PROFILE_UNKNOWN) {
    gcomp_free(alloc, state);
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "LZW format must be gif or tiff");
  }

  uint64_t u64 = 0;
  if (options &&
      gcomp_options_get_uint64(options, "lzw.lit_width", &u64) == GCOMP_OK &&
      u64 != 0) {
    state->lit_width = u64;
  }
  else {
    state->lit_width = (strcmp(state->format, LZW_FORMAT_TIFF) == 0)
        ? LZW_LIT_WIDTH_TIFF
        : LZW_LIT_WIDTH_GIF;
  }

  if (options &&
      gcomp_options_get_uint64(options, "lzw.max_code_bits", &u64) ==
          GCOMP_OK) {
    state->max_code_bits = u64;
  }
  else {
    state->max_code_bits = LZW_MAX_CODE_BITS_DEFAULT;
  }

  if (options) {
    u64 = 0;
    if (gcomp_options_get_uint64(options, "limits.max_output_bytes", &u64) ==
        GCOMP_OK) {
      state->max_output_bytes = u64;
    }
    if (gcomp_options_get_uint64(options, "limits.max_memory_bytes", &u64) ==
        GCOMP_OK) {
      state->max_memory_bytes = u64;
    }
    if (gcomp_options_get_uint64(options, "limits.max_expansion_ratio", &u64) ==
        GCOMP_OK) {
      state->max_expansion_ratio = u64;
    }
  }

  unsigned lit = (unsigned)state->lit_width;
  state->clear_code = lzw_profile_clear_code(state->profile_id, lit);
  state->eoi_code = lzw_profile_eoi_code(state->profile_id, lit);
  state->current_bits = lzw_profile_initial_code_bits(state->profile_id, lit);
  state->prefix_code = LZW_NO_PREFIX;
  state->pending_code = 0;
  state->pending_bits = 0;
  state->header_emitted = 0;

  gcomp_status_t s = lzw_core_encoder_init(&state->core, alloc,
      (unsigned)state->max_code_bits, state->clear_code, state->eoi_code);
  if (s != GCOMP_OK) {
    gcomp_free(alloc, state);
    return gcomp_encoder_set_error(encoder, s, "LZW encoder core init failed");
  }

  lzw_bitwriter_init(
      &state->writer, NULL, 0, lzw_profile_bit_order(state->profile_id));

  encoder->method_state = state;
  return GCOMP_OK;
}

void lzw_encoder_destroy(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return;
  }

  lzw_encoder_state_t * state = (lzw_encoder_state_t *)encoder->method_state;
  lzw_core_encoder_destroy(&state->core);
  const gcomp_allocator_t * alloc = state->allocator;
  gcomp_free(alloc, state);
  encoder->method_state = NULL;
}

/** Emit one code; on LIMIT set pending and return GCOMP_ERR_LIMIT. */
static gcomp_status_t emit_code(
    lzw_encoder_state_t * state, gcomp_encoder_t * encoder, uint32_t code) {
  gcomp_status_t s =
      lzw_bitwriter_write_bits(&state->writer, code, state->current_bits);
  if (s == GCOMP_ERR_LIMIT) {
    state->pending_code = code;
    state->pending_bits = state->current_bits;
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_LIMIT, "LZW encoder output buffer full");
  }
  return s;
}

gcomp_status_t lzw_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (input->size > 0 && input->data == NULL) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "input->data is NULL but size > 0");
  }
  if (output->size > 0 && output->data == NULL) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "output->data is NULL but size > 0");
  }

  lzw_encoder_state_t * state = (lzw_encoder_state_t *)encoder->method_state;
  gcomp_status_t s;
  size_t output_avail = output->size - output->used;
  if (output_avail == 0) {
    return GCOMP_OK;
  }

  uint8_t * out_base = (uint8_t *)output->data + output->used;
  lzw_bitwriter_set_buffer(&state->writer, out_base, output_avail);

  /* Retry pending code from previous LIMIT */
  if (state->pending_bits != 0) {
    s = lzw_bitwriter_write_bits(
        &state->writer, state->pending_code, state->pending_bits);
    if (s == GCOMP_ERR_LIMIT) {
      return gcomp_encoder_set_error(
          encoder, GCOMP_ERR_LIMIT, "LZW encoder output buffer full");
    }
    state->pending_code = 0;
    state->pending_bits = 0;
  }

  /* Emit CLEAR at start of stream */
  if (!state->header_emitted) {
    s = emit_code(state, encoder, state->clear_code);
    if (s != GCOMP_OK) {
      return s;
    }
    state->header_emitted = 1;
  }

  const uint8_t * in_ptr = (const uint8_t *)input->data + input->used;
  size_t in_avail = input->size - input->used;

  for (size_t i = 0; i < in_avail; i++) {
    uint8_t byte = in_ptr[i];
    uint32_t prefix = state->prefix_code;
    uint32_t code_out = 0;
    int found = 0;

    if (prefix == LZW_NO_PREFIX) {
      /* First byte: current string is just this literal */
      state->prefix_code = (uint32_t)byte;
      input->used++;
      continue;
    }

    found = lzw_core_encoder_find(&state->core, prefix, byte, &code_out);
    if (found) {
      state->prefix_code = code_out;
      input->used++;
      continue;
    }

    /* Not in table: emit prefix, add (prefix, byte), new prefix = byte */
    s = emit_code(state, encoder, prefix);
    if (s != GCOMP_OK) {
      return s;
    }

    if (lzw_core_encoder_is_full(&state->core)) {
      s = emit_code(state, encoder, state->clear_code);
      if (s != GCOMP_OK) {
        return s;
      }
      lzw_core_encoder_reset(&state->core, state->clear_code, state->eoi_code);
      state->current_bits = lzw_profile_initial_code_bits(
          state->profile_id, (unsigned)state->lit_width);
    }
    else {
      lzw_core_encoder_add(&state->core, prefix, byte);
      if (lzw_profile_should_increment_bits(
              state->profile_id, state->core.next_code, state->current_bits)) {
        if (state->current_bits < (unsigned)state->max_code_bits) {
          state->current_bits++;
        }
      }
    }

    state->prefix_code = (uint32_t)byte;
    input->used++;
  }

  output->used += lzw_bitwriter_bytes_written(&state->writer);
  return GCOMP_OK;
}

gcomp_status_t lzw_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output->size > 0 && output->data == NULL) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INVALID_ARG, "output->data is NULL but size > 0");
  }

  lzw_encoder_state_t * state = (lzw_encoder_state_t *)encoder->method_state;
  size_t output_avail = output->size - output->used;
  uint8_t * out_base = (uint8_t *)output->data + output->used;
  lzw_bitwriter_set_buffer(&state->writer, out_base, output_avail);

  /* Retry any pending code */
  if (state->pending_bits != 0) {
    gcomp_status_t s = lzw_bitwriter_write_bits(
        &state->writer, state->pending_code, state->pending_bits);
    if (s != GCOMP_OK) {
      return gcomp_encoder_set_error(
          encoder, s, "LZW encoder flush pending code failed");
    }
    state->pending_code = 0;
    state->pending_bits = 0;
  }

  /* Emit CLEAR if we never did (empty input) */
  if (!state->header_emitted) {
    gcomp_status_t s = emit_code(state, encoder, state->clear_code);
    if (s != GCOMP_OK) {
      return s;
    }
    state->header_emitted = 1;
  }

  /* Emit current string then EOI */
  if (state->prefix_code != LZW_NO_PREFIX) {
    gcomp_status_t s = emit_code(state, encoder, state->prefix_code);
    if (s != GCOMP_OK) {
      return s;
    }
  }

  gcomp_status_t s = emit_code(state, encoder, state->eoi_code);
  if (s != GCOMP_OK) {
    return s;
  }

  s = lzw_bitwriter_flush(&state->writer);
  if (s != GCOMP_OK) {
    return gcomp_encoder_set_error(encoder, s, "LZW encoder bit flush failed");
  }

  output->used += lzw_bitwriter_bytes_written(&state->writer);
  return GCOMP_OK;
}

gcomp_status_t lzw_encoder_reset(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }

  lzw_encoder_state_t * state = (lzw_encoder_state_t *)encoder->method_state;
  lzw_core_encoder_reset(&state->core, state->clear_code, state->eoi_code);
  state->current_bits = lzw_profile_initial_code_bits(
      state->profile_id, (unsigned)state->lit_width);
  state->prefix_code = LZW_NO_PREFIX;
  state->pending_code = 0;
  state->pending_bits = 0;
  state->header_emitted = 0;
  state->writer.bit_buffer = 0;
  state->writer.bit_count = 0;
  return GCOMP_OK;
}
