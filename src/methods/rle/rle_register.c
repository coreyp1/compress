/**
 * @file rle_register.c
 *
 * RLE method registration for the Ghoti.io Compress library.
 *
 * - Method descriptor with vtable hooks for encoder/decoder
 * - Option schema: rle.format (string), limits.max_output_bytes,
 *   limits.max_memory_bytes, limits.max_expansion_ratio. Unknown keys
 *   cause GCOMP_UNKNOWN_KEY_ERROR at create time.
 * - Public registration and auto-registration (GCOMP_AUTOREG_METHOD)
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>
#include "../../autoreg/autoreg_platform.h"
#include "../../core/stream_internal.h"
#include "rle_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/rle.h>
#include "../../core/bound_internal.h"
#include <stddef.h>
#include <stdint.h>

//
// Option schema (R1.1): rle.format + limits.*, unknown key = error
//

static const char RLE_DEFAULT_FORMAT[] = "packbits";

static const gcomp_option_schema_t g_rle_option_schemas[] = {
    {
        "rle.format",                  // key
        GCOMP_OPT_STRING,              // type
        1,                             // has_default
        {.str = RLE_DEFAULT_FORMAT},   // default_value
        0,                             // has_min
        0,                             // has_max
        0,                             // min_int
        0,                             // max_int
        0,                             // min_uint
        0,                             // max_uint
        "RLE format: packbits or tga", // help
    },
    {
        "limits.max_output_bytes", // key
        GCOMP_OPT_UINT64,          // type
        1,                         // has_default
        {.ui64 = 0},               // default_value (0 = unlimited)
        0,                         // has_min
        0,                         // has_max
        0,                         // min_int
        0,                         // max_int
        0,                         // min_uint
        0,                         // max_uint
        "Maximum decompressed output bytes",
    },
    {
        "limits.max_memory_bytes", // key
        GCOMP_OPT_UINT64,          // type
        1,                         // has_default
        {.ui64 = 0},               // default_value
        0,                         // has_min
        0,                         // has_max
        0,                         // min_int
        0,                         // max_int
        0,                         // min_uint
        0,                         // max_uint
        "Maximum memory usage",
    },
    {
        "limits.max_expansion_ratio", // key
        GCOMP_OPT_UINT64,             // type
        1,                            // has_default
        {.ui64 = 0},                  // default_value
        0,                            // has_min
        0,                            // has_max
        0,                            // min_int
        0,                            // max_int
        0,                            // min_uint
        0,                            // max_uint
        "Decompression bomb protection (output/input ratio)",
    },
};

static const char * const g_rle_option_keys[] = {
    "rle.format",
    "limits.max_output_bytes",
    "limits.max_memory_bytes",
    "limits.max_expansion_ratio",
};

static const gcomp_method_schema_t g_rle_schema = {
    g_rle_option_schemas,
    sizeof(g_rle_option_schemas) / sizeof(g_rle_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR,
    g_rle_option_keys,
};

static const gcomp_method_schema_t * rle_get_schema(void) {
  return &g_rle_schema;
}

//
// Encoder/decoder wrappers
//

static gcomp_status_t rle_encoder_update_wrapper(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  return rle_encoder_update(encoder, input, output);
}

static gcomp_status_t rle_encoder_finish_wrapper(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  return rle_encoder_finish(encoder, output);
}

static gcomp_status_t rle_decoder_update_wrapper(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  return rle_decoder_update(decoder, input, output);
}

static gcomp_status_t rle_decoder_finish_wrapper(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  return rle_decoder_finish(decoder, output);
}

//
// Factory functions
//

static gcomp_status_t rle_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t status = rle_encoder_init(registry, options, *encoder_out);
  if (status != GCOMP_OK) {
    return status;
  }

  (*encoder_out)->update_fn = rle_encoder_update_wrapper;
  (*encoder_out)->finish_fn = rle_encoder_finish_wrapper;
  (*encoder_out)->flush_fn = rle_encoder_flush;
  (*encoder_out)->reset_fn = rle_encoder_reset;
  return GCOMP_OK;
}

static gcomp_status_t rle_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t status = rle_decoder_init(registry, options, *decoder_out);
  if (status != GCOMP_OK) {
    return status;
  }

  (*decoder_out)->update_fn = rle_decoder_update_wrapper;
  (*decoder_out)->finish_fn = rle_decoder_finish_wrapper;
  (*decoder_out)->reset_fn = rle_decoder_reset;
  return GCOMP_OK;
}

static void rle_destroy_encoder_wrapper(gcomp_encoder_t * encoder) {
  rle_encoder_destroy(encoder);
}

static void rle_destroy_decoder_wrapper(gcomp_decoder_t * decoder) {
  rle_decoder_destroy(decoder);
}

//
// Method descriptor
//

//
// Worst-case encoded size
//

/**
 * @brief Largest RLE stream this encoder can produce for @p input_size.
 *
 * Not "every byte a literal", which is the number TIFF 6.0 section 9 suggests
 * allocating and which is wrong for this encoder.  A literal packet costs one
 * control byte and carries up to 128 bytes, and a run packet costs two bytes
 * and carries from two bytes upward - and this encoder starts a run at a
 * length of two, so a two-byte run interrupts the literal packet it is sitting
 * in and forces a second control byte.
 *
 * The worst case is therefore data that alternates: one byte that cannot run,
 * then two that can.  Three bytes in, four bytes out.
 *
 * Writing L for the number of literal packets and R for run packets, with Lb
 * and Rb the input bytes each carries:
 *
 *   output = L + Lb + 2R,   input = Lb + Rb,   Rb >= 2R
 *
 * so output <= L + Lb + Rb = L + n.  Two literal packets cannot be adjacent -
 * they would have been one - so L <= R + 1 <= Rb/2 + 1, and L <= Lb.  Both
 * bounds on L are largest together when Lb = Rb/2, which with Lb + Rb = n puts
 * L at n/3.
 *
 * Hence n + n/3, rounded up, plus one for the final partial packet.  Both
 * profiles have the same shape - the TGA packet header carries the same 7-bit
 * count, and its run packet is likewise a header and one byte - so the bound
 * is the same for each.
 */
static gcomp_status_t rle_encode_bound(
    gcomp_options_t * options, size_t input_size, size_t * bound_out) {
  (void)options;
  if (!bound_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (input_size == 0) {
    *bound_out = 0;
    return GCOMP_OK;
  }

  size_t packets = 0;
  gcomp_status_t s = gcomp_bound_block_count(input_size, 3u, &packets);
  if (s != GCOMP_OK) {
    return s;
  }

  size_t bound = input_size;
  s = gcomp_bound_add(&bound, packets);
  if (s == GCOMP_OK) {
    s = gcomp_bound_add(&bound, 1u);
  }
  if (s != GCOMP_OK) {
    return s;
  }
  *bound_out = bound;
  return GCOMP_OK;
}

static const gcomp_method_t g_rle_method = {
    .abi_version = GCOMP_METHOD_ABI_VERSION,
    .size = sizeof(gcomp_method_t),
    .name = "rle",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = rle_create_encoder,
    .create_decoder = rle_create_decoder,
    .destroy_encoder = rle_destroy_encoder_wrapper,
    .destroy_decoder = rle_destroy_decoder_wrapper,
    .get_schema = rle_get_schema,
    .encode_bound = rle_encode_bound,
};

gcomp_status_t gcomp_method_rle_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_rle_method);
}

GCOMP_AUTOREG_METHOD(rle, gcomp_method_rle_register)
