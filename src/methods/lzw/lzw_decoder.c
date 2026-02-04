/**
 * @file lzw_decoder.c
 *
 * LZW decoder: streaming update/finish/reset. Parses bitstream with profile;
 * uses LZW core for dictionary and output; enforces output and expansion
 * limits.
 *
 * DESIGN NOTES
 * ============
 *
 * This decoder is designed for incremental streaming:
 *
 * - **Bit-level streaming:** LZW codes are packed densely. A read may end with
 *   a partially consumed byte; `lzw_bitreader_set_buffer()` advances the input
 *   window while preserving the reader's `bit_buffer`/`bit_count` so the next
 *   `update()` continues at the correct bit offset.
 *
 * - **Output backpressure:** A single LZW code can expand to a long string
 *   (up to the dictionary capacity). To avoid consuming input bits that cannot
 *   be written to the caller's output buffer, the decoder always decodes into a
 *   fixed pending buffer (`pending_buf`) and then copies as much as possible to
 *   the user's output. Remaining bytes stay pending for the next call.
 *
 * - **EOI requirement:** `finish()` requires that the stream ended with an EOI
 *   code; otherwise it reports `GCOMP_ERR_CORRUPT`. This library does not
 *   define a container, so EOI is the method-level end-of-stream marker.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "lzw_internal.h"
#include <ghoti.io/compress/limits.h>
#include <string.h>

#define LZW_FORMAT_DEFAULT LZW_FORMAT_GIF
#define LZW_LIT_WIDTH_GIF 8
#define LZW_LIT_WIDTH_TIFF 9
#define LZW_MAX_CODE_BITS_DEFAULT 12

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

gcomp_status_t lzw_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder) {
  if (!registry || !decoder) {
    if (decoder) {
      return gcomp_decoder_set_error(
          decoder, GCOMP_ERR_INVALID_ARG, "registry must be non-NULL");
    }
    return GCOMP_ERR_INVALID_ARG;
  }

  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);
  lzw_decoder_state_t * state =
      gcomp_calloc(alloc, 1, sizeof(lzw_decoder_state_t));
  if (!state) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_MEMORY, "LZW decoder allocation failed");
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
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_INVALID_ARG, "LZW format must be gif or tiff");
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
  state->pending_buf = (uint8_t *)gcomp_malloc(alloc, LZW_DECODER_PENDING_MAX);
  if (!state->pending_buf) {
    gcomp_free(alloc, state);
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_MEMORY,
        "LZW decoder pending buffer allocation failed");
  }
  state->pending_off = 0;
  state->pending_len = 0;
  state->done = 0;

  gcomp_status_t s = lzw_core_decoder_init(&state->core, alloc,
      (unsigned)state->max_code_bits, state->clear_code, state->eoi_code);
  if (s != GCOMP_OK) {
    gcomp_free(alloc, state->pending_buf);
    gcomp_free(alloc, state);
    return gcomp_decoder_set_error(decoder, s, "LZW decoder core init failed");
  }

  lzw_bitreader_init(
      &state->reader, NULL, 0, lzw_profile_bit_order(state->profile_id));

  decoder->method_state = state;
  return GCOMP_OK;
}

void lzw_decoder_destroy(gcomp_decoder_t * decoder) {
  if (!decoder || !decoder->method_state) {
    return;
  }

  lzw_decoder_state_t * state = (lzw_decoder_state_t *)decoder->method_state;
  lzw_core_decoder_destroy(&state->core);
  if (state->pending_buf) {
    gcomp_free(state->allocator, state->pending_buf);
    state->pending_buf = NULL;
  }
  const gcomp_allocator_t * alloc = state->allocator;
  gcomp_free(alloc, state);
  decoder->method_state = NULL;
}

/** Copy pending bytes to output; check limits. Returns GCOMP_OK or
 * limit/corrupt. */
static gcomp_status_t flush_pending(lzw_decoder_state_t * state,
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  if (state->pending_len == 0) {
    return GCOMP_OK;
  }
  size_t output_avail = output->size - output->used;
  size_t to_copy = state->pending_len;
  if (to_copy > output_avail) {
    to_copy = output_avail;
  }
  if (to_copy == 0) {
    return GCOMP_OK;
  }

  uint64_t new_total = state->total_output_bytes + (uint64_t)to_copy;
  if (state->max_output_bytes != 0) {
    gcomp_status_t lim =
        gcomp_limits_check_output((size_t)new_total, state->max_output_bytes);
    if (lim != GCOMP_OK) {
      return gcomp_decoder_set_error(
          decoder, GCOMP_ERR_LIMIT, "LZW decoder max output bytes exceeded");
    }
  }
  if (state->max_expansion_ratio != 0 && state->total_input_bytes > 0) {
    gcomp_status_t lim = gcomp_limits_check_expansion_ratio(
        state->total_input_bytes, new_total, state->max_expansion_ratio);
    if (lim != GCOMP_OK) {
      return gcomp_decoder_set_error(
          decoder, GCOMP_ERR_LIMIT, "LZW decoder expansion ratio exceeded");
    }
  }

  uint8_t * out_ptr = (uint8_t *)output->data + output->used;
  const uint8_t * src = state->pending_buf + state->pending_off;
  for (size_t i = 0; i < to_copy; i++) {
    out_ptr[i] = src[i];
  }
  output->used += to_copy;
  state->total_output_bytes += (uint64_t)to_copy;
  state->pending_off += to_copy;
  state->pending_len -= to_copy;
  return GCOMP_OK;
}

gcomp_status_t lzw_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (input->size > 0 && input->data == NULL) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_INVALID_ARG, "input->data is NULL but size > 0");
  }
  if (output->size > 0 && output->data == NULL) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_INVALID_ARG, "output->data is NULL but size > 0");
  }

  lzw_decoder_state_t * state = (lzw_decoder_state_t *)decoder->method_state;

  gcomp_status_t s = flush_pending(state, decoder, output);
  if (s != GCOMP_OK) {
    return s;
  }
  if (state->done) {
    return GCOMP_OK;
  }

  size_t input_avail = input->size - input->used;
  if (input_avail == 0) {
    return GCOMP_OK;
  }

  lzw_bitreader_set_buffer(
      &state->reader, (const uint8_t *)input->data + input->used, input_avail);

  for (;;) {
    uint32_t code = 0;
    gcomp_status_t r =
        lzw_bitreader_read_bits(&state->reader, state->current_bits, &code);
    if (r == GCOMP_ERR_CORRUPT) {
      input->used += state->reader.byte_pos;
      state->total_input_bytes += (uint64_t)state->reader.byte_pos;
      return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
          "LZW decoder truncated or invalid stream");
    }
    if (r != GCOMP_OK) {
      break;
    }

    if (code == state->clear_code) {
      lzw_core_decoder_reset(&state->core, state->clear_code, state->eoi_code);
      state->current_bits = lzw_profile_initial_code_bits(
          state->profile_id, (unsigned)state->lit_width);
      continue;
    }
    if (code == state->eoi_code) {
      input->used += state->reader.byte_pos;
      state->total_input_bytes += (uint64_t)state->reader.byte_pos;
      state->done = 1;
      s = flush_pending(state, decoder, output);
      if (s != GCOMP_OK) {
        return s;
      }
      return GCOMP_OK;
    }

    size_t dec_len = 0;
    s = lzw_core_decoder_decode(&state->core, code, state->pending_buf,
        LZW_DECODER_PENDING_MAX, &dec_len);
    if (s == GCOMP_ERR_CORRUPT) {
      input->used += state->reader.byte_pos;
      state->total_input_bytes += (uint64_t)state->reader.byte_pos;
      return gcomp_decoder_set_error(
          decoder, GCOMP_ERR_CORRUPT, "LZW decoder invalid code or KwKwK");
    }
    if (s == GCOMP_ERR_LIMIT) {
      input->used += state->reader.byte_pos;
      state->total_input_bytes += (uint64_t)state->reader.byte_pos;
      return gcomp_decoder_set_error(
          decoder, GCOMP_ERR_LIMIT, "LZW decoder single code exceeds buffer");
    }
    state->pending_off = 0;
    state->pending_len = dec_len;

    if (state->max_output_bytes != 0) {
      uint64_t new_total = state->total_output_bytes + (uint64_t)dec_len;
      gcomp_status_t lim =
          gcomp_limits_check_output((size_t)new_total, state->max_output_bytes);
      if (lim != GCOMP_OK) {
        input->used += state->reader.byte_pos;
        state->total_input_bytes += (uint64_t)state->reader.byte_pos;
        return gcomp_decoder_set_error(
            decoder, GCOMP_ERR_LIMIT, "LZW decoder max output bytes exceeded");
      }
    }
    if (state->max_expansion_ratio != 0 && state->total_input_bytes > 0) {
      uint64_t new_total = state->total_output_bytes + (uint64_t)dec_len;
      gcomp_status_t lim = gcomp_limits_check_expansion_ratio(
          state->total_input_bytes, new_total, state->max_expansion_ratio);
      if (lim != GCOMP_OK) {
        input->used += state->reader.byte_pos;
        state->total_input_bytes += (uint64_t)state->reader.byte_pos;
        return gcomp_decoder_set_error(
            decoder, GCOMP_ERR_LIMIT, "LZW decoder expansion ratio exceeded");
      }
    }

    if (lzw_profile_should_increment_bits(
            state->profile_id, state->core.next_code, state->current_bits)) {
      if (state->current_bits < (unsigned)state->max_code_bits) {
        state->current_bits++;
      }
    }

    s = flush_pending(state, decoder, output);
    if (s != GCOMP_OK) {
      input->used += state->reader.byte_pos;
      state->total_input_bytes += (uint64_t)state->reader.byte_pos;
      return s;
    }
    if (output->size - output->used == 0) {
      input->used += state->reader.byte_pos;
      state->total_input_bytes += (uint64_t)state->reader.byte_pos;
      return GCOMP_OK;
    }
  }

  input->used += state->reader.byte_pos;
  state->total_input_bytes += (uint64_t)state->reader.byte_pos;
  /* Flush any pending bytes when we run out of input (stream may continue
   * later) */
  s = flush_pending(state, decoder, output);
  if (s != GCOMP_OK) {
    return s;
  }
  return GCOMP_OK;
}

gcomp_status_t lzw_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output->size > 0 && output->data == NULL) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_INVALID_ARG, "output->data is NULL but size > 0");
  }

  lzw_decoder_state_t * state = (lzw_decoder_state_t *)decoder->method_state;
  gcomp_status_t s = flush_pending(state, decoder, output);
  if (s != GCOMP_OK) {
    return s;
  }
  if (!state->done) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_CORRUPT, "LZW decoder stream ended without EOI");
  }
  return GCOMP_OK;
}

gcomp_status_t lzw_decoder_reset(gcomp_decoder_t * decoder) {
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }
  lzw_decoder_state_t * state = (lzw_decoder_state_t *)decoder->method_state;
  lzw_core_decoder_reset(&state->core, state->clear_code, state->eoi_code);
  state->current_bits = lzw_profile_initial_code_bits(
      state->profile_id, (unsigned)state->lit_width);
  state->pending_off = 0;
  state->pending_len = 0;
  state->done = 0;
  state->total_input_bytes = 0;
  state->total_output_bytes = 0;
  state->reader.bit_buffer = 0;
  state->reader.bit_count = 0;
  return GCOMP_OK;
}
