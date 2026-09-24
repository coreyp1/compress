/**
 * @file fuzz_zlib_encoder.c
 *
 * AFL++ fuzz harness for the zlib encoder (RFC 1950).
 *
 * This harness reads arbitrary bytes from stdin and compresses them as zlib
 * streams under a range of option shapes, checking the two-or-six byte header
 * it wrote against RFC 1950 every time.
 *
 * It exists because `fuzz_zlib_roundtrip` does not cover the encoder. That
 * harness calls `gcomp_encode_buffer` only, with one output buffer large
 * enough for the whole stream, and sets no zlib option at all - so three
 * things reachable from the public API had never been fuzzed in this format:
 *
 * - **The preset dictionary.** `zlib.dictionary` puts FDICT in FLG, four
 *   DICTID bytes after the header, and the same bytes into deflate as starting
 *   history (RFC 1950 section 2.2). None of that was being driven.
 * - **The streaming encoder.** `update`/`finish`/`flush` on zlib, as opposed to
 *   the one-shot buffer call.
 * - **Partial header emission.** The header is 2 bytes, or 6 with a
 *   dictionary, and `zlib_encoder_state_t` carries a `header_pos` because it
 *   may be handed out across several calls. An output buffer of one byte is
 *   what walks that; a buffer sized for the whole stream never does.
 *
 * Key areas being tested:
 * - CMF/FLG construction: CM, CINFO from the window, FLEVEL from the level,
 *   and FCHECK making the big-endian pair a multiple of 31
 * - FDICT set exactly when a dictionary was supplied, and DICTID being the
 *   Adler-32 of it
 * - Header emission through output buffers too small to hold it
 * - Streaming update/finish/flush, and both flush modes
 * - The inner deflate encoder across levels, windows and strategies
 *
 * The header is checked against the RFC arithmetic written out here rather
 * than against `gcomp_zlib_peek_header()`. Asking the library's own parser
 * whether the library's writer was right is a self-consistency check: both
 * sides can share one wrong convention and agree. Adler-32 is recomputed here
 * for the same reason.
 *
 * Build with AFL++:
 *   afl-gcc -O2 -o fuzz_zlib_encoder fuzz/fuzz_zlib_encoder.c \
 *       -I include/ -L build/linux/release/apps -lghoti.io-compress-dev
 *
 * Run:
 *   afl-fuzz -i fuzz/corpus/zlib_encoder -o fuzz/findings/zlib_encoder \
 *       -- ./fuzz_zlib_encoder
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ghoti.io/compress/compress.h"
#include "ghoti.io/compress/macros.h"
#include "ghoti.io/compress/options.h"
#include "ghoti.io/compress/stream.h"
#include "ghoti.io/compress/zlib.h"

// Maximum input size to prevent excessive memory usage
#define MAX_INPUT_SIZE (256 * 1024) // 256 KB

// Output buffer size - room for incompressible data plus both headers
#define OUTPUT_BUFFER_SIZE (MAX_INPUT_SIZE + MAX_INPUT_SIZE / 10 + 1024)

// RFC 1950 section 2.2, restated locally so the check does not borrow the
// library's own names for them.
#define RFC1950_CM_DEFLATE 8u
#define RFC1950_FDICT 0x20u
#define RFC1950_HEADER_SIZE 2u
#define RFC1950_DICTID_SIZE 4u

/**
 * @brief Read all data from stdin into a buffer
 *
 * @param size_out Output parameter for the number of bytes read
 * @return Pointer to allocated buffer (caller must free), or NULL on error
 */
static uint8_t * read_stdin(size_t * size_out) {
  size_t capacity = 4096;
  size_t size = 0;
  uint8_t * buffer = malloc(capacity);
  if (!buffer) {
    return NULL;
  }

  int c;
  while ((c = getchar()) != EOF) {
    if (size >= MAX_INPUT_SIZE) {
      break;
    }
    if (size >= capacity) {
      capacity *= 2;
      if (capacity > MAX_INPUT_SIZE) {
        capacity = MAX_INPUT_SIZE;
      }
      uint8_t * new_buffer = realloc(buffer, capacity);
      if (!new_buffer) {
        free(buffer);
        return NULL;
      }
      buffer = new_buffer;
    }
    buffer[size++] = (uint8_t)c;
  }

  *size_out = size;
  return buffer;
}

/**
 * @brief Adler-32 of a buffer, computed here rather than borrowed.
 *
 * RFC 1950 section 9. The modulus is 65521, the largest prime below 2^16.
 */
static uint32_t local_adler32(const uint8_t * data, size_t len) {
  uint32_t a = 1;
  uint32_t b = 0;
  for (size_t i = 0; i < len; i++) {
    a = (a + data[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

/**
 * @brief Check a written zlib header against RFC 1950.
 *
 * @param out The stream as written.
 * @param out_len How many bytes of it exist.
 * @param dict The dictionary handed to the encoder, or NULL.
 * @param dict_len Its length.
 * @param window_bits The window the caller asked for, 8 to 15.
 * @param what A label for the abort message.
 */
static void check_header(const uint8_t * out, size_t out_len,
    const uint8_t * dict, size_t dict_len, int window_bits, const char * what) {
  // Nothing was emitted. That is a legitimate outcome for a run that stopped
  // on a full output buffer before the header went out, so it is not a defect;
  // there is simply nothing to check.
  if (out_len < RFC1950_HEADER_SIZE) {
    return;
  }

  uint8_t cmf = out[0];
  uint8_t flg = out[1];

  // FCHECK: "The FCHECK value must be such that CMF and FLG, when viewed as a
  // 16-bit unsigned integer stored in MSB order, is a multiple of 31."
  unsigned pair = ((unsigned)cmf << 8) | (unsigned)flg;
  if (pair % 31u != 0u) {
    fprintf(stderr,
        "FATAL: %s wrote a zlib header that is not a multiple of 31: "
        "CMF=0x%02X FLG=0x%02X pair=%u remainder=%u\n",
        what, cmf, flg, pair, pair % 31u);
    abort();
  }

  // CM is the low nibble of CMF and must be 8 for deflate.
  unsigned cm = cmf & 0x0Fu;
  if (cm != RFC1950_CM_DEFLATE) {
    fprintf(stderr, "FATAL: %s wrote CM=%u, not %u (deflate)\n", what, cm,
        RFC1950_CM_DEFLATE);
    abort();
  }

  // CINFO is the high nibble and is log2(window) - 8, so it names the window
  // the decoder must allocate. Getting this wrong is silent: a decoder with a
  // larger window still decodes a stream that used a smaller one.
  unsigned cinfo = (unsigned)(cmf >> 4);
  if (cinfo > 7u) {
    fprintf(stderr, "FATAL: %s wrote CINFO=%u, above the RFC 1950 maximum 7\n",
        what, cinfo);
    abort();
  }
  if (window_bits >= 8 && window_bits <= 15 &&
      cinfo != (unsigned)(window_bits - 8)) {
    fprintf(stderr,
        "FATAL: %s asked for window_bits=%d but CINFO=%u says %u\n", what,
        window_bits, cinfo, cinfo + 8u);
    abort();
  }

  // FDICT must be set exactly when a dictionary was supplied. Both directions
  // matter: set without one makes the stream undecodable, and clear with one
  // makes a decoder start from empty history and quietly produce garbage.
  int fdict = (flg & RFC1950_FDICT) ? 1 : 0;
  int wanted = (dict && dict_len > 0) ? 1 : 0;
  if (fdict != wanted) {
    fprintf(stderr, "FATAL: %s wrote FDICT=%d with dict_len=%zu\n", what, fdict,
        dict_len);
    abort();
  }

  if (fdict) {
    if (out_len < RFC1950_HEADER_SIZE + RFC1950_DICTID_SIZE) {
      // Same as above: a truncated emission is not itself wrong.
      return;
    }
    uint32_t got = ((uint32_t)out[2] << 24) | ((uint32_t)out[3] << 16) |
        ((uint32_t)out[4] << 8) | (uint32_t)out[5];
    uint32_t want = local_adler32(dict, dict_len);
    if (got != want) {
      fprintf(stderr,
          "FATAL: %s wrote DICTID=0x%08X, but the Adler-32 of the %zu-byte "
          "dictionary is 0x%08X\n",
          what, got, dict_len, want);
      abort();
    }
  }
}

/**
 * @brief Build an options object for one configuration.
 *
 * @return The options, or NULL if they could not be created.
 */
static gcomp_options_t * make_options(int level, int window_bits,
    const char * strategy, const uint8_t * dict, size_t dict_len) {
  gcomp_options_t * opts = NULL;
  if (gcomp_options_create(&opts) != GCOMP_OK || !opts) {
    return NULL;
  }
  gcomp_options_set_int64(opts, "deflate.level", level);
  gcomp_options_set_uint64(opts, "deflate.window_bits", (uint64_t)window_bits);
  if (strategy) {
    gcomp_options_set_string(opts, "deflate.strategy", strategy);
  }
  if (dict && dict_len > 0) {
    gcomp_options_set_bytes(opts, "zlib.dictionary", dict, dict_len);
  }
  return opts;
}

/**
 * @brief Encode through the streaming API, in chunks, into a bounded buffer.
 *
 * @param out_chunk How much output buffer to offer per call. One byte is what
 *   forces the header out a piece at a time.
 */
static void fuzz_encoder_streaming(const uint8_t * input, size_t input_size,
    uint8_t * output, int level, int window_bits, const char * strategy,
    const uint8_t * dict, size_t dict_len, size_t out_chunk, int do_flush) {
  gcomp_options_t * opts =
      make_options(level, window_bits, strategy, dict, dict_len);
  gcomp_encoder_t * encoder = NULL;
  gcomp_status_t status = gcomp_encoder_create(NULL, "zlib", opts, &encoder);
  if (opts) {
    gcomp_options_destroy(opts);
  }
  if (status != GCOMP_OK) {
    return;
  }

  size_t input_offset = 0;
  size_t output_offset = 0;
  size_t chunk_size = 1;
  int flushed = 0;

  while (input_offset < input_size) {
    chunk_size = (chunk_size * 7 + 13) % 1024 + 1;
    size_t remaining = input_size - input_offset;
    if (chunk_size > remaining) {
      chunk_size = remaining;
    }

    gcomp_buffer_t in_buf = {
        .data = (void *)(input + input_offset), .size = chunk_size, .used = 0};

    size_t out_remaining = OUTPUT_BUFFER_SIZE - output_offset;
    if (out_remaining > out_chunk) {
      out_remaining = out_chunk;
    }
    if (out_remaining == 0) {
      break;
    }

    gcomp_buffer_t out_buf = {
        .data = output + output_offset, .size = out_remaining, .used = 0};

    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);

    input_offset += in_buf.used;
    output_offset += out_buf.used;

    if (status != GCOMP_OK) {
      check_header(output, output_offset, dict, dict_len, window_bits,
          "streaming update");
      gcomp_encoder_destroy(encoder);
      return;
    }

    // Flush once, partway through, when asked. GCOMP_FLUSH_FULL drops the
    // history, which is a different path from a sync flush and one a preset
    // dictionary interacts with.
    if (do_flush && !flushed && input_offset * 2 >= input_size) {
      flushed = 1;
      // GCOMP_ERR_LIMIT from a flush means "more output space", not failure, so
      // it has to be looped the way stream.h documents finish. Treating it as
      // an error would end the run at the first small buffer and never reach
      // the rest of the stream.
      for (;;) {
        size_t room = OUTPUT_BUFFER_SIZE - output_offset;
        if (room > out_chunk) {
          room = out_chunk;
        }
        if (room == 0) {
          break;
        }
        gcomp_buffer_t flush_buf = {
            .data = output + output_offset, .size = room, .used = 0};
        status = gcomp_encoder_flush(encoder, &flush_buf,
            (do_flush == 2) ? GCOMP_FLUSH_FULL : GCOMP_FLUSH_SYNC);
        output_offset += flush_buf.used;
        if (status == GCOMP_OK) {
          break;
        }
        if (status != GCOMP_ERR_LIMIT || flush_buf.used == 0) {
          check_header(
              output, output_offset, dict, dict_len, window_bits, "flush");
          gcomp_encoder_destroy(encoder);
          return;
        }
      }
    }

    // A call that consumed nothing and produced nothing cannot make progress;
    // stop rather than spin. A one-byte output buffer reaches this legitimately
    // once the encoder has a block it cannot start emitting.
    if (in_buf.used == 0 && out_buf.used == 0) {
      break;
    }
  }

  // Finish. stream.h: GCOMP_OK means the stream is complete, GCOMP_ERR_LIMIT
  // means call again with more room. A call that produces no output cannot
  // make progress, so that ends the loop rather than spinning.
  for (;;) {
    size_t room = OUTPUT_BUFFER_SIZE - output_offset;
    if (room > out_chunk) {
      room = out_chunk;
    }
    if (room == 0) {
      break;
    }
    gcomp_buffer_t out_buf = {
        .data = output + output_offset, .size = room, .used = 0};
    status = gcomp_encoder_finish(encoder, &out_buf);
    output_offset += out_buf.used;
    if (status == GCOMP_OK) {
      break;
    }
    if (status != GCOMP_ERR_LIMIT || out_buf.used == 0) {
      break;
    }
  }

  check_header(
      output, output_offset, dict, dict_len, window_bits, "streaming finish");
  gcomp_encoder_destroy(encoder);
}

/**
 * @brief Encode through the one-shot buffer API.
 */
static void fuzz_encoder_buffer(const uint8_t * input, size_t input_size,
    uint8_t * output, int level, int window_bits, const char * strategy,
    const uint8_t * dict, size_t dict_len) {
  gcomp_options_t * opts =
      make_options(level, window_bits, strategy, dict, dict_len);

  size_t output_size = OUTPUT_BUFFER_SIZE;
  gcomp_status_t status = gcomp_encode_buffer(NULL, "zlib", opts, input,
      input_size, output, OUTPUT_BUFFER_SIZE, &output_size);

  if (opts) {
    gcomp_options_destroy(opts);
  }

  if (status == GCOMP_OK) {
    check_header(
        output, output_size, dict, dict_len, window_bits, "encode_buffer");
  }
}

int main(GCOMP_MAYBE_UNUSED(int argc), GCOMP_MAYBE_UNUSED(char ** argv)) {
  size_t input_size = 0;
  uint8_t * input = read_stdin(&input_size);
  if (!input) {
    return 0;
  }

  uint8_t * output = malloc(OUTPUT_BUFFER_SIZE);
  if (!output) {
    free(input);
    return 0;
  }

  // The configuration comes from the input, so the fuzzer can steer it.
  uint8_t flags = input_size > 0 ? input[0] : 0;
  int level = (int)((flags & 0x0Fu) % 10u);
  int window_bits = 8 + (int)((flags >> 4) & 0x07u);
  // Exactly the set deflate.strategy declares in its schema. A string outside
  // it is refused by gcomp_encoder_create(), which would make the whole
  // configuration encode nothing and report nothing - the harness would look
  // like it was running.
  static const char * const strategies[] = {
      "default", "lazy", "huffman_only", "rle", "fixed"};
  const char * strategy =
      strategies[(input_size > 1 ? input[1] : 0) % (sizeof(strategies) /
                                                      sizeof(strategies[0]))];

  // A dictionary taken from the input's own tail. RFC 1950 places no limit on
  // its length, and the useful case is one that shares content with the data,
  // which this does by construction.
  const uint8_t * dict = NULL;
  size_t dict_len = 0;
  if (input_size >= 8) {
    dict_len = input_size / 4;
    if (dict_len > 32768u) {
      dict_len = 32768u;
    }
    dict = input + (input_size - dict_len);
  }

  // The steered configuration, three ways: whole-buffer output, one byte at a
  // time, and with a flush in the middle.
  fuzz_encoder_buffer(
      input, input_size, output, level, window_bits, strategy, NULL, 0);
  fuzz_encoder_streaming(input, input_size, output, level, window_bits,
      strategy, NULL, 0, OUTPUT_BUFFER_SIZE, 0);
  fuzz_encoder_streaming(input, input_size, output, level, window_bits,
      strategy, NULL, 0, 1, 0);
  fuzz_encoder_streaming(input, input_size, output, level, window_bits,
      strategy, NULL, 0, 64, 1);

  // The same again with a preset dictionary, which is the half no harness was
  // reaching. The one-byte run is the one that emits the six-byte header
  // piecewise.
  if (dict) {
    fuzz_encoder_buffer(input, input_size, output, level, window_bits, strategy,
        dict, dict_len);
    fuzz_encoder_streaming(input, input_size, output, level, window_bits,
        strategy, dict, dict_len, OUTPUT_BUFFER_SIZE, 0);
    fuzz_encoder_streaming(input, input_size, output, level, window_bits,
        strategy, dict, dict_len, 1, 0);
    fuzz_encoder_streaming(input, input_size, output, level, window_bits,
        strategy, dict, dict_len, 64, 2);
  }

  // Two fixed configurations every run, so the extremes of the window and the
  // stored-block path at level 0 are always covered whatever the first byte
  // happens to be.
  fuzz_encoder_buffer(input, input_size, output, 0, 15, "default", NULL, 0);
  fuzz_encoder_buffer(input, input_size, output, 9, 8, "default", NULL, 0);

  free(output);
  free(input);

  return 0;
}
