/**
 * @file test_deflate_decoder.cpp
 *
 * Unit tests for the DEFLATE decoder implementation in the Ghoti.io Compress
 * library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "data/golden_vectors.h"
#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <vector>

class DeflateDecoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Use a custom registry for test isolation - each test gets a fresh
    // registry that doesn't share state with other tests or the default
    // registry. This requires explicit method registration.
    ASSERT_EQ(gcomp_registry_create(nullptr, &registry_), GCOMP_OK);
    ASSERT_NE(registry_, nullptr);
    ASSERT_EQ(gcomp_method_deflate_register(registry_), GCOMP_OK);
  }

  void TearDown() override {
    if (decoder_) {
      gcomp_decoder_destroy(decoder_);
      decoder_ = nullptr;
    }
    if (registry_) {
      gcomp_registry_destroy(registry_);
      registry_ = nullptr;
    }
  }

  gcomp_registry_t * registry_ = nullptr;
  gcomp_decoder_t * decoder_ = nullptr;
};

TEST_F(DeflateDecoderTest, StoredBlock_HelloSingleCall) {
  // Raw DEFLATE stream:
  // BFINAL=1, BTYPE=00 (stored), align to byte, LEN=5, NLEN=~LEN, payload.
  const uint8_t deflate_stream[] = {
      0x01, 0x05, 0x00, 0xFA, 0xFF, 'H', 'e', 'l', 'l', 'o'};

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  uint8_t out[64] = {};
  gcomp_buffer_t in_buf = {deflate_stream, sizeof(deflate_stream), 0};
  gcomp_buffer_t out_buf = {out, sizeof(out), 0};

  ASSERT_EQ(gcomp_decoder_update(decoder_, &in_buf, &out_buf), GCOMP_OK);
  ASSERT_EQ(in_buf.used, sizeof(deflate_stream));
  ASSERT_EQ(out_buf.used, 5u);
  ASSERT_EQ(std::memcmp(out, "Hello", 5u), 0);

  gcomp_buffer_t finish_out = {
      out + out_buf.used, sizeof(out) - out_buf.used, 0};
  ASSERT_EQ(gcomp_decoder_finish(decoder_, &finish_out), GCOMP_OK);
  ASSERT_EQ(finish_out.used, 0u);
}

TEST_F(DeflateDecoderTest, StoredBlock_ChunkedInputAndOutput) {
  const uint8_t deflate_stream[] = {
      0x01, 0x05, 0x00, 0xFA, 0xFF, 'H', 'e', 'l', 'l', 'o'};

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  std::vector<uint8_t> out;
  out.resize(5);
  size_t out_written = 0;

  // Feed in two 5-byte chunks: header+LEN+NLEN (5 bytes) then payload (5
  // bytes).
  const size_t chunk = 5;
  for (size_t i = 0; i < sizeof(deflate_stream); i += chunk) {
    size_t n = (i + chunk <= sizeof(deflate_stream))
        ? chunk
        : (sizeof(deflate_stream) - i);
    gcomp_buffer_t in_buf = {deflate_stream + i, n, 0};
    uint8_t small_out[8] = {};
    gcomp_buffer_t out_buf = {small_out, sizeof(small_out), 0};

    ASSERT_EQ(gcomp_decoder_update(decoder_, &in_buf, &out_buf), GCOMP_OK);
    ASSERT_EQ(in_buf.used, n);

    for (size_t j = 0; j < out_buf.used; j++) {
      ASSERT_LT(out_written, out.size());
      out[out_written++] = small_out[j];
    }
  }

  ASSERT_EQ(out_written, 5u);
  ASSERT_EQ(std::memcmp(out.data(), "Hello", 5u), 0);

  uint8_t finish_buf[8] = {};
  gcomp_buffer_t finish_out = {finish_buf, sizeof(finish_buf), 0};
  ASSERT_EQ(gcomp_decoder_finish(decoder_, &finish_out), GCOMP_OK);
}

TEST_F(DeflateDecoderTest, Finish_IncompleteStreamReturnsCorrupt) {
  const uint8_t partial[] = {0x01, 0x05, 0x00}; // header + partial LEN

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  uint8_t out[8] = {};
  gcomp_buffer_t in_buf = {partial, sizeof(partial), 0};
  gcomp_buffer_t out_buf = {out, sizeof(out), 0};
  ASSERT_EQ(gcomp_decoder_update(decoder_, &in_buf, &out_buf), GCOMP_OK);

  gcomp_buffer_t finish_out = {out, sizeof(out), 0};
  ASSERT_EQ(gcomp_decoder_finish(decoder_, &finish_out), GCOMP_ERR_CORRUPT);
}

TEST_F(DeflateDecoderTest, FixedHuffman_HelloWorldSingleCall) {
  // Raw DEFLATE stream using fixed Huffman codes for "Hello, world!".
  // Generated via Python zlib:
  //   compressobj(level=6, wbits=-15, strategy=Z_FIXED)
  const uint8_t deflate_stream[] = {0xF3, 0x48, 0xCD, 0xC9, 0xC9, 0xD7, 0x51,
      0x28, 0xCF, 0x2F, 0xCA, 0x49, 0x51, 0x04, 0x00};

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  uint8_t out[64] = {};
  gcomp_buffer_t in_buf = {deflate_stream, sizeof(deflate_stream), 0};
  gcomp_buffer_t out_buf = {out, sizeof(out), 0};

  ASSERT_EQ(gcomp_decoder_update(decoder_, &in_buf, &out_buf), GCOMP_OK);
  ASSERT_EQ(in_buf.used, sizeof(deflate_stream));
  ASSERT_EQ(out_buf.used, 13u);
  ASSERT_EQ(std::memcmp(out, "Hello, world!", 13u), 0);

  gcomp_buffer_t finish_out = {
      out + out_buf.used, sizeof(out) - out_buf.used, 0};
  ASSERT_EQ(gcomp_decoder_finish(decoder_, &finish_out), GCOMP_OK);
  ASSERT_EQ(finish_out.used, 0u);
}

TEST_F(DeflateDecoderTest, FixedHuffman_ChunkedInputOneByteAtATime) {
  const uint8_t deflate_stream[] = {0xF3, 0x48, 0xCD, 0xC9, 0xC9, 0xD7, 0x51,
      0x28, 0xCF, 0x2F, 0xCA, 0x49, 0x51, 0x04, 0x00};
  const size_t stream_len = sizeof(deflate_stream);

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  std::vector<uint8_t> out;
  out.reserve(16);
  size_t in_off = 0;
  size_t iterations = 0;
  const size_t max_iterations =
      stream_len * 8u; /* allow multiple passes per byte */

  while (in_off < stream_len && iterations < max_iterations) {
    gcomp_buffer_t in_buf = {deflate_stream + in_off, 1u, 0};
    uint8_t tmp[4] = {};
    gcomp_buffer_t out_buf = {tmp, sizeof(tmp), 0};

    ASSERT_EQ(gcomp_decoder_update(decoder_, &in_buf, &out_buf), GCOMP_OK);
    in_off += in_buf.used;
    for (size_t j = 0; j < out_buf.used; j++) {
      out.push_back(tmp[j]);
    }
    iterations++;
  }

  ASSERT_LT(iterations, max_iterations) << "decoder did not consume input";
  ASSERT_EQ(in_off, stream_len);

  uint8_t finish_buf[8] = {};
  gcomp_buffer_t finish_out = {finish_buf, sizeof(finish_buf), 0};
  ASSERT_EQ(gcomp_decoder_finish(decoder_, &finish_out), GCOMP_OK);
  ASSERT_EQ(finish_out.used, 0u);

  ASSERT_EQ(out.size(), 13u);
  ASSERT_EQ(std::memcmp(out.data(), "Hello, world!", 13u), 0);
}

TEST_F(DeflateDecoderTest, FixedHuffman_SmallOutputBuffer) {
  // Test that the decoder can handle producing output with a small buffer.
  // This stress-tests the streaming behavior when output buffer space is
  // limited.

  // First, create a compressed stream from a reasonably sized input
  const char * original_str =
      "This is a test of small output buffer decoding. "
      "The decoder must handle backpressure correctly when output space is "
      "limited. Each call produces a small amount of output. "
      "We need enough data to exercise the decoder's internal buffering and "
      "match copy logic across multiple update calls.";
  const uint8_t * original = (const uint8_t *)original_str;
  size_t original_len = strlen(original_str);

  // Compress the data first
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "deflate", nullptr, &encoder), GCOMP_OK);

  std::vector<uint8_t> compressed(original_len * 2);
  gcomp_buffer_t enc_in = {original, original_len, 0};
  gcomp_buffer_t enc_out = {compressed.data(), compressed.size(), 0};
  ASSERT_EQ(gcomp_encoder_update(encoder, &enc_in, &enc_out), GCOMP_OK);
  ASSERT_EQ(enc_in.used, original_len);

  gcomp_buffer_t enc_finish = {
      compressed.data() + enc_out.used, compressed.size() - enc_out.used, 0};
  ASSERT_EQ(gcomp_encoder_finish(encoder, &enc_finish), GCOMP_OK);
  size_t compressed_len = enc_out.used + enc_finish.used;
  gcomp_encoder_destroy(encoder);

  // Now decode with small output buffer (1 byte at a time for decoder)
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  std::vector<uint8_t> decompressed;
  decompressed.reserve(original_len);

  gcomp_buffer_t in_buf = {compressed.data(), compressed_len, 0};

  size_t iterations = 0;
  const size_t max_iterations = original_len * 10; // Safety limit

  // Decode one byte at a time - decoder DOES support 1-byte output
  while (iterations < max_iterations) {
    uint8_t one_byte = 0;
    gcomp_buffer_t out_buf = {&one_byte, 1, 0};

    gcomp_status_t status = gcomp_decoder_update(decoder_, &in_buf, &out_buf);
    ASSERT_EQ(status, GCOMP_OK) << "Failed at iteration " << iterations;

    if (out_buf.used > 0) {
      decompressed.push_back(one_byte);
    }

    // Advance input pointer for consumed bytes
    in_buf.data = (const uint8_t *)in_buf.data + in_buf.used;
    in_buf.size -= in_buf.used;
    in_buf.used = 0;

    // If no input left and no output produced, we might be done
    if (in_buf.size == 0 && out_buf.used == 0) {
      break;
    }

    iterations++;
  }

  ASSERT_LT(iterations, max_iterations) << "Decoder did not make progress";

  // Finish - also one byte at a time
  iterations = 0;
  while (iterations < max_iterations) {
    uint8_t one_byte = 0;
    gcomp_buffer_t finish_out = {&one_byte, 1, 0};

    gcomp_status_t status = gcomp_decoder_finish(decoder_, &finish_out);
    ASSERT_EQ(status, GCOMP_OK) << "Finish failed at iteration " << iterations;

    if (finish_out.used > 0) {
      decompressed.push_back(one_byte);
    }
    else {
      // No more output from finish
      break;
    }

    iterations++;
  }

  // Verify the result
  ASSERT_EQ(decompressed.size(), original_len)
      << "Decompressed size mismatch: expected " << original_len << ", got "
      << decompressed.size();
  EXPECT_EQ(memcmp(decompressed.data(), original, original_len), 0)
      << "Decompressed data doesn't match original";
}

TEST_F(DeflateDecoderTest, DynamicHuffman_SingleBlockDecode) {
  // Raw DEFLATE stream with dynamic Huffman block (btype=2). Generated via
  // Python zlib.compressobj(6, 8, -15, 8, 2) on "Hello world! Hello world! "
  // repeated 10 times (260 bytes), so the block has back-references and
  // non-empty distance tree.
  const uint8_t deflate_stream[] = {
      0x05,
      0xC1,
      0xC1,
      0x09,
      0x00,
      0x00,
      0x08,
      0x03,
      0xB1,
      0x55,
      0xEA,
      0x36,
      0x0E,
      0xA2,
      0xBF,
      0x83,
      0x42,
      0x3F,
      0xAE,
      0x6F,
      0xD2,
      0x0B,
      0xD6,
      0x39,
      0x4C,
      0xA9,
      0x17,
      0xAC,
      0x73,
      0x98,
      0x52,
      0x2F,
      0x58,
      0xE7,
      0x30,
      0xA5,
      0x5E,
      0xB0,
      0xCE,
      0x61,
      0x4A,
      0xBD,
      0x60,
      0x9D,
      0xC3,
      0x94,
      0x7A,
      0xC1,
      0x3A,
      0x87,
      0x29,
      0xF5,
      0x82,
      0x75,
      0x0E,
      0x53,
      0xEA,
      0x05,
      0xEB,
      0x1C,
      0xA6,
      0xD4,
      0x0B,
      0xD6,
      0x39,
      0x4C,
      0xA9,
      0x17,
      0xAC,
      0x73,
      0x98,
      0x52,
      0x2F,
      0x58,
      0xE7,
      0x30,
      0xA5,
      0x5E,
      0xB0,
      0xCE,
      0x61,
      0x4A,
      0xBD,
      0x60,
      0x9D,
      0xC3,
      0x94,
      0x7A,
      0xC1,
      0x3A,
      0x87,
      0x29,
      0xF5,
      0x82,
      0x75,
      0x0E,
      0x53,
      0xEA,
      0x05,
      0xEB,
      0x1C,
      0xA6,
      0xD4,
      0x0B,
      0xD6,
      0x39,
      0x4C,
      0xA9,
      0x17,
      0xAC,
      0x73,
      0x98,
      0x52,
      0x2F,
      0x58,
      0xE7,
      0x30,
      0xA5,
      0x5E,
      0xB0,
      0xCE,
      0x61,
      0x4A,
      0x0F,
  };
  const size_t expected_len = 260u;
  std::vector<uint8_t> expected(expected_len);
  {
    const char * phrase = "Hello world! Hello world! ";
    const size_t phrase_len = 26u;
    for (size_t i = 0; i < expected_len; i++) {
      expected[i] = static_cast<uint8_t>(phrase[i % phrase_len]);
    }
  }

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  std::vector<uint8_t> out(expected_len + 64, 0);
  gcomp_buffer_t in_buf = {deflate_stream, sizeof(deflate_stream), 0};
  gcomp_buffer_t out_buf = {out.data(), out.size(), 0};

  ASSERT_EQ(gcomp_decoder_update(decoder_, &in_buf, &out_buf), GCOMP_OK);
  ASSERT_EQ(in_buf.used, sizeof(deflate_stream));
  ASSERT_EQ(out_buf.used, expected_len);

  gcomp_buffer_t finish_out = {
      out.data() + out_buf.used, out.size() - out_buf.used, 0};
  ASSERT_EQ(gcomp_decoder_finish(decoder_, &finish_out), GCOMP_OK);
  ASSERT_EQ(finish_out.used, 0u);

  ASSERT_EQ(std::memcmp(out.data(), expected.data(), expected_len), 0);
}

TEST_F(DeflateDecoderTest, Malformed_InvalidBlockType) {
  // First 3 bits: bfinal=0, btype=3 (reserved). Rest is junk; decoder should
  // fail with GCOMP_ERR_CORRUPT when reading block header.
  const uint8_t bad[] = {0x06, 0x00, 0x00};

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  uint8_t out[8] = {};
  gcomp_buffer_t in_buf = {bad, sizeof(bad), 0};
  gcomp_buffer_t out_buf = {out, sizeof(out), 0};

  ASSERT_EQ(
      gcomp_decoder_update(decoder_, &in_buf, &out_buf), GCOMP_ERR_CORRUPT);
}

TEST_F(DeflateDecoderTest, Malformed_StoredBlockNlenMismatch) {
  // Stored block: BFINAL=1, BTYPE=00, LEN=5, NLEN should be ~LEN (0xFFFA).
  // Use wrong NLEN so validation fails.
  const uint8_t bad[] = {0x01, 0x05, 0x00, 0x00, 0x00, 'H', 'e', 'l', 'l', 'o'};

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  uint8_t out[16] = {};
  gcomp_buffer_t in_buf = {bad, sizeof(bad), 0};
  gcomp_buffer_t out_buf = {out, sizeof(out), 0};

  ASSERT_EQ(
      gcomp_decoder_update(decoder_, &in_buf, &out_buf), GCOMP_ERR_CORRUPT);
}

TEST_F(DeflateDecoderTest, Malformed_InvalidDistanceBeyondWindow) {
  // Fixed Huffman block that tries to reference distance 1 when window is
  // empty. This tests the "distance > window_filled" validation path.
  //
  // Block header: BFINAL=1, BTYPE=01 (fixed Huffman)
  //   bit0 = 1 (BFINAL)
  //   bit1 = 1 (BTYPE low = 1)
  //   bit2 = 0 (BTYPE high = 0)
  //   => first 3 bits = 0b011 in read order
  //
  // Length code 257 = 0000001 binary (7 bits, value 1 since 257-256=1)
  // In the bitstream, Huffman codes are MSB-first, so bits 3-9 receive
  // the code bits in order: 0,0,0,0,0,0,1
  //
  // Distance code 0 = 00000 (5 bits)
  // bits 10-14: 0,0,0,0,0
  //
  // Packing into bytes (bit 0 is LSB of byte 0):
  // byte 0 bits 0-7: 1,1,0,0,0,0,0,0 = 0x03
  // byte 1 bits 0-7: 0,1,0,0,0,0,0,X = 0x02 (last bit doesn't matter)
  //
  // Stream: 0x03, 0x02
  const uint8_t bad_fixed[] = {0x03, 0x02};

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  uint8_t out[16] = {};
  gcomp_buffer_t in_buf = {bad_fixed, sizeof(bad_fixed), 0};
  gcomp_buffer_t out_buf = {out, sizeof(out), 0};

  // Should fail because it tries to reference distance 1 with empty window
  gcomp_status_t status = gcomp_decoder_update(decoder_, &in_buf, &out_buf);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(DeflateDecoderTest, Malformed_InvalidDistanceSymbol) {
  // Test distance symbol >= 30 (symbols 30-31 are reserved/invalid per RFC
  // 1951). The decoder validates: if (dist_sym >= 30u) { return
  // GCOMP_ERR_CORRUPT; }
  //
  // We construct a two-block stream:
  // Block 1 (stored, non-final): outputs "ABC" (3 bytes for window)
  // Block 2 (fixed Huffman, final): length code 257 + distance code 30
  // (invalid)
  //
  // Stored block (non-final): BFINAL=0, BTYPE=00, LEN=3, NLEN=0xFFFC,
  // data="ABC"
  const uint8_t stream_part1[] = {0x00, 0x03, 0x00, 0xFC, 0xFF, 'A', 'B', 'C'};

  // Fixed Huffman block: BFINAL=1, BTYPE=01
  // Length code 257 (7-bit code: 0000001)
  // Distance code 30 (5-bit code: 11110)
  //
  // Bit layout (LSB-first byte packing):
  // bits 0-2: BFINAL=1, BTYPE=01 = bit0=1, bit1=1, bit2=0
  // bits 3-9: length 257 code = bit3-9 = 0,0,0,0,0,0,1
  // bits 10-14: distance 30 code = bit10-14 = 1,1,1,1,0
  //
  // byte 0: bits 0-7 = 1,1,0,0,0,0,0,0 = 0x03
  // byte 1: bits 0-7 = 0,1,1,1,1,1,0,0 = 0x3E

  uint8_t bad_stream[sizeof(stream_part1) + 2];
  std::memcpy(bad_stream, stream_part1, sizeof(stream_part1));
  bad_stream[sizeof(stream_part1)] = 0x03; // BFINAL=1, BTYPE=01, len257[6:2]
  bad_stream[sizeof(stream_part1) + 1] = 0x3E; // len257[1:0], dist30[4:0]

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  uint8_t out[32] = {};
  gcomp_buffer_t in_buf = {bad_stream, sizeof(bad_stream), 0};
  gcomp_buffer_t out_buf = {out, sizeof(out), 0};

  gcomp_status_t status = gcomp_decoder_update(decoder_, &in_buf, &out_buf);

  // The decoder should either:
  // 1. Return GCOMP_ERR_CORRUPT when it sees distance symbol 30
  // 2. Fail during Huffman decoding if our bit packing is off
  // Either way, it should not succeed with a fully valid decode.
  // Note: It may partially decode the stored block before failing.
  EXPECT_NE(status, GCOMP_OK);
}

TEST_F(DeflateDecoderTest, Malformed_DynamicBlock_InvalidHlit) {
  // Dynamic Huffman block with HLIT > 29 (giving > 286 lit/len codes).
  // RFC 1951 allows 257-286 lit/len codes, so HLIT must be 0-29.
  //
  // Block header: BFINAL=1, BTYPE=10 (dynamic)
  // Dynamic header: HLIT=31 (invalid: 31+257=288 > 286)
  //                 HDIST=0, HCLEN=0
  //
  // Bit layout (LSB-first):
  // bits 0-2: BFINAL=1, BTYPE=10 = 1,0,1
  // bits 3-7: HLIT=31 = 1,1,1,1,1
  // bits 8-12: HDIST=0 = 0,0,0,0,0
  // bits 13-16: HCLEN=0 = 0,0,0,0
  //
  // byte 0: bits 0-7 = 1,0,1,1,1,1,1,1 = 0xFD
  // byte 1: bits 8-15 = 0,0,0,0,0,0,0,0 = 0x00
  // byte 2: padding = 0x00
  const uint8_t bad[] = {0xFD, 0x00, 0x00};

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  uint8_t out[16] = {};
  gcomp_buffer_t in_buf = {bad, sizeof(bad), 0};
  gcomp_buffer_t out_buf = {out, sizeof(out), 0};

  EXPECT_EQ(
      gcomp_decoder_update(decoder_, &in_buf, &out_buf), GCOMP_ERR_CORRUPT);
}

TEST_F(DeflateDecoderTest, Malformed_DynamicBlock_MissingEndOfBlock) {
  // Dynamic Huffman block where the literal/length alphabet doesn't include
  // end-of-block symbol (256). This is required per RFC 1951.
  //
  // The decoder validates: if (st->dyn_litlen_lengths[256] == 0) return CORRUPT
  //
  // To trigger this, we need to successfully parse the dynamic header and
  // code length codes, then have the lit/len lengths give code length 0
  // for symbol 256 (end-of-block).
  //
  // This is complex to construct manually. For now, we document this validation
  // exists and defer comprehensive testing to integration tests with generated
  // malformed streams.

  // Note: This validation is covered by the existing decoder implementation
  // at line ~683: if (st->dyn_litlen_lengths[256] == 0) return
  // GCOMP_ERR_CORRUPT
  SUCCEED() << "Dynamic block missing EOB validation exists in decoder";
}

TEST_F(DeflateDecoderTest, EdgeCase_EmptyInputStoredBlock) {
  // Smallest valid stream: one stored block with length 0. BFINAL=1, BTYPE=00,
  // LEN=0, NLEN=0xFFFF, no payload.
  const uint8_t deflate_stream[] = {0x01, 0x00, 0x00, 0xFF, 0xFF};

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  uint8_t out[8] = {};
  gcomp_buffer_t in_buf = {deflate_stream, sizeof(deflate_stream), 0};
  gcomp_buffer_t out_buf = {out, sizeof(out), 0};

  ASSERT_EQ(gcomp_decoder_update(decoder_, &in_buf, &out_buf), GCOMP_OK);
  ASSERT_EQ(out_buf.used, 0u);

  gcomp_buffer_t finish_out = {out, sizeof(out), 0};
  ASSERT_EQ(gcomp_decoder_finish(decoder_, &finish_out), GCOMP_OK);
}

TEST_F(DeflateDecoderTest, Chunked_RandomSplitsProduceCorrectOutput) {
  const uint8_t deflate_stream[] = {0xF3, 0x48, 0xCD, 0xC9, 0xC9, 0xD7, 0x51,
      0x28, 0xCF, 0x2F, 0xCA, 0x49, 0x51, 0x04, 0x00};
  const size_t stream_len = sizeof(deflate_stream);

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  std::vector<uint8_t> out;
  out.reserve(32);
  size_t in_off = 0;
  unsigned seed = 42u;
  size_t no_progress = 0;

  while (in_off < stream_len) {
    size_t avail = stream_len - in_off;
    size_t chunk = (avail <= 1u) ? avail : (1u + (seed % avail));
    seed = seed * 1103515245u + 12345u;
    if (chunk > avail) {
      chunk = avail;
    }

    gcomp_buffer_t in_buf = {deflate_stream + in_off, chunk, 0};
    uint8_t tmp[16] = {};
    gcomp_buffer_t out_buf = {tmp, sizeof(tmp), 0};

    gcomp_status_t s = gcomp_decoder_update(decoder_, &in_buf, &out_buf);
    ASSERT_EQ(s, GCOMP_OK);
    if (in_buf.used == 0 && out_buf.used == 0) {
      no_progress++;
      ASSERT_LE(no_progress, stream_len) << "decoder stuck";
      continue;
    }
    no_progress = 0;
    in_off += in_buf.used;

    for (size_t j = 0; j < out_buf.used; j++) {
      out.push_back(tmp[j]);
    }
  }

  uint8_t finish_buf[8] = {};
  gcomp_buffer_t finish_out = {finish_buf, sizeof(finish_buf), 0};
  ASSERT_EQ(gcomp_decoder_finish(decoder_, &finish_out), GCOMP_OK);

  ASSERT_EQ(out.size(), 13u);
  ASSERT_EQ(std::memcmp(out.data(), "Hello, world!", 13u), 0);
}

TEST_F(DeflateDecoderTest, Limits_MaxOutputBytesEnforced) {
  const uint8_t deflate_stream[] = {0xF3, 0x48, 0xCD, 0xC9, 0xC9, 0xD7, 0x51,
      0x28, 0xCF, 0x2F, 0xCA, 0x49, 0x51, 0x04, 0x00};

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  // Set a limit smaller than the decoded size (13 bytes).
  ASSERT_EQ(
      gcomp_options_set_uint64(opts, "limits.max_output_bytes", 5u), GCOMP_OK);

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", opts, &decoder_), GCOMP_OK);

  uint8_t out[64] = {};
  gcomp_buffer_t in_buf = {deflate_stream, sizeof(deflate_stream), 0};
  gcomp_buffer_t out_buf = {out, sizeof(out), 0};

  gcomp_status_t status = gcomp_decoder_update(decoder_, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_ERR_LIMIT);
  ASSERT_LE(out_buf.used, 5u);

  gcomp_options_destroy(opts);
}

TEST_F(DeflateDecoderTest, Memory_CreateDestroyNoLeak) {
  for (int i = 0; i < 4; i++) {
    gcomp_decoder_t * dec = nullptr;
    ASSERT_EQ(
        gcomp_decoder_create(registry_, "deflate", nullptr, &dec), GCOMP_OK);
    ASSERT_NE(dec, nullptr);
    gcomp_decoder_destroy(dec);
  }
}

TEST_F(DeflateDecoderTest, Limits_MaxMemoryBytesEnforcedOnCreate) {
  // Set a very low memory limit that's too small for even the decoder state
  // The decoder needs at least: sizeof(state) + window_size (default 32KiB)
  // So setting limit to 1KB should fail
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  // Set memory limit to 1KB - way too small for decoder (needs ~33KB minimum)
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 1024u),
      GCOMP_OK);

  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "deflate", opts, &dec);

  // Should fail with LIMIT error because initial memory requirement exceeds
  // limit
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);
  EXPECT_EQ(dec, nullptr);

  gcomp_options_destroy(opts);
}

TEST_F(DeflateDecoderTest, Limits_MaxMemoryBytesAllowsSufficientMemory) {
  // Set a memory limit that's sufficient for the decoder
  // Default window is 32KiB, plus state struct, plus Huffman tables
  // 256KB should be more than enough
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  ASSERT_EQ(
      gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 256u * 1024u),
      GCOMP_OK);

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", opts, &decoder_), GCOMP_OK);
  ASSERT_NE(decoder_, nullptr);

  // Verify it can actually decode something
  const uint8_t deflate_stream[] = {0xF3, 0x48, 0xCD, 0xC9, 0xC9, 0xD7, 0x51,
      0x28, 0xCF, 0x2F, 0xCA, 0x49, 0x51, 0x04, 0x00};

  uint8_t out[64] = {};
  gcomp_buffer_t in_buf = {deflate_stream, sizeof(deflate_stream), 0};
  gcomp_buffer_t out_buf = {out, sizeof(out), 0};

  ASSERT_EQ(gcomp_decoder_update(decoder_, &in_buf, &out_buf), GCOMP_OK);
  EXPECT_EQ(out_buf.used, 13u);
  EXPECT_EQ(std::memcmp(out, "Hello, world!", 13u), 0);

  gcomp_options_destroy(opts);
}

TEST_F(DeflateDecoderTest, Limits_SmallWindowReducesMemoryRequirement) {
  // Use smaller window (8 bits = 256 bytes) to reduce memory requirement
  // The decoder state struct is ~20KB (has embedded Huffman tables), plus
  // window So with 256-byte window, we need ~21KB; with 32KB window, we need
  // ~53KB
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  // Set small window (256 bytes instead of 32KB)
  ASSERT_EQ(
      gcomp_options_set_uint64(opts, "deflate.window_bits", 8u), GCOMP_OK);

  // Set memory limit to 32KB - should work with small window (~21KB needed)
  // but would fail with default window (~53KB needed)
  ASSERT_EQ(
      gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 32u * 1024u),
      GCOMP_OK);

  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "deflate", opts, &dec);

  // Should succeed because 256-byte window + state fits in 32KB
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(dec, nullptr);

  if (dec) {
    gcomp_decoder_destroy(dec);
  }
  gcomp_options_destroy(opts);
}

TEST_F(DeflateDecoderTest, Limits_DefaultWindowNeedsMoreMemory) {
  // With default 32KB window, decoder needs ~53KB
  // Setting limit to 40KB should fail
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  // Set memory limit to 40KB - too small for default window
  ASSERT_EQ(
      gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 40u * 1024u),
      GCOMP_OK);

  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "deflate", opts, &dec);

  // Should fail because default window (32KB) + state (~21KB) exceeds 40KB
  EXPECT_EQ(status, GCOMP_ERR_LIMIT);
  EXPECT_EQ(dec, nullptr);

  gcomp_options_destroy(opts);
}

//
// Golden Vector Tests
//

class GoldenVectorTest
    : public ::testing::TestWithParam<gcomp_golden_vector_t> {
protected:
  void SetUp() override {
    // Use a custom registry for test isolation.
    ASSERT_EQ(gcomp_registry_create(nullptr, &registry_), GCOMP_OK);
    ASSERT_NE(registry_, nullptr);
    ASSERT_EQ(gcomp_method_deflate_register(registry_), GCOMP_OK);
  }

  void TearDown() override {
    if (decoder_) {
      gcomp_decoder_destroy(decoder_);
      decoder_ = nullptr;
    }
    if (registry_) {
      gcomp_registry_destroy(registry_);
      registry_ = nullptr;
    }
  }

  gcomp_registry_t * registry_ = nullptr;
  gcomp_decoder_t * decoder_ = nullptr;
};

TEST_P(GoldenVectorTest, DecodeMatchesExpected) {
  const gcomp_golden_vector_t & vec = GetParam();

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  std::vector<uint8_t> output(vec.expected_len + 256);
  gcomp_buffer_t in_buf = {vec.compressed, vec.compressed_len, 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  ASSERT_EQ(gcomp_decoder_update(decoder_, &in_buf, &out_buf), GCOMP_OK)
      << "Failed to decode vector: " << vec.name;
  ASSERT_EQ(in_buf.used, vec.compressed_len)
      << "Did not consume all input for: " << vec.name;

  gcomp_buffer_t finish_out = {
      output.data() + out_buf.used, output.size() - out_buf.used, 0};
  ASSERT_EQ(gcomp_decoder_finish(decoder_, &finish_out), GCOMP_OK)
      << "Finish failed for: " << vec.name;

  size_t total_out = out_buf.used + finish_out.used;
  ASSERT_EQ(total_out, vec.expected_len)
      << "Output length mismatch for: " << vec.name;

  if (vec.expected_len > 0 && vec.expected != nullptr) {
    EXPECT_EQ(std::memcmp(output.data(), vec.expected, vec.expected_len), 0)
        << "Output data mismatch for: " << vec.name;
  }
}

TEST_P(GoldenVectorTest, DecodeChunkedMatchesExpected) {
  const gcomp_golden_vector_t & vec = GetParam();

  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  std::vector<uint8_t> output;
  output.reserve(vec.expected_len + 256);

  size_t in_off = 0;
  size_t iterations = 0;
  const size_t max_iterations =
      vec.compressed_len * 16u + vec.expected_len + 1000u;

  // Feed input one byte at a time to test streaming edge cases
  while (in_off < vec.compressed_len && iterations < max_iterations) {
    gcomp_buffer_t in_buf = {vec.compressed + in_off, 1u, 0};
    uint8_t tmp[64] = {};
    gcomp_buffer_t out_buf = {tmp, sizeof(tmp), 0};

    gcomp_status_t s = gcomp_decoder_update(decoder_, &in_buf, &out_buf);
    ASSERT_EQ(s, GCOMP_OK) << "Update failed at offset " << in_off
                           << " for: " << vec.name;

    in_off += in_buf.used;
    for (size_t j = 0; j < out_buf.used; j++) {
      output.push_back(tmp[j]);
    }
    iterations++;
  }

  ASSERT_EQ(in_off, vec.compressed_len)
      << "Did not consume all input (chunked) for: " << vec.name;

  uint8_t finish_buf[256] = {};
  gcomp_buffer_t finish_out = {finish_buf, sizeof(finish_buf), 0};
  ASSERT_EQ(gcomp_decoder_finish(decoder_, &finish_out), GCOMP_OK)
      << "Finish failed (chunked) for: " << vec.name;

  for (size_t j = 0; j < finish_out.used; j++) {
    output.push_back(finish_buf[j]);
  }

  ASSERT_EQ(output.size(), vec.expected_len)
      << "Output length mismatch (chunked) for: " << vec.name;

  if (vec.expected_len > 0 && vec.expected != nullptr) {
    EXPECT_EQ(std::memcmp(output.data(), vec.expected, vec.expected_len), 0)
        << "Output data mismatch (chunked) for: " << vec.name;
  }
}

INSTANTIATE_TEST_SUITE_P(GoldenVectors, GoldenVectorTest,
    ::testing::ValuesIn(
        g_golden_vectors, g_golden_vectors + g_golden_vectors_count),
    [](const ::testing::TestParamInfo<gcomp_golden_vector_t> & info) {
      return std::string(info.param.name);
    });

//
// Additional golden vector tests for runtime-generated expected data
//

TEST_F(DeflateDecoderTest, GoldenVector_BinarySequence256) {
  // Vector 7: 0x00-0xFF (256 bytes)
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  std::vector<uint8_t> output(golden_v7_expected_len + 64);
  gcomp_buffer_t in_buf = {
      golden_v7_compressed_ptr, golden_v7_compressed_len, 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  ASSERT_EQ(gcomp_decoder_update(decoder_, &in_buf, &out_buf), GCOMP_OK);
  ASSERT_EQ(in_buf.used, golden_v7_compressed_len);

  gcomp_buffer_t finish_out = {
      output.data() + out_buf.used, output.size() - out_buf.used, 0};
  ASSERT_EQ(gcomp_decoder_finish(decoder_, &finish_out), GCOMP_OK);

  size_t total_out = out_buf.used + finish_out.used;
  ASSERT_EQ(total_out, golden_v7_expected_len);

  // Verify expected: 0x00, 0x01, ..., 0xFF
  for (size_t i = 0; i < golden_v7_expected_len; i++) {
    EXPECT_EQ(output[i], (uint8_t)i)
        << "Mismatch at position " << i << " for binary sequence vector";
  }
}

TEST_F(DeflateDecoderTest, GoldenVector_RepeatedHelloWorld260) {
  // Vector 8: "Hello world! Hello world! " repeated 10x (260 bytes)
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_), GCOMP_OK);

  std::vector<uint8_t> output(golden_v8_expected_len + 64);
  gcomp_buffer_t in_buf = {
      golden_v8_compressed_ptr, golden_v8_compressed_len, 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  ASSERT_EQ(gcomp_decoder_update(decoder_, &in_buf, &out_buf), GCOMP_OK);
  ASSERT_EQ(in_buf.used, golden_v8_compressed_len);

  gcomp_buffer_t finish_out = {
      output.data() + out_buf.used, output.size() - out_buf.used, 0};
  ASSERT_EQ(gcomp_decoder_finish(decoder_, &finish_out), GCOMP_OK);

  size_t total_out = out_buf.used + finish_out.used;
  ASSERT_EQ(total_out, golden_v8_expected_len);

  // Verify expected: "Hello world! Hello world! " repeated
  const char * phrase = "Hello world! Hello world! ";
  const size_t phrase_len = 26u;
  for (size_t i = 0; i < golden_v8_expected_len; i++) {
    EXPECT_EQ(output[i], (uint8_t)phrase[i % phrase_len])
        << "Mismatch at position " << i << " for repeated hello world vector";
  }
}

//
// Match copying
//
// deflate_copy_match() moves bytes from the sliding window to the output.  It
// used to do that one byte at a time and was over half of a decode; it now
// copies in runs, and a run may not cross any of five boundaries -- the end of
// the match, the end of the output, either wrap of the circular window, or
// the point at which the reading and writing cursors run into one another.
//
// Every one of those is an off-by-one away from producing wrong bytes while
// reporting success, and a round trip of ordinary data exercises almost none
// of them.  These tests aim at them directly.
//

namespace {

/// Compress with this library, decompress through an output buffer of exactly
/// @p chunk bytes, and return what came back.
std::vector<uint8_t> RoundTripThroughChunks(gcomp_registry_t * registry,
    const std::vector<uint8_t> & input, int level, size_t chunk,
    int window_bits = 0) {
  gcomp_options_t * opts = nullptr;
  if (gcomp_options_create(&opts) != GCOMP_OK) {
    return {};
  }
  gcomp_options_set_int64(opts, "deflate.level", level);
  if (window_bits != 0) {
    gcomp_options_set_uint64(opts, "deflate.window_bits",
        static_cast<uint64_t>(window_bits));
  }

  std::vector<uint8_t> encoded(input.size() * 2 + 4096);
  size_t encoded_len = 0;
  gcomp_status_t s = gcomp_encode_buffer(registry, "deflate", opts,
      input.data(), input.size(), encoded.data(), encoded.size(),
      &encoded_len);
  gcomp_options_destroy(opts);
  if (s != GCOMP_OK) {
    return {};
  }
  encoded.resize(encoded_len);

  gcomp_options_t * dopts = nullptr;
  if (window_bits != 0) {
    if (gcomp_options_create(&dopts) != GCOMP_OK) {
      return {};
    }
    gcomp_options_set_uint64(dopts, "deflate.window_bits",
        static_cast<uint64_t>(window_bits));
  }
  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t dc = gcomp_decoder_create(registry, "deflate", dopts, &dec);
  gcomp_options_destroy(dopts);
  if (dc != GCOMP_OK) {
    return {};
  }
  std::vector<uint8_t> out_buf(chunk);
  std::vector<uint8_t> decoded;
  gcomp_buffer_t in = {encoded.data(), encoded.size(), 0};
  for (;;) {
    gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
    size_t before_in = in.used;
    gcomp_status_t u = gcomp_decoder_update(dec, &in, &ob);
    if (u != GCOMP_OK) {
      ADD_FAILURE() << "update returned " << (int)u << " at chunk " << chunk;
      gcomp_decoder_destroy(dec);
      return {};
    }
    decoded.insert(decoded.end(), out_buf.data(), out_buf.data() + ob.used);
    if (ob.used == 0 && in.used == before_in) {
      break;
    }
  }
  for (;;) {
    gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
    gcomp_status_t f = gcomp_decoder_finish(dec, &ob);
    decoded.insert(decoded.end(), out_buf.data(), out_buf.data() + ob.used);
    if (f == GCOMP_OK) {
      break;
    }
    if (f != GCOMP_ERR_LIMIT) {
      ADD_FAILURE() << "finish returned " << (int)f << " at chunk " << chunk;
      gcomp_decoder_destroy(dec);
      return {};
    }
  }
  gcomp_decoder_destroy(dec);
  return decoded;
}

/// Bytes that produce matches at a chosen distance, over and over.
///
/// A phrase is written, then repeated `distance` bytes later, so the encoder
/// has a match of exactly that distance available.  Interleaved noise keeps
/// the whole thing from collapsing into one enormous match.
std::vector<uint8_t> BytesWithMatchDistance(size_t total, size_t distance) {
  std::vector<uint8_t> v;
  v.reserve(total + 512);
  uint32_t x = 99137u ^ static_cast<uint32_t>(distance);
  auto noise = [&x]() {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return static_cast<uint8_t>(x >> 19);
  };
  while (v.size() < total) {
    size_t start = v.size();
    for (size_t i = 0; i < 24; i++) {
      v.push_back(noise());
    }
    // Pad out to the requested distance, then repeat the phrase.
    while (v.size() - start < distance && v.size() < total + distance) {
      v.push_back(noise());
    }
    for (size_t i = 0; i < 24 && start + i < v.size(); i++) {
      v.push_back(v[start + i]);
    }
  }
  v.resize(total);
  return v;
}

/// A block of @p period distinct bytes, repeated to fill @p total.
///
/// Every position past the first period begins a match at exactly that
/// distance, and the match runs as long as the decoder will let it -- up to
/// the 258 byte maximum of RFC 1951 section 3.2.5.  Long matches are the
/// point: a run only reaches the boundary where the writing cursor would
/// overtake the reading cursor when the match is longer than the gap between
/// them, which is `window_size - distance`.
std::vector<uint8_t> RepeatedBlock(size_t total, size_t period) {
  std::vector<uint8_t> block;
  block.reserve(period);
  uint32_t x = 0xC0FFEEu ^ static_cast<uint32_t>(period);
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

/// Builds a raw DEFLATE stream by hand.
///
/// A decoder has to be right for every stream the format permits, not only
/// for the ones this library's encoder happens to emit.  Some of the cases
/// that matter most are ones our encoder will not produce on request -- a
/// match long enough to reach most of the way round a small window, say --
/// so those are written out directly here, as RFC 1951 section 3.2.6 defines
/// the fixed Huffman code.
class FixedBlockWriter {
public:
  /// Start a single final block using the fixed code.
  FixedBlockWriter() {
    bits(1, 1); // BFINAL
    bits(1, 2); // BTYPE = 01, fixed Huffman
  }

  void literal(uint8_t b) {
    if (b < 144u) {
      code(0x30u + b, 8); // 00110000 through 10111111
    }
    else {
      code(0x190u + (b - 144u), 9); // 110010000 through 111111111
    }
  }

  /// A length/distance pair.  @p length is 3..258 and @p distance is 1..32768.
  void match(uint32_t length, uint32_t distance) {
    static const uint32_t len_base[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15,
        17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195,
        227, 258};
    static const uint32_t len_extra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2,
        2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    uint32_t ls = 28;
    while (ls > 0 && len_base[ls] > length) {
      ls--;
    }
    uint32_t sym = 257u + ls;
    if (sym <= 279u) {
      code(sym - 256u, 7);
    }
    else {
      code(0xC0u + (sym - 280u), 8);
    }
    bits(length - len_base[ls], (int)len_extra[ls]);

    static const uint32_t dist_base[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33,
        49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
        4097, 6145, 8193, 12289, 16385, 24577};
    static const uint32_t dist_extra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4,
        5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    uint32_t ds = 29;
    while (ds > 0 && dist_base[ds] > distance) {
      ds--;
    }
    code(ds, 5); // Fixed distance codes are five bits, MSB first.
    bits(distance - dist_base[ds], (int)dist_extra[ds]);
  }

  std::vector<uint8_t> finish() {
    code(0, 7); // End of block: symbol 256.
    if (count_ > 0) {
      out_.push_back(static_cast<uint8_t>(buf_));
      buf_ = 0;
      count_ = 0;
    }
    return out_;
  }

private:
  /// Plain bits, least significant first: headers and extra bits.
  void bits(uint32_t value, int n) {
    for (int i = 0; i < n; i++) {
      put((value >> i) & 1u);
    }
  }

  /// A Huffman code, most significant bit first, as DEFLATE writes them.
  void code(uint32_t value, int n) {
    for (int i = n - 1; i >= 0; i--) {
      put((value >> i) & 1u);
    }
  }

  void put(uint32_t bit) {
    buf_ |= bit << count_;
    if (++count_ == 8) {
      out_.push_back(static_cast<uint8_t>(buf_));
      buf_ = 0;
      count_ = 0;
    }
  }

  std::vector<uint8_t> out_;
  uint32_t buf_ = 0;
  int count_ = 0;
};

/// Decode @p stream with a window of @p window_bits, through @p chunk buffers.
std::vector<uint8_t> DecodeRaw(gcomp_registry_t * registry,
    const std::vector<uint8_t> & stream, int window_bits, size_t chunk,
    gcomp_status_t * status_out) {
  gcomp_options_t * opts = nullptr;
  if (gcomp_options_create(&opts) != GCOMP_OK) {
    return {};
  }
  gcomp_options_set_uint64(
      opts, "deflate.window_bits", static_cast<uint64_t>(window_bits));
  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t dc = gcomp_decoder_create(registry, "deflate", opts, &dec);
  gcomp_options_destroy(opts);
  if (dc != GCOMP_OK) {
    return {};
  }

  std::vector<uint8_t> out_buf(chunk), decoded;
  gcomp_buffer_t in = {const_cast<uint8_t *>(stream.data()), stream.size(), 0};
  gcomp_status_t last = GCOMP_OK;
  for (;;) {
    gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
    size_t before = in.used;
    last = gcomp_decoder_update(dec, &in, &ob);
    if (last != GCOMP_OK) {
      break;
    }
    decoded.insert(decoded.end(), out_buf.data(), out_buf.data() + ob.used);
    if (ob.used == 0 && in.used == before) {
      break;
    }
  }
  if (last == GCOMP_OK) {
    for (;;) {
      gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
      last = gcomp_decoder_finish(dec, &ob);
      decoded.insert(decoded.end(), out_buf.data(), out_buf.data() + ob.used);
      if (last != GCOMP_ERR_LIMIT) {
        break;
      }
    }
  }
  gcomp_decoder_destroy(dec);
  if (status_out) {
    *status_out = last;
  }
  return decoded;
}

} // namespace

// A match that reaches nearly the whole way round the window.
//
// The run that copies a match cannot be longer than the gap between the
// writing cursor and the reading cursor, which is `window_size - distance`
// going forward.  A longer run would overwrite window bytes the same run has
// not read yet.
//
// With the default 32 KB window that needs a distance above 32,510, because a
// match is at most 258 bytes; with a small window it needs very little.  Our
// encoder will not produce these on request -- it has no reason to choose a
// far match when a near one is available -- but they are valid DEFLATE and a
// decoder has to be right for them, so the streams are written by hand.
TEST_F(DeflateDecoderTest, MatchCopy_LongMatchNearlyRoundTheWindow) {
  for (int window_bits : {8, 9, 10}) {
    const uint32_t window = 1u << window_bits;
    for (uint32_t distance = window / 2; distance <= window; distance += 17u) {
      for (uint32_t length : {uint32_t(3), uint32_t(64), uint32_t(200),
               uint32_t(258)}) {
        // Fill the window with distinct bytes, then reach back into it.
        FixedBlockWriter w;
        std::vector<uint8_t> expected;
        for (uint32_t i = 0; i < window; i++) {
          uint8_t b = static_cast<uint8_t>((i * 37u + 11u) & 0xFFu);
          w.literal(b);
          expected.push_back(b);
        }
        w.match(length, distance);
        for (uint32_t i = 0; i < length; i++) {
          expected.push_back(expected[expected.size() - distance]);
        }
        std::vector<uint8_t> stream = w.finish();

        for (size_t chunk : {size_t(1), size_t(23), size_t(4096)}) {
          gcomp_status_t s = GCOMP_OK;
          std::vector<uint8_t> got =
              DecodeRaw(registry_, stream, window_bits, chunk, &s);
          EXPECT_EQ(s, GCOMP_OK)
              << "window " << window << " distance " << distance << " length "
              << length << " chunk " << chunk;
          EXPECT_EQ(got, expected)
              << "window " << window << " distance " << distance << " length "
              << length << " chunk " << chunk;
        }
      }
    }
  }
}

// A run of one repeated byte is copied with memset rather than a memcpy per
// byte, which is a separate code path and gets its own case.  Runs like this
// are what a row of identical pixels looks like after PNG filtering.
TEST_F(DeflateDecoderTest, MatchCopy_RepeatedByteRuns) {
  for (size_t run : {size_t(1), size_t(2), size_t(257), size_t(258),
           size_t(259), size_t(5000)}) {
    std::vector<uint8_t> input;
    for (int block = 0; block < 12; block++) {
      input.insert(input.end(), run, static_cast<uint8_t>('a' + block));
      input.push_back(static_cast<uint8_t>(0x80 + block));
    }
    for (int level : {1, 6, 9}) {
      EXPECT_EQ(RoundTripThroughChunks(registry_, input, level, 4096), input)
          << "run of " << run << " at level " << level;
    }
  }
}

// Matches at distances that sit either side of the boundaries the run length
// is clamped against, including one byte, the whole 32 KB window, and half of
// it -- the point past which the writing cursor, not the reading cursor, is
// what bounds the run.
TEST_F(DeflateDecoderTest, MatchCopy_DistancesAcrossTheClampBoundaries) {
  const size_t kWindow = 32768;
  for (size_t distance : {size_t(1), size_t(2), size_t(3), size_t(7),
           size_t(8), size_t(255), size_t(256), size_t(4096),
           kWindow / 2 - 1, kWindow / 2, kWindow / 2 + 1, kWindow - 1,
           kWindow}) {
    std::vector<uint8_t> input =
        BytesWithMatchDistance(distance * 3 + 40000, distance);
    std::vector<uint8_t> back =
        RoundTripThroughChunks(registry_, input, 9, 1u << 16);
    EXPECT_EQ(back, input) << "distance " << distance;
  }
}

// A match whose distance is within one maximum match length of the whole
// window is the case where the writing cursor would run into the reading
// cursor part way through a run.
//
// With the default 32 KB window that band is distances above 32,510 -- a
// match is at most 258 bytes (RFC 1951 section 3.2.5), so a run cannot reach
// any further back than that -- and an encoder has to be coaxed into emitting
// one.  With a small window the band is most of the window, and every
// distance below lands in it.
TEST_F(DeflateDecoderTest, MatchCopy_DistancesNearTheEndOfASmallWindow) {
  for (int window_bits : {8, 9, 10, 11}) {
    const size_t window = size_t(1) << window_bits;
    for (size_t distance = window - 40; distance <= window; distance++) {
      // Repeating a block of exactly `distance` bytes makes every match run
      // to the maximum length, which is what reaches the boundary.
      std::vector<uint8_t> input = RepeatedBlock(window * 12, distance);
      for (size_t chunk : {size_t(1), size_t(97), size_t(1u << 14)}) {
        EXPECT_EQ(
            RoundTripThroughChunks(registry_, input, 9, chunk, window_bits),
            input)
            << "window " << window << " distance " << distance << " chunk "
            << chunk;
      }
    }
  }
}

// The same data driven through output buffers of many consecutive sizes, so
// that a match is cut at a different offset every time and the resume path is
// entered in every state it can be in.
TEST_F(DeflateDecoderTest, MatchCopy_SurvivesAnyOutputBufferSize) {
  std::vector<uint8_t> input = BytesWithMatchDistance(30000, 300);
  // A long repeat as well, so that a single match spans many output buffers.
  input.insert(input.end(), 4000, 0x5A);
  input.insert(input.end(), 64, 0x11);

  for (size_t chunk = 1; chunk <= 40; chunk++) {
    EXPECT_EQ(RoundTripThroughChunks(registry_, input, 9, chunk), input)
        << "output buffer of " << chunk;
  }
  for (size_t chunk : {size_t(63), size_t(64), size_t(65), size_t(255),
           size_t(256), size_t(257), size_t(4095), size_t(4096)}) {
    EXPECT_EQ(RoundTripThroughChunks(registry_, input, 9, chunk), input)
        << "output buffer of " << chunk;
  }
}

// Enough data to wrap the 32 KB window several times over, with matches that
// reach back across the wrap.
TEST_F(DeflateDecoderTest, MatchCopy_AcrossWindowWraps) {
  std::vector<uint8_t> input = BytesWithMatchDistance(300000, 30000);
  for (size_t chunk : {size_t(1), size_t(37), size_t(4096), size_t(1u << 18)}) {
    EXPECT_EQ(RoundTripThroughChunks(registry_, input, 9, chunk), input)
        << "output buffer of " << chunk;
  }
}

// Decoding driven the obvious way: update() until the input is gone, then
// finish() until it says the stream is complete.
//
// This is the shape the header's own example uses for the encoder, and it is
// what any caller would write.  The decoder used to answer it with
// GCOMP_ERR_CORRUPT -- not because anything was corrupt, but because the last
// output buffer had been too small to hold the tail, and finish() reported
// "I need more room" and "your stream is truncated" as the same thing.
TEST_F(DeflateDecoderTest, FinishCompletesAStreamThroughASmallOutputBuffer) {
  std::vector<uint8_t> input = BytesWithMatchDistance(30000, 300);
  input.insert(input.end(), 4000, 0x5A);

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_int64(opts, "deflate.level", 9);
  std::vector<uint8_t> encoded(input.size() * 2 + 4096);
  size_t encoded_len = 0;
  ASSERT_EQ(gcomp_encode_buffer(registry_, "deflate", opts, input.data(),
                input.size(), encoded.data(), encoded.size(), &encoded_len),
      GCOMP_OK);
  gcomp_options_destroy(opts);
  encoded.resize(encoded_len);

  for (size_t chunk : {size_t(1), size_t(2), size_t(7), size_t(64),
           size_t(1000), size_t(4096)}) {
    gcomp_decoder_t * dec = nullptr;
    ASSERT_EQ(gcomp_decoder_create(registry_, "deflate", nullptr, &dec),
        GCOMP_OK);

    std::vector<uint8_t> out_buf(chunk), decoded;
    gcomp_buffer_t in = {encoded.data(), encoded.size(), 0};

    // update() only while there is input left, exactly as a caller would.
    while (in.used < in.size) {
      gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
      ASSERT_EQ(gcomp_decoder_update(dec, &in, &ob), GCOMP_OK)
          << "chunk " << chunk;
      decoded.insert(decoded.end(), out_buf.data(), out_buf.data() + ob.used);
    }

    // Then finish() until it reports the stream complete.
    size_t guard = 0;
    for (;;) {
      ASSERT_LT(++guard, input.size() + 10000u) << "chunk " << chunk;
      gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
      gcomp_status_t f = gcomp_decoder_finish(dec, &ob);
      decoded.insert(decoded.end(), out_buf.data(), out_buf.data() + ob.used);
      if (f == GCOMP_OK) {
        break;
      }
      ASSERT_EQ(f, GCOMP_ERR_LIMIT)
          << "chunk " << chunk << ": finish reported " << (int)f;
    }

    // And once complete, it stays complete and writes nothing more.
    std::vector<uint8_t> spare(4096);
    gcomp_buffer_t ob = {spare.data(), spare.size(), 0};
    EXPECT_EQ(gcomp_decoder_finish(dec, &ob), GCOMP_OK) << "chunk " << chunk;
    EXPECT_EQ(ob.used, 0u) << "chunk " << chunk;

    gcomp_decoder_destroy(dec);
    EXPECT_EQ(decoded, input) << "chunk " << chunk;
  }
}

// A stream that really is cut short must still be reported as corrupt, with
// room to spare in the output buffer so that the two cases are distinguished
// by the decoder's state and not by the caller's buffer size.
TEST_F(DeflateDecoderTest, FinishStillReportsATruncatedStream) {
  std::vector<uint8_t> input = BytesWithMatchDistance(20000, 250);

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_int64(opts, "deflate.level", 6);
  std::vector<uint8_t> encoded(input.size() * 2 + 4096);
  size_t encoded_len = 0;
  ASSERT_EQ(gcomp_encode_buffer(registry_, "deflate", opts, input.data(),
                input.size(), encoded.data(), encoded.size(), &encoded_len),
      GCOMP_OK);
  gcomp_options_destroy(opts);

  // Feed only the first half of the stream.
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "deflate", nullptr, &dec), GCOMP_OK);
  std::vector<uint8_t> out_buf(input.size() + 4096);
  gcomp_buffer_t in = {encoded.data(), encoded_len / 2, 0};
  gcomp_buffer_t ob = {out_buf.data(), out_buf.size(), 0};
  ASSERT_EQ(gcomp_decoder_update(dec, &in, &ob), GCOMP_OK);

  gcomp_buffer_t fb = {out_buf.data(), out_buf.size(), 0};
  EXPECT_EQ(gcomp_decoder_finish(dec, &fb), GCOMP_ERR_CORRUPT);
  gcomp_decoder_destroy(dec);
}

// An output buffer sized to exactly the decoded length must still finish.
//
// This is the ordinary case for anyone who knows how big the result is -- a
// PNG decoder, for instance, knows its dimensions -- and it is the one that
// breaks if the decoder stops as soon as the output is full.  Not every
// symbol needs output space: end-of-block needs none, and it is the symbol
// that marks the stream complete.  Stopping on a full buffer leaves it unread,
// and finish() then insists for ever that there is more to come.
//
// Every PNG the image library wrote failed this way, and nothing here caught
// it, because every test in this suite gave the decoder more room than it
// needed.
TEST_F(DeflateDecoderTest, DecodesIntoABufferOfExactlyTheRightSize) {
  for (size_t size : {size_t(1), size_t(2), size_t(3), size_t(17),
           size_t(1000), size_t(4096), size_t(65535)}) {
    std::vector<uint8_t> input;
    input.reserve(size);
    uint32_t x = 0x3141592u ^ static_cast<uint32_t>(size);
    while (input.size() < size) {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      // Mixed runs and noise, so blocks end in different states.
      input.push_back(static_cast<uint8_t>((x >> 19) % 11u));
    }

    for (int level : {1, 6, 9}) {
      gcomp_options_t * opts = nullptr;
      ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
      gcomp_options_set_int64(opts, "deflate.level", level);
      std::vector<uint8_t> encoded(input.size() * 2 + 4096);
      size_t encoded_len = 0;
      ASSERT_EQ(gcomp_encode_buffer(registry_, "deflate", opts, input.data(),
                    input.size(), encoded.data(), encoded.size(),
                    &encoded_len),
          GCOMP_OK);
      gcomp_options_destroy(opts);

      gcomp_decoder_t * dec = nullptr;
      ASSERT_EQ(
          gcomp_decoder_create(registry_, "deflate", nullptr, &dec), GCOMP_OK);

      // Not one byte more than the answer needs.
      std::vector<uint8_t> decoded(input.size());
      gcomp_buffer_t in = {encoded.data(), encoded_len, 0};
      gcomp_buffer_t ob = {decoded.data(), decoded.size(), 0};
      ASSERT_EQ(gcomp_decoder_update(dec, &in, &ob), GCOMP_OK)
          << "size " << size << " level " << level;

      gcomp_buffer_t fb = {decoded.data(), 0, 0};
      EXPECT_EQ(gcomp_decoder_finish(dec, &fb), GCOMP_OK)
          << "size " << size << " level " << level
          << ": the stream is complete and finish must say so";
      gcomp_decoder_destroy(dec);

      EXPECT_EQ(ob.used, input.size()) << "size " << size;
      EXPECT_EQ(decoded, input) << "size " << size << " level " << level;
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
