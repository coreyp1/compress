/**
 * @file rle_profile.c
 *
 * RLE profile: PackBits and TGA decode/encode.
 *
 * Two reference profiles are implemented; they differ only in token grammar:
 *
 * - PackBits (TIFF / Apple): One-byte control. 0..127 = (n+1) literal bytes;
 *   128 = no-op; 129..255 = a run, whose length is (257-n).  Reading the
 *   control as a signed byte n' = n - 256, the rule in Apple TN1023 and TIFF
 *   6.0 is "copy the next byte -n'+1 times", which is 257-n.  Max literal 128,
 *   max run 128 (control 129); a run of 128 does not collide with the no-op.
 *
 * - TGA (Truevision Targa): One-byte header. Bit7=0: raw, (header&0x7F)+1
 *   literal bytes. Bit7=1: run, (header&0x7F)+1 copies of next byte.
 *   Count 1..128 for both.
 *
 * Profiles map tokens to rle_core: decode calls rle_emit_literal /
 * rle_emit_repeat; encode writes control and data bytes directly. Decoder path
 * also calls gcomp_limits_check_output and gcomp_limits_check_expansion_ratio.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>
#include "rle_profile.h"
#include <ghoti.io/cutil/safemath.h>
#include "rle_core.h"
#include "rle_internal.h"
#include <ghoti.io/compress/limits.h>
#include <string.h>

rle_profile_id_t rle_profile_from_string(const char * format) {
  if (!format) {
    return RLE_PROFILE_UNKNOWN;
  }
  if (strcmp(format, RLE_FORMAT_PACKBITS) == 0) {
    return RLE_PROFILE_PACKBITS;
  }
  if (strcmp(format, RLE_FORMAT_TGA) == 0) {
    return RLE_PROFILE_TGA;
  }
  return RLE_PROFILE_UNKNOWN;
}

//
// PackBits decode: 0-127 = (n+1) literal; 128 = no-op; 129-255 = (257-n) run
//

static gcomp_status_t decode_packbits(rle_decoder_state_t * state,
    const uint8_t * input_data, size_t input_size, size_t * input_used_out,
    uint8_t * output_data, size_t output_size, size_t * output_used_inout,
    uint64_t max_out, uint64_t max_ratio) {
  size_t in_pos = 0;
  size_t out_used = *output_used_inout;
  rle_decoder_partial_t * p = &state->partial;

  while (1) {
    if (p->phase == RLE_DEC_CONTROL) {
      if (in_pos >= input_size) {
        break;
      }
      uint8_t c = input_data[in_pos++];
      state->total_input_bytes++;

      if (c <= 127) {
        p->phase = RLE_DEC_LITERAL_BYTES;
        p->pending_count = (uint32_t)(c + 1);
      }
      else if (c == 128) {
        continue; // No-op
      }
      else {
        p->phase = RLE_DEC_RUN_BYTE;
        p->pending_count = (uint32_t)(257 - c);
      }
    }

    if (p->phase == RLE_DEC_LITERAL_BYTES) {
      size_t want = (size_t)p->pending_count;
      if (want > input_size - in_pos) {
        want = input_size - in_pos;
      }
      // The output buffer bounds the span too.  gcomp_decoder_update()
      // (stream.h) promises that a small output buffer is ordinary -- "a small
      // output buffer fills long before the input is used up, and the call
      // returns having consumed only part of it" -- so a literal run longer
      // than the buffer is written across several calls, with pending_count
      // carrying the remainder.  Refusing it instead, which is what this did,
      // made a 16-byte output buffer fail outright.
      if (want > output_size - out_used) {
        want = output_size - out_used;
      }
      // limits.max_output_bytes is a whole-stream ceiling, so it is measured
      // against the running total rather than against this buffer's offset.
      if (max_out != 0) {
        uint64_t remaining = (max_out > state->total_output_bytes)
            ? max_out - state->total_output_bytes
            : 0;
        if (remaining == 0) {
          *input_used_out = in_pos;
          *output_used_inout = out_used;
          return GCOMP_ERR_LIMIT;
        }
        if ((uint64_t)want > remaining) {
          want = (size_t)remaining;
        }
      }
      if (want == 0) {
        break;
      }
      // The budget above is the whole-stream one, so the emit helper is asked
      // for no ceiling of its own; its check is against this buffer's offset.
      gcomp_status_t s = rle_emit_literal(output_data, output_size, &out_used,
          input_data + in_pos, want, 0);
      if (s != GCOMP_OK) {
        *input_used_out = in_pos;
        *output_used_inout = out_used;
        return s;
      }
      in_pos += want;
      state->total_input_bytes += want;
      state->total_output_bytes += want;
      p->pending_count -= (uint32_t)want;
      if (p->pending_count == 0) {
        p->phase = RLE_DEC_CONTROL;
      }

      gcomp_status_t lim = gcomp_limits_check_expansion_ratio(
          state->total_input_bytes, state->total_output_bytes, max_ratio);
      if (lim != GCOMP_OK) {
        *input_used_out = in_pos;
        *output_used_inout = out_used;
        return lim;
      }
      continue;
    }

    if (p->phase == RLE_DEC_RUN_BYTE) {
      if (in_pos >= input_size) {
        break;
      }
      p->pending_byte = input_data[in_pos++];
      state->total_input_bytes++;
      // The byte is consumed exactly once; everything after it is output, and
      // the run may need more than one call to write.
      p->phase = RLE_DEC_RUN_EMIT;
    }

    if (p->phase == RLE_DEC_RUN_EMIT) {
      size_t want = (size_t)p->pending_count;
      if (want > output_size - out_used) {
        want = output_size - out_used;
      }
      if (max_out != 0) {
        uint64_t remaining = (max_out > state->total_output_bytes)
            ? max_out - state->total_output_bytes
            : 0;
        if (remaining == 0) {
          *input_used_out = in_pos;
          *output_used_inout = out_used;
          return GCOMP_ERR_LIMIT;
        }
        if ((uint64_t)want > remaining) {
          want = (size_t)remaining;
        }
      }
      if (want == 0) {
        break;
      }
      gcomp_status_t s = rle_emit_repeat(
          output_data, output_size, &out_used, p->pending_byte, want, 0);
      if (s != GCOMP_OK) {
        *input_used_out = in_pos;
        *output_used_inout = out_used;
        return s;
      }
      state->total_output_bytes += (uint64_t)want;
      p->pending_count -= (uint32_t)want;
      if (p->pending_count == 0) {
        p->phase = RLE_DEC_CONTROL;
      }

      gcomp_status_t lim = gcomp_limits_check_expansion_ratio(
          state->total_input_bytes, state->total_output_bytes, max_ratio);
      if (lim != GCOMP_OK) {
        *input_used_out = in_pos;
        *output_used_inout = out_used;
        return lim;
      }
      continue;
    }
  }

  *input_used_out = in_pos;
  *output_used_inout = out_used;
  return GCOMP_OK;
}

//
// TGA decode: header bit7=0 raw (count = (h&0x7F)+1), bit7=1 run
//

static gcomp_status_t decode_tga(rle_decoder_state_t * state,
    const uint8_t * input_data, size_t input_size, size_t * input_used_out,
    uint8_t * output_data, size_t output_size, size_t * output_used_inout,
    uint64_t max_out, uint64_t max_ratio) {
  size_t in_pos = 0;
  size_t out_used = *output_used_inout;
  rle_decoder_partial_t * p = &state->partial;

  while (1) {
    if (p->phase == RLE_DEC_CONTROL) {
      if (in_pos >= input_size) {
        break;
      }
      uint8_t h = input_data[in_pos++];
      state->total_input_bytes++;

      p->pending_count = (uint32_t)((h & 0x7F) + 1);
      if ((h & 0x80) == 0) {
        p->phase = RLE_DEC_LITERAL_BYTES;
      }
      else {
        p->phase = RLE_DEC_RUN_BYTE;
      }
    }

    if (p->phase == RLE_DEC_LITERAL_BYTES) {
      size_t want = (size_t)p->pending_count;
      if (want > input_size - in_pos) {
        want = input_size - in_pos;
      }
      // The output buffer bounds the span too.  gcomp_decoder_update()
      // (stream.h) promises that a small output buffer is ordinary -- "a small
      // output buffer fills long before the input is used up, and the call
      // returns having consumed only part of it" -- so a literal run longer
      // than the buffer is written across several calls, with pending_count
      // carrying the remainder.  Refusing it instead, which is what this did,
      // made a 16-byte output buffer fail outright.
      if (want > output_size - out_used) {
        want = output_size - out_used;
      }
      // limits.max_output_bytes is a whole-stream ceiling, so it is measured
      // against the running total rather than against this buffer's offset.
      if (max_out != 0) {
        uint64_t remaining = (max_out > state->total_output_bytes)
            ? max_out - state->total_output_bytes
            : 0;
        if (remaining == 0) {
          *input_used_out = in_pos;
          *output_used_inout = out_used;
          return GCOMP_ERR_LIMIT;
        }
        if ((uint64_t)want > remaining) {
          want = (size_t)remaining;
        }
      }
      if (want == 0) {
        break;
      }
      // The budget above is the whole-stream one, so the emit helper is asked
      // for no ceiling of its own; its check is against this buffer's offset.
      gcomp_status_t s = rle_emit_literal(output_data, output_size, &out_used,
          input_data + in_pos, want, 0);
      if (s != GCOMP_OK) {
        *input_used_out = in_pos;
        *output_used_inout = out_used;
        return s;
      }
      in_pos += want;
      state->total_input_bytes += want;
      state->total_output_bytes += want;
      p->pending_count -= (uint32_t)want;
      if (p->pending_count == 0) {
        p->phase = RLE_DEC_CONTROL;
      }

      gcomp_status_t lim = gcomp_limits_check_expansion_ratio(
          state->total_input_bytes, state->total_output_bytes, max_ratio);
      if (lim != GCOMP_OK) {
        *input_used_out = in_pos;
        *output_used_inout = out_used;
        return lim;
      }
      continue;
    }

    if (p->phase == RLE_DEC_RUN_BYTE) {
      if (in_pos >= input_size) {
        break;
      }
      p->pending_byte = input_data[in_pos++];
      state->total_input_bytes++;
      // The byte is consumed exactly once; everything after it is output, and
      // the run may need more than one call to write.
      p->phase = RLE_DEC_RUN_EMIT;
    }

    if (p->phase == RLE_DEC_RUN_EMIT) {
      size_t want = (size_t)p->pending_count;
      if (want > output_size - out_used) {
        want = output_size - out_used;
      }
      if (max_out != 0) {
        uint64_t remaining = (max_out > state->total_output_bytes)
            ? max_out - state->total_output_bytes
            : 0;
        if (remaining == 0) {
          *input_used_out = in_pos;
          *output_used_inout = out_used;
          return GCOMP_ERR_LIMIT;
        }
        if ((uint64_t)want > remaining) {
          want = (size_t)remaining;
        }
      }
      if (want == 0) {
        break;
      }
      gcomp_status_t s = rle_emit_repeat(
          output_data, output_size, &out_used, p->pending_byte, want, 0);
      if (s != GCOMP_OK) {
        *input_used_out = in_pos;
        *output_used_inout = out_used;
        return s;
      }
      state->total_output_bytes += (uint64_t)want;
      p->pending_count -= (uint32_t)want;
      if (p->pending_count == 0) {
        p->phase = RLE_DEC_CONTROL;
      }

      gcomp_status_t lim = gcomp_limits_check_expansion_ratio(
          state->total_input_bytes, state->total_output_bytes, max_ratio);
      if (lim != GCOMP_OK) {
        *input_used_out = in_pos;
        *output_used_inout = out_used;
        return lim;
      }
      continue;
    }
  }

  *input_used_out = in_pos;
  *output_used_inout = out_used;
  return GCOMP_OK;
}

gcomp_status_t rle_profile_decode(rle_decoder_state_t * state,
    const uint8_t * input_data, size_t input_size, size_t * input_used_out,
    uint8_t * output_data, size_t output_size, size_t * output_used_inout) {
  if (!state || !input_used_out || !output_used_inout) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if ((input_size > 0 && !input_data) || (output_size > 0 && !output_data)) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t s = gcomp_limits_check_output(
      (size_t)state->total_output_bytes, state->max_output_bytes);
  if (s != GCOMP_OK) {
    return s;
  }

  rle_profile_id_t id = rle_profile_from_string(state->format);
  if (id == RLE_PROFILE_PACKBITS) {
    return decode_packbits(state, input_data, input_size, input_used_out,
        output_data, output_size, output_used_inout, state->max_output_bytes,
        state->max_expansion_ratio);
  }
  if (id == RLE_PROFILE_TGA) {
    return decode_tga(state, input_data, input_size, input_used_out,
        output_data, output_size, output_used_inout, state->max_output_bytes,
        state->max_expansion_ratio);
  }
  return GCOMP_ERR_INVALID_ARG;
}

//
// PackBits encode: emit literal control (n-1) then n bytes, or run control
// (257-n) then 1 byte. Max literal 128, max run 128.
//

// A run of L bytes is written as control 257-L, so the longest run, 128,
// becomes control 129 and the shortest, 2, becomes 255.  Nothing in that range
// collides with 128, the no-op.
//
// This used to be 127, with a comment explaining that 128 had to be avoided
// because it would emit the no-op control.  That was a symptom: the run
// control was being computed as 256-L, one too low, which put a 128-byte run
// on the no-op and shifted every other run by one byte as far as any other
// implementation was concerned.  Both the cap and the collision go away once
// the arithmetic is right.
#define PACKBITS_MAX_RUN 128

static gcomp_status_t encode_packbits(rle_encoder_state_t * state,
    const uint8_t * input_data, size_t input_size, size_t * input_consumed_out,
    uint8_t * output_data, size_t output_size, size_t * output_used_inout) {
  size_t in_pos = 0;
  size_t out_used = *output_used_inout;
  uint64_t max_out = state->max_output_bytes;

  while (in_pos < input_size) {
    uint8_t b = input_data[in_pos];

    if (state->run_len > 0 && b == state->run_byte &&
        state->run_len < PACKBITS_MAX_RUN) {
      state->run_len++;
      in_pos++;
      continue;
    }

    if (state->run_len > 0) {
      if (out_used + 2 > output_size) {
        break;
      }
      if (max_out != 0 && out_used + 2 > max_out) {
        break;
      }
      output_data[out_used++] = (uint8_t)(257 - (int)state->run_len);
      output_data[out_used++] = state->run_byte;
      state->run_len = 0;
      continue;
    }

    if (state->literal_count == 128) {
      if (out_used + 1 + 128 > output_size) {
        break;
      }
      if (max_out != 0 && out_used + 1 + 128 > max_out) {
        break;
      }
      output_data[out_used++] = 127;
      memcpy(output_data + out_used, state->literal_buf, 128);
      out_used += 128;
      state->literal_count = 0;
      continue;
    }

    // Start a run if next two bytes equal (max PACKBITS_MAX_RUN to avoid 128).
    if (in_pos + 1 < input_size && input_data[in_pos + 1] == b) {
      size_t run_len = 1;
      while (in_pos + run_len < input_size &&
          input_data[in_pos + run_len] == b && run_len < PACKBITS_MAX_RUN) {
        run_len++;
      }
      if (state->literal_count > 0) {
        if (out_used + 1 + state->literal_count > output_size) {
          break;
        }
        if (max_out != 0 && out_used + 1 + state->literal_count > max_out) {
          break;
        }
        output_data[out_used++] = (uint8_t)(state->literal_count - 1);
        memcpy(
            output_data + out_used, state->literal_buf, state->literal_count);
        out_used += state->literal_count;
        state->literal_count = 0;
      }
      if (out_used + 2 > output_size) {
        state->run_byte = b;
        state->run_len = run_len;
        in_pos += run_len;
        break;
      }
      if (max_out != 0 && out_used + 2 > max_out) {
        state->run_byte = b;
        state->run_len = run_len;
        in_pos += run_len;
        break;
      }
      output_data[out_used++] = (uint8_t)(257 - (int)run_len);
      output_data[out_used++] = b;
      in_pos += run_len;
      continue;
    }

    state->literal_buf[state->literal_count++] = b;
    in_pos++;
  }

  *input_consumed_out = in_pos;
  *output_used_inout = out_used;
  return GCOMP_OK;
}

//
// TGA encode: raw = header (count-1) bit7=0 + bytes; run = header
// 0x80|(count-1) + 1 byte
//

static gcomp_status_t encode_tga(rle_encoder_state_t * state,
    const uint8_t * input_data, size_t input_size, size_t * input_consumed_out,
    uint8_t * output_data, size_t output_size, size_t * output_used_inout) {
  size_t in_pos = 0;
  size_t out_used = *output_used_inout;
  uint64_t max_out = state->max_output_bytes;

  while (in_pos < input_size) {
    uint8_t b = input_data[in_pos];

    if (state->run_len > 0 && b == state->run_byte && state->run_len < 128) {
      state->run_len++;
      in_pos++;
      continue;
    }

    if (state->run_len > 0) {
      if (out_used + 2 > output_size) {
        break;
      }
      if (max_out != 0 && out_used + 2 > max_out) {
        break;
      }
      output_data[out_used++] = (uint8_t)(0x80 | (state->run_len - 1));
      output_data[out_used++] = state->run_byte;
      state->run_len = 0;
      continue;
    }

    if (state->literal_count == 128) {
      if (out_used + 1 + 128 > output_size) {
        break;
      }
      if (max_out != 0 && out_used + 1 + 128 > max_out) {
        break;
      }
      output_data[out_used++] = 127; // (128-1), raw
      memcpy(output_data + out_used, state->literal_buf, 128);
      out_used += 128;
      state->literal_count = 0;
      continue;
    }

    if (in_pos + 1 < input_size && input_data[in_pos + 1] == b) {
      size_t run_len = 1;
      while (in_pos + run_len < input_size &&
          input_data[in_pos + run_len] == b && run_len < 128) {
        run_len++;
      }
      if (state->literal_count > 0) {
        if (out_used + 1 + state->literal_count > output_size) {
          break;
        }
        if (max_out != 0 && out_used + 1 + state->literal_count > max_out) {
          break;
        }
        output_data[out_used++] = (uint8_t)(state->literal_count - 1);
        memcpy(
            output_data + out_used, state->literal_buf, state->literal_count);
        out_used += state->literal_count;
        state->literal_count = 0;
      }
      if (out_used + 2 > output_size) {
        state->run_byte = b;
        state->run_len = run_len;
        in_pos += run_len;
        break;
      }
      if (max_out != 0 && out_used + 2 > max_out) {
        state->run_byte = b;
        state->run_len = run_len;
        in_pos += run_len;
        break;
      }
      output_data[out_used++] = (uint8_t)(0x80 | (run_len - 1));
      output_data[out_used++] = b;
      in_pos += run_len;
      continue;
    }

    state->literal_buf[state->literal_count++] = b;
    in_pos++;
  }

  *input_consumed_out = in_pos;
  *output_used_inout = out_used;
  return GCOMP_OK;
}

static gcomp_status_t packbits_finish(rle_encoder_state_t * state,
    uint8_t * output_data, size_t output_size, size_t * output_used_inout) {
  size_t out_used = *output_used_inout;
  uint64_t max_out = state->max_output_bytes;

  if (state->run_len > 0) {
    if (out_used + 2 > output_size) {
      return GCOMP_ERR_LIMIT;
    }
    if (max_out != 0 && out_used + 2 > max_out) {
      return GCOMP_ERR_LIMIT;
    }
    output_data[out_used++] = (uint8_t)(257 - (int)state->run_len);
    output_data[out_used++] = state->run_byte;
    state->run_len = 0;
  }

  if (state->literal_count > 0) {
    if (out_used + 1 + state->literal_count > output_size) {
      return GCOMP_ERR_LIMIT;
    }
    if (max_out != 0 && out_used + 1 + state->literal_count > max_out) {
      return GCOMP_ERR_LIMIT;
    }
    output_data[out_used++] = (uint8_t)(state->literal_count - 1);
    memcpy(output_data + out_used, state->literal_buf, state->literal_count);
    out_used += state->literal_count;
    state->literal_count = 0;
  }

  *output_used_inout = out_used;
  return GCOMP_OK;
}

static gcomp_status_t tga_finish(rle_encoder_state_t * state,
    uint8_t * output_data, size_t output_size, size_t * output_used_inout) {
  size_t out_used = *output_used_inout;
  uint64_t max_out = state->max_output_bytes;

  if (state->run_len > 0) {
    if (out_used + 2 > output_size) {
      return GCOMP_ERR_LIMIT;
    }
    if (max_out != 0 && out_used + 2 > max_out) {
      return GCOMP_ERR_LIMIT;
    }
    output_data[out_used++] = (uint8_t)(0x80 | (state->run_len - 1));
    output_data[out_used++] = state->run_byte;
    state->run_len = 0;
  }

  if (state->literal_count > 0) {
    if (out_used + 1 + state->literal_count > output_size) {
      return GCOMP_ERR_LIMIT;
    }
    if (max_out != 0 && out_used + 1 + state->literal_count > max_out) {
      return GCOMP_ERR_LIMIT;
    }
    output_data[out_used++] = (uint8_t)(state->literal_count - 1);
    memcpy(output_data + out_used, state->literal_buf, state->literal_count);
    out_used += state->literal_count;
    state->literal_count = 0;
  }

  *output_used_inout = out_used;
  return GCOMP_OK;
}

gcomp_status_t rle_profile_encode(rle_encoder_state_t * state,
    const uint8_t * input_data, size_t input_size, size_t * input_consumed_out,
    uint8_t * output_data, size_t output_size, size_t * output_used_inout) {
  if (!state || !input_consumed_out || !output_used_inout) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if ((input_size > 0 && !input_data) || (output_size > 0 && !output_data)) {
    return GCOMP_ERR_INVALID_ARG;
  }

  rle_profile_id_t id = rle_profile_from_string(state->format);
  if (id == RLE_PROFILE_PACKBITS) {
    return encode_packbits(state, input_data, input_size, input_consumed_out,
        output_data, output_size, output_used_inout);
  }
  if (id == RLE_PROFILE_TGA) {
    return encode_tga(state, input_data, input_size, input_consumed_out,
        output_data, output_size, output_used_inout);
  }
  return GCOMP_ERR_INVALID_ARG;
}

gcomp_status_t rle_profile_encode_finish(rle_encoder_state_t * state,
    uint8_t * output_data, size_t output_size, size_t * output_used_inout) {
  if (!state || !output_used_inout) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output_size > 0 && !output_data) {
    return GCOMP_ERR_INVALID_ARG;
  }

  rle_profile_id_t id = rle_profile_from_string(state->format);
  if (id == RLE_PROFILE_PACKBITS) {
    return packbits_finish(state, output_data, output_size, output_used_inout);
  }
  if (id == RLE_PROFILE_TGA) {
    return tga_finish(state, output_data, output_size, output_used_inout);
  }
  return GCOMP_OK;
}