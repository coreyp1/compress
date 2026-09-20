/**
 * @file gzip_register.c
 *
 * GZIP (RFC 1952) method registration for the Ghoti.io Compress library.
 *
 * This file provides:
 * - Method descriptor with vtable hooks for encoder/decoder
 * - Option schema defining gzip-specific and pass-through options
 * - Public registration function `gcomp_method_gzip_register()`
 * - Auto-registration hook for the default registry
 *
 * ## Architecture
 *
 * The gzip method is a **wrapper** around the deflate method. It adds:
 * - RFC 1952 header (magic bytes, flags, mtime, OS, optional fields)
 * - RFC 1952 trailer (CRC32 of uncompressed data, ISIZE)
 *
 * The actual compression/decompression is delegated to the deflate method.
 * This design avoids code duplication and ensures gzip benefits from any
 * improvements to deflate.
 *
 * ## Option Pass-Through
 *
 * Options with `deflate.*` prefix are forwarded to the inner deflate
 * encoder/decoder. Options with `limits.*` prefix are forwarded as well.
 * Gzip-specific options use the `gzip.*` prefix.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/macros.h>
#include "../../autoreg/autoreg_platform.h"
#include "../../core/stream_internal.h"
#include "gzip_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/gzip.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include "../../core/bound_internal.h"
#include <ghoti.io/compress/compress.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

//
// Option Schema
//
// The gzip method exposes these options:
//
// gzip-specific options:
// - `gzip.mtime` (uint64, default 0): Modification time as Unix timestamp
// - `gzip.os` (uint64, default 255): Operating system (255 = unknown)
// - `gzip.name` (string, optional): Original filename
// - `gzip.comment` (string, optional): File comment
// - `gzip.extra` (bytes, optional): FEXTRA field data
// - `gzip.header_crc` (bool, default false): Include FHCRC
// - `gzip.xfl` (uint64, optional): Extra flags (auto-calculated if not set)
// - `gzip.header_flags` (uint64, optional): Header flags byte (auto-calculated)
// - `gzip.concat` (bool, default false): Decoder: support concatenated members
//
// Header field size limits (decoder safety):
// - `gzip.max_name_bytes` (uint64, default 1 MiB)
// - `gzip.max_comment_bytes` (uint64, default 1 MiB)
// - `gzip.max_extra_bytes` (uint64, default 64 KiB)
//
// Pass-through options (forwarded to deflate):
// - `deflate.level`, `deflate.window_bits`, `deflate.strategy`
// - `limits.max_output_bytes`, `limits.max_memory_bytes`,
//   `limits.max_expansion_ratio`
//

#define GZIP_MTIME_DEFAULT 0
#define GZIP_OS_DEFAULT 255
#define GZIP_HEADER_CRC_DEFAULT 0
#define GZIP_CONCAT_DEFAULT 0
// GZIP_MAX_*_BYTES_DEFAULT are defined in gzip_internal.h

static const gcomp_option_schema_t g_gzip_option_schemas[] = {
    {
        "gzip.mtime",                         // key
        GCOMP_OPT_UINT64,                     // type
        1,                                    // has_default
        {.ui64 = GZIP_MTIME_DEFAULT},         // default_value
        0,                                    // has_min
        0,                                    // has_max
        0,                                    // min_int
        0,                                    // max_int
        0,                                    // min_uint
        0,                                    // max_uint
        "Modification time (Unix timestamp)", // help
    },
    {
        "gzip.os",                                 // key
        GCOMP_OPT_UINT64,                          // type
        1,                                         // has_default
        {.ui64 = GZIP_OS_DEFAULT},                 // default_value
        1,                                         // has_min
        1,                                         // has_max
        0,                                         // min_int
        0,                                         // max_int
        0,                                         // min_uint
        255,                                       // max_uint
        "Operating system (0-255, 255 = unknown)", // help
    },
    {
        "gzip.name",         // key
        GCOMP_OPT_STRING,    // type
        0,                   // has_default (optional)
        {.str = NULL},       // default_value
        0,                   // has_min
        0,                   // has_max
        0,                   // min_int
        0,                   // max_int
        0,                   // min_uint
        0,                   // max_uint
        "Original filename", // help
    },
    {
        "gzip.comment",   // key
        GCOMP_OPT_STRING, // type
        0,                // has_default (optional)
        {.str = NULL},    // default_value
        0,                // has_min
        0,                // has_max
        0,                // min_int
        0,                // max_int
        0,                // min_uint
        0,                // max_uint
        "File comment",   // help
    },
    {
        "gzip.extra",       // key
        GCOMP_OPT_BYTES,    // type
        0,                  // has_default (optional)
        {.str = NULL},      // default_value
        0,                  // has_min
        0,                  // has_max
        0,                  // min_int
        0,                  // max_int
        0,                  // min_uint
        0,                  // max_uint
        "Extra field data", // help
    },
    {
        "gzip.header_crc",                 // key
        GCOMP_OPT_BOOL,                    // type
        1,                                 // has_default
        {.b = GZIP_HEADER_CRC_DEFAULT},    // default_value
        0,                                 // has_min
        0,                                 // has_max
        0,                                 // min_int
        0,                                 // max_int
        0,                                 // min_uint
        0,                                 // max_uint
        "Include header CRC (FHCRC flag)", // help
    },
    {
        "gzip.text",                                     // key
        GCOMP_OPT_BOOL,                                  // type
        1,                                               // has_default
        {.b = 0},                                        // default_value
        0,                                               // has_min
        0,                                               // has_max
        0,                                               // min_int
        0,                                               // max_int
        0,                                               // min_uint
        0,                                               // max_uint
        "Set FTEXT flag to indicate ASCII text content", // help
    },
    {
        "gzip.xfl",       // key
        GCOMP_OPT_UINT64, // type
        0,                // has_default (auto-calculated)
        {.ui64 = 0},      // default_value
        1,                // has_min
        1,                // has_max
        0,                // min_int
        0,                // max_int
        0,                // min_uint
        255,              // max_uint
        "Extra flags (auto-calculated if not set)", // help
    },
    {
        "gzip.header_flags", // key
        GCOMP_OPT_UINT64,    // type
        0,                   // has_default (auto-calculated)
        {.ui64 = 0},         // default_value
        1,                   // has_min
        1,                   // has_max
        0,                   // min_int
        0,                   // max_int
        0,                   // min_uint
        255,                 // max_uint
        "Header flags byte (auto-calculated if not set)", // help
    },
    {
        "gzip.concat",                           // key
        GCOMP_OPT_BOOL,                          // type
        1,                                       // has_default
        {.b = GZIP_CONCAT_DEFAULT},              // default_value
        0,                                       // has_min
        0,                                       // has_max
        0,                                       // min_int
        0,                                       // max_int
        0,                                       // min_uint
        0,                                       // max_uint
        "Decoder: support concatenated members", // help
    },
    {
        "gzip.max_name_bytes",                 // key
        GCOMP_OPT_UINT64,                      // type
        1,                                     // has_default
        {.ui64 = GZIP_MAX_NAME_BYTES_DEFAULT}, // default_value
        1,                                     // has_min
        0,                                     // has_max
        0,                                     // min_int
        0,                                     // max_int
        1,                                     // min_uint
        0,                                     // max_uint
        "Decoder: max FNAME length in bytes",  // help
    },
    {
        "gzip.max_comment_bytes",                 // key
        GCOMP_OPT_UINT64,                         // type
        1,                                        // has_default
        {.ui64 = GZIP_MAX_COMMENT_BYTES_DEFAULT}, // default_value
        1,                                        // has_min
        0,                                        // has_max
        0,                                        // min_int
        0,                                        // max_int
        1,                                        // min_uint
        0,                                        // max_uint
        "Decoder: max FCOMMENT length in bytes",  // help
    },
    {
        "gzip.max_extra_bytes",                 // key
        GCOMP_OPT_UINT64,                       // type
        1,                                      // has_default
        {.ui64 = GZIP_MAX_EXTRA_BYTES_DEFAULT}, // default_value
        1,                                      // has_min
        0,                                      // has_max
        0,                                      // min_int
        0,                                      // max_int
        1,                                      // min_uint
        0,                                      // max_uint
        "Decoder: max FEXTRA length in bytes",  // help
    },
    // Core limit options.  Declared here because this method honours them --
    // a schema that omits what the method accepts cannot be used to validate
    // a caller's options, which is what gcomp_options_validate() does at
    // create time.
    {
        "limits.max_output_bytes",           // key
        GCOMP_OPT_UINT64,                    // type
        0,                                   // has_default
        {.ui64 = 0},                         // default_value
        0,                                   // has_min
        0,                                   // has_max
        0,                                   // min_int
        0,                                   // max_int
        0,                                   // min_uint
        0,                                   // max_uint
        "Maximum decompressed output bytes", // help
    },
    {
        "limits.max_memory_bytes",    // key
        GCOMP_OPT_UINT64,             // type
        0,                            // has_default
        {.ui64 = 0},                  // default_value
        0,                            // has_min
        0,                            // has_max
        0,                            // min_int
        0,                            // max_int
        0,                            // min_uint
        0,                            // max_uint
        "Maximum memory usage bytes", // help
    },
    {
        "limits.max_expansion_ratio",              // key
        GCOMP_OPT_UINT64,                          // type
        1,                                         // has_default
        {.ui64 = GCOMP_GZIP_MAX_EXPANSION_RATIO},  // default_value
        0,                                         // has_min
        0,                                         // has_max
        0,                                         // min_int
        0,                                         // max_int
        0,                                         // min_uint
        0,                                         // max_uint
        "Maximum output/input ratio; default is this format's own ceiling",  // help
    },
};

static const char * const g_gzip_option_keys[] = {
    "gzip.mtime",
    "gzip.os",
    "gzip.name",
    "gzip.comment",
    "gzip.extra",
    "gzip.header_crc",
    "gzip.text",
    "gzip.xfl",
    "gzip.header_flags",
    "gzip.concat",
    "gzip.max_name_bytes",
    "gzip.max_comment_bytes",
    "gzip.max_extra_bytes",
    "limits.max_output_bytes",
    "limits.max_memory_bytes",
    "limits.max_expansion_ratio",
};

static const gcomp_method_schema_t g_gzip_schema = {
    g_gzip_option_schemas,
    sizeof(g_gzip_option_schemas) / sizeof(g_gzip_option_schemas[0]),
    GCOMP_UNKNOWN_KEY_IGNORE, // Allow deflate.* and limits.* to pass through
    g_gzip_option_keys,
};

static const gcomp_method_schema_t * gzip_get_schema(void) {
  return &g_gzip_schema;
}

//
// Encoder/Decoder Wrappers
//
// These thin wrappers adapt the gzip-specific implementation functions
// to the generic function pointer signature expected by the method vtable.
//

static gcomp_status_t gzip_encoder_update_wrapper(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  return gzip_encoder_update(encoder, input, output);
}

static gcomp_status_t gzip_encoder_finish_wrapper(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  return gzip_encoder_finish(encoder, output);
}

static gcomp_status_t gzip_decoder_update_wrapper(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  return gzip_decoder_update(decoder, input, output);
}

static gcomp_status_t gzip_decoder_finish_wrapper(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  return gzip_decoder_finish(decoder, output);
}

//
// Encoder/Decoder Factory Functions
//

static gcomp_status_t gzip_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  if (!encoder_out || !*encoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t status = gzip_encoder_init(registry, options, *encoder_out);
  if (status != GCOMP_OK) {
    return status;
  }

  (*encoder_out)->update_fn = gzip_encoder_update_wrapper;
  (*encoder_out)->finish_fn = gzip_encoder_finish_wrapper;
  (*encoder_out)->flush_fn = gzip_encoder_flush;
  (*encoder_out)->reset_fn = gzip_encoder_reset;
  return GCOMP_OK;
}

static gcomp_status_t gzip_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  if (!decoder_out || !*decoder_out) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t status = gzip_decoder_init(registry, options, *decoder_out);
  if (status != GCOMP_OK) {
    return status;
  }

  (*decoder_out)->update_fn = gzip_decoder_update_wrapper;
  (*decoder_out)->finish_fn = gzip_decoder_finish_wrapper;
  (*decoder_out)->reset_fn = gzip_decoder_reset;
  return GCOMP_OK;
}

static void gzip_destroy_encoder_wrapper(gcomp_encoder_t * encoder) {
  gzip_encoder_destroy(encoder);
}

static void gzip_destroy_decoder_wrapper(gcomp_decoder_t * decoder) {
  gzip_decoder_destroy(decoder);
}

//
// Method Descriptor
//

//
// Worst-case encoded size
//

/**
 * @brief DEFLATE's bound plus RFC 1952's member framing.
 *
 * Section 2.3: a ten-byte fixed header and an eight-byte trailer (CRC32 and
 * ISIZE).  The optional fields are counted from the options that produce them,
 * because each is written verbatim: FEXTRA costs two bytes of XLEN plus the
 * bytes themselves, FNAME and FCOMMENT cost their length plus the NUL that
 * terminates them, and FHCRC costs two.
 */
static gcomp_status_t gzip_encode_bound(
    gcomp_options_t * options, size_t input_size, size_t * bound_out) {
  if (!bound_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  size_t bound = 0;
  gcomp_status_t s = gcomp_bound_deflate_raw(
      input_size, gcomp_bound_deflate_window_bits(options), &bound);
  if (s != GCOMP_OK) {
    return s;
  }
  s = gcomp_bound_add(&bound, GZIP_HEADER_MIN_SIZE + GZIP_TRAILER_SIZE);
  if (s != GCOMP_OK) {
    return s;
  }

  if (options) {
    const void * extra = NULL;
    size_t extra_len = 0;
    if (gcomp_options_get_bytes(options, "gzip.extra", &extra, &extra_len) ==
            GCOMP_OK &&
        extra && extra_len > 0) {
      s = gcomp_bound_add(&bound, 2u);
      if (s == GCOMP_OK) {
        s = gcomp_bound_add(&bound, extra_len);
      }
      if (s != GCOMP_OK) {
        return s;
      }
    }

    const char * str = NULL;
    if (gcomp_options_get_string(options, "gzip.name", &str) == GCOMP_OK &&
        str) {
      s = gcomp_bound_add(&bound, strlen(str) + 1u);
      if (s != GCOMP_OK) {
        return s;
      }
    }
    str = NULL;
    if (gcomp_options_get_string(options, "gzip.comment", &str) == GCOMP_OK &&
        str) {
      s = gcomp_bound_add(&bound, strlen(str) + 1u);
      if (s != GCOMP_OK) {
        return s;
      }
    }

    int header_crc = 0;
    if (gcomp_options_get_bool(options, "gzip.header_crc", &header_crc) ==
            GCOMP_OK &&
        header_crc) {
      s = gcomp_bound_add(&bound, 2u);
      if (s != GCOMP_OK) {
        return s;
      }
    }
  }

  *bound_out = bound;
  return GCOMP_OK;
}


//
// Peeking
//

/**
 * @brief Walk the gzip member header, RFC 1952 section 2.3.
 *
 * Ten fixed bytes, then the optional fields FLG selects, in the order the
 * specification lists them: FEXTRA's two-byte length and its bytes, then the
 * NUL-terminated FNAME and FCOMMENT, then FHCRC's two bytes.
 *
 * ISIZE is deliberately not reported as a content size.  It sits in the
 * trailer rather than the header, it is only the size modulo 2^32, and a gzip
 * file may hold several members - so reading it here would mean seeking to the
 * end and would still be wrong for a multi-member file and for anything above
 * 4 GB.  has_content_size stays zero, which is the truth about the header.
 */
static gcomp_status_t gzip_peek(gcomp_options_t * options, const void * input,
    size_t input_size, gcomp_stream_info_t * info_out, size_t * needed_out) {
  (void)options;
  if (!info_out) {
    return GCOMP_ERR_INVALID_ARG;
  }
  memset(info_out, 0, sizeof(*info_out));
  // Deflate's window, which is what a gzip member holds: RFC 1952 section 2.3
  // fixes CM to 8 and defers to RFC 1951 for the stream itself.
  info_out->window_size = 32768u;

  const uint8_t * p = (const uint8_t *)input;
  if (input_size < GZIP_HEADER_MIN_SIZE) {
    if (needed_out) {
      *needed_out = GZIP_HEADER_MIN_SIZE;
    }
    return GCOMP_ERR_LIMIT;
  }
  if (p[0] != 0x1Fu || p[1] != 0x8Bu) {
    return GCOMP_ERR_CORRUPT;
  }
  if (p[2] != 8u) {
    // Section 2.3.1: CM 0-7 are reserved and 8 is deflate.  Anything else is
    // a compression method this format does not define.
    return GCOMP_ERR_UNSUPPORTED;
  }
  uint8_t flg = p[3];
  if (flg & GZIP_FLG_RESERVED) {
    return GCOMP_ERR_CORRUPT;
  }

  size_t pos = GZIP_HEADER_MIN_SIZE;

  if (flg & GZIP_FLG_FEXTRA) {
    if (input_size < pos + 2) {
      if (needed_out) {
        *needed_out = pos + 2;
      }
      return GCOMP_ERR_LIMIT;
    }
    size_t xlen = (size_t)p[pos] | ((size_t)p[pos + 1] << 8);
    pos += 2;
    if (input_size < pos + xlen) {
      if (needed_out) {
        *needed_out = pos + xlen;
      }
      return GCOMP_ERR_LIMIT;
    }
    pos += xlen;
  }

  for (int field = 0; field < 2; field++) {
    uint8_t bit = field == 0 ? GZIP_FLG_FNAME : GZIP_FLG_FCOMMENT;
    if (!(flg & bit)) {
      continue;
    }
    size_t start = pos;
    while (pos < input_size && p[pos] != 0) {
      pos++;
    }
    if (pos >= input_size) {
      if (needed_out) {
        // The string has no end in what we have; one more byte at least, and
        // the caller will be asked again if that is still not enough.
        *needed_out = input_size + 1;
      }
      return GCOMP_ERR_LIMIT;
    }
    pos++; // the NUL
    (void)start;
  }

  if (flg & GZIP_FLG_FHCRC) {
    if (input_size < pos + 2) {
      if (needed_out) {
        *needed_out = pos + 2;
      }
      return GCOMP_ERR_LIMIT;
    }
    pos += 2;
  }

  info_out->header_size = pos;
  // The trailer always carries a CRC-32 of the uncompressed data.
  info_out->has_checksum = 1;
  if (needed_out) {
    *needed_out = pos;
  }
  return GCOMP_OK;
}


static const gcomp_method_t g_gzip_method = {
    .abi_version = GCOMP_METHOD_ABI_VERSION,
    .size = sizeof(gcomp_method_t),
    .name = "gzip",
    .capabilities = GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE,
    .create_encoder = gzip_create_encoder,
    .create_decoder = gzip_create_decoder,
    .destroy_encoder = gzip_destroy_encoder_wrapper,
    .destroy_decoder = gzip_destroy_decoder_wrapper,
    .get_schema = gzip_get_schema,
    .encode_bound = gzip_encode_bound,
    .peek = gzip_peek,
};

//
// Public Registration Function
//

gcomp_status_t gcomp_method_gzip_register(gcomp_registry_t * registry) {
  if (!registry) {
    return GCOMP_ERR_INVALID_ARG;
  }
  return gcomp_registry_register(registry, &g_gzip_method);
}

//
// Auto-Registration Hook
//
GCOMP_AUTOREG_METHOD(gzip, gcomp_method_gzip_register)
