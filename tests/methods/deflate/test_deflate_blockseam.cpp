/**
 * @file test_deflate_blockseam.cpp
 *
 * Tests for a DEFLATE stream whose block headers land on a call boundary.
 *
 * WHY THESE EXIST AS A FILE OF THEIR OWN
 * ======================================
 *
 * A caller that hands the decoder a few bytes at a time can split the stream
 * anywhere, including in the middle of a three-bit block header.  The decoder
 * has a mechanism for that -- read the whole header in one go, or read none
 * of it and come back -- and it is applied to the stored header (LEN/NLEN),
 * to the dynamic header (HLIT/HDIST/HCLEN), and, until this file was written,
 * not to the block header itself.  BFINAL was read on its own and BTYPE after
 * it, and a call that ran out of input between the two consumed BFINAL and
 * stored it nowhere.  The next call read BFINAL from BTYPE's low bit and
 * every bit after that was off by one.
 *
 * The visible result is the reserved block type: the decoder reports a
 * perfectly good stream as corrupt.  Not a wrong byte -- a refusal, on a
 * stream zlib accepts, and only when the caller's input chunking happens to
 * put the seam in that one place.
 *
 * Which is why the input chunk size is a test axis here, exactly as the
 * output chunk size is one in test_deflate_window.cpp.  The same stream is
 * decoded with the input revealed a byte at a time, two at a time, and so on,
 * and the bytes must not depend on how it was revealed.
 *
 * The first test does not rely on an encoder putting a header in the right
 * place.  It builds a ten-byte stream by hand so that the second block's
 * header begins on the last bit of byte seven, which is the one arrangement
 * that triggers it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "deflate_bits.h"
#include "deflate_noise.h"
#include "test_helpers.h"
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

using gcomp_test::Lcg;
using gcomp_test::Xorshift;

using gcomp_test::BitWriter;

/**
 * @brief Decode with the input revealed @p in_chunk bytes at a time.
 *
 * The input buffer grows; it is never handed over in isolated slices and
 * never rewound, because that is what a caller reading from a socket or a
 * file actually does, and because the decoder is allowed to leave bytes
 * unconsumed for the next call to see again.
 */
std::vector<uint8_t> DecodeRevealed(const char * method,
    const std::vector<uint8_t> & stream, size_t in_chunk, size_t cap,
    gcomp_status_t * status_out) {
  gcomp_registry_t * reg = gcomp_registry_default();
  gcomp_decoder_t * dec = nullptr;
  std::vector<uint8_t> out(cap + 64u, 0u);
  size_t produced = 0;
  size_t revealed = 0;
  *status_out = GCOMP_OK;

  if (gcomp_decoder_create(reg, method, nullptr, &dec) != GCOMP_OK) {
    *status_out = GCOMP_ERR_INTERNAL;
    return {};
  }
  gcomp_buffer_t input = {(void *)stream.data(), 0u, 0u};
  for (;;) {
    if (revealed < stream.size()) {
      revealed += in_chunk;
      if (revealed > stream.size()) {
        revealed = stream.size();
      }
    }
    input.size = revealed;
    gcomp_buffer_t output = {out.data() + produced, out.size() - produced, 0u};
    const size_t before = input.used;
    gcomp_status_t s = gcomp_decoder_update(dec, &input, &output);
    if (s != GCOMP_OK) {
      *status_out = s;
      gcomp_decoder_destroy(dec);
      return {};
    }
    produced += output.used;
    if (input.used == before && output.used == 0u &&
        revealed >= stream.size()) {
      break;
    }
  }
  for (;;) {
    gcomp_buffer_t output = {out.data() + produced, out.size() - produced, 0u};
    gcomp_status_t s = gcomp_decoder_finish(dec, &output);
    produced += output.used;
    if (s == GCOMP_OK) {
      break;
    }
    if (s != GCOMP_ERR_LIMIT || output.used == 0u) {
      *status_out = s;
      gcomp_decoder_destroy(dec);
      return {};
    }
  }
  gcomp_decoder_destroy(dec);
  out.resize(produced);
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

/**
 * @brief A stream whose second block header begins on the last bit of a byte.
 *
 * Block one is a fixed-Huffman block holding five nine-bit literals and an
 * end-of-block: 3 + 45 + 7 = 55 bits.  So the second header starts at bit 55,
 * and a caller who has revealed exactly seven bytes leaves the decoder
 * holding one bit with nothing behind it -- enough for BFINAL, not enough for
 * BTYPE.
 */
TEST(DeflateBlockSeam, ABlockHeaderSplitBetweenBfinalAndBtype) {
  BitWriter w;
  w.Field(0u, 1u); // BFINAL = 0
  w.Field(1u, 2u); // BTYPE  = 01, fixed Huffman
  for (int i = 0; i < 5; i++) {
    w.FixedLitLen(200u); // nine bits each
  }
  w.FixedEndOfBlock();
  ASSERT_EQ(w.BitsWritten(), 55u) << "the seam is only at bit 55 if this holds";

  w.Field(1u, 1u); // BFINAL = 1
  w.Field(1u, 2u); // BTYPE  = 01
  w.FixedLitLen('A');
  w.FixedEndOfBlock();
  const std::vector<uint8_t> stream = w.Finish();

  const std::vector<uint8_t> want = {200u, 200u, 200u, 200u, 200u, 'A'};

  // One byte at a time puts the seam there; seven at a time puts it there as
  // well, which is the case a real caller is likelier to produce.  The rest
  // are here so that a change which moves the seam somewhere else is still
  // caught.
  for (size_t chunk = 1; chunk <= 16u; chunk++) {
    gcomp_status_t status = GCOMP_OK;
    std::vector<uint8_t> got =
        DecodeRevealed("deflate", stream, chunk, want.size(), &status);
    EXPECT_EQ(status, GCOMP_OK)
        << "input revealed " << chunk << " bytes at a time";
    EXPECT_EQ(got, want) << "input revealed " << chunk << " bytes at a time";
  }
}

/**
 * @brief The bytes must not depend on how the input was revealed.
 *
 * The hand-built case above pins the one arrangement that was wrong.  This
 * one is the general statement, and it is what found it: incompressible data
 * long enough for the encoder to emit three blocks, decoded a byte at a time.
 */
TEST(DeflateBlockSeam, EveryInputChunkSizeGivesTheSameBytes) {
  struct Corpus {
    const char * name;
    std::vector<uint8_t> bytes;
  };
  std::vector<Corpus> corpora;

  {
    // Incompressible, and long enough that the encoder emits three coded
    // blocks.  This is the shape that found the bug: its second block ends
    // one bit into byte 53,792.
    Xorshift rng(12345u);
    std::vector<uint8_t> noise(70000u);
    for (auto & b : noise) {
      b = rng.Byte();
    }
    corpora.push_back({"incompressible-70k", std::move(noise)});
  }
  {
    // Half a fixed byte, half noise: compressible enough to be coded, random
    // enough that the blocks end wherever the entropy coder leaves them.
    Lcg rng(2u);
    std::vector<uint8_t> mixed(70000u);
    for (auto & b : mixed) {
      b = (rng.Byte() & 1u) ? (uint8_t)'x' : rng.Byte();
    }
    corpora.push_back({"mixed-70k", std::move(mixed)});
  }
  {
    // Ordinary compressible text, and big enough to be split.  120 KB is one
    // block and tests nothing about block boundaries; 400 KB is several.
    Lcg rng(1u);
    const char * words[] = {"the ", "quick ", "brown ", "fox ", "jumps ",
        "over ", "lazy ", "dog "};
    std::vector<uint8_t> wordy;
    wordy.reserve(400000u);
    while (wordy.size() < 400000u) {
      const char * w = words[rng.Byte() & 7u];
      wordy.insert(wordy.end(), w, w + std::strlen(w));
    }
    wordy.resize(400000u);
    corpora.push_back({"wordy-400k", std::move(wordy)});
  }

  const size_t chunks[] = {1u, 2u, 3u, 5u, 7u, 8u, 9u, 16u, 41u, 1024u};
  const char * methods[] = {"deflate", "zlib", "gzip"};
  const int levels[] = {1, 6, 9};

  for (const auto & corpus : corpora) {
    for (const char * method : methods) {
      for (int level : levels) {
        const std::vector<uint8_t> enc = Encode(method, corpus.bytes, level);
        for (size_t chunk : chunks) {
          gcomp_status_t status = GCOMP_OK;
          std::vector<uint8_t> got = DecodeRevealed(
              method, enc, chunk, corpus.bytes.size(), &status);
          ASSERT_EQ(status, GCOMP_OK)
              << corpus.name << " " << method << " level " << level
              << " revealed " << chunk << " bytes at a time";
          ASSERT_EQ(got.size(), corpus.bytes.size())
              << corpus.name << " " << method << " level " << level
              << " revealed " << chunk;
          ASSERT_EQ(std::memcmp(got.data(), corpus.bytes.data(), got.size()), 0)
              << corpus.name << " " << method << " level " << level
              << " revealed " << chunk;
        }
      }
    }
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
