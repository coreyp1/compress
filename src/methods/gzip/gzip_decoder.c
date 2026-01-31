/**
 * @file gzip_decoder.c
 *
 * Streaming gzip (RFC 1952) wrapper decoder for the Ghoti.io Compress library.
 *
 * The gzip decoder wraps the deflate decoder, handling:
 * - RFC 1952 header parsing (magic, CM, FLG, MTIME, XFL, OS, optional fields)
 * - CRC32 tracking of decompressed output
 * - RFC 1952 trailer validation (CRC32, ISIZE)
 * - Optional support for concatenated gzip members
 *
 * ## Decoder State Machine
 *
 * The decoder operates in three main stages:
 *
 * 1. **HEADER**: Parse gzip header from input
 *    - Validates magic bytes (0x1F 0x8B), compression method (8 = deflate)
 *    - Reads MTIME, XFL, OS, and optional fields (FEXTRA, FNAME, FCOMMENT)
 *    - Optionally validates header CRC (FHCRC)
 *    - Fully streaming: can pause at any byte boundary
 *
 * 2. **BODY**: Decompress via inner deflate decoder
 *    - Passes input to deflate decoder
 *    - Tracks CRC32 of decompressed output incrementally
 *    - Tracks ISIZE (uncompressed size mod 2^32)
 *    - Handles unconsumed bytes from deflate's bit buffer for trailer
 *
 * 3. **TRAILER**: Read and validate CRC32 and ISIZE
 *    - Compares computed CRC32 against stored value
 *    - Compares computed ISIZE against stored value
 *    - Mismatch returns GCOMP_ERR_CORRUPT
 *
 * ## Concatenated Members
 *
 * RFC 1952 allows multiple gzip members to be concatenated into a single
 * stream. When `gzip.concat` is enabled:
 *
 * - After successful trailer validation, the decoder resets for the next member
 * - CRC32 and ISIZE tracking restart from initial values
 * - Inner deflate decoder is reset
 * - Processing continues in the same update() call via an outer loop
 * - Output is continuous across members (no separation)
 * - Limits (max_output_bytes, max_expansion_ratio) apply to total output
 *
 * The outer loop in gzip_decoder_update() continues as long as:
 * - We're not in DONE or ERROR state
 * - There's input available (input->used < input->size)
 * - There's output space available (output->used < output->size)
 *
 * This ensures concatenated members are processed efficiently without
 * requiring multiple update() calls when data and space are available.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "../deflate/deflate_internal.h"
#include "gzip_internal.h"
#include <ghoti.io/compress/crc32.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/stream.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//
// Helper: Read decoder options
//

static gcomp_status_t read_decoder_options(
    const gcomp_options_t * options, gzip_decoder_state_t * state) {
  gcomp_status_t status;
  uint64_t u64_val;
  int bool_val;

  // Set defaults
  state->concat_enabled = 0;
  state->max_name_bytes = GZIP_MAX_NAME_BYTES_DEFAULT;
  state->max_comment_bytes = GZIP_MAX_COMMENT_BYTES_DEFAULT;
  state->max_extra_bytes = GZIP_MAX_EXTRA_BYTES_DEFAULT;

  // Use limit helper functions for default values
  state->max_output_bytes =
      gcomp_limits_read_output_max(options, GCOMP_DEFAULT_MAX_OUTPUT_BYTES);
  state->max_expansion_ratio = gcomp_limits_read_expansion_ratio_max(
      options, GCOMP_DEFAULT_MAX_EXPANSION_RATIO);

  if (!options) {
    return GCOMP_OK;
  }

  // gzip.concat
  status = gcomp_options_get_bool(options, "gzip.concat", &bool_val);
  if (status == GCOMP_OK) {
    state->concat_enabled = bool_val;
  }

  // gzip.max_name_bytes
  status = gcomp_options_get_uint64(options, "gzip.max_name_bytes", &u64_val);
  if (status == GCOMP_OK) {
    state->max_name_bytes = u64_val;
  }

  // gzip.max_comment_bytes
  status =
      gcomp_options_get_uint64(options, "gzip.max_comment_bytes", &u64_val);
  if (status == GCOMP_OK) {
    state->max_comment_bytes = u64_val;
  }

  // gzip.max_extra_bytes
  status = gcomp_options_get_uint64(options, "gzip.max_extra_bytes", &u64_val);
  if (status == GCOMP_OK) {
    state->max_extra_bytes = u64_val;
  }

  return GCOMP_OK;
}

//
// Header parsing helpers
//

static gcomp_status_t parse_header_byte(
    gzip_decoder_state_t * state, uint8_t byte, gcomp_decoder_t * decoder);

static void reset_header_parser(gzip_decoder_state_t * state) {
  state->header_stage = GZIP_HEADER_MAGIC;
  state->header_accum_pos = 0;
  state->header_field_target = 0;
  state->header_crc_accum = GCOMP_CRC32_INIT;

  // Track frees for header info allocations before freeing
  if (state->header_info.name) {
    gcomp_memory_track_free(
        &state->mem_tracker, strlen(state->header_info.name) + 1);
  }
  if (state->header_info.comment) {
    gcomp_memory_track_free(
        &state->mem_tracker, strlen(state->header_info.comment) + 1);
  }
  if (state->header_info.extra) {
    gcomp_memory_track_free(&state->mem_tracker, state->header_info.extra_len);
  }

  gzip_header_info_free(&state->header_info);
  memset(&state->header_info, 0, sizeof(state->header_info));
}

//
// Decoder Init
//

gcomp_status_t gzip_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder) {
  if (!registry || !decoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Look up deflate method
  const gcomp_method_t * deflate_method =
      gcomp_registry_find(registry, "deflate");
  if (!deflate_method) {
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_UNSUPPORTED,
        "gzip requires deflate method to be registered");
  }

  // Allocate state
  gzip_decoder_state_t * state =
      (gzip_decoder_state_t *)calloc(1, sizeof(gzip_decoder_state_t));
  if (!state) {
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_MEMORY, "failed to allocate gzip decoder state");
  }

  // Initialize memory tracker and track state allocation
  state->mem_tracker.current_bytes = 0;
  gcomp_memory_track_alloc(&state->mem_tracker, sizeof(gzip_decoder_state_t));
  state->max_memory_bytes =
      gcomp_limits_read_memory_max(options, GCOMP_DEFAULT_MAX_MEMORY_BYTES);

  // Read options
  gcomp_status_t status = read_decoder_options(options, state);
  if (status != GCOMP_OK) {
    free(state);
    return status;
  }

  // Extract pass-through options for deflate
  gcomp_options_t * deflate_options = NULL;
  status = gzip_extract_passthrough_options(options, &deflate_options);
  if (status != GCOMP_OK) {
    free(state);
    return status;
  }

  // Create inner deflate decoder
  status = gcomp_decoder_create(
      registry, "deflate", deflate_options, &state->inner_decoder);
  if (deflate_options) {
    gcomp_options_destroy(deflate_options);
  }
  if (status != GCOMP_OK) {
    free(state);
    return gcomp_decoder_set_error(
        decoder, status, "failed to create inner deflate decoder");
  }

  // Initialize state
  state->crc32 = GCOMP_CRC32_INIT;
  state->isize = 0;
  state->stage = GZIP_DEC_STAGE_HEADER;
  reset_header_parser(state);
  state->trailer_pos = 0;
  state->total_input_bytes = 0;
  state->total_output_bytes = 0;

  decoder->method_state = state;
  return GCOMP_OK;
}

//
// Header Parsing
//

static gcomp_status_t parse_header_byte(
    gzip_decoder_state_t * state, uint8_t byte, gcomp_decoder_t * decoder) {
  // Always accumulate bytes for header CRC.
  // We don't know if FHCRC is set until we parse byte 3 (FLG), so we must
  // accumulate all bytes and only check the CRC later if FHCRC is set.
  // Don't include the FHCRC bytes themselves in the CRC calculation.
  if (state->header_stage != GZIP_HEADER_FHCRC) {
    state->header_crc_accum =
        gcomp_crc32_update(state->header_crc_accum, &byte, 1);
  }

  switch (state->header_stage) {
  case GZIP_HEADER_MAGIC:
    state->header_accum[state->header_accum_pos++] = byte;
    if (state->header_accum_pos == 2) {
      // Validate magic
      if (state->header_accum[0] != GZIP_ID1 ||
          state->header_accum[1] != GZIP_ID2) {
        return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
            "invalid gzip magic: 0x%02X 0x%02X (expected 0x1F 0x8B)",
            state->header_accum[0], state->header_accum[1]);
      }
      state->header_stage = GZIP_HEADER_CM_FLG;
      state->header_accum_pos = 0;
    }
    break;

  case GZIP_HEADER_CM_FLG:
    state->header_accum[state->header_accum_pos++] = byte;
    if (state->header_accum_pos == 2) {
      uint8_t cm = state->header_accum[0];
      uint8_t flg = state->header_accum[1];

      // Validate CM
      if (cm != GZIP_CM_DEFLATE) {
        return gcomp_decoder_set_error(decoder, GCOMP_ERR_UNSUPPORTED,
            "unsupported gzip compression method: %d (only deflate=8 "
            "supported)",
            cm);
      }

      // Validate reserved bits
      if (flg & GZIP_FLG_RESERVED) {
        return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
            "invalid gzip flags: reserved bits set (0x%02X)", flg);
      }

      state->header_info.flg = flg;
      state->header_stage = GZIP_HEADER_MTIME;
      state->header_accum_pos = 0;
    }
    break;

  case GZIP_HEADER_MTIME:
    state->header_accum[state->header_accum_pos++] = byte;
    if (state->header_accum_pos == 4) {
      state->header_info.mtime = gzip_read_le32(state->header_accum);
      state->header_stage = GZIP_HEADER_XFL_OS;
      state->header_accum_pos = 0;
    }
    break;

  case GZIP_HEADER_XFL_OS:
    state->header_accum[state->header_accum_pos++] = byte;
    if (state->header_accum_pos == 2) {
      state->header_info.xfl = state->header_accum[0];
      state->header_info.os = state->header_accum[1];

      // Determine next stage based on flags
      if (state->header_info.flg & GZIP_FLG_FEXTRA) {
        state->header_stage = GZIP_HEADER_FEXTRA_LEN;
      }
      else if (state->header_info.flg & GZIP_FLG_FNAME) {
        state->header_stage = GZIP_HEADER_FNAME;
      }
      else if (state->header_info.flg & GZIP_FLG_FCOMMENT) {
        state->header_stage = GZIP_HEADER_FCOMMENT;
      }
      else if (state->header_info.flg & GZIP_FLG_FHCRC) {
        state->header_stage = GZIP_HEADER_FHCRC;
      }
      else {
        state->header_stage = GZIP_HEADER_DONE;
      }
      state->header_accum_pos = 0;
    }
    break;

  case GZIP_HEADER_FEXTRA_LEN:
    state->header_accum[state->header_accum_pos++] = byte;
    if (state->header_accum_pos == 2) {
      uint16_t extra_len = gzip_read_le16(state->header_accum);

      // Check limit
      if (extra_len > state->max_extra_bytes) {
        return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
            "gzip FEXTRA length %u exceeds limit %lu", extra_len,
            (unsigned long)state->max_extra_bytes);
      }

      if (extra_len > 0) {
        state->header_info.extra = (uint8_t *)malloc(extra_len);
        if (!state->header_info.extra) {
          return gcomp_decoder_set_error(
              decoder, GCOMP_ERR_MEMORY, "failed to allocate FEXTRA buffer");
        }
        gcomp_memory_track_alloc(&state->mem_tracker, extra_len);
        state->header_info.extra_len = extra_len;
        state->header_field_target = extra_len;
        state->header_stage = GZIP_HEADER_FEXTRA_DATA;
      }
      else {
        // Empty FEXTRA, move to next field
        if (state->header_info.flg & GZIP_FLG_FNAME) {
          state->header_stage = GZIP_HEADER_FNAME;
        }
        else if (state->header_info.flg & GZIP_FLG_FCOMMENT) {
          state->header_stage = GZIP_HEADER_FCOMMENT;
        }
        else if (state->header_info.flg & GZIP_FLG_FHCRC) {
          state->header_stage = GZIP_HEADER_FHCRC;
        }
        else {
          state->header_stage = GZIP_HEADER_DONE;
        }
      }
      state->header_accum_pos = 0;
    }
    break;

  case GZIP_HEADER_FEXTRA_DATA:
    state->header_info.extra[state->header_accum_pos++] = byte;
    if (state->header_accum_pos >= state->header_field_target) {
      // Move to next field
      if (state->header_info.flg & GZIP_FLG_FNAME) {
        state->header_stage = GZIP_HEADER_FNAME;
      }
      else if (state->header_info.flg & GZIP_FLG_FCOMMENT) {
        state->header_stage = GZIP_HEADER_FCOMMENT;
      }
      else if (state->header_info.flg & GZIP_FLG_FHCRC) {
        state->header_stage = GZIP_HEADER_FHCRC;
      }
      else {
        state->header_stage = GZIP_HEADER_DONE;
      }
      state->header_accum_pos = 0;
    }
    break;

  case GZIP_HEADER_FNAME:
    // Accumulate until null terminator
    if (state->header_accum_pos >= state->max_name_bytes) {
      return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
          "gzip FNAME exceeds limit %lu bytes",
          (unsigned long)state->max_name_bytes);
    }
    state->header_accum[state->header_accum_pos++] = byte;
    if (byte == 0) {
      // Null terminator found
      state->header_info.name = (char *)malloc(state->header_accum_pos);
      if (!state->header_info.name) {
        return gcomp_decoder_set_error(
            decoder, GCOMP_ERR_MEMORY, "failed to allocate FNAME buffer");
      }
      gcomp_memory_track_alloc(&state->mem_tracker, state->header_accum_pos);
      memcpy(state->header_info.name, state->header_accum,
          state->header_accum_pos);

      // Move to next field
      if (state->header_info.flg & GZIP_FLG_FCOMMENT) {
        state->header_stage = GZIP_HEADER_FCOMMENT;
      }
      else if (state->header_info.flg & GZIP_FLG_FHCRC) {
        state->header_stage = GZIP_HEADER_FHCRC;
      }
      else {
        state->header_stage = GZIP_HEADER_DONE;
      }
      state->header_accum_pos = 0;
    }
    break;

  case GZIP_HEADER_FCOMMENT:
    // Accumulate until null terminator
    if (state->header_accum_pos >= state->max_comment_bytes) {
      return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
          "gzip FCOMMENT exceeds limit %lu bytes",
          (unsigned long)state->max_comment_bytes);
    }
    state->header_accum[state->header_accum_pos++] = byte;
    if (byte == 0) {
      // Null terminator found
      state->header_info.comment = (char *)malloc(state->header_accum_pos);
      if (!state->header_info.comment) {
        return gcomp_decoder_set_error(
            decoder, GCOMP_ERR_MEMORY, "failed to allocate FCOMMENT buffer");
      }
      gcomp_memory_track_alloc(&state->mem_tracker, state->header_accum_pos);
      memcpy(state->header_info.comment, state->header_accum,
          state->header_accum_pos);

      // Move to next field
      if (state->header_info.flg & GZIP_FLG_FHCRC) {
        state->header_stage = GZIP_HEADER_FHCRC;
      }
      else {
        state->header_stage = GZIP_HEADER_DONE;
      }
      state->header_accum_pos = 0;
    }
    break;

  case GZIP_HEADER_FHCRC:
    state->header_accum[state->header_accum_pos++] = byte;
    if (state->header_accum_pos == 2) {
      uint16_t header_crc = gzip_read_le16(state->header_accum);
      state->header_info.header_crc = header_crc;

      // Validate header CRC (lower 16 bits of CRC32)
      uint32_t computed = gcomp_crc32_finalize(state->header_crc_accum);
      uint16_t computed_crc16 = (uint16_t)(computed & 0xFFFF);
      if (header_crc != computed_crc16) {
        return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
            "gzip header CRC mismatch: expected 0x%04X, got 0x%04X", header_crc,
            computed_crc16);
      }

      state->header_stage = GZIP_HEADER_DONE;
      state->header_accum_pos = 0;
    }
    break;

  case GZIP_HEADER_DONE:
    // Should not reach here
    break;
  }

  return GCOMP_OK;
}

//
// Decoder Update
//

/**
 * @brief Process input and produce decompressed output.
 *
 * This function implements the main decoding loop, processing gzip data
 * through HEADER → BODY → TRAILER stages. It handles streaming semantics
 * by maintaining internal state across calls.
 *
 * ## Concatenated Member Handling
 *
 * The outer while loop is critical for correctly handling concatenated gzip
 * streams (multiple gzip members joined together). Without this loop:
 *
 * - After processing member 1's trailer, the decoder would return
 * - The caller would need to call update() again to process member 2
 * - But member 2's header might already be in the current input buffer
 *
 * With the outer loop:
 * - After member 1's trailer validates, if concat is enabled, we reset state
 * - The loop continues, immediately processing member 2's header
 * - Output is continuous across members
 * - Single update() call can process multiple complete members
 *
 * ## Loop Termination Conditions
 *
 * The outer loop continues while ALL of these are true:
 * 1. state->stage != DONE (not finished decoding)
 * 2. state->stage != ERROR (no unrecoverable error)
 * 3. input->used < input->size (input available)
 * 4. output->used < output->size (output space available)
 *
 * The output space check is essential: without it, the loop would spin
 * indefinitely when the output buffer fills (e.g., 1-byte output tests).
 * The decoder must return control to the caller to consume output.
 *
 * @param decoder Decoder instance
 * @param input Input buffer (consumed bytes tracked via input->used)
 * @param output Output buffer (produced bytes tracked via output->used)
 * @return GCOMP_OK on progress, error code on failure
 */
gcomp_status_t gzip_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!decoder || !decoder->method_state || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gzip_decoder_state_t * state = (gzip_decoder_state_t *)decoder->method_state;
  gcomp_status_t status;
  const uint8_t * input_data = (const uint8_t *)input->data;

  // Outer loop for concatenated member handling. This loop continues as long
  // as we have input, output space, and haven't finished or hit an error.
  // See function documentation for detailed explanation of why this is needed.
  while (state->stage != GZIP_DEC_STAGE_DONE &&
      state->stage != GZIP_DEC_STAGE_ERROR && input->used < input->size &&
      output->used < output->size) {

    // HEADER stage: parse header bytes
  while (state->stage == GZIP_DEC_STAGE_HEADER && input->used < input->size) {
    uint8_t byte = input_data[input->used++];
    state->total_input_bytes++;

    status = parse_header_byte(state, byte, decoder);
    if (status != GCOMP_OK) {
      state->stage = GZIP_DEC_STAGE_ERROR;
      return status;
    }

    if (state->header_stage == GZIP_HEADER_DONE) {
      state->stage = GZIP_DEC_STAGE_BODY;
      state->crc32 = GCOMP_CRC32_INIT;
      state->isize = 0;
      break;
    }
  }

  // BODY stage: pass through deflate, tracking CRC32/ISIZE
  uint8_t * output_data = (uint8_t *)output->data;
  if (state->stage == GZIP_DEC_STAGE_BODY) {
    size_t output_before = output->used;
    size_t input_before = input->used;

    gcomp_status_t deflate_status =
        gcomp_decoder_update(state->inner_decoder, input, output);

    // Track input/output bytes
    state->total_input_bytes += (input->used - input_before);
    size_t output_produced = output->used - output_before;
    state->total_output_bytes += output_produced;

    // Update CRC32 and ISIZE with output
    if (output_produced > 0) {
      state->crc32 = gcomp_crc32_update(
          state->crc32, output_data + output_before, output_produced);
      state->isize += (uint32_t)output_produced;
    }

    // Check for errors from deflate update
    if (deflate_status != GCOMP_OK) {
      state->stage = GZIP_DEC_STAGE_ERROR;
      return gcomp_decoder_set_error(decoder, deflate_status,
          "deflate decoder update failed: %s",
          gcomp_decoder_get_error_detail(state->inner_decoder));
    }

    // Check output size limit (gzip-level check, in addition to deflate's)
    if (state->max_output_bytes > 0 &&
        state->total_output_bytes > state->max_output_bytes) {
      state->stage = GZIP_DEC_STAGE_ERROR;
      return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
          "gzip output size %llu exceeds limit %llu",
          (unsigned long long)state->total_output_bytes,
          (unsigned long long)state->max_output_bytes);
    }

    // Check expansion ratio limit (gzip-level check)
    if (state->max_expansion_ratio > 0 && state->total_input_bytes > 0) {
      status = gcomp_limits_check_expansion_ratio(state->total_input_bytes,
          state->total_output_bytes, state->max_expansion_ratio);
      if (status != GCOMP_OK) {
        state->stage = GZIP_DEC_STAGE_ERROR;
        return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
            "gzip expansion ratio %llu exceeds limit %llu (input=%llu, "
            "output=%llu)",
            (unsigned long long)(state->total_output_bytes /
                (state->total_input_bytes > 0 ? state->total_input_bytes : 1)),
            (unsigned long long)state->max_expansion_ratio,
            (unsigned long long)state->total_input_bytes,
            (unsigned long long)state->total_output_bytes);
      }
    }

    // Check if deflate is done by trying finish
    // This is a simplification - ideally deflate would signal stream end
    gcomp_buffer_t empty_out = {
        output_data + output->used, output->size - output->used, 0};
    status = gcomp_decoder_finish(state->inner_decoder, &empty_out);
    if (status == GCOMP_OK) {
      // Deflate stream complete, move to trailer
      output->used += empty_out.used;

      // Update CRC32/ISIZE for any final output
      if (empty_out.used > 0) {
        state->crc32 = gcomp_crc32_update(state->crc32,
            output_data + output->used - empty_out.used, empty_out.used);
        state->isize += (uint32_t)empty_out.used;
        state->total_output_bytes += empty_out.used;

        // Check output size limit for final output
        if (state->max_output_bytes > 0 &&
            state->total_output_bytes > state->max_output_bytes) {
          state->stage = GZIP_DEC_STAGE_ERROR;
          return gcomp_decoder_set_error(decoder, GCOMP_ERR_LIMIT,
              "gzip output size %llu exceeds limit %llu",
              (unsigned long long)state->total_output_bytes,
              (unsigned long long)state->max_output_bytes);
        }
      }

      state->stage = GZIP_DEC_STAGE_TRAILER;
      state->trailer_pos = 0;

      // Retrieve any unconsumed bytes from deflate's bit buffer.
      // In streaming mode, deflate may have read trailer bytes into its
      // bit buffer before detecting end-of-stream. These bytes weren't
      // returned to input (they were consumed in previous calls), so we
      // need to retrieve them explicitly for trailer parsing.
      uint32_t unconsumed = gcomp_deflate_decoder_get_unconsumed_bytes(
          state->inner_decoder);
      if (unconsumed > 0 && unconsumed <= GZIP_TRAILER_SIZE) {
        gcomp_deflate_decoder_get_unconsumed_data(state->inner_decoder,
            state->trailer_buf, unconsumed);
        state->trailer_pos = unconsumed;
      }
    }
  }

  // TRAILER stage: accumulate and validate trailer
  while (state->stage == GZIP_DEC_STAGE_TRAILER && input->used < input->size) {
    state->trailer_buf[state->trailer_pos++] = input_data[input->used++];
    state->total_input_bytes++;

    if (state->trailer_pos >= GZIP_TRAILER_SIZE) {
      // Parse trailer
      uint32_t expected_crc = gzip_read_le32(state->trailer_buf);
      uint32_t expected_isize = gzip_read_le32(state->trailer_buf + 4);

      // Finalize CRC32
      uint32_t actual_crc = gcomp_crc32_finalize(state->crc32);

      // Validate CRC32
      if (actual_crc != expected_crc) {
        state->stage = GZIP_DEC_STAGE_ERROR;
        return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
            "gzip CRC32 mismatch: expected 0x%08X, computed 0x%08X",
            expected_crc, actual_crc);
      }

      // Validate ISIZE
      if (state->isize != expected_isize) {
        state->stage = GZIP_DEC_STAGE_ERROR;
        return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
            "gzip ISIZE mismatch: expected %u, computed %u", expected_isize,
            state->isize);
      }

      // Check for concatenated members
      if (state->concat_enabled && input->used < input->size) {
        // More data available, check if it's another gzip member
        if (input->size - input->used >= 2 &&
            input_data[input->used] == GZIP_ID1 &&
            input_data[input->used + 1] == GZIP_ID2) {
          // Reset for next member
          reset_header_parser(state);
          state->stage = GZIP_DEC_STAGE_HEADER;
          state->crc32 = GCOMP_CRC32_INIT;
          state->isize = 0;
          state->trailer_pos = 0;

          // Reset inner deflate decoder
          status = gcomp_decoder_reset(state->inner_decoder);
          if (status != GCOMP_OK) {
            state->stage = GZIP_DEC_STAGE_ERROR;
            return status;
          }
          continue;
        }
      }
      // If concat is disabled and there's extra data, we just mark as DONE.
      // The extra input bytes remain unconsumed (input->used < input->size).
      // This matches standard gzip behavior where trailing data is ignored.

      state->stage = GZIP_DEC_STAGE_DONE;
      break;
    }
  }

  } // end outer while loop for concatenated members

  return GCOMP_OK;
}

//
// Decoder Finish
//

gcomp_status_t gzip_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gzip_decoder_state_t * state = (gzip_decoder_state_t *)decoder->method_state;

  switch (state->stage) {
  case GZIP_DEC_STAGE_DONE:
    return GCOMP_OK;

  case GZIP_DEC_STAGE_ERROR:
    return decoder->last_error;

  case GZIP_DEC_STAGE_HEADER:
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
        "gzip stream truncated in header (stage %d)", state->header_stage);

  case GZIP_DEC_STAGE_BODY:
    return gcomp_decoder_set_error(
        decoder, GCOMP_ERR_CORRUPT, "gzip stream truncated in deflate data");

  case GZIP_DEC_STAGE_TRAILER:
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_CORRUPT,
        "gzip stream truncated in trailer (%zu of %d bytes)",
        state->trailer_pos, GZIP_TRAILER_SIZE);

  default:
    return gcomp_decoder_set_error(decoder, GCOMP_ERR_INTERNAL,
        "unexpected gzip decoder stage: %d", state->stage);
  }

  (void)output; // Output not used in finish
}

//
// Decoder Reset
//

gcomp_status_t gzip_decoder_reset(gcomp_decoder_t * decoder) {
  if (!decoder || !decoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gzip_decoder_state_t * state = (gzip_decoder_state_t *)decoder->method_state;

  // Reset inner deflate decoder
  gcomp_status_t status = gcomp_decoder_reset(state->inner_decoder);
  if (status != GCOMP_OK) {
    return status;
  }

  // Reset gzip state
  state->crc32 = GCOMP_CRC32_INIT;
  state->isize = 0;
  state->stage = GZIP_DEC_STAGE_HEADER;
  reset_header_parser(state);
  state->trailer_pos = 0;
  state->total_input_bytes = 0;
  state->total_output_bytes = 0;

  // Clear error state
  decoder->last_error = GCOMP_OK;
  decoder->error_detail[0] = '\0';

  return GCOMP_OK;
}

//
// Decoder Destroy
//

void gzip_decoder_destroy(gcomp_decoder_t * decoder) {
  if (!decoder || !decoder->method_state) {
    return;
  }

  gzip_decoder_state_t * state = (gzip_decoder_state_t *)decoder->method_state;

  // Destroy inner decoder
  if (state->inner_decoder) {
    gcomp_decoder_destroy(state->inner_decoder);
    state->inner_decoder = NULL;
  }

  // Track frees for header info allocations
  if (state->header_info.name) {
    gcomp_memory_track_free(
        &state->mem_tracker, strlen(state->header_info.name) + 1);
  }
  if (state->header_info.comment) {
    gcomp_memory_track_free(
        &state->mem_tracker, strlen(state->header_info.comment) + 1);
  }
  if (state->header_info.extra) {
    gcomp_memory_track_free(&state->mem_tracker, state->header_info.extra_len);
  }

  // Free header info
  gzip_header_info_free(&state->header_info);

  // Track state free
  gcomp_memory_track_free(&state->mem_tracker, sizeof(gzip_decoder_state_t));

  // Free state
  free(state);
  decoder->method_state = NULL;
}
