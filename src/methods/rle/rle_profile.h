/**
 * @file rle_profile.h
 *
 * RLE profile interface: token parsing, span classification, stream
 * termination. Decode path: profiles parse tokens and call rle_core
 * (rle_emit_literal, rle_emit_repeat). Encode path: profiles write
 * control and data bytes directly. Profile id is selected by rle.format.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_RLE_PROFILE_H
#define GHOTI_IO_GCOMP_RLE_PROFILE_H

#include "rle_core.h"
#include "rle_internal.h"
#include <ghoti.io/compress/errors.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RLE profile identifier (matches rle.format option).
 */
typedef enum {
  RLE_PROFILE_PACKBITS,
  RLE_PROFILE_TGA,
  RLE_PROFILE_UNKNOWN,
} rle_profile_id_t;

/**
 * @brief Resolve profile id from format string.
 *
 * @param format "packbits" or "tga"
 * @return Profile id or RLE_PROFILE_UNKNOWN
 */
rle_profile_id_t rle_profile_from_string(const char * format);

/**
 * @brief Decode one profile's stream into raw output via rle_core.
 *
 * Consumes input and produces output. Updates state->partial for
 * mid-token state. Checks limits (max_output_bytes, max_expansion_ratio).
 *
 * @param state Decoder state (format, limits, partial, totals)
 * @param input_data Input encoded bytes
 * @param input_size Input length
 * @param input_used_out Bytes consumed from input
 * @param output_data Output buffer
 * @param output_size Output capacity
 * @param output_used_inout Current used (in), updated (out)
 * @return GCOMP_OK, GCOMP_ERR_LIMIT, or GCOMP_ERR_CORRUPT
 */
gcomp_status_t rle_profile_decode(rle_decoder_state_t * state,
    const uint8_t * input_data, size_t input_size, size_t * input_used_out,
    uint8_t * output_data, size_t output_size, size_t * output_used_inout);

/**
 * @brief Encode raw input into one profile's stream.
 *
 * Consumes input and writes encoded bytes to output. Uses state
 * for literal/run accumulation and format.
 *
 * @param state Encoder state (format, literal_buf, run state)
 * @param input_data Input raw bytes
 * @param input_size Input length
 * @param input_consumed_out Bytes consumed from input
 * @param output_data Output buffer
 * @param output_size Output capacity
 * @param output_used_inout Current used (in), updated (out)
 * @return GCOMP_OK or GCOMP_ERR_LIMIT
 */
gcomp_status_t rle_profile_encode(rle_encoder_state_t * state,
    const uint8_t * input_data, size_t input_size, size_t * input_consumed_out,
    uint8_t * output_data, size_t output_size, size_t * output_used_inout);

/**
 * @brief Flush encoder state (literal/run buffer) at end of stream.
 *
 * @param state Encoder state
 * @param output_data Output buffer
 * @param output_size Output capacity
 * @param output_used_inout Current used (in), updated (out)
 * @return GCOMP_OK or GCOMP_ERR_LIMIT
 */
gcomp_status_t rle_profile_encode_finish(rle_encoder_state_t * state,
    uint8_t * output_data, size_t output_size, size_t * output_used_inout);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_RLE_PROFILE_H
