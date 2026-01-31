/**
 * @file gzip_encoder.c
 *
 * Streaming gzip (RFC 1952) wrapper encoder for the Ghoti.io Compress library.
 *
 * The gzip encoder wraps the deflate encoder, adding:
 * - RFC 1952 header (magic, CM, FLG, MTIME, XFL, OS, optional fields)
 * - CRC32 tracking of uncompressed input
 * - RFC 1952 trailer (CRC32, ISIZE)
 *
 * The encoder operates in three stages:
 * 1. HEADER: Write gzip header to output
 * 2. BODY: Pass input through deflate encoder, tracking CRC32/ISIZE
 * 3. TRAILER: Write CRC32 and ISIZE to output
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../core/alloc_internal.h"
#include "../../core/registry_internal.h"
#include "gzip_internal.h"
#include <ghoti.io/compress/crc32.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/stream.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//
// Helper: Extract deflate/limits options for pass-through
//

static gcomp_status_t extract_passthrough_options(
    const gcomp_options_t * src, gcomp_options_t ** dst_out) {
  // For now, just clone the entire options object
  // The deflate method will ignore unknown keys with its schema
  if (!src) {
    *dst_out = NULL;
    return GCOMP_OK;
  }
  return gcomp_options_clone(src, dst_out);
}

//
// Helper: Compute XFL based on compression level
//

static uint8_t compute_xfl(int64_t level) {
  // Per RFC 1952:
  // XFL = 2 for maximum compression (slowest algorithm)
  // XFL = 4 for fastest algorithm
  if (level <= 2) {
    return 4; // Fastest
  }
  else if (level >= 6) {
    return 2; // Maximum compression
  }
  else {
    return 0; // Default
  }
}

//
// Helper: Read options into header_info
//

static gcomp_status_t read_encoder_options(const gcomp_options_t * options,
    gzip_header_info_t * info, uint8_t * xfl_out, int * has_explicit_xfl) {
  gcomp_status_t status;
  uint64_t u64_val;
  int bool_val;
  const char * str_val;
  const void * bytes_val;
  size_t bytes_len;

  // Initialize defaults
  info->mtime = 0;
  info->os = GZIP_OS_UNKNOWN;
  info->xfl = 0;
  info->flg = 0;
  info->extra = NULL;
  info->extra_len = 0;
  info->name = NULL;
  info->comment = NULL;
  info->header_crc = 0;
  *xfl_out = 0;
  *has_explicit_xfl = 0;

  if (!options) {
    return GCOMP_OK;
  }

  // gzip.mtime
  status = gcomp_options_get_uint64(options, "gzip.mtime", &u64_val);
  if (status == GCOMP_OK) {
    info->mtime = (uint32_t)u64_val;
  }

  // gzip.os
  status = gcomp_options_get_uint64(options, "gzip.os", &u64_val);
  if (status == GCOMP_OK) {
    info->os = (uint8_t)u64_val;
  }

  // gzip.xfl (explicit)
  status = gcomp_options_get_uint64(options, "gzip.xfl", &u64_val);
  if (status == GCOMP_OK) {
    *xfl_out = (uint8_t)u64_val;
    *has_explicit_xfl = 1;
  }

  // gzip.name
  status = gcomp_options_get_string(options, "gzip.name", &str_val);
  if (status == GCOMP_OK && str_val) {
    size_t len = strlen(str_val);
    info->name = (char *)malloc(len + 1);
    if (!info->name) {
      return GCOMP_ERR_MEMORY;
    }
    memcpy(info->name, str_val, len + 1);
    info->flg |= GZIP_FLG_FNAME;
  }

  // gzip.comment
  status = gcomp_options_get_string(options, "gzip.comment", &str_val);
  if (status == GCOMP_OK && str_val) {
    size_t len = strlen(str_val);
    info->comment = (char *)malloc(len + 1);
    if (!info->comment) {
      gzip_header_info_free(info);
      return GCOMP_ERR_MEMORY;
    }
    memcpy(info->comment, str_val, len + 1);
    info->flg |= GZIP_FLG_FCOMMENT;
  }

  // gzip.extra
  status =
      gcomp_options_get_bytes(options, "gzip.extra", &bytes_val, &bytes_len);
  if (status == GCOMP_OK && bytes_val && bytes_len > 0) {
    info->extra = (uint8_t *)malloc(bytes_len);
    if (!info->extra) {
      gzip_header_info_free(info);
      return GCOMP_ERR_MEMORY;
    }
    memcpy(info->extra, bytes_val, bytes_len);
    info->extra_len = bytes_len;
    info->flg |= GZIP_FLG_FEXTRA;
  }

  // gzip.header_crc
  status = gcomp_options_get_bool(options, "gzip.header_crc", &bool_val);
  if (status == GCOMP_OK && bool_val) {
    info->flg |= GZIP_FLG_FHCRC;
  }

  return GCOMP_OK;
}

//
// Encoder Init
//

gcomp_status_t gzip_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  if (!registry || !encoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Look up deflate method
  const gcomp_method_t * deflate_method =
      gcomp_registry_find(registry, "deflate");
  if (!deflate_method) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_UNSUPPORTED,
        "gzip requires deflate method to be registered");
  }

  // Allocate state
  gzip_encoder_state_t * state =
      (gzip_encoder_state_t *)calloc(1, sizeof(gzip_encoder_state_t));
  if (!state) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_MEMORY, "failed to allocate gzip encoder state");
  }

  // Read options and prepare header info
  uint8_t explicit_xfl;
  int has_explicit_xfl;
  gcomp_status_t status = read_encoder_options(
      options, &state->header_info, &explicit_xfl, &has_explicit_xfl);
  if (status != GCOMP_OK) {
    free(state);
    return status;
  }

  // Extract pass-through options for deflate
  gcomp_options_t * deflate_options = NULL;
  status = extract_passthrough_options(options, &deflate_options);
  if (status != GCOMP_OK) {
    gzip_header_info_free(&state->header_info);
    free(state);
    return status;
  }

  // Create inner deflate encoder
  status = gcomp_encoder_create(
      registry, "deflate", deflate_options, &state->inner_encoder);
  if (deflate_options) {
    gcomp_options_destroy(deflate_options);
  }
  if (status != GCOMP_OK) {
    gzip_header_info_free(&state->header_info);
    free(state);
    return gcomp_encoder_set_error(
        encoder, status, "failed to create inner deflate encoder");
  }

  // Compute XFL if not explicitly set
  if (!has_explicit_xfl) {
    int64_t level = 6; // Default
    if (options) {
      gcomp_options_get_int64(options, "deflate.level", &level);
    }
    state->header_info.xfl = compute_xfl(level);
  }
  else {
    state->header_info.xfl = explicit_xfl;
  }

  // Build header
  status = gzip_write_header(&state->header_info, state->header_buf,
      sizeof(state->header_buf), &state->header_len);
  if (status != GCOMP_OK) {
    gcomp_encoder_destroy(state->inner_encoder);
    gzip_header_info_free(&state->header_info);
    free(state);
    return gcomp_encoder_set_error(
        encoder, status, "failed to build gzip header");
  }

  // Initialize state
  state->crc32 = GCOMP_CRC32_INIT;
  state->isize = 0;
  state->stage = GZIP_ENC_STAGE_HEADER;
  state->header_pos = 0;
  state->trailer_pos = 0;

  encoder->method_state = state;
  return GCOMP_OK;
}

//
// Encoder Update
//

gcomp_status_t gzip_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!encoder || !encoder->method_state || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gzip_encoder_state_t * state = (gzip_encoder_state_t *)encoder->method_state;
  gcomp_status_t status;

  // HEADER stage: write header bytes
  uint8_t * output_data = (uint8_t *)output->data;
  if (state->stage == GZIP_ENC_STAGE_HEADER) {
    size_t avail_out = output->size - output->used;
    size_t header_remaining = state->header_len - state->header_pos;
    size_t to_write =
        (avail_out < header_remaining) ? avail_out : header_remaining;

    if (to_write > 0) {
      memcpy(output_data + output->used, state->header_buf + state->header_pos,
          to_write);
      output->used += to_write;
      state->header_pos += to_write;
    }

    if (state->header_pos >= state->header_len) {
      state->stage = GZIP_ENC_STAGE_BODY;
    }

    // If output buffer is full, return and let caller provide more space
    if (output->used >= output->size) {
      return GCOMP_OK;
    }
  }

  // BODY stage: pass through deflate, tracking CRC32/ISIZE
  if (state->stage == GZIP_ENC_STAGE_BODY) {
    // Track input bytes for CRC32/ISIZE before calling deflate
    size_t input_before = input->used;

    status = gcomp_encoder_update(state->inner_encoder, input, output);
    if (status != GCOMP_OK) {
      return gcomp_encoder_set_error(encoder, status,
          "deflate encoder update failed: %s",
          gcomp_encoder_get_error_detail(state->inner_encoder));
    }

    // Update CRC32 and ISIZE with consumed input
    size_t consumed = input->used - input_before;
    if (consumed > 0) {
      const uint8_t * input_data = (const uint8_t *)input->data;
      state->crc32 =
          gcomp_crc32_update(state->crc32, input_data + input_before, consumed);
      state->isize += (uint32_t)consumed;
    }
  }

  return GCOMP_OK;
}

//
// Encoder Finish
//

gcomp_status_t gzip_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  if (!encoder || !encoder->method_state || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gzip_encoder_state_t * state = (gzip_encoder_state_t *)encoder->method_state;
  gcomp_status_t status;
  uint8_t * output_data = (uint8_t *)output->data;

  // If still in HEADER stage with remaining header bytes
  if (state->stage == GZIP_ENC_STAGE_HEADER) {
    size_t avail_out = output->size - output->used;
    size_t header_remaining = state->header_len - state->header_pos;
    size_t to_write =
        (avail_out < header_remaining) ? avail_out : header_remaining;

    if (to_write > 0) {
      memcpy(output_data + output->used, state->header_buf + state->header_pos,
          to_write);
      output->used += to_write;
      state->header_pos += to_write;
    }

    if (state->header_pos >= state->header_len) {
      state->stage = GZIP_ENC_STAGE_BODY;
    }
    else {
      // Need more output space
      return GCOMP_OK;
    }
  }

  // BODY stage: finish deflate
  if (state->stage == GZIP_ENC_STAGE_BODY) {
    status = gcomp_encoder_finish(state->inner_encoder, output);
    if (status != GCOMP_OK) {
      return gcomp_encoder_set_error(encoder, status,
          "deflate encoder finish failed: %s",
          gcomp_encoder_get_error_detail(state->inner_encoder));
    }

    // Check if deflate is done (output buffer not full means complete)
    // Note: We need a way to detect deflate completion
    // For now, assume if finish returns OK and there's space, it's done
    state->stage = GZIP_ENC_STAGE_TRAILER;

    // Build trailer
    uint32_t final_crc = gcomp_crc32_finalize(state->crc32);
    gzip_write_trailer(final_crc, state->isize, state->trailer_buf);
    state->trailer_pos = 0;
  }

  // TRAILER stage: write trailer
  if (state->stage == GZIP_ENC_STAGE_TRAILER) {
    size_t avail_out = output->size - output->used;
    size_t trailer_remaining = GZIP_TRAILER_SIZE - state->trailer_pos;
    size_t to_write =
        (avail_out < trailer_remaining) ? avail_out : trailer_remaining;

    if (to_write > 0) {
      memcpy(output_data + output->used,
          state->trailer_buf + state->trailer_pos, to_write);
      output->used += to_write;
      state->trailer_pos += to_write;
    }

    if (state->trailer_pos >= GZIP_TRAILER_SIZE) {
      state->stage = GZIP_ENC_STAGE_DONE;
    }
    else {
      // Need more output space
      return GCOMP_OK;
    }
  }

  // DONE stage
  if (state->stage == GZIP_ENC_STAGE_DONE) {
    return GCOMP_OK;
  }

  return GCOMP_OK;
}

//
// Encoder Reset
//

gcomp_status_t gzip_encoder_reset(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gzip_encoder_state_t * state = (gzip_encoder_state_t *)encoder->method_state;

  // Reset inner deflate encoder
  gcomp_status_t status = gcomp_encoder_reset(state->inner_encoder);
  if (status != GCOMP_OK) {
    return status;
  }

  // Reset gzip state
  state->crc32 = GCOMP_CRC32_INIT;
  state->isize = 0;
  state->stage = GZIP_ENC_STAGE_HEADER;
  state->header_pos = 0;
  state->trailer_pos = 0;

  // Clear error state
  encoder->last_error = GCOMP_OK;
  encoder->error_detail[0] = '\0';

  return GCOMP_OK;
}

//
// Encoder Destroy
//

void gzip_encoder_destroy(gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return;
  }

  gzip_encoder_state_t * state = (gzip_encoder_state_t *)encoder->method_state;

  // Destroy inner encoder
  if (state->inner_encoder) {
    gcomp_encoder_destroy(state->inner_encoder);
    state->inner_encoder = NULL;
  }

  // Free header info
  gzip_header_info_free(&state->header_info);

  // Free state
  free(state);
  encoder->method_state = NULL;
}
