/**
 * @file deflate_register.c
 *
 * DEFLATE (RFC 1951) method registration for the Ghoti.io Compress library.
 *
 * This file provides:
 * - Method descriptor (`g_deflate_method`) with vtable hooks for
 * encoder/decoder
 * - Option schema defining `deflate.level` and `deflate.window_bits`
 * - Public registration function `gcomp_method_deflate_register()`
 * - Auto-registration hook for the default registry
 *
 * ## Architecture
 *
 * The method registration follows the compress library's plugin architecture:
 *
 * 1. **Method Descriptor**: A static `gcomp_method_t` struct containing:
 *    - Metadata (name, ABI version, capabilities)
 *    - Function pointers for create/destroy encoder/decoder
 *    - Schema introspection hook
 *
 * 2. **Option Schema**: Defines valid options with types, defaults, and ranges.
 *    The schema is used for:
 *    - Validation via `gcomp_options_validate()`
 *    - Introspection via `gcomp_method_get_option_schema()`
 *    - Documentation generation
 *
 * 3. **Registration**: Methods are registered with a registry (hash table) by
 *    name. The registry owns a pointer to the method descriptor (not a copy).
 *
 * ## Auto-Registration
 *
 * When the library is loaded, `GCOMP_AUTOREG_METHOD` triggers a constructor
 * function that registers deflate with the default registry. This runs before
 * `main()` and allows immediate use without explicit initialization.
 *
 * To disable auto-registration, define `GCOMP_NO_AUTOREG` before including
 * library headers. See `documentation/auto-registration.md` for details.
 *
 * ## Thread Safety
 *
 * The method descriptor and option schema are immutable after compilation.
 * Registration with the default registry happens in a single-threaded context
 * (before `main()`). After initialization, the registry is read-only for
 * method lookups.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../autoreg/autoreg_platform.h"
#include "../../core/stream_internal.h"
#include "deflate_internal.h"
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <stddef.h>
#include <stdint.h>

//
// Option Schema
//
// The deflate method exposes two options:
//
// - `deflate.level` (int64, 0-9): Controls compression effort.
//   Level 0 produces stored blocks (no compression).
//   Levels 1-3 use fixed Huffman with increasing hash chain lengths.
//   Levels 4-9 use dynamic Huffman with longer searches for better ratios.
//
// - `deflate.window_bits` (uint64, 8-15): LZ77 window size as log2(bytes).
//   Default 15 gives 32 KiB, the maximum allowed by RFC 1951.
//   Smaller windows reduce memory usage but may hurt compression.
//
// Core limit options (`limits.max_output_bytes`, `limits.max_memory_bytes`)
// are handled by the core infrastructure, not the method schema.
//

#define DEFLATE_LEVEL_DEFAULT 6
#define DEFLATE_LEVEL_MIN 0
#define DEFLATE_LEVEL_MAX 9
#define DEFLATE_WINDOW_BITS_DEFAULT 15
#define DEFLATE_WINDOW_BITS_MIN 8
#define DEFLATE_WINDOW_BITS_MAX 15

static const gcomp_option_schema_t g_deflate_option_schemas[] = {
    {
        "deflate.level",                          /* key */
        GCOMP_OPT_INT64,                          /* type */
        1,                                        /* has_default */
        {.i64 = DEFLATE_LEVEL_DEFAULT},           /* default_value */
        1,                                        /* has_min */
        1,                                        /* has_max */
        DEFLATE_LEVEL_MIN,                        /* min_int */
        DEFLATE_LEVEL_MAX,                        /* max_int */
        0,                                        /* min_uint */
        0,                                        /* max_uint */
        "Compression level 0 (none) to 9 (best)", /* help */
    },
    {
        "deflate.window_bits",                         /* key */
        GCOMP_OPT_UINT64,                              /* type */
        1,                                             /* has_default */
        {.ui64 = DEFLATE_WINDOW_BITS_DEFAULT},         /* default_value */
        1,                                             /* has_min */
        1,                                             /* has_max */
        0,                                             /* min_int */
        0,                                             /* max_int */
        DEFLATE_WINDOW_BITS_MIN,                       /* min_uint */
        DEFLATE_WINDOW_BITS_MAX,                       /* max_uint */
        "LZ77 window size in bits (8..15, 32KiB max)", /* help */
    },
};

static const char * const g_deflate_option_keys[] = {
    "deflate.level",
    "deflate.window_bits",
};

static const gcomp_method_schema_t g_deflate_schema = {
    g_deflate_option_schemas,
    sizeof(g_deflate_option_schemas) / sizeof(g_deflate_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_ERROR,
    g_deflate_option_keys,
};

static const gcomp_method_schema_t * deflate_get_schema(void) {
  return &g_deflate_schema;
}

//
// Encoder/Decoder Wrappers
//
// These thin wrappers adapt the deflate-specific implementation functions
// to the generic function pointer signature expected by the method vtable.
// The actual compression logic lives in deflate_encode.c and deflate_decode.c.
//

static gcomp_status_t deflate_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  return gcomp_deflate_encoder_update(encoder, input, output);
}

static gcomp_status_t deflate_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  return gcomp_deflate_encoder_finish(encoder, output);
}

static gcomp_status_t deflate_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  return gcomp_deflate_decoder_update(decoder, input, output);
}

static gcomp_status_t deflate_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  return gcomp_deflate_decoder_finish(decoder, output);
}

//
// Encoder/Decoder Factory Functions
//
// The core stream infrastructure calls these to create method-specific
// encoders/decoders. The pattern is:
//
// 1. Core allocates the base encoder/decoder struct
// 2. Factory function initializes method-specific state (via _init)
// 3. Factory function sets the update/finish function pointers
// 4. Core returns the encoder/decoder to the caller
//
// On destruction, the method's destroy function frees method-specific state,
// then core frees the base struct.
//

static gcomp_status_t deflate_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t status =
      gcomp_deflate_encoder_init(registry, options, *encoder_out);
  if (status != GCOMP_OK) {
    return status;
  }

  (*encoder_out)->update_fn = deflate_encoder_update;
  (*encoder_out)->finish_fn = deflate_encoder_finish;
  return GCOMP_OK;
}

static gcomp_status_t deflate_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t status =
      gcomp_deflate_decoder_init(registry, options, *decoder_out);
  if (status != GCOMP_OK) {
    return status;
  }

  (*decoder_out)->update_fn = deflate_decoder_update;
  (*decoder_out)->finish_fn = deflate_decoder_finish;
  return GCOMP_OK;
}

static void deflate_destroy_encoder(gcomp_encoder_t * encoder) {
  gcomp_deflate_encoder_destroy(encoder);
}

static void deflate_destroy_decoder(gcomp_decoder_t * decoder) {
  gcomp_deflate_decoder_destroy(decoder);
}

//
// Method Descriptor
//
// The method descriptor is a static, immutable struct that describes the
// deflate compression method to the registry. Key fields:
//
// - abi_version: Version of the method ABI (for future compatibility)
// - size: sizeof(gcomp_method_t), allows ABI-safe struct extension
// - name: "deflate" - used for registry lookup
// - capabilities: GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE (both supported)
// - create_*/destroy_*: Factory and cleanup functions
// - get_schema: Returns option schema for validation/introspection
//
// The descriptor is registered by pointer (not copied), so it must have
// static storage duration.
//

static const gcomp_method_t g_deflate_method = {
    .abi_version = 1,
    .size = sizeof(gcomp_method_t),
    .name = "deflate",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = deflate_create_encoder,
    .create_decoder = deflate_create_decoder,
    .destroy_encoder = deflate_destroy_encoder,
    .destroy_decoder = deflate_destroy_decoder,
    .get_schema = deflate_get_schema,
};

//
// Public Registration Function
//
// Registers the deflate method with the given registry. This is idempotent:
// calling it when deflate is already registered returns GCOMP_OK.
//
// Most applications don't need to call this directly because auto-registration
// handles it. Use explicit registration when:
// - Using a custom registry (not the default)
// - Auto-registration is disabled (GCOMP_NO_AUTOREG defined)
// - Error handling for registration failure is required
//

gcomp_status_t gcomp_method_deflate_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_deflate_method);
}

//
// Auto-Registration Hook
//
// GCOMP_AUTOREG_METHOD expands to a constructor function that runs at library
// load time (before main). It registers deflate with the default registry.
//
// Implementation details:
// - GCC/Clang: __attribute__((constructor)) function
// - MSVC: Function pointer in .CRT$XCU section
// - Errors are silently ignored (void cast on return value)
//
// Define GCOMP_NO_AUTOREG to disable this and require explicit registration.
//
GCOMP_AUTOREG_METHOD(deflate, gcomp_method_deflate_register)
