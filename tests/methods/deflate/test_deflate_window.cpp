/**
 * @file test_deflate_window.cpp
 *
 * Tests for where the DEFLATE decoder keeps its history.
 *
 * WHY THESE EXIST AS A FILE OF THEIR OWN
 * ======================================
 *
 * The decoder no longer keeps a private copy of everything it has decoded.
 * The bytes it has just written into the caller's output buffer *are* the
 * history for as long as that call lasts, and only what has fallen out of the
 * buffer is kept in a window of its own.  A match therefore reads from one of
 * two places, and which one is decided entirely by something the stream does
 * not control: how much room the caller offered this time.
 *
 * That makes the output buffer size a test axis, and it is a nasty one,
 * because the natural way to write a decoder test -- decode the whole thing
 * into one big buffer -- exercises exactly one of the two paths and never
 * even reaches the other.  Measured on a 3 MB decode into a whole-file
 * buffer, the window path runs zero times.
 *
 * So the same stream is decoded here at output buffer sizes from one byte
 * upwards, and the bytes must not depend on the size.  The two ends of that
 * range are not two settings of a tuning knob; they are two implementations:
 *
 *   - a one-byte output buffer means the call has written at most one byte,
 *     so every match of distance two or more comes out of the window;
 *   - a buffer that holds the whole file means no match ever does.
 *
 * Both are ordinary things for a caller to do.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstdlib>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/gzip.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zlib.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

/// Deterministic noise, so a failure can be reproduced from the seed alone.
class Rng {
public:
  explicit Rng(uint32_t seed) : state_(seed ? seed : 1u) {}
  uint32_t Next() {
    state_ = state_ * 1103515245u + 12345u;
    return state_ >> 8;
  }
  uint8_t Byte() { return (uint8_t)(Next() & 0xFFu); }

private:
  uint32_t state_;
};

/**
 * The shapes worth decoding, chosen for what they do to a match copy rather
 * than for what they look like.
 */
std::vector<std::pair<std::string, std::vector<uint8_t>>> Corpora() {
  std::vector<std::pair<std::string, std::vector<uint8_t>>> out;

  // A single byte repeated: distance 1, which the copy answers with memset.
  out.emplace_back("run-of-one", std::vector<uint8_t>(40000, 'A'));

  // Patterns shorter than the matches that encode them, so the copy has to
  // grow the pattern rather than copy it.  Three is the awkward one: it
  // divides neither the word size nor the match length.
  for (size_t period : {3u, 5u, 7u, 8u, 16u}) {
    std::vector<uint8_t> v;
    v.reserve(40000);
    while (v.size() < 40000) {
      v.push_back((uint8_t)('a' + (v.size() % period)));
    }
    out.emplace_back("period-" + std::to_string(period), std::move(v));
  }

  // Noise, then more noise, then the first block again.  Nothing nearer
  // matches, so the encoder must reach 16 KB back -- past anything a small
  // output buffer can hold, and across the window's own wrap-around.
  {
    Rng rng(20260921u);
    const size_t blk = 8192;
    std::vector<uint8_t> v(blk * 3);
    for (size_t i = 0; i < blk * 2; i++) {
      v[i] = rng.Byte();
    }
    std::memcpy(v.data() + blk * 2, v.data(), blk);
    out.emplace_back("far-repeat", std::move(v));
  }

  // Text-shaped: many short matches at many distances, which is what real
  // input looks like and what the ordinary path has to get right.
  {
    Rng rng(7u);
    const char * words[] = {"alpha", "beta", "gamma", "delta", "epsilon",
        "zeta", "eta", "theta"};
    std::vector<uint8_t> v;
    while (v.size() < 60000) {
      const char * w = words[rng.Next() % 8];
      v.insert(v.end(), w, w + std::strlen(w));
      v.push_back(' ');
    }
    out.emplace_back("wordy", std::move(v));
  }

  // Incompressible, so the encoder emits stored blocks.  Those used to write
  // the window a byte at a time too, and now do not write it at all.
  {
    Rng rng(99u);
    std::vector<uint8_t> v(40000);
    for (auto & b : v) {
      b = rng.Byte();
    }
    out.emplace_back("incompressible", std::move(v));
  }

  return out;
}

class DeflateWindowTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_EQ(gcomp_registry_create(nullptr, &registry_), GCOMP_OK);
    ASSERT_EQ(gcomp_method_deflate_register(registry_), GCOMP_OK);
    ASSERT_EQ(gcomp_method_gzip_register(registry_), GCOMP_OK);
    ASSERT_EQ(gcomp_method_zlib_register(registry_), GCOMP_OK);
  }
  void TearDown() override {
    if (registry_) {
      gcomp_registry_destroy(registry_);
      registry_ = nullptr;
    }
  }

  std::vector<uint8_t> Encode(
      const char * method, const std::vector<uint8_t> & raw, int level) {
    gcomp_options_t * opt = nullptr;
    EXPECT_EQ(gcomp_options_create(&opt), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_int64(opt, "deflate.level", level), GCOMP_OK);
    std::vector<uint8_t> enc(raw.size() + raw.size() / 2 + 4096);
    size_t used = 0;
    EXPECT_EQ(gcomp_encode_buffer(registry_, method, opt, raw.data(),
                  raw.size(), enc.data(), enc.size(), &used),
        GCOMP_OK);
    gcomp_options_destroy(opt);
    enc.resize(used);
    return enc;
  }

  gcomp_registry_t * registry_ = nullptr;
};

/**
 * Decode with a fresh output buffer of @p out_chunk bytes each time, with the
 * input revealed @p in_chunk bytes at a time.
 *
 * Revealed, not handed over in isolated slices.  A container's header is a
 * field at a time and the decoder will not consume half of one, so a driver
 * that offers byte 0, throws it away and then offers byte 1 makes no progress
 * for ever and blames the library.  What a streaming caller actually has is a
 * buffer that grows as bytes arrive, and that is what this models: `size`
 * moves forward, `used` is never rewound.
 */
std::vector<uint8_t> DecodeChunked(gcomp_registry_t * reg, const char * method,
    const std::vector<uint8_t> & enc, size_t expect_len, size_t in_chunk,
    size_t out_chunk, bool * ok) {
  *ok = false;
  gcomp_decoder_t * dec = nullptr;
  std::vector<uint8_t> sink(expect_len + 64);
  size_t produced = 0, revealed = 0;
  if (gcomp_decoder_create(reg, method, nullptr, &dec) != GCOMP_OK) {
    return sink;
  }
  gcomp_buffer_t input = {enc.data(), 0, 0};
  for (;;) {
    size_t before_in = input.used, before_out = produced;
    if (input.used >= revealed && revealed < enc.size()) {
      revealed = std::min(revealed + in_chunk, enc.size());
      input.size = revealed;
    }
    size_t want = std::min(out_chunk, sink.size() - produced);
    gcomp_buffer_t output = {sink.data() + produced, want, 0};
    if (gcomp_decoder_update(dec, &input, &output) != GCOMP_OK) {
      gcomp_decoder_destroy(dec);
      return sink;
    }
    produced += output.used;
    if (input.used == before_in && produced == before_out &&
        revealed >= enc.size()) {
      break;
    }
  }
  for (;;) {
    size_t want = std::min(out_chunk, sink.size() - produced);
    gcomp_buffer_t output = {sink.data() + produced, want, 0};
    gcomp_status_t s = gcomp_decoder_finish(dec, &output);
    produced += output.used;
    if (s == GCOMP_OK) {
      break;
    }
    if (s != GCOMP_ERR_LIMIT || output.used == 0) {
      gcomp_decoder_destroy(dec);
      return sink;
    }
  }
  gcomp_decoder_destroy(dec);
  sink.resize(produced);
  *ok = true;
  return sink;
}

/**
 * Run update() only until the input is gone, then take the whole remaining
 * tail out through finish().
 *
 * This is the shape finish()'s own documentation describes, and it is the only
 * one that hands finish() a half-copied match: update() stops the moment the
 * output is full, so with a small buffer the last thing it was doing is
 * almost always the middle of a match.  Every byte finish() then emits is
 * history the rest of the tail may reach back into, which means finish() has
 * the same obligation to the window that update() has.
 */
std::vector<uint8_t> DecodeTailThroughFinish(gcomp_registry_t * reg,
    const char * method, const std::vector<uint8_t> & enc, size_t expect_len,
    size_t chunk, bool * ok) {
  *ok = false;
  gcomp_decoder_t * dec = nullptr;
  std::vector<uint8_t> sink(expect_len + 64);
  size_t produced = 0;
  gcomp_buffer_t input = {enc.data(), enc.size(), 0};
  if (gcomp_decoder_create(reg, method, nullptr, &dec) != GCOMP_OK) {
    return sink;
  }
  while (input.used < input.size) {
    size_t room = sink.size() - produced;
    size_t want = std::min(chunk, room);
    if (want == 0) {
      break;
    }
    size_t before_in = input.used;
    gcomp_buffer_t output = {sink.data() + produced, want, 0};
    if (gcomp_decoder_update(dec, &input, &output) != GCOMP_OK) {
      gcomp_decoder_destroy(dec);
      return sink;
    }
    produced += output.used;
    if (output.used == 0 && input.used == before_in) {
      break;
    }
  }
  for (;;) {
    size_t room = sink.size() - produced;
    size_t want = std::min(chunk, room);
    gcomp_buffer_t output = {sink.data() + produced, want, 0};
    gcomp_status_t s = gcomp_decoder_finish(dec, &output);
    produced += output.used;
    if (s == GCOMP_OK) {
      break;
    }
    if (s != GCOMP_ERR_LIMIT || output.used == 0) {
      gcomp_decoder_destroy(dec);
      return sink;
    }
  }
  gcomp_decoder_destroy(dec);
  sink.resize(produced);
  *ok = true;
  return sink;
}

/**
 * One buffer for the whole decode, with `used` carried forward across calls
 * instead of a fresh buffer each time, and @p prefix bytes of somebody else's
 * data in front.
 *
 * This is what gcomp_buffer_t is written for -- `used` is "bytes produced",
 * not "bytes produced this call" -- and it is what the gzip decoder does to
 * its inner deflate decoder, since it has to leave room for what it has
 * already emitted.  It is the only shape in which the mark the decoder puts
 * down is not zero, so a decoder that forgot to put one down would look
 * perfectly correct against a caller who always hands over an empty buffer.
 *
 * The prefix is the other half of the same question: those bytes are in the
 * buffer, they are in front of the decoder's own output, and they are not
 * history.  They are poisoned here so that reading them shows up as wrong
 * bytes rather than as a plausible-looking decode.
 */
std::vector<uint8_t> DecodeSharedBuffer(gcomp_registry_t * reg,
    const char * method, const std::vector<uint8_t> & enc, size_t expect_len,
    size_t prefix, size_t bite, bool * ok) {
  *ok = false;
  gcomp_decoder_t * dec = nullptr;
  std::vector<uint8_t> sink(prefix + expect_len + 64, 0xA5);
  size_t produced = prefix;
  gcomp_buffer_t input = {enc.data(), enc.size(), 0};
  if (gcomp_decoder_create(reg, method, nullptr, &dec) != GCOMP_OK) {
    return sink;
  }
  for (;;) {
    size_t before_in = input.used, before_out = produced;
    gcomp_buffer_t output = {
        sink.data(), std::min(produced + bite, sink.size()), produced};
    if (gcomp_decoder_update(dec, &input, &output) != GCOMP_OK) {
      gcomp_decoder_destroy(dec);
      return sink;
    }
    produced = output.used;
    if (input.used >= input.size) {
      break;
    }
    if (produced == before_out && input.used == before_in) {
      break;
    }
  }
  for (;;) {
    size_t before = produced;
    gcomp_buffer_t output = {
        sink.data(), std::min(produced + bite, sink.size()), produced};
    gcomp_status_t s = gcomp_decoder_finish(dec, &output);
    produced = output.used;
    if (s == GCOMP_OK) {
      break;
    }
    if (s != GCOMP_ERR_LIMIT || produced == before) {
      gcomp_decoder_destroy(dec);
      return sink;
    }
  }
  gcomp_decoder_destroy(dec);
  std::vector<uint8_t> got(sink.begin() + (long)prefix,
      sink.begin() + (long)produced);
  *ok = true;
  return got;
}

const char * const kMethods[] = {"deflate", "gzip", "zlib"};

TEST_F(DeflateWindowTest, EveryOutputChunkSizeGivesTheSameBytes) {
  // The sizes either side of 32768 are there on purpose: the window is
  // circular, so a run of writes that crosses its seam is copied in two
  // pieces, and 32769 is the smallest buffer that makes that happen on every
  // call rather than occasionally.
  const size_t out_chunks[] = {1, 2, 3, 7, 64, 511, 4096, 32768, 32769, 0};
  const size_t in_chunks[] = {1, 997, 0};
  for (const auto & [name, raw] : Corpora()) {
    for (const char * method : kMethods) {
      for (int level : {1, 6, 9}) {
        std::vector<uint8_t> enc = Encode(method, raw, level);
        for (size_t oc : out_chunks) {
          for (size_t ic : in_chunks) {
            size_t o = oc ? oc : raw.size() + 1;
            size_t i = ic ? ic : enc.size() + 1;
            // The tiny sizes are quadratic in the number of calls; they are
            // the interesting ones, so they run on the smaller shapes rather
            // than not at all.
            if (o < 64 && raw.size() > 40000u) {
              continue;
            }
            if (o < 64 && i < 64) {
              continue;
            }
            bool ok = false;
            std::vector<uint8_t> got =
                DecodeChunked(registry_, method, enc, raw.size(), i, o, &ok);
            ASSERT_TRUE(ok) << name << " " << method << " level " << level
                            << " out=" << o << " in=" << i;
            ASSERT_EQ(got, raw) << name << " " << method << " level " << level
                                << " out=" << o << " in=" << i;
          }
        }
      }
    }
  }
}

TEST_F(DeflateWindowTest, TheTailComesOutThroughFinish) {
  const size_t chunks[] = {1, 3, 64, 1000, 4096, 32768};
  for (const auto & [name, raw] : Corpora()) {
    for (const char * method : kMethods) {
      std::vector<uint8_t> enc = Encode(method, raw, 6);
      for (size_t c : chunks) {
        if (c < 64 && raw.size() > 40000u) {
          continue;
        }
        bool ok = false;
        std::vector<uint8_t> got = DecodeTailThroughFinish(
            registry_, method, enc, raw.size(), c, &ok);
        ASSERT_TRUE(ok) << name << " " << method << " chunk " << c;
        ASSERT_EQ(got, raw) << name << " " << method << " chunk " << c;
      }
    }
  }
}

TEST_F(DeflateWindowTest, OneBufferCarriedAcrossCallsWithSomebodyElseInFront) {
  const size_t prefixes[] = {0, 1, 17, 4096};
  const size_t bites[] = {1, 5, 300, 8192, 100000};
  for (const auto & [name, raw] : Corpora()) {
    for (const char * method : kMethods) {
      std::vector<uint8_t> enc = Encode(method, raw, 6);
      for (size_t p : prefixes) {
        for (size_t b : bites) {
          if (b < 64 && raw.size() > 40000u) {
            continue;
          }
          bool ok = false;
          std::vector<uint8_t> got = DecodeSharedBuffer(
              registry_, method, enc, raw.size(), p, b, &ok);
          ASSERT_TRUE(ok) << name << " " << method << " prefix " << p
                          << " bite " << b;
          ASSERT_EQ(got, raw) << name << " " << method << " prefix " << p
                              << " bite " << b;
        }
      }
    }
  }
}

/**
 * A distance the decoder's window cannot reach must be refused, even though
 * the bytes it names are sitting right there in the output buffer.
 *
 * This is the one place where the split history could have quietly changed
 * what the decoder accepts.  `deflate.window_bits` is how a caller says how
 * much history it is willing to pay for -- a zlib header states it, and RFC
 * 1951 section 3.2.3 bounds a distance by it.  While the decoder kept its own
 * window, the bound enforced itself: there was nowhere else for the bytes to
 * be.  Now the caller's buffer holds them too, and a decoder that simply
 * asked "can I reach it?" would happily decode a stream it had been told it
 * could not decode, and would do it correctly, which is the kind of bug that
 * survives every round-trip test ever written.
 *
 * The corpus puts the only repeat exactly 16384 bytes back, so the answer has
 * a sharp edge: an 8 KB window must refuse and a 16 KB window must accept.
 * Both halves are asserted, because a decoder that refused everything would
 * pass the half that matters.
 */
TEST_F(DeflateWindowTest, ARepeatFartherBackThanTheWindowIsRefused) {
  Rng rng(20260921u);
  const size_t blk = 8192;
  std::vector<uint8_t> raw(blk * 3);
  for (size_t i = 0; i < blk * 2; i++) {
    raw[i] = rng.Byte();
  }
  std::memcpy(raw.data() + blk * 2, raw.data(), blk);

  std::vector<uint8_t> enc = Encode("deflate", raw, 9);
  std::vector<uint8_t> out(raw.size() + 64);

  for (uint64_t bits = 9; bits <= 15; bits++) {
    gcomp_options_t * opt = nullptr;
    ASSERT_EQ(gcomp_options_create(&opt), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(opt, "deflate.window_bits", bits),
        GCOMP_OK);
    size_t used = 0;
    gcomp_status_t s = gcomp_decode_buffer(registry_, "deflate", opt,
        enc.data(), enc.size(), out.data(), out.size(), &used);
    gcomp_options_destroy(opt);

    if ((size_t)1u << bits >= blk * 2) {
      ASSERT_EQ(s, GCOMP_OK) << "window_bits " << bits << " reaches 16384";
      ASSERT_EQ(used, raw.size());
      ASSERT_EQ(std::memcmp(out.data(), raw.data(), raw.size()), 0);
    }
    else {
      ASSERT_EQ(s, GCOMP_ERR_CORRUPT)
          << "window_bits " << bits << " cannot reach 16384 and must say so";
    }
  }
}

/**
 * The same question for a preset dictionary, which is history the stream
 * never decoded.
 *
 * The dictionary is put straight into the window, so the first match of the
 * stream reaches back into bytes that are not in anybody's output buffer.
 * That is the one case where the window is the whole of the history and the
 * output buffer none of it, and it is the reverse of every other test here.
 */
TEST_F(DeflateWindowTest, APresetDictionaryIsHistoryTheOutputNeverHeld) {
  const std::string dict =
      "the quick brown fox jumps over the lazy dog; "
      "pack my box with five dozen liquor jugs; "
      "how vexingly quick daft zebras jump";
  std::vector<uint8_t> raw;
  for (int i = 0; i < 200; i++) {
    raw.insert(raw.end(), dict.begin(), dict.end());
  }

  gcomp_options_t * enc_opt = nullptr;
  ASSERT_EQ(gcomp_options_create(&enc_opt), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bytes(enc_opt, "deflate.dictionary", dict.data(),
                dict.size()),
      GCOMP_OK);
  std::vector<uint8_t> enc(raw.size() + 4096);
  size_t enc_used = 0;
  ASSERT_EQ(gcomp_encode_buffer(registry_, "deflate", enc_opt, raw.data(),
                raw.size(), enc.data(), enc.size(), &enc_used),
      GCOMP_OK);
  gcomp_options_destroy(enc_opt);
  enc.resize(enc_used);

  for (size_t chunk : {1u, 7u, 512u, 100000u}) {
    gcomp_options_t * dec_opt = nullptr;
    gcomp_decoder_t * dec = nullptr;
    ASSERT_EQ(gcomp_options_create(&dec_opt), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_bytes(dec_opt, "deflate.dictionary",
                  dict.data(), dict.size()),
        GCOMP_OK);
    ASSERT_EQ(gcomp_decoder_create(registry_, "deflate", dec_opt, &dec),
        GCOMP_OK);
    gcomp_options_destroy(dec_opt);

    std::vector<uint8_t> sink(raw.size() + 64);
    size_t produced = 0;
    gcomp_buffer_t input = {enc.data(), enc.size(), 0};
    for (;;) {
      size_t want = std::min(chunk, sink.size() - produced);
      size_t before_in = input.used;
      gcomp_buffer_t output = {sink.data() + produced, want, 0};
      ASSERT_EQ(gcomp_decoder_update(dec, &input, &output), GCOMP_OK);
      produced += output.used;
      if (output.used == 0 && input.used == before_in) {
        break;
      }
    }
    for (;;) {
      size_t want = std::min(chunk, sink.size() - produced);
      gcomp_buffer_t output = {sink.data() + produced, want, 0};
      gcomp_status_t s = gcomp_decoder_finish(dec, &output);
      produced += output.used;
      if (s == GCOMP_OK) {
        break;
      }
      ASSERT_EQ(s, GCOMP_ERR_LIMIT) << "chunk " << chunk;
      ASSERT_GT(output.used, 0u) << "chunk " << chunk;
    }
    gcomp_decoder_destroy(dec);
    ASSERT_EQ(produced, raw.size()) << "chunk " << chunk;
    ASSERT_EQ(std::memcmp(sink.data(), raw.data(), raw.size()), 0)
        << "chunk " << chunk;
  }
}

/**
 * Decode into a buffer with not one byte to spare.
 *
 * The match copy moves eight bytes at a time and lets the last of those
 * overrun the end of the run, which is what makes it branchless -- so it is
 * allowed only where there are eight bytes of slack to overrun into, and
 * falls back to a plain memcpy where there are not.  Every other test here
 * hands over a buffer with room after it, which means an overrun would land
 * on the test's own spare bytes and nothing would notice.
 *
 * This one allocates exactly what the decode produces, so the byte after the
 * last one belongs to the allocator.  Under AddressSanitizer or Valgrind
 * that is a redzone and a single byte over is a reported error; in an
 * ordinary build it is at least a buffer the decoder has no business
 * touching.  The chunk sizes matter for the same reason: each one makes the
 * final buffer of the decode end at a different point inside a match.
 */
TEST_F(DeflateWindowTest, DecodeIntoExactlyEnoughRoom) {
  for (const auto & [name, raw] : Corpora()) {
    for (const char * method : kMethods) {
      std::vector<uint8_t> enc = Encode(method, raw, 6);
      for (size_t chunk : {1u, 3u, 9u, 64u, 4096u, 40000u, 1000000u}) {
        if (chunk < 64 && raw.size() > 40000u) {
          continue;
        }
        // Not a vector: an exact-sized allocation of our own, so that the
        // byte after the buffer is the allocator's and not slack a vector
        // happened to round up to.
        uint8_t * sink = (uint8_t *)malloc(raw.size());
        ASSERT_NE(sink, nullptr);
        gcomp_decoder_t * dec = nullptr;
        ASSERT_EQ(gcomp_decoder_create(registry_, method, nullptr, &dec),
            GCOMP_OK);

        size_t produced = 0;
        gcomp_buffer_t input = {enc.data(), enc.size(), 0};
        for (;;) {
          size_t before_in = input.used, before_out = produced;
          size_t want = std::min(chunk, raw.size() - produced);
          gcomp_buffer_t output = {sink + produced, want, 0};
          ASSERT_EQ(gcomp_decoder_update(dec, &input, &output), GCOMP_OK)
              << name << " " << method << " chunk " << chunk << ": "
              << gcomp_decoder_get_error_detail(dec);
          produced += output.used;
          if (produced >= raw.size()) {
            break;
          }
          if (input.used == before_in && produced == before_out) {
            break;
          }
        }
        for (;;) {
          size_t want = std::min(chunk, raw.size() - produced);
          gcomp_buffer_t output = {sink + produced, want, 0};
          gcomp_status_t s = gcomp_decoder_finish(dec, &output);
          produced += output.used;
          if (s == GCOMP_OK) {
            break;
          }
          ASSERT_EQ(s, GCOMP_ERR_LIMIT)
              << name << " " << method << " chunk " << chunk << ": "
              << gcomp_decoder_get_error_detail(dec);
          ASSERT_GT(output.used, 0u) << name << " " << method;
        }
        EXPECT_EQ(produced, raw.size()) << name << " " << method
                                        << " chunk " << chunk;
        EXPECT_EQ(std::memcmp(sink, raw.data(), raw.size()), 0)
            << name << " " << method << " chunk " << chunk;
        gcomp_decoder_destroy(dec);
        free(sink);
      }
    }
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
