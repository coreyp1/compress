/**
 * @file test_deflate_readahead.cpp
 *
 * Tests for the bytes the DEFLATE decoder reads past the end of the stream.
 *
 * WHY THIS MATTERS TO ANYTHING
 * ============================
 *
 * Nothing in RFC 1951 says how long the stream is; the decoder finds out by
 * decoding the final block.  By then its bit buffer has already pulled in
 * eight bytes at a time, so whatever follows the stream -- for gzip and zlib,
 * the checksum trailer -- is usually sitting inside it.  Those bytes belong to
 * whoever wrapped the stream, and there are two ways to give them back:
 *
 *  - rewind the input buffer, when they came from the buffer this call was
 *    given; or
 *  - keep them, when they were taken during an earlier call and there is no
 *    longer anywhere to put them back, and let the container ask.
 *
 * The second path is the one with a trap in it.  Bits are consumed from the
 * bottom of the buffer, so after the end-of-block symbol the low bits are the
 * encoder's padding to a byte boundary and the whole bytes start above them.
 * Reading from bit zero returns every byte shifted, and the container then
 * compares a perfectly correct decode against a checksum it misread.
 *
 * That went unnoticed because the path is hard to reach deliberately: it needs
 * the stream to end on a call whose input is already spent, which is what
 * happens when the output buffer is much smaller than the output.  A
 * whole-buffer decode takes the rewind path every time.  It also needs the
 * last block to be a Huffman one -- a stored block is byte-aligned, so the
 * padding is empty and the shift is zero.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/gzip.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/zlib.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "methods/deflate/deflate_internal.h"

namespace {

/// What a container would find sitting after the deflate stream.
const uint8_t kMarkers[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

class DeflateReadaheadTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_EQ(gcomp_registry_create(nullptr, &registry_), GCOMP_OK);
    ASSERT_EQ(gcomp_method_deflate_register(registry_), GCOMP_OK);
  }
  void TearDown() override {
    if (registry_) {
      gcomp_registry_destroy(registry_);
      registry_ = nullptr;
    }
  }
  gcomp_registry_t * registry_ = nullptr;
};

/**
 * Drive a raw decode the way a container does, and check that what the
 * decoder kept back is what actually followed the stream.
 *
 * "The way a container does" is the important part.  Left to itself, a caller
 * hands over one long input buffer and the stream ends while that buffer
 * still has the trailer in it, so the decoder rewinds and the keeping path
 * never runs.  A container cannot work that way: nothing in the deflate
 * stream says where it ends, so gzip and zlib ask by calling finish() after
 * every update(), and finish() decodes against an empty input buffer.  The
 * end of the stream is therefore recognised with nothing to rewind into, and
 * the read-ahead has nowhere to go but the decoder's own keeping.
 */
TEST_F(DeflateReadaheadTest, KeptBytesAreTheBytesThatFollowedTheStream) {
  int asked = 0;

  // Several lengths, because how many bits of padding the final byte carries
  // is a property of the particular stream, and a shifted read is invisible
  // when that number happens to be zero.
  for (size_t len : {301u, 1000u, 4097u, 20011u}) {
    std::vector<uint8_t> raw;
    raw.reserve(len);
    for (size_t i = 0; i < len; i++) {
      // Compressible, so the encoder chooses a Huffman block over a stored
      // one; a stored block is byte-aligned and leaves no padding to shift.
      raw.push_back((uint8_t)('a' + (i % 17) + ((i / 97) % 5)));
    }

    std::vector<uint8_t> stream(raw.size() + 4096);
    size_t enc_used = 0;
    ASSERT_EQ(gcomp_encode_buffer(registry_, "deflate", nullptr, raw.data(),
                  raw.size(), stream.data(), stream.size(), &enc_used),
        GCOMP_OK);
    stream.resize(enc_used);
    stream.insert(stream.end(), kMarkers, kMarkers + sizeof(kMarkers));

    gcomp_decoder_t * dec = nullptr;
    ASSERT_EQ(
        gcomp_decoder_create(registry_, "deflate", nullptr, &dec), GCOMP_OK);

    std::vector<uint8_t> sink(raw.size() + 64);
    size_t produced = 0;
    gcomp_buffer_t input = {stream.data(), stream.size(), 0};
    for (int guard = 0; guard < 100000; guard++) {
      if (gcomp_deflate_decoder_is_done(dec)) {
        break;
      }
      size_t before_in = input.used, before_out = produced;
      {
        gcomp_buffer_t output = {sink.data() + produced,
            produced < sink.size() ? 1u : 0u, 0};
        ASSERT_EQ(gcomp_decoder_update(dec, &input, &output), GCOMP_OK);
        produced += output.used;
      }
      {
        // The container's "are you finished yet?", with whatever room is
        // left over -- which is none, because the byte above filled it.
        gcomp_buffer_t output = {sink.data() + produced,
            produced < sink.size() ? sink.size() - produced : 0u, 0};
        (void)gcomp_decoder_finish(dec, &output);
        produced += output.used;
      }
      if (input.used == before_in && produced == before_out &&
          !gcomp_deflate_decoder_is_done(dec)) {
        break;
      }
    }

    ASSERT_TRUE(gcomp_deflate_decoder_is_done(dec)) << "length " << len;
    ASSERT_EQ(produced, raw.size()) << "length " << len;
    ASSERT_EQ(std::memcmp(sink.data(), raw.data(), raw.size()), 0)
        << "length " << len;

    uint32_t held = gcomp_deflate_decoder_get_unconsumed_bytes(dec);
    if (held > 0) {
      asked++;
      ASSERT_LE(held, sizeof(kMarkers)) << "length " << len;
      uint8_t got[16] = {};
      ASSERT_EQ(
          gcomp_deflate_decoder_get_unconsumed_data(dec, got, sizeof(got)),
          held);
      for (uint32_t i = 0; i < held; i++) {
        EXPECT_EQ(got[i], kMarkers[i])
            << "length " << len << ", kept byte " << i << " of " << held
            << " -- a wrong value here means the padding was read as data "
               "and every kept byte came out shifted";
      }
    }

    gcomp_decoder_destroy(dec);
  }

  // Not a claim about the library but about this test: if no decode had ever
  // kept anything, every assertion above would have been skipped and the test
  // would pass however the decoder behaved.
  ASSERT_GT(asked, 0)
      << "no decode reached the path where read-ahead bytes are kept, so "
         "nothing here was actually tested";
}

/**
 * The same defect seen from where it hurt: a container comparing its checksum
 * against a decode that was perfectly correct.
 *
 * These seeds are not arbitrary.  Reaching the keeping path needs the stream
 * to end inside one of the container's own completion checks rather than
 * during an update, which depends on where the block boundaries fall, and
 * these are shapes that were observed to do it.  Before the fix, six of sixty
 * such shapes failed; the ones kept here are those six.
 */
TEST_F(DeflateReadaheadTest, ASmallOutputBufferStillVerifiesTheTrailer) {
  ASSERT_EQ(gcomp_method_gzip_register(registry_), GCOMP_OK);
  ASSERT_EQ(gcomp_method_zlib_register(registry_), GCOMP_OK);

  const uint32_t seeds[] = {79190u, 166299u, 269246u, 293003u, 443464u,
      451383u};
  const char * methods[] = {"zlib", "gzip"};

  for (uint32_t seed : seeds) {
    const size_t blk = 8192;
    std::vector<uint8_t> raw(blk * 3);
    uint32_t st = seed;
    for (size_t i = 0; i < blk * 2; i++) {
      st = st * 1103515245u + 12345u;
      raw[i] = (uint8_t)((st >> 8) & 0xFFu);
    }
    std::memcpy(raw.data() + blk * 2, raw.data(), blk);

    for (const char * method : methods) {
      gcomp_options_t * opt = nullptr;
      ASSERT_EQ(gcomp_options_create(&opt), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_int64(opt, "deflate.level", 6), GCOMP_OK);
      std::vector<uint8_t> enc(raw.size() * 2);
      size_t enc_used = 0;
      ASSERT_EQ(gcomp_encode_buffer(registry_, method, opt, raw.data(),
                    raw.size(), enc.data(), enc.size(), &enc_used),
          GCOMP_OK);
      gcomp_options_destroy(opt);
      enc.resize(enc_used);

      gcomp_decoder_t * dec = nullptr;
      ASSERT_EQ(gcomp_decoder_create(registry_, method, nullptr, &dec),
          GCOMP_OK);
      std::vector<uint8_t> sink(raw.size() + 64);
      size_t produced = 0;
      gcomp_buffer_t input = {enc.data(), enc.size(), 0};
      while (input.used < input.size && produced < sink.size()) {
        size_t before = input.used;
        gcomp_buffer_t output = {sink.data() + produced, 1, 0};
        ASSERT_EQ(gcomp_decoder_update(dec, &input, &output), GCOMP_OK)
            << method << " seed " << seed << ": "
            << gcomp_decoder_get_error_detail(dec);
        produced += output.used;
        if (output.used == 0 && input.used == before) {
          break;
        }
      }
      for (int i = 0; i < 100000; i++) {
        gcomp_buffer_t output = {sink.data() + produced,
            produced < sink.size() ? sink.size() - produced : 0u, 0};
        gcomp_status_t s = gcomp_decoder_finish(dec, &output);
        produced += output.used;
        if (s == GCOMP_OK) {
          break;
        }
        ASSERT_EQ(s, GCOMP_ERR_LIMIT)
            << method << " seed " << seed << ": "
            << gcomp_decoder_get_error_detail(dec);
        ASSERT_GT(output.used, 0u) << method << " seed " << seed;
      }
      EXPECT_EQ(produced, raw.size()) << method << " seed " << seed;
      EXPECT_EQ(std::memcmp(sink.data(), raw.data(), raw.size()), 0)
          << method << " seed " << seed;
      gcomp_decoder_destroy(dec);
    }
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
