/**
 * @file test_zstd_decoder.cpp
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
#include "data/golden_vectors.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>
#include <vector>

class ZstdDecoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
  }

  // Helper: compress data with optional options (for producing test frames)
  std::vector<uint8_t> compress(
      const void * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_encoder_t * enc = nullptr;
    if (gcomp_encoder_create(registry_, "zstd", opts, &enc) != GCOMP_OK)
      return {};
    std::vector<uint8_t> out(len + 256);
    gcomp_buffer_t in = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};
    if (gcomp_encoder_update(enc, &in, &ob) != GCOMP_OK) {
      gcomp_encoder_destroy(enc);
      return {};
    }
    if (gcomp_encoder_finish(enc, &ob) != GCOMP_OK) {
      gcomp_encoder_destroy(enc);
      return {};
    }
    out.resize(ob.used);
    gcomp_encoder_destroy(enc);
    return out;
  }

  // Helper: decode compressed data with optional options
  std::vector<uint8_t> decode(const void * data, size_t len,
      gcomp_options_t * opts = nullptr, gcomp_status_t * status_out = nullptr) {
    gcomp_decoder_t * dec = nullptr;
    if (gcomp_decoder_create(registry_, "zstd", opts, &dec) != GCOMP_OK) {
      if (status_out)
        *status_out = GCOMP_ERR_INVALID_ARG;
      return {};
    }
    std::vector<uint8_t> out(len * 100 + 4096);
    gcomp_buffer_t in = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};
    gcomp_status_t st = gcomp_decoder_update(dec, &in, &ob);
    if (st != GCOMP_OK) {
      if (status_out)
        *status_out = st;
      gcomp_decoder_destroy(dec);
      return {};
    }
    st = gcomp_decoder_finish(dec, &ob);
    if (status_out)
      *status_out = st;
    if (st != GCOMP_OK) {
      gcomp_decoder_destroy(dec);
      return {};
    }
    out.resize(ob.used);
    gcomp_decoder_destroy(dec);
    return out;
  }

  gcomp_registry_t * registry_ = nullptr;
};

TEST_F(ZstdDecoderTest, CreateSuccess) {
  gcomp_decoder_t * dec = nullptr;
  EXPECT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  gcomp_decoder_destroy(dec);
}

TEST_F(ZstdDecoderTest, GoldenVectorsDecode) {
  // Decode minimal frame (empty payload)
  auto out1 =
      decode(zstd_v1_empty_compressed, sizeof(zstd_v1_empty_compressed));
  EXPECT_EQ(out1.size(), (size_t)zstd_v1_empty_expected_len);

  // Decode small payload "Hello"
  auto out2 =
      decode(zstd_v2_hello_compressed, sizeof(zstd_v2_hello_compressed));
  ASSERT_EQ(out2.size(), sizeof(zstd_v2_hello_expected));
  EXPECT_EQ(memcmp(out2.data(), zstd_v2_hello_expected, out2.size()), 0);
}

TEST_F(ZstdDecoderTest, DecodeInvalidMagic) {
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  uint8_t bad[] = {0x00, 0x00, 0x00, 0x00, 0x00};
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {bad, sizeof(bad), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &in, &ob), GCOMP_ERR_CORRUPT);
  gcomp_decoder_destroy(dec);
}

TEST_F(ZstdDecoderTest, DestroyImmediately) {
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  gcomp_decoder_destroy(dec);
}

TEST_F(ZstdDecoderTest, NullInputWithSize) {
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {nullptr, 100, 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_decoder_update(dec, &in, &ob), GCOMP_ERR_INVALID_ARG);
  gcomp_decoder_destroy(dec);
}

TEST_F(ZstdDecoderTest, ResetAndReuse) {
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  EXPECT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);
  gcomp_decoder_destroy(dec);
}

TEST_F(ZstdDecoderTest, BasicDecode) {
  const char data[] = "Hello, Zstd!";
  auto compressed = compress(data, strlen(data));
  ASSERT_GT(compressed.size(), 0u);
  auto decoded = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decoded.size(), strlen(data));
  EXPECT_EQ(memcmp(decoded.data(), data, strlen(data)), 0);
}

TEST_F(ZstdDecoderTest, DecodeWithContentChecksum) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.checksum", true);
  const char data[] = "Data with content checksum";
  auto compressed = compress(data, strlen(data), opts);
  gcomp_options_destroy(opts);
  ASSERT_GT(compressed.size(), 0u);
  auto decoded = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decoded.size(), strlen(data));
  EXPECT_EQ(memcmp(decoded.data(), data, strlen(data)), 0);
}

TEST_F(ZstdDecoderTest, DecodeWithContentSizeValidation) {
  const char data[] = "Content size in frame header";
  const size_t data_len = strlen(data);
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "zstd.content_size", data_len);
  auto compressed = compress(data, data_len, opts);
  gcomp_options_destroy(opts);
  ASSERT_GT(compressed.size(), 0u);
  auto decoded = decode(compressed.data(), compressed.size());
  ASSERT_EQ(decoded.size(), data_len);
  EXPECT_EQ(memcmp(decoded.data(), data, data_len), 0);
}

TEST_F(ZstdDecoderTest, DecodeWithVariousWindowSizes) {
  std::vector<uint8_t> data(4096);
  for (size_t i = 0; i < data.size(); i++)
    data[i] = (uint8_t)(i * 31 + i / 256);
  const unsigned window_logs[] = {16, 20};
  for (unsigned wlog : window_logs) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    gcomp_options_set_uint64(opts, "zstd.window_log", wlog);
    auto compressed = compress(data.data(), data.size(), opts);
    gcomp_options_destroy(opts);
    ASSERT_GT(compressed.size(), 0u) << "window_log " << wlog;
    auto decoded = decode(compressed.data(), compressed.size());
    ASSERT_EQ(decoded.size(), data.size()) << "window_log " << wlog;
    EXPECT_EQ(memcmp(decoded.data(), data.data(), data.size()), 0)
        << "window_log " << wlog;
  }
}

TEST_F(ZstdDecoderTest, Decode1ByteInputChunks) {
  const char data[] = "Chunked input decode test";
  auto compressed = compress(data, strlen(data));
  ASSERT_GT(compressed.size(), 0u);

  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> out(strlen(data) + 256);
  size_t out_offset = 0;
  size_t in_offset = 0;

  while (in_offset < compressed.size()) {
    size_t in_chunk = 1;
    if (in_offset + in_chunk > compressed.size())
      in_chunk = compressed.size() - in_offset;
    gcomp_buffer_t in_buf = {compressed.data() + in_offset, in_chunk, 0};
    gcomp_buffer_t ob = {out.data() + out_offset, out.size() - out_offset, 0};
    gcomp_status_t st = gcomp_decoder_update(dec, &in_buf, &ob);
    ASSERT_EQ(st, GCOMP_OK);
    in_offset += in_buf.used;
    out_offset += ob.used;
  }

  gcomp_buffer_t ob = {out.data() + out_offset, out.size() - out_offset, 0};
  EXPECT_EQ(gcomp_decoder_finish(dec, &ob), GCOMP_OK);
  out_offset += ob.used;
  gcomp_decoder_destroy(dec);

  ASSERT_EQ(out_offset, strlen(data));
  EXPECT_EQ(memcmp(out.data(), data, strlen(data)), 0);
}

TEST_F(ZstdDecoderTest, Decode1ByteOutputBuffer) {
  const char data[] = "Small output";
  auto compressed = compress(data, strlen(data));
  ASSERT_GT(compressed.size(), 0u);

  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> out;
  uint8_t one_byte[1];
  size_t in_offset = 0;

  while (in_offset < compressed.size()) {
    gcomp_buffer_t in_buf = {
        compressed.data() + in_offset, compressed.size() - in_offset, 0};
    gcomp_buffer_t ob = {one_byte, 1, 0};
    gcomp_status_t st = gcomp_decoder_update(dec, &in_buf, &ob);
    ASSERT_EQ(st, GCOMP_OK);
    in_offset += in_buf.used;
    if (ob.used > 0)
      out.push_back(one_byte[0]);
  }

  bool done = false;
  while (!done) {
    gcomp_buffer_t ob = {one_byte, 1, 0};
    gcomp_status_t st = gcomp_decoder_finish(dec, &ob);
    ASSERT_EQ(st, GCOMP_OK);
    if (ob.used > 0)
      out.push_back(one_byte[0]);
    else
      done = true;
  }

  gcomp_decoder_destroy(dec);
  ASSERT_EQ(out.size(), strlen(data));
  EXPECT_EQ(memcmp(out.data(), data, strlen(data)), 0);
}

//
// Match copying
//
// A Zstandard match reads from one of two places: the window, which holds
// what earlier blocks decoded, or the block's own output.  One match can
// start in the window and finish in the output, and inside the window it can
// wrap the circular buffer.  That is three flat runs, and the copy now works
// out which apply once per match rather than once per byte.
//
// Each of those boundaries is an off-by-one away from producing wrong bytes
// while reporting success, and ordinary data barely touches them.
//

namespace {

/// A block of @p period distinct bytes repeated to fill @p total, so that
/// every match has exactly that offset.  A period shorter than a match is
/// what makes the copy overlap its own source.
std::vector<uint8_t> RepeatingBytes(size_t total, size_t period) {
  std::vector<uint8_t> block;
  uint32_t x = 0x9E3779Bu ^ static_cast<uint32_t>(period);
  for (size_t i = 0; i < period; i++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    block.push_back(static_cast<uint8_t>(x >> 19));
  }
  std::vector<uint8_t> v;
  v.reserve(total + period);
  while (v.size() < total) {
    v.insert(v.end(), block.begin(), block.end());
  }
  v.resize(total);
  return v;
}

// Decode into a buffer sized from the answer we already know, with the
// decompression-bomb guard lifted.
//
// Two things get in the way of testing a match copy with deliberately
// repetitive data.  The shared decode() helper sizes its output buffer at a
// hundred times the compressed length, which is a fine guess for ordinary
// data and far too small here -- 300 KB of one repeated byte compresses to a
// few dozen.  And that same ratio trips the expansion limit, which defaults
// to 1000x and exists to stop a decompression bomb: this input *is* one, on
// purpose, because a match that repeats one byte for a long way is exactly
// what the copy has to get right.
std::vector<uint8_t> ZstdDecodeExpecting(gcomp_registry_t * registry,
    const std::vector<uint8_t> & encoded, size_t expected_size,
    gcomp_status_t * status_out) {
  gcomp_options_t * opts = nullptr;
  if (gcomp_options_create(&opts) != GCOMP_OK) {
    return {};
  }
  gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 0u);
  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t dc = gcomp_decoder_create(registry, "zstd", opts, &dec);
  gcomp_options_destroy(opts);
  if (dc != GCOMP_OK) {
    return {};
  }
  std::vector<uint8_t> out(expected_size + 4096);
  gcomp_buffer_t in = {
      const_cast<uint8_t *>(encoded.data()), encoded.size(), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  gcomp_status_t st = gcomp_decoder_update(dec, &in, &ob);
  if (st == GCOMP_OK) {
    st = gcomp_decoder_finish(dec, &ob);
  }
  gcomp_decoder_destroy(dec);
  if (status_out) {
    *status_out = st;
  }
  if (st != GCOMP_OK) {
    return {};
  }
  out.resize(ob.used);
  return out;
}

} // namespace

// Offsets shorter than the match they serve, which is where source and
// destination overlap.  An offset of one is a single byte repeated and takes
// a separate path.
TEST_F(ZstdDecoderTest, MatchCopy_OverlappingMatches) {
  for (size_t period : {size_t(1), size_t(2), size_t(3), size_t(4), size_t(7),
           size_t(8), size_t(15), size_t(16), size_t(17), size_t(31),
           size_t(64), size_t(128), size_t(255), size_t(256)}) {
    std::vector<uint8_t> input = RepeatingBytes(300000, period);
    for (int level : {1, 3, 9}) {
      gcomp_options_t * opts = nullptr;
      ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
      gcomp_options_set_int64(opts, "zstd.level", level);
      std::vector<uint8_t> enc = compress(input.data(), input.size(), opts);
      gcomp_options_destroy(opts);
      ASSERT_FALSE(enc.empty()) << "period " << period << " level " << level;

      gcomp_status_t st = GCOMP_OK;
      std::vector<uint8_t> back =
          ZstdDecodeExpecting(registry_, enc, input.size(), &st);
      EXPECT_EQ(st, GCOMP_OK) << "period " << period << " level " << level;
      EXPECT_EQ(back, input) << "period " << period << " level " << level;
    }
  }
}

// Matches that reach back further than the block being decoded, so the copy
// has to come out of the window -- and, with a window small enough to wrap
// several times over, out of both ends of it.
TEST_F(ZstdDecoderTest, MatchCopy_ReachesIntoTheWindowAndWraps) {
  // Blocks are at most 128 KB (RFC 8878 section 3.1.1), so a repeat at a
  // longer period than that can only be satisfied from the window.
  for (uint64_t window_log : {(uint64_t)17, (uint64_t)18, (uint64_t)20}) {
    const size_t window = (size_t)1 << window_log;
    std::vector<uint8_t> input = RepeatingBytes(window * 5, window / 3 + 7);

    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    gcomp_options_set_int64(opts, "zstd.level", 9);
    gcomp_options_set_uint64(opts, "zstd.window_log", window_log);
    std::vector<uint8_t> enc = compress(input.data(), input.size(), opts);
    gcomp_options_destroy(opts);
    ASSERT_FALSE(enc.empty()) << "window_log " << window_log;

    gcomp_status_t st = GCOMP_OK;
    std::vector<uint8_t> back =
        ZstdDecodeExpecting(registry_, enc, input.size(), &st);
    EXPECT_EQ(st, GCOMP_OK) << "window_log " << window_log;
    EXPECT_EQ(back, input) << "window_log " << window_log;
  }
}

// A phrase that recurs just under, at, and just over a block boundary, so
// that a single match starts in the window and finishes in the output.
TEST_F(ZstdDecoderTest, MatchCopy_CrossesFromWindowIntoOutput) {
  const size_t kBlock = 128u * 1024u;
  for (size_t gap : {kBlock - 600, kBlock - 1, kBlock, kBlock + 1,
           kBlock + 600}) {
    std::vector<uint8_t> phrase;
    uint32_t x = 0xFEEDu ^ static_cast<uint32_t>(gap);
    for (int i = 0; i < 1200; i++) {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      phrase.push_back(static_cast<uint8_t>(x >> 19));
    }
    std::vector<uint8_t> input;
    for (int rep = 0; rep < 4; rep++) {
      input.insert(input.end(), phrase.begin(), phrase.end());
      while (input.size() % gap != 0) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        input.push_back(static_cast<uint8_t>(x >> 19));
      }
    }

    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    gcomp_options_set_int64(opts, "zstd.level", 9);
    std::vector<uint8_t> enc = compress(input.data(), input.size(), opts);
    gcomp_options_destroy(opts);
    ASSERT_FALSE(enc.empty()) << "gap " << gap;

    gcomp_status_t st = GCOMP_OK;
    std::vector<uint8_t> back =
        ZstdDecodeExpecting(registry_, enc, input.size(), &st);
    EXPECT_EQ(st, GCOMP_OK) << "gap " << gap;
    EXPECT_EQ(back, input) << "gap " << gap;
  }
}

// The decoder must not read past the input it was handed.
//
// Feeding it small chunks is not enough to prove that: the usual way to write
// that test leaves `data` pointing into the whole compressed stream and only
// shrinks `size`, so a decoder that reads past `size` still finds exactly the
// bytes it wanted and the test passes.  Every chunk here is copied into its
// own allocation of exactly the right length, so reading one byte too far is
// a heap overflow -- wrong data in an ordinary build, and a diagnostic under
// AddressSanitizer.
//
// This is what a compressed block being copied in with the wrong length looks
// like, and nothing else caught it.
TEST_F(ZstdDecoderTest, DoesNotReadPastTheInputItWasGiven) {
  std::vector<uint8_t> input = RepeatingBytes(200000, 61);
  // Some incompressible tail, so blocks are not all the same shape.
  uint32_t x = 0xBEEFu;
  for (int i = 0; i < 40000; i++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    input.push_back(static_cast<uint8_t>(x >> 19));
  }

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_int64(opts, "zstd.level", 3);
  std::vector<uint8_t> enc = compress(input.data(), input.size(), opts);
  gcomp_options_destroy(opts);
  ASSERT_FALSE(enc.empty());

  for (size_t chunk : {size_t(1), size_t(2), size_t(7), size_t(64),
           size_t(1000)}) {
    gcomp_options_t * dopts = nullptr;
    ASSERT_EQ(gcomp_options_create(&dopts), GCOMP_OK);
    gcomp_options_set_uint64(dopts, "limits.max_expansion_ratio", 0u);
    gcomp_decoder_t * dec = nullptr;
    ASSERT_EQ(gcomp_decoder_create(registry_, "zstd", dopts, &dec), GCOMP_OK);
    gcomp_options_destroy(dopts);

    std::vector<uint8_t> out(input.size() + 4096), decoded;
    size_t fed = 0;
    while (fed < enc.size()) {
      size_t n = enc.size() - fed;
      if (n > chunk) {
        n = chunk;
      }
      // Exactly n bytes, in an allocation of exactly n bytes.
      std::vector<uint8_t> piece(enc.begin() + (long)fed,
          enc.begin() + (long)(fed + n));
      gcomp_buffer_t in = {piece.data(), piece.size(), 0};
      while (in.used < in.size) {
        gcomp_buffer_t ob = {out.data(), out.size(), 0};
        size_t before = in.used;
        ASSERT_EQ(gcomp_decoder_update(dec, &in, &ob), GCOMP_OK)
            << "chunk " << chunk;
        decoded.insert(decoded.end(), out.data(), out.data() + ob.used);
        if (in.used == before && ob.used == 0) {
          break;
        }
      }
      fed += n;
    }
    for (;;) {
      gcomp_buffer_t ob = {out.data(), out.size(), 0};
      gcomp_status_t f = gcomp_decoder_finish(dec, &ob);
      decoded.insert(decoded.end(), out.data(), out.data() + ob.used);
      if (f == GCOMP_OK) {
        break;
      }
      ASSERT_EQ(f, GCOMP_ERR_LIMIT) << "chunk " << chunk;
    }
    gcomp_decoder_destroy(dec);
    EXPECT_EQ(decoded, input) << "chunk " << chunk;
  }
}

// Sequence bitstreams of every small length, including the ones that do not
// fill the reader's 64-bit window.
//
// The reader loads sixty-four bits at a time.  A stream of eight bytes or more
// can be loaded straight out of the caller's buffer; a shorter one is copied
// into eight bytes of the reader's own, right-aligned, so that the same load
// does not read past the end of what the caller owns.  That padding shifts
// every bit position by a constant, and the arithmetic either agrees with
// itself or it does not.
//
// Short inputs produce short bitstreams, so sweeping the input size sweeps
// the bitstream length across that boundary and either side of it.
TEST_F(ZstdDecoderTest, BitstreamsOfEverySmallLength) {
  // How short the bitstream comes out depends far more on the shape of the
  // input than on its length: a mixed input of a few hundred bytes still
  // produces nine or more bytes of sequences.  These four shapes are the ones
  // measured to produce the shortest -- a small repeating cycle, long runs, a
  // two symbol alphabet, and one byte throughout -- and between them they
  // reach bitstream lengths of one, three, four and seven bytes as well as
  // every length from eight upwards.
  for (int shape = 0; shape < 4; shape++) {
    for (size_t size = 1; size <= 300; size++) {
      std::vector<uint8_t> input;
      input.reserve(size);
      uint32_t x = 0x51ED5EEDu ^ static_cast<uint32_t>(size);
      for (size_t i = 0; i < size; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        switch (shape) {
        case 0:
          input.push_back(static_cast<uint8_t>('a' + (i % 3)));
          break;
        case 1:
          input.push_back(static_cast<uint8_t>((i / 8) % 2 ? 'z' : 'q'));
          break;
        case 2:
          input.push_back(static_cast<uint8_t>('a' + (x % 2)));
          break;
        default:
          input.push_back(static_cast<uint8_t>('k'));
          break;
        }
      }

    for (int level : {1, 3, 9}) {
      gcomp_options_t * opts = nullptr;
      ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
      gcomp_options_set_int64(opts, "zstd.level", level);
      std::vector<uint8_t> enc = compress(input.data(), input.size(), opts);
      gcomp_options_destroy(opts);
      ASSERT_FALSE(enc.empty()) << "size " << size << " level " << level;

      gcomp_status_t st = GCOMP_OK;
      std::vector<uint8_t> back =
          ZstdDecodeExpecting(registry_, enc, input.size(), &st);
      EXPECT_EQ(st, GCOMP_OK)
          << "shape " << shape << " size " << size << " level " << level;
      EXPECT_EQ(back, input)
          << "shape " << shape << " size " << size << " level " << level;
    }
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
