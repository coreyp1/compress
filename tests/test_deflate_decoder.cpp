/**
 * @file test_deflate_decoder.cpp
 *
 * Unit tests for the DEFLATE decoder implementation in the Ghoti.io Compress
 * library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
