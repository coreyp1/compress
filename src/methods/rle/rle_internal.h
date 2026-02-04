/**
 * @file rle_internal.h
 *
 * Internal declarations for the RLE (Run-Length Encoding) method.
 * Shared between encoder, decoder, core, and profile.
 *
 * DESIGN
 * ------
 * RLE is profile-driven: the same core (literal/repeat emission and output
 * bounds) is used by different token grammars (PackBits, TGA). Encoder state
 * holds a pending literal buffer and optional run (run_byte, run_len); the
 * profile turns input bytes into tokens and flushes on finish(). Decoder state
 * holds a partial-token phase so we can suspend and resume across update()
 * calls when input runs out mid-token (e.g. after a control byte but before
 * all literal bytes or the run byte).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_RLE_INTERNAL_H
#define GHOTI_IO_GCOMP_RLE_INTERNAL_H

#include "../../core/stream_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//
// RLE format names (option rle.format)
//

#define RLE_FORMAT_PACKBITS "packbits"
#define RLE_FORMAT_TGA "tga"

//
// Encoder state (accumulates literal run or run-length for next token)
//

#define RLE_ENCODER_MAX_LITERAL 128

typedef struct {
  const gcomp_allocator_t * allocator;
  char format[16]; ///< "packbits" or "tga"
  uint64_t max_output_bytes;
  uint64_t max_memory_bytes;
  uint64_t max_expansion_ratio;
  uint8_t literal_buf[RLE_ENCODER_MAX_LITERAL];
  size_t literal_count; ///< Current literal run length (0..128)
  uint8_t run_byte;     ///< Byte value of current run (if run_len > 0)
  size_t run_len;       ///< Current run length (0 = not in run)
} rle_encoder_state_t;

//
// Decoder partial-token state (mid-stream).
// Phases allow streaming: we can stop after consuming a control byte and
// resume in the next update() to read the following literal bytes or run byte.
//

typedef enum {
  RLE_DEC_CONTROL = 0,   ///< Need control/header byte
  RLE_DEC_LITERAL_BYTES, ///< Need N more literal bytes
  RLE_DEC_RUN_BYTE,      ///< Need the run byte (then emit N copies)
} rle_decoder_phase_t;

typedef struct {
  rle_decoder_phase_t phase;
  uint32_t pending_count; ///< Literal bytes to read, or run length to emit
  uint8_t pending_byte;   ///< Run byte (when phase == RLE_DEC_RUN_BYTE)
} rle_decoder_partial_t;

//
// Decoder state
//

typedef struct {
  const gcomp_allocator_t * allocator;
  char format[16]; ///< "packbits" or "tga"
  uint64_t max_output_bytes;
  uint64_t max_memory_bytes;
  uint64_t max_expansion_ratio;
  uint64_t total_input_bytes;
  uint64_t total_output_bytes;
  rle_decoder_partial_t partial; ///< Mid-token state across update() calls
} rle_decoder_state_t;

//
// Internal API: Encoder
//

gcomp_status_t rle_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder);

void rle_encoder_destroy(gcomp_encoder_t * encoder);

gcomp_status_t rle_encoder_update(
    gcomp_encoder_t * encoder, gcomp_buffer_t * input, gcomp_buffer_t * output);

gcomp_status_t rle_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output);

gcomp_status_t rle_encoder_reset(gcomp_encoder_t * encoder);

//
// Internal API: Decoder
//

gcomp_status_t rle_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder);

void rle_decoder_destroy(gcomp_decoder_t * decoder);

gcomp_status_t rle_decoder_update(
    gcomp_decoder_t * decoder, gcomp_buffer_t * input, gcomp_buffer_t * output);

gcomp_status_t rle_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output);

gcomp_status_t rle_decoder_reset(gcomp_decoder_t * decoder);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_RLE_INTERNAL_H
