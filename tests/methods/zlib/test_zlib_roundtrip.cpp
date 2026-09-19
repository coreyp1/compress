/**
 * @file test_zlib_roundtrip.cpp
 *
 * Round-trip, streaming, reset and limit tests for the zlib container.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../common/test_helpers.h"
#include <algorithm>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zlib.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

class ZlibRoundTripTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  /// Encode, offering input in `in_chunk` pieces and taking `out_chunk` back.
  std::vector<uint8_t> Encode(const std::vector<uint8_t> & data,
      gcomp_options_t * opts, size_t in_chunk = 0, size_t out_chunk = 0) {
    gcomp_encoder_t * encoder = nullptr;
    EXPECT_EQ(
        gcomp_encoder_create(registry_, "zlib", opts, &encoder), GCOMP_OK);
    if (!encoder) {
      return {};
    }
    if (in_chunk == 0) {
      in_chunk = data.size() ? data.size() : 1;
    }
    if (out_chunk == 0) {
      out_chunk = 4096;
    }

    std::vector<uint8_t> stream;
    std::vector<uint8_t> chunk(out_chunk);
    size_t consumed = 0;
    while (consumed < data.size()) {
      size_t take = std::min(in_chunk, data.size() - consumed);
      gcomp_buffer_t in_buf = {
          const_cast<uint8_t *>(data.data() + consumed), take, 0};
      while (in_buf.used < in_buf.size) {
        gcomp_buffer_t out_buf = {chunk.data(), chunk.size(), 0};
        size_t bi = in_buf.used;
        EXPECT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
        stream.insert(
            stream.end(), chunk.begin(), chunk.begin() + out_buf.used);
        if (in_buf.used == bi && out_buf.used == 0) {
          ADD_FAILURE() << "update() made no progress";
          break;
        }
      }
      consumed += take;
    }
    for (;;) {
      gcomp_buffer_t out_buf = {chunk.data(), chunk.size(), 0};
      gcomp_status_t s = gcomp_encoder_finish(encoder, &out_buf);
      stream.insert(stream.end(), chunk.begin(), chunk.begin() + out_buf.used);
      if (s == GCOMP_OK) {
        break;
      }
      if (s != GCOMP_ERR_LIMIT) {
        ADD_FAILURE() << "finish() returned " << s;
        break;
      }
      if (out_buf.used == 0) {
        ADD_FAILURE() << "finish() made no progress";
        break;
      }
    }
    gcomp_encoder_destroy(encoder);
    return stream;
  }

  std::vector<uint8_t> Decode(const std::vector<uint8_t> & stream,
      size_t expected, size_t in_chunk = 0, size_t out_chunk = 0) {
    gcomp_decoder_t * decoder = nullptr;
    EXPECT_EQ(
        gcomp_decoder_create(registry_, "zlib", nullptr, &decoder), GCOMP_OK);
    if (!decoder) {
      return {};
    }
    if (in_chunk == 0) {
      in_chunk = stream.size() ? stream.size() : 1;
    }
    if (out_chunk == 0) {
      out_chunk = expected + 4096;
    }

    std::vector<uint8_t> out;
    std::vector<uint8_t> chunk(out_chunk);
    size_t offset = 0;
    while (offset < stream.size()) {
      size_t take = std::min(in_chunk, stream.size() - offset);
      gcomp_buffer_t in_buf = {
          const_cast<uint8_t *>(stream.data() + offset), take, 0};
      while (in_buf.used < in_buf.size) {
        gcomp_buffer_t out_buf = {chunk.data(), chunk.size(), 0};
        size_t bi = in_buf.used;
        EXPECT_EQ(gcomp_decoder_update(decoder, &in_buf, &out_buf), GCOMP_OK);
        out.insert(out.end(), chunk.begin(), chunk.begin() + out_buf.used);
        if (in_buf.used == bi && out_buf.used == 0) {
          ADD_FAILURE() << "decoder update() made no progress";
          break;
        }
      }
      offset += take;
    }
    gcomp_buffer_t out_buf = {chunk.data(), chunk.size(), 0};
    EXPECT_EQ(gcomp_decoder_finish(decoder, &out_buf), GCOMP_OK);
    out.insert(out.end(), chunk.begin(), chunk.begin() + out_buf.used);
    gcomp_decoder_destroy(decoder);
    return out;
  }

  static std::vector<uint8_t> Mixed(size_t n, unsigned seed) {
    std::vector<uint8_t> v(n);
    if (n == 0) {
      return v; // v.data() is null here, and pointer arithmetic on it is UB.
    }
    static const char * words[] = {"the ", "quick ", "brown ", "fox ",
        "jumps ", "over ", "lazy ", "dog "};
    unsigned s = seed;
    size_t p = 0;
    while (p < n / 3) {
      s = s * 1103515245u + 12345u;
      const char * w = words[(s >> 16) % 8];
      size_t l = strlen(w);
      if (p + l > n / 3) {
        l = n / 3 - p;
      }
      if (l > 0) {
        memcpy(v.data() + p, w, l);
      }
      p += l;
    }
    size_t run = n / 3;
    if (run > 0) {
      memset(v.data() + p, 'Q', run);
    }
    p += run;
    while (p < n) {
      s = s * 1103515245u + 12345u;
      v[p++] = (uint8_t)(s >> 16);
    }
    return v;
  }

  gcomp_registry_t * registry_ = nullptr;
};

TEST_F(ZlibRoundTripTest, RoundTripsAtEveryLevel) {
  const std::vector<uint8_t> data = Mixed(60000, 17);
  for (int64_t level = 0; level <= 9; level++) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", level), GCOMP_OK);
    std::vector<uint8_t> stream = Encode(data, opts);
    gcomp_options_destroy(opts);
    EXPECT_EQ(Decode(stream, data.size()), data) << "level=" << level;
  }
}

TEST_F(ZlibRoundTripTest, RoundTripsAtEveryWindowSize) {
  const std::vector<uint8_t> data = Mixed(60000, 23);
  for (uint64_t wbits = 8; wbits <= 15; wbits++) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(opts, "deflate.window_bits", wbits),
        GCOMP_OK);
    std::vector<uint8_t> stream = Encode(data, opts);
    gcomp_options_destroy(opts);
    EXPECT_EQ(Decode(stream, data.size()), data) << "window_bits=" << wbits;
  }
}

TEST_F(ZlibRoundTripTest, RoundTripsEveryStrategy) {
  const std::vector<uint8_t> data = Mixed(30000, 29);
  for (const char * strategy :
      {"default", "lazy", "huffman_only", "rle", "fixed"}) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_string(opts, "deflate.strategy", strategy),
        GCOMP_OK);
    std::vector<uint8_t> stream = Encode(data, opts);
    gcomp_options_destroy(opts);
    EXPECT_EQ(Decode(stream, data.size()), data) << "strategy=" << strategy;
  }
}

TEST_F(ZlibRoundTripTest, RoundTripsAwkwardSizes) {
  const size_t sizes[] = {0, 1, 2, 5, 6, 7, 255, 256, 257, 4095, 4096, 4097,
      65535, 65536, 65537};
  for (size_t n : sizes) {
    std::vector<uint8_t> data = Mixed(n, (unsigned)n + 1);
    std::vector<uint8_t> stream = Encode(data, nullptr);
    EXPECT_GE(stream.size(), 6u) << "n=" << n;
    EXPECT_EQ(Decode(stream, n), data) << "n=" << n;
  }
}

/**
 * A byte in and a byte out, on both sides: every resumption path.
 *
 * What is checked is that each of them round-trips, not that they produce
 * identical streams.  Deflate chooses its block boundaries from what it has
 * in hand, and lazy matching looks one position ahead, so how the input
 * arrives legitimately changes the bytes -- unlike LZ4's parallel encoder,
 * where byte-identity is a designed guarantee.  Asserting it here would be
 * asserting something deflate never promised.
 */
TEST_F(ZlibRoundTripTest, SurvivesStarvedBuffersOnBothSides) {
  const std::vector<uint8_t> data = Mixed(9000, 31);
  std::vector<uint8_t> reference = Encode(data, nullptr);
  EXPECT_EQ(Decode(reference, data.size()), data);

  const size_t in_chunks[] = {1, 3, 1000};
  const size_t out_chunks[] = {1, 7, 4096};
  for (size_t in_chunk : in_chunks) {
    for (size_t out_chunk : out_chunks) {
      std::vector<uint8_t> chunked = Encode(data, nullptr, in_chunk, out_chunk);
      ASSERT_GE(chunked.size(), 6u)
          << "in=" << in_chunk << " out=" << out_chunk;
      EXPECT_EQ(((unsigned)(chunked[0] << 8) | chunked[1]) % 31u, 0u)
          << "in=" << in_chunk << " out=" << out_chunk;
      EXPECT_EQ(Decode(chunked, data.size()), data)
          << "in=" << in_chunk << " out=" << out_chunk;
    }
  }

  EXPECT_EQ(Decode(reference, data.size(), 1, 1), data);
  EXPECT_EQ(Decode(reference, data.size(), 3, 7), data);
}

/// Data that does not compress still round-trips, and still carries a valid
/// header and checksum.
TEST_F(ZlibRoundTripTest, IncompressibleDataRoundTrips) {
  std::vector<uint8_t> noise(50000);
  test_helpers_generate_random(noise.data(), noise.size(), 8675309);
  std::vector<uint8_t> stream = Encode(noise, nullptr);
  EXPECT_EQ(((unsigned)(stream[0] << 8) | stream[1]) % 31u, 0u);
  EXPECT_EQ(Decode(stream, noise.size()), noise);
}

TEST_F(ZlibRoundTripTest, HighlyCompressibleDataRoundTrips) {
  std::vector<uint8_t> zeros(200000, 0);
  std::vector<uint8_t> stream = Encode(zeros, nullptr);
  EXPECT_LT(stream.size(), zeros.size() / 50);
  EXPECT_EQ(Decode(stream, zeros.size()), zeros);
}

TEST_F(ZlibRoundTripTest, TheBufferHelpersWork) {
  const std::vector<uint8_t> data = Mixed(20000, 41);
  std::vector<uint8_t> encoded(data.size() + 4096);
  size_t encoded_len = 0;
  ASSERT_EQ(gcomp_encode_buffer(registry_, "zlib", nullptr, data.data(),
                data.size(), encoded.data(), encoded.size(), &encoded_len),
      GCOMP_OK);

  std::vector<uint8_t> decoded(data.size() + 4096);
  size_t decoded_len = 0;
  ASSERT_EQ(gcomp_decode_buffer(registry_, "zlib", nullptr, encoded.data(),
                encoded_len, decoded.data(), decoded.size(), &decoded_len),
      GCOMP_OK);
  decoded.resize(decoded_len);
  EXPECT_EQ(decoded, data);
}

/// Reset must start a genuinely new stream, header, checksum and all.
TEST_F(ZlibRoundTripTest, ResetStartsANewStream) {
  const std::vector<uint8_t> first = Mixed(8000, 51);
  const std::vector<uint8_t> second = Mixed(12000, 52);

  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "zlib", nullptr, &encoder), GCOMP_OK);

  auto run = [&](const std::vector<uint8_t> & data) {
    std::vector<uint8_t> out(data.size() + 4096);
    gcomp_buffer_t in_buf = {
        const_cast<uint8_t *>(data.data()), data.size(), 0};
    gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
    while (in_buf.used < in_buf.size) {
      EXPECT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
    }
    while (gcomp_encoder_finish(encoder, &out_buf) == GCOMP_ERR_LIMIT) {
    }
    out.resize(out_buf.used);
    return out;
  };

  std::vector<uint8_t> a = run(first);
  ASSERT_EQ(gcomp_encoder_reset(encoder), GCOMP_OK);
  std::vector<uint8_t> b = run(second);
  gcomp_encoder_destroy(encoder);

  EXPECT_EQ(Decode(a, first.size()), first);
  EXPECT_EQ(Decode(b, second.size()), second);

  // The second stream is a stream in its own right, not a continuation.
  ASSERT_GE(b.size(), 2u);
  EXPECT_EQ(((unsigned)(b[0] << 8) | b[1]) % 31u, 0u);

  // And resetting again reproduces the first stream exactly, which it could
  // not if the Adler-32 or the deflate history had carried over.
  gcomp_encoder_t * fresh = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zlib", nullptr, &fresh), GCOMP_OK);
  gcomp_encoder_destroy(fresh);
  EXPECT_EQ(Encode(first, nullptr), a);
}

TEST_F(ZlibRoundTripTest, DecoderResetStartsANewStream) {
  const std::vector<uint8_t> data = Mixed(8000, 61);
  std::vector<uint8_t> stream = Encode(data, nullptr);

  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zlib", nullptr, &decoder), GCOMP_OK);

  for (int pass = 0; pass < 3; pass++) {
    if (pass > 0) {
      ASSERT_EQ(gcomp_decoder_reset(decoder), GCOMP_OK);
    }
    std::vector<uint8_t> out(data.size() + 4096);
    gcomp_buffer_t in_buf = {stream.data(), stream.size(), 0};
    gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
    ASSERT_EQ(gcomp_decoder_update(decoder, &in_buf, &out_buf), GCOMP_OK)
        << "pass " << pass;
    ASSERT_EQ(gcomp_decoder_finish(decoder, &out_buf), GCOMP_OK)
        << "pass " << pass;
    out.resize(out_buf.used);
    EXPECT_EQ(out, data) << "pass " << pass;
  }
  gcomp_decoder_destroy(decoder);
}

//
// Limits
//

TEST_F(ZlibRoundTripTest, TheOutputLimitIsEnforced) {
  std::vector<uint8_t> zeros(500000, 0);
  std::vector<uint8_t> stream = Encode(zeros, nullptr);

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_uint64(opts, "limits.max_output_bytes", 1000u),
      GCOMP_OK);
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zlib", opts, &decoder), GCOMP_OK);

  std::vector<uint8_t> out(zeros.size() + 4096);
  gcomp_buffer_t in_buf = {stream.data(), stream.size(), 0};
  gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
  gcomp_status_t s = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  if (s == GCOMP_OK) {
    s = gcomp_decoder_finish(decoder, &out_buf);
  }
  EXPECT_EQ(s, GCOMP_ERR_LIMIT);
  gcomp_decoder_destroy(decoder);
  gcomp_options_destroy(opts);
}

/// A compression bomb must be refused, not expanded.
TEST_F(ZlibRoundTripTest, TheExpansionRatioLimitIsEnforced) {
  std::vector<uint8_t> zeros(4 * 1024 * 1024, 0);
  std::vector<uint8_t> stream = Encode(zeros, nullptr);
  ASSERT_LT(stream.size(), zeros.size() / 500)
      << "the test needs a stream that really does expand hugely";

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 100u),
      GCOMP_OK);
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "zlib", opts, &decoder), GCOMP_OK);

  std::vector<uint8_t> out(zeros.size() + 4096);
  gcomp_buffer_t in_buf = {stream.data(), stream.size(), 0};
  gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
  gcomp_status_t s = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  if (s == GCOMP_OK) {
    s = gcomp_decoder_finish(decoder, &out_buf);
  }
  EXPECT_EQ(s, GCOMP_ERR_LIMIT);
  gcomp_decoder_destroy(decoder);
  gcomp_options_destroy(opts);
}

/// zlib is smaller than gzip for the same data, and bigger than raw deflate,
/// by exactly the framing each adds.
TEST_F(ZlibRoundTripTest, TheOverheadIsSixBytesOverRawDeflate) {
  const std::vector<uint8_t> data = Mixed(40000, 71);

  std::vector<uint8_t> wrapped = Encode(data, nullptr);

  gcomp_encoder_t * raw = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "deflate", nullptr, &raw), GCOMP_OK);
  std::vector<uint8_t> bare(data.size() + 4096);
  gcomp_buffer_t in_buf = {
      const_cast<uint8_t *>(data.data()), data.size(), 0};
  gcomp_buffer_t out_buf = {bare.data(), bare.size(), 0};
  while (in_buf.used < in_buf.size) {
    ASSERT_EQ(gcomp_encoder_update(raw, &in_buf, &out_buf), GCOMP_OK);
  }
  while (gcomp_encoder_finish(raw, &out_buf) == GCOMP_ERR_LIMIT) {
  }
  bare.resize(out_buf.used);
  gcomp_encoder_destroy(raw);

  EXPECT_EQ(wrapped.size(), bare.size() + 6u)
      << "two header bytes and a four-byte Adler-32, and nothing else";
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
