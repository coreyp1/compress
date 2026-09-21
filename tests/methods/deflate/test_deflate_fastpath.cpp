/**
 * @file test_deflate_fastpath.cpp
 *
 * Tests for the DEFLATE decoder's two symbol loops.
 *
 * WHY THESE EXIST AS A FILE OF THEIR OWN
 * ======================================
 *
 * There are now two pieces of code that decode a Huffman symbol.  The
 * ordinary one is written for a caller who may hand over a byte at a time and
 * take a byte at a time, so every read asks whether it can proceed and every
 * half-finished symbol can be put down and picked up again.  The fast one is
 * entered only when neither buffer can run out mid-symbol -- eight bytes of
 * input, 266 bytes of output room -- and therefore asks none of that.
 *
 * Two implementations of one rule is the risk, and the rule is not only "what
 * do these bits mean".  It is also:
 *
 *   - how far back a match may reach (the window bound, RFC 1951 3.2.3);
 *   - what makes a stream corrupt;
 *   - when the output limit and the expansion-ratio limit say stop.
 *
 * The limits are the sharpest of the three, because the fast loop does not
 * consult them per symbol.  It works out once, before it runs, how many bytes
 * it may write, and then writes without asking.  If that arithmetic is wrong
 * in the permissive direction the decoder overruns a limit the caller set --
 * and a caller sets @c limits.max_output_bytes precisely because it does not
 * trust the stream.
 *
 * So the tests here are mostly of the form "the same question, asked of both
 * loops, must get the same answer", and the way to choose the loop from
 * outside the library is the output buffer size: 265 bytes is one below the
 * fast loop's threshold and it never runs; the whole file is well above it
 * and it does almost all the work.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "deflate_bits.h"
#include "deflate_noise.h"
#include "test_helpers.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using gcomp_test::BitWriter;

/**
 * @brief One below the fast loop's output threshold, and well above it.
 *
 * The threshold is the longest match DEFLATE can express plus the slack the
 * eight-byte copy is allowed to overrun by: 258 + 8.  A caller offering 265
 * bytes gets the ordinary loop for every symbol; a caller offering the whole
 * file gets the fast one for all but the last few hundred bytes.
 */
constexpr size_t kBelowFastThreshold = 265u;

using gcomp_test::Lcg;
using gcomp_test::Xorshift;

std::vector<uint8_t> Wordy(size_t n, uint32_t seed) {
  static const char * words[] = {"the ", "quick ", "brown ", "fox ", "jumps ",
      "over ", "lazy ", "dog ", "and ", "again "};
  Lcg rng(seed);
  std::vector<uint8_t> out;
  out.reserve(n + 16u);
  while (out.size() < n) {
    const char * w = words[rng.Byte() % 10u];
    out.insert(out.end(), w, w + std::strlen(w));
  }
  out.resize(n);
  return out;
}

std::vector<uint8_t> Encode(const char * method,
    const std::vector<uint8_t> & raw, int level) {
  gcomp_registry_t * reg = gcomp_registry_default();
  gcomp_options_t * opt = nullptr;
  EXPECT_EQ(gcomp_options_create(&opt), GCOMP_OK);
  gcomp_options_set_int64(opt, "deflate.level", level);
  std::vector<uint8_t> enc(raw.size() + raw.size() / 2u + 65536u, 0u);
  size_t elen = 0;
  EXPECT_EQ(gcomp_encode_buffer(reg, method, opt, raw.data(), raw.size(),
                enc.data(), enc.size(), &elen),
      GCOMP_OK);
  gcomp_options_destroy(opt);
  enc.resize(elen);
  return enc;
}

struct Decoded {
  gcomp_status_t status = GCOMP_OK;
  std::vector<uint8_t> bytes;
};

/**
 * @brief Decode a whole stream, handing over @p chunk bytes of room at a time.
 *
 * @p chunk of 0 means "all of it", which is the shape that puts the fast loop
 * to work.  @p opts carries any limits the test wants applied.
 *
 * The result's status is the first thing that was not GCOMP_OK, and the bytes
 * are everything the decoder had handed over by then -- a decoder that stops
 * on a limit has still produced what came before it, and the caller keeps it.
 */
Decoded Decode(const char * method, const std::vector<uint8_t> & enc,
    size_t chunk, size_t cap, gcomp_options_t * opts) {
  Decoded result;
  gcomp_registry_t * reg = gcomp_registry_default();
  gcomp_decoder_t * dec = nullptr;
  if (gcomp_decoder_create(reg, method, opts, &dec) != GCOMP_OK) {
    result.status = GCOMP_ERR_INTERNAL;
    return result;
  }
  std::vector<uint8_t> out(cap, 0u);
  size_t produced = 0;
  gcomp_buffer_t input = {(void *)enc.data(), enc.size(), 0u};
  for (;;) {
    const size_t room = out.size() - produced;
    if (room == 0u) {
      break;
    }
    const size_t want = chunk ? (room < chunk ? room : chunk) : room;
    gcomp_buffer_t output = {out.data() + produced, want, 0u};
    const size_t before = input.used;
    gcomp_status_t s = gcomp_decoder_update(dec, &input, &output);
    produced += output.used;
    if (s != GCOMP_OK) {
      result.status = s;
      goto finished;
    }
    if (input.used == before && output.used == 0u) {
      break;
    }
  }
  for (;;) {
    const size_t room = out.size() - produced;
    gcomp_buffer_t output = {out.data() + produced, room, 0u};
    gcomp_status_t s = gcomp_decoder_finish(dec, &output);
    produced += output.used;
    if (s == GCOMP_OK) {
      break;
    }
    if (s != GCOMP_ERR_LIMIT || output.used == 0u || room == 0u) {
      result.status = s;
      break;
    }
  }
finished:
  gcomp_decoder_destroy(dec);
  out.resize(produced);
  result.bytes = std::move(out);
  return result;
}

gcomp_options_t * MakeLimit(const char * key, uint64_t value) {
  gcomp_options_t * opt = nullptr;
  EXPECT_EQ(gcomp_options_create(&opt), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_uint64(opt, key, value), GCOMP_OK);
  return opt;
}

/**
 * @brief An output cap must be honoured whichever loop is doing the writing.
 *
 * The fast loop decides once, before it starts, how many bytes it may write;
 * the ordinary loop asks again for every literal and every match.  Both have
 * to arrive at the same answer to the only question a caller cares about:
 * never more than the cap, and a refusal rather than a truncation when the
 * stream is longer than the cap allows.
 *
 * How *much* comes out before the refusal is deliberately not asserted.  A
 * match that will not fit is refused whole, so a caller offering a lot of
 * room stops a little earlier than one offering a little -- true before the
 * fast loop existed, and not a property of either loop.
 */
TEST(DeflateFastPath, AnOutputLimitIsHonouredByBothLoops) {
  const std::vector<uint8_t> raw = Wordy(300000u, 11u);
  for (const char * method : {"deflate", "zlib", "gzip"}) {
    const std::vector<uint8_t> enc = Encode(method, raw, 6);
    for (uint64_t cap : {(uint64_t)1u, (uint64_t)257u, (uint64_t)258u,
             (uint64_t)259u, (uint64_t)266u, (uint64_t)4096u,
             (uint64_t)150000u, (uint64_t)(raw.size() - 1u),
             (uint64_t)raw.size(), (uint64_t)(raw.size() + 1u)}) {
      for (size_t chunk : {(size_t)0u, kBelowFastThreshold}) {
        gcomp_options_t * opt = MakeLimit("limits.max_output_bytes", cap);
        Decoded got = Decode(method, enc, chunk, raw.size() + 1024u, opt);
        gcomp_options_destroy(opt);

        const std::string where = std::string(method) + " cap " +
            std::to_string(cap) + " chunk " + std::to_string(chunk);
        EXPECT_LE(got.bytes.size(), cap) << where;
        EXPECT_LE(got.bytes.size(), raw.size()) << where;
        EXPECT_EQ(std::memcmp(got.bytes.data(), raw.data(), got.bytes.size()),
            0) << where;
        if (cap >= raw.size()) {
          EXPECT_EQ(got.status, GCOMP_OK) << where;
          EXPECT_EQ(got.bytes.size(), raw.size()) << where;
        }
        else {
          EXPECT_EQ(got.status, GCOMP_ERR_LIMIT) << where;
        }
      }
    }
  }
}

/**
 * @brief The bomb guard must refuse the same streams either way.
 *
 * The fast loop reads the ratio against the input it had consumed when it
 * started, not when it writes, so its allowance is an under-estimate and it
 * stops sooner than it had to.  That is the safe direction and the ordinary
 * loop picks up where it left off -- but only if the under-estimate is
 * genuinely an under-estimate.  A ratio far below what the corpus needs must
 * refuse; one far above, or switched off, must not.
 */
TEST(DeflateFastPath, AnExpansionRatioIsHonouredByBothLoops) {
  const std::vector<uint8_t> raw = Wordy(300000u, 12u);
  for (const char * method : {"deflate", "zlib", "gzip"}) {
    const std::vector<uint8_t> enc = Encode(method, raw, 6);
    // The corpus compresses far better than 2x and nothing like 100000x.
    ASSERT_LT(enc.size() * 2u, raw.size());
    for (size_t chunk : {(size_t)0u, kBelowFastThreshold}) {
      // 1 and 2 are so tight that the fast loop never gets a budget worth
      // having and the ordinary loop refuses on its own -- which is why they
      // are not enough on their own.  3 through 6 leave room for the fast
      // loop to start and still cannot cover the corpus, so they are the
      // ratios that actually ask the fast loop the question.
      for (uint64_t ratio :
          {(uint64_t)1u, (uint64_t)2u, (uint64_t)3u, (uint64_t)4u,
              (uint64_t)5u, (uint64_t)6u}) {
        gcomp_options_t * opt = MakeLimit("limits.max_expansion_ratio", ratio);
        Decoded got = Decode(method, enc, chunk, raw.size() + 1024u, opt);
        gcomp_options_destroy(opt);
        EXPECT_EQ(got.status, GCOMP_ERR_LIMIT)
            << method << " ratio " << ratio << " chunk " << chunk;
        EXPECT_EQ(std::memcmp(got.bytes.data(), raw.data(), got.bytes.size()),
            0) << method << " ratio " << ratio << " chunk " << chunk;
        // The status alone is not the test.  A fast loop that ignored the
        // ratio would write nearly the whole file and then hand the last
        // few dozen bytes to the ordinary loop, which would refuse them --
        // same status, and a decompression bomb already in the caller's
        // buffer.  What the limit promises is a bound on the bytes, so that
        // is what is asserted: never more than the ratio times the input,
        // and the whole input is an over-estimate of what was consumed.
        EXPECT_LE(got.bytes.size(), ratio * enc.size())
            << method << " ratio " << ratio << " chunk " << chunk;
      }
      for (uint64_t ratio : {(uint64_t)0u, (uint64_t)100000u}) {
        gcomp_options_t * opt = MakeLimit("limits.max_expansion_ratio", ratio);
        Decoded got = Decode(method, enc, chunk, raw.size() + 1024u, opt);
        gcomp_options_destroy(opt);
        EXPECT_EQ(got.status, GCOMP_OK)
            << method << " ratio " << ratio << " chunk " << chunk;
        EXPECT_EQ(got.bytes, raw) << method << " ratio " << ratio;
      }
    }
  }
}

/**
 * @brief A broken stream must be broken for both loops.
 *
 * Each loop has its own copy of what makes a stream corrupt: a bit pattern
 * that is no code, a distance symbol above 29, a distance reaching further
 * back than there is history.  A mutation that one refuses and the other
 * accepts means the two copies have drifted.
 *
 * Most single-byte mutations of a compressed stream are not detectable -- the
 * bits still spell a valid sequence of symbols, just a different one -- so
 * this asserts that the two agree, not that either refuses.  Where both
 * succeed, they must also agree on the bytes.
 */
TEST(DeflateFastPath, ACorruptStreamIsRefusedByBothLoopsOrNeither) {
  const std::vector<uint8_t> raw = Wordy(60000u, 13u);
  const std::vector<uint8_t> clean = Encode("deflate", raw, 6);
  size_t disagreements = 0;
  size_t refused = 0;
  size_t examined = 0;

  // Every 37th byte, so the sweep covers the dynamic header, the symbol
  // stream and the tail without taking a second per corpus.
  for (size_t at = 0; at < clean.size(); at += 37u) {
    for (uint8_t mask : {(uint8_t)0x01u, (uint8_t)0x80u, (uint8_t)0xFFu}) {
      std::vector<uint8_t> broken = clean;
      broken[at] = (uint8_t)(broken[at] ^ mask);
      // Room for more than the original: a mutation can lengthen the output.
      const size_t cap = raw.size() * 2u + 4096u;
      Decoded fast = Decode("deflate", broken, 0u, cap, nullptr);
      Decoded slow =
          Decode("deflate", broken, kBelowFastThreshold, cap, nullptr);
      examined++;
      if (fast.status != GCOMP_OK) {
        refused++;
      }
      if (fast.status != slow.status || fast.bytes != slow.bytes) {
        disagreements++;
        EXPECT_EQ(fast.status, slow.status)
            << "byte " << at << " xor 0x" << std::hex << (unsigned)mask;
        EXPECT_EQ(fast.bytes.size(), slow.bytes.size())
            << "byte " << at << " xor 0x" << std::hex << (unsigned)mask;
      }
    }
  }
  EXPECT_EQ(disagreements, 0u);
  // If nothing was refused the sweep is not asking the question it claims to.
  EXPECT_GT(refused, 0u) << "no mutation was detected at all";
  EXPECT_GT(examined, 100u);
}

/**
 * @brief The last bytes of the input belong to the ordinary loop.
 *
 * The fast loop refills with a single eight-byte load, so it stops with seven
 * bytes still unread and the ordinary loop finishes from there.  That handover
 * happens in the middle of a Huffman block, with a bit buffer that is part
 * full, and it must be invisible.
 *
 * It is also where the two loops disagree about what is in that buffer.  The
 * fast loop's refill leaves the stream's continuation sitting above the bit
 * count on purpose; the ordinary loop is promised zeros there, because that is
 * what lets it decode a short code out of the padding when the input has run
 * out.  Hand it the continuation instead and it indexes its table with bits
 * that are not padding, decodes a symbol that was never written, and reports
 * a good stream as corrupt -- which is why deflate_fast_commit() masks.
 *
 * Reaching it takes a caller whose input runs out with a long code pending,
 * so the input chunk size is swept and several corpora are used: the pending
 * code has to be longer than what is left in the buffer, and how often that
 * happens depends on the code lengths the encoder chose.
 */
TEST(DeflateFastPath, TheHandoverAtTheEndOfTheInputIsInvisible) {
  struct Corpus {
    const char * name;
    std::vector<uint8_t> bytes;
  };
  std::vector<Corpus> corpora;
  corpora.push_back({"wordy", Wordy(200000u, 14u)});
  {
    // Noise: long codes everywhere, which is what the padding trick is for.
    Lcg rng(31u);
    std::vector<uint8_t> noise(200000u);
    for (auto & b : noise) {
      b = rng.Byte();
    }
    corpora.push_back({"noise", std::move(noise)});
  }
  {
    // Half fixed, half noise: a mixture of very short and very long codes.
    Lcg rng(32u);
    std::vector<uint8_t> mixed(200000u);
    for (auto & b : mixed) {
      b = (rng.Byte() & 1u) ? (uint8_t)'q' : rng.Byte();
    }
    corpora.push_back({"mixed", std::move(mixed)});
  }

  // Well above the fast loop's threshold, so it does all but the last few
  // hundred bytes and the handover is the thing under test.
  const size_t kWholeBuffer = 0u;
  const size_t in_chunks[] = {1u, 2u, 3u, 7u, 8u, 9u, 13u, 16u, 64u, 1024u,
      4096u, 65536u};

  for (const Corpus & corpus : corpora) {
    for (const char * method : {"deflate", "zlib", "gzip"}) {
      for (int level : {1, 6, 9}) {
        const std::vector<uint8_t> enc = Encode(method, corpus.bytes, level);
        for (size_t in_chunk : in_chunks) {
          gcomp_registry_t * reg = gcomp_registry_default();
          gcomp_decoder_t * dec = nullptr;
          ASSERT_EQ(gcomp_decoder_create(reg, method, nullptr, &dec), GCOMP_OK);
          std::vector<uint8_t> out(corpus.bytes.size() + 1024u, 0u);
          size_t produced = 0;
          size_t revealed = 0;
          gcomp_buffer_t input = {(void *)enc.data(), 0u, 0u};
          const std::string where = std::string(corpus.name) + " " + method +
              " level " + std::to_string(level) + " revealed " +
              std::to_string(in_chunk);
          for (;;) {
            if (revealed < enc.size()) {
              revealed += in_chunk;
              if (revealed > enc.size()) {
                revealed = enc.size();
              }
            }
            input.size = revealed;
            gcomp_buffer_t output = {
                out.data() + produced, out.size() - produced, 0u};
            const size_t before = input.used;
            ASSERT_EQ(gcomp_decoder_update(dec, &input, &output), GCOMP_OK)
                << where;
            produced += output.used;
            if (input.used == before && output.used == 0u &&
                revealed >= enc.size()) {
              break;
            }
          }
          for (;;) {
            gcomp_buffer_t output = {
                out.data() + produced, out.size() - produced, 0u};
            gcomp_status_t st = gcomp_decoder_finish(dec, &output);
            produced += output.used;
            if (st == GCOMP_OK) {
              break;
            }
            ASSERT_EQ(st, GCOMP_ERR_LIMIT) << where;
            ASSERT_GT(output.used, 0u) << where;
          }
          gcomp_decoder_destroy(dec);
          out.resize(produced);
          ASSERT_EQ(out, corpus.bytes) << where;
        }
      }
    }
  }
  (void)kWholeBuffer;
}

/**
 * @brief A match reaching before this call's output, with room for the file.
 *
 * The fast loop copies a match out of the caller's own output buffer, which
 * only works for the part of the history that is in there.  A preset
 * dictionary is history that never was: the first match of the stream can
 * reach back into it with nothing written yet.  The fast loop hands that one
 * to the general copier and picks up where it left off, and this is the case
 * where the handover happens with the fast loop otherwise fully in charge.
 */
TEST(DeflateFastPath, APresetDictionaryIsReachedFromInsideTheFastLoop) {
  const std::vector<uint8_t> dict = Wordy(30000u, 15u);
  std::vector<uint8_t> raw = Wordy(120000u, 16u);
  // Start the body with a long stretch of the dictionary's tail, so the very
  // first symbols are matches that can only be satisfied from it.
  std::copy(dict.end() - 8000, dict.end(), raw.begin());

  gcomp_registry_t * reg = gcomp_registry_default();
  gcomp_options_t * enc_opt = nullptr;
  ASSERT_EQ(gcomp_options_create(&enc_opt), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bytes(enc_opt, "deflate.dictionary", dict.data(),
                dict.size()),
      GCOMP_OK);
  std::vector<uint8_t> enc(raw.size() + 65536u, 0u);
  size_t elen = 0;
  ASSERT_EQ(gcomp_encode_buffer(reg, "deflate", enc_opt, raw.data(),
                raw.size(), enc.data(), enc.size(), &elen),
      GCOMP_OK);
  gcomp_options_destroy(enc_opt);
  enc.resize(elen);

  for (size_t chunk : {(size_t)0u, kBelowFastThreshold, (size_t)70000u}) {
    gcomp_options_t * dec_opt = nullptr;
    ASSERT_EQ(gcomp_options_create(&dec_opt), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_bytes(dec_opt, "deflate.dictionary",
                  dict.data(), dict.size()),
        GCOMP_OK);
    Decoded got = Decode("deflate", enc, chunk, raw.size() + 1024u, dec_opt);
    gcomp_options_destroy(dec_opt);
    EXPECT_EQ(got.status, GCOMP_OK) << "chunk " << chunk;
    EXPECT_EQ(got.bytes, raw) << "chunk " << chunk;
  }
}

/**
 * @brief The cap must hold at every offset, not just at convenient ones.
 *
 * The fast loop turns the cap into a pointer once and then writes without
 * looking at it, so an error of one in that arithmetic lets one byte past a
 * limit the caller set.  A handful of round numbers will not find that: the
 * boundary has to fall inside a match, and a match is a few bytes long in a
 * buffer of hundreds of thousands.  So the cap is swept one byte at a time
 * across a stretch wider than the longest match there is.
 */
TEST(DeflateFastPath, AnOutputLimitHoldsAtEveryOffset) {
  // Two corpora, because the boundary has to land inside a match to matter
  // and the two have very different matches.  The run is the sharper of the
  // two: every match in it is the longest DEFLATE can express, so the cap
  // falls inside one at almost every offset.
  const std::vector<std::vector<uint8_t>> corpora = {
      Wordy(40000u, 21u), std::vector<uint8_t>(40000u, 0x41u)};
  for (const std::vector<uint8_t> & raw : corpora) {
    const std::vector<uint8_t> enc = Encode("deflate", raw, 6);
    for (uint64_t cap = 20000u; cap <= 20000u + 600u; cap++) {
      gcomp_options_t * opt = MakeLimit("limits.max_output_bytes", cap);
      Decoded got = Decode("deflate", enc, 0u, raw.size() + 1024u, opt);
      gcomp_options_destroy(opt);
      ASSERT_LE(got.bytes.size(), cap) << "cap " << cap;
      ASSERT_EQ(got.status, GCOMP_ERR_LIMIT) << "cap " << cap;
      ASSERT_EQ(std::memcmp(got.bytes.data(), raw.data(), got.bytes.size()), 0)
          << "cap " << cap;
    }
  }
}

/**
 * @brief A distance the window cannot reach, refused by the fast loop itself.
 *
 * test_deflate_window.cpp asks this question of the decoder as a whole, and
 * gets the right answer for the wrong reason: its repeat runs to the end of
 * the stream, so the ordinary loop -- which decodes the last few hundred
 * bytes -- meets the same distance and refuses it there.  Take the fast
 * loop's own copy of the bound away and that test still passes.
 *
 * Here the over-long repeat is in the middle and is followed by a block of
 * fresh noise, so the only code that ever sees it is the fast loop.  An 8 KB
 * window must refuse; a 16 KB window must accept, because a test that refused
 * everything would pass the half that matters.
 */
TEST(DeflateFastPath, ADistanceBeyondTheWindowIsRefusedInsideTheFastLoop) {
  const size_t blk = 8192u;
  Lcg rng(22u);
  std::vector<uint8_t> raw(blk * 4u);
  for (size_t i = 0; i < blk * 2u; i++) {
    raw[i] = rng.Byte();
  }
  // A repeat 16384 bytes back...
  std::memcpy(raw.data() + blk * 2u, raw.data(), blk);
  // ...and then something no match can reach, so the tail the ordinary loop
  // decodes contains no long distance of its own.
  for (size_t i = blk * 3u; i < raw.size(); i++) {
    raw[i] = rng.Byte();
  }

  const std::vector<uint8_t> enc = Encode("deflate", raw, 9);
  for (uint64_t bits = 9; bits <= 15; bits++) {
    gcomp_options_t * opt = nullptr;
    ASSERT_EQ(gcomp_options_create(&opt), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(opt, "deflate.window_bits", bits),
        GCOMP_OK);
    Decoded got = Decode("deflate", enc, 0u, raw.size() + 1024u, opt);
    gcomp_options_destroy(opt);
    if (((size_t)1u << bits) >= blk * 2u) {
      EXPECT_EQ(got.status, GCOMP_OK) << "window_bits " << bits;
      EXPECT_EQ(got.bytes, raw) << "window_bits " << bits;
    }
    else {
      EXPECT_EQ(got.status, GCOMP_ERR_CORRUPT)
          << "window_bits " << bits << " cannot reach 16384 and must say so";
    }
  }
}

/**
 * @brief Symbols an encoder never writes, and a decoder must still refuse.
 *
 * The fixed alphabet defines 288 literal/length codes and 32 distance codes,
 * but RFC 1951 section 3.2.6 leaves 286, 287 and distance 30, 31 with no
 * meaning.  They are not a corrupt bit pattern -- they decode perfectly well
 * -- so the only thing standing between them and an out-of-range read of
 * k_len_base[] or k_dist_base[] is a comparison, and the fast loop has its
 * own copy of that comparison.
 *
 * No encoder emits these, so the streams are built by hand, and padded so
 * that the bad symbol is decoded with eight bytes of input still in front of
 * it -- otherwise the fast loop stops first and the ordinary path answers.
 */
TEST(DeflateFastPath, ReservedSymbolsAreRefusedInsideTheFastLoop) {
  struct Case {
    const char * name;
    unsigned litlen;  // The symbol to emit.
    bool with_distance;
    unsigned distance;
  };
  const Case cases[] = {
      {"litlen 286", 286u, false, 0u},
      {"litlen 287", 287u, false, 0u},
      {"distance 30", 257u, true, 30u},
      {"distance 31", 257u, true, 31u},
  };

  for (const Case & c : cases) {
    BitWriter w;
    w.Field(0u, 1u); // BFINAL = 0, so the decoder has more to look forward to
    w.Field(1u, 2u); // BTYPE  = 01, fixed Huffman
    for (int i = 0; i < 40; i++) {
      w.FixedLitLen('a'); // History, so a distance of 30 is otherwise legal.
    }
    w.FixedLitLen(c.litlen);
    if (c.with_distance) {
      w.FixedDistance(c.distance);
    }
    std::vector<uint8_t> stream = w.Finish();
    // Enough trailing bytes that the fast loop's eight-byte refill is still
    // satisfied when it reaches the bad symbol.
    stream.resize(stream.size() + 64u, 0u);

    Decoded got = Decode("deflate", stream, 0u, 8192u, nullptr);
    EXPECT_EQ(got.status, GCOMP_ERR_CORRUPT) << c.name;
    EXPECT_EQ(got.bytes.size(), 40u) << c.name;
  }
}

/**
 * @brief A match at the very start of a call, with the fast loop in charge.
 *
 * The fast loop copies a match out of the caller's own output buffer and
 * hands anything reaching further back to deflate_copy_match().  The boundary
 * between the two is "further back than this call has written", and at the
 * first symbol of a call this call has written nothing -- so even a distance
 * of one belongs to the window.  Get that boundary wrong by one and the copy
 * reads the byte in front of the caller's buffer, which belongs to somebody
 * else.
 *
 * Reaching it needs a call to end exactly on a match boundary, which depends
 * on the output chunk size and the corpus in a way nothing can predict.  So
 * the chunk size is swept across a whole match length, above the threshold
 * so that the fast loop is the one doing the work, and the corpus is a run of
 * one byte: every match in it is 258 long with a distance of one.
 */
TEST(DeflateFastPath, EveryOutputChunkAboveTheThresholdGivesTheSameBytes) {
  struct Corpus {
    const char * name;
    std::vector<uint8_t> bytes;
  };
  std::vector<Corpus> corpora;
  corpora.push_back({"run", std::vector<uint8_t>(40000u, 0x41u)});
  {
    std::vector<uint8_t> period(40000u);
    for (size_t i = 0; i < period.size(); i++) {
      period[i] = (uint8_t)('A' + (i % 3u));
    }
    corpora.push_back({"period-3", std::move(period)});
  }
  corpora.push_back({"wordy", Wordy(40000u, 23u)});

  for (const Corpus & corpus : corpora) {
    const std::vector<uint8_t> enc = Encode("deflate", corpus.bytes, 6);
    // 266 is the threshold and 258 is the longest match, so this covers every
    // way a call can end relative to a match boundary.
    for (size_t chunk = 266u; chunk <= 266u + 258u; chunk++) {
      Decoded got = Decode(
          "deflate", enc, chunk, corpus.bytes.size() + 1024u, nullptr);
      ASSERT_EQ(got.status, GCOMP_OK) << corpus.name << " chunk " << chunk;
      ASSERT_EQ(got.bytes, corpus.bytes) << corpus.name << " chunk " << chunk;
    }
  }
}

/**
 * @brief Every call's output buffer is its own allocation, exactly full.
 *
 * The fast loop's match copy moves eight bytes at a time and lets the last of
 * those run past the end of the match by up to six.  That is only safe
 * because the loop keeps the end of the caller's buffer back for it -- eight
 * bytes, which is the copy's own unit; six is what the arithmetic actually
 * needs.  Nothing in an ordinary test notices when it does not: the decoded
 * bytes still come out right and the overrun lands in whatever slack the test
 * happened to leave after its buffer.
 *
 * So here the caller is the awkward kind -- a fixed buffer, filled to the
 * brim, freshly allocated each call, which is what a stack buffer looks like
 * to a memory checker.
 *
 * Three things have to line up before the overrun is even reachable, and the
 * corpus is built around all three:
 *
 *   - the match must be copied by the wide path, which means a distance of
 *     eight or more and a source inside what this call has written.  With a
 *     266-byte buffer that rules out every long-distance match: they reach
 *     before the call began and go to deflate_copy_match() instead.  So the
 *     repeats here are periodic, with periods that fit inside one call.
 *   - the match must be very nearly the longest there is, because the excess
 *     the copy writes is only six bytes and the loop stops 258 from the end.
 *     A period of a few dozen bytes gives matches of exactly 258.
 *   - there must still be input in hand, or the fast loop stops first --
 *     hence the noise between the periodic stretches, which costs input
 *     bytes and buys no output.
 *
 * Then the buffer size is swept across a whole match length so that the last
 * match of a call ends every possible distance from the end.
 */
TEST(DeflateFastPath, EachCallFillsItsOwnBufferToTheBrim) {
  Lcg rng(24u);
  uint8_t pattern[256];
  for (uint8_t & b : pattern) {
    b = rng.Byte();
  }
  std::vector<uint8_t> raw;
  raw.reserve(80000u);
  const size_t periods[] = {8u, 16u, 31u, 64u, 200u};
  size_t which = 0;
  while (raw.size() < 80000u) {
    const size_t period = periods[which++ % 5u];
    for (size_t i = 0; i < 2400u; i++) {
      raw.push_back(pattern[i % period]);
    }
    for (int i = 0; i < 600; i++) {
      raw.push_back(rng.Byte());
    }
  }
  const std::vector<uint8_t> enc = Encode("deflate", raw, 6);

  gcomp_registry_t * reg = gcomp_registry_default();
  for (size_t chunk = 266u; chunk <= 266u + 258u; chunk++) {
    gcomp_decoder_t * dec = nullptr;
    ASSERT_EQ(gcomp_decoder_create(reg, "deflate", nullptr, &dec), GCOMP_OK);
    std::vector<uint8_t> got;
    got.reserve(raw.size());
    gcomp_buffer_t input = {(void *)enc.data(), enc.size(), 0u};
    bool done = false;
    for (int phase = 0; phase < 2 && !done; phase++) {
      for (;;) {
        uint8_t * exact = (uint8_t *)std::malloc(chunk);
        ASSERT_NE(exact, nullptr);
        gcomp_buffer_t output = {exact, chunk, 0u};
        const size_t before = input.used;
        gcomp_status_t st = (phase == 0)
            ? gcomp_decoder_update(dec, &input, &output)
            : gcomp_decoder_finish(dec, &output);
        got.insert(got.end(), exact, exact + output.used);
        std::free(exact);
        ASSERT_TRUE(st == GCOMP_OK || st == GCOMP_ERR_LIMIT)
            << "chunk " << chunk << " status " << st;
        if (phase == 0) {
          if (input.used == before && output.used == 0u) {
            break;
          }
        }
        else {
          if (st == GCOMP_OK) {
            done = true;
            break;
          }
          ASSERT_GT(output.used, 0u) << "chunk " << chunk;
        }
      }
    }
    gcomp_decoder_destroy(dec);
    ASSERT_EQ(got.size(), raw.size()) << "chunk " << chunk;
    ASSERT_EQ(std::memcmp(got.data(), raw.data(), got.size()), 0)
        << "chunk " << chunk;
  }
}

/**
 * @brief A stored block after the fast loop has been running.
 *
 * The fast loop's refill leaves the stream's continuation in the bit buffer
 * above the bit count, on purpose, and commits it masked off.  That mask is
 * the whole of what keeps a mixed stream decodable, and the reason is not the
 * obvious one.
 *
 * deflate_try_fill_bits() ORs new bytes into the buffer at the bit count, so
 * it needs nothing to be there already.  Inside the fast loop that is fine:
 * the leftovers are the bytes the cursor is pointing at, so the next load ORs
 * the same bits over themselves.  A stored block breaks the relationship --
 * its payload is copied straight out of the input in bulk, moving the cursor
 * tens of kilobytes with the bit buffer standing still -- and from then on
 * the leftovers are bits from somewhere else.  The next header read ORs them
 * into a good byte and comes back with a BFINAL that was never written; the
 * decoder then stops three quarters of the way through a valid stream and
 * reports that it finished.
 *
 * The corpus has to make the encoder emit both kinds of block, and that turns
 * out not to be something a test can arrange by mixing compressible and
 * incompressible data by hand -- do that and the encoder codes the lot.  It
 * has to be uniform data near enough to the line that the encoder decides
 * differently from one block to the next, which for this encoder is noise of
 * a few hundred kilobytes.  Xorshift and not Lcg for the reason in
 * deflate_noise.h: Lcg noise is stored end to end, and a stream with no coded
 * blocks never enters the fast loop at all.
 *
 * Everything here is whole buffers -- the fast loop in full charge -- because
 * this has nothing to do with running out of room or out of input.
 */
TEST(DeflateFastPath, AStoredBlockAfterTheFastLoopHasBeenRunning) {
  Xorshift noise(88172645463325252ull);
  std::vector<uint8_t> raw(300000u);
  for (auto & b : raw) {
    b = noise.Byte();
  }

  for (const char * method : {"deflate", "zlib", "gzip"}) {
    for (int level : {1, 5, 9}) {
      const std::vector<uint8_t> enc = Encode(method, raw, level);
      // If this ever stops holding, the encoder has stopped storing anything
      // and the test is no longer about a mixed stream.
      ASSERT_GT(enc.size(), raw.size())
          << method << " level " << level << ": corpus is compressible now";
      Decoded got = Decode(method, enc, 0u, raw.size() + 1024u, nullptr);
      const std::string where =
          std::string(method) + " level " + std::to_string(level);
      ASSERT_EQ(got.status, GCOMP_OK) << where;
      ASSERT_EQ(got.bytes.size(), raw.size()) << where;
      ASSERT_EQ(std::memcmp(got.bytes.data(), raw.data(), raw.size()), 0)
          << where;
    }
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
