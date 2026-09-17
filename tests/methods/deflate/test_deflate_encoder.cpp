/**
 * @file test_deflate_encoder.cpp
 *
 * Unit tests for the DEFLATE encoder implementation in the Ghoti.io Compress
 * library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/namespace.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <vector>

class DeflateEncoderTest : public ::testing::Test {
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
    if (encoder_) {
      gcomp_encoder_destroy(encoder_);
      encoder_ = nullptr;
    }
    if (decoder_) {
      gcomp_decoder_destroy(decoder_);
      decoder_ = nullptr;
    }
    if (registry_) {
      gcomp_registry_destroy(registry_);
      registry_ = nullptr;
    }
  }

  // Helper: encode data to a vector
  gcomp_status_t encode_data(const uint8_t * data, size_t len,
      std::vector<uint8_t> & out, gcomp_options_t * opts = nullptr) {
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "deflate", opts, &encoder_);
    if (status != GCOMP_OK) {
      return status;
    }

    out.resize(len * 2 + 1024); // Generous output buffer
    gcomp_buffer_t input = {data, len, 0};
    gcomp_buffer_t output = {out.data(), out.size(), 0};

    status = gcomp_encoder_update(encoder_, &input, &output);
    if (status != GCOMP_OK) {
      return status;
    }

    gcomp_buffer_t finish_out = {
        out.data() + output.used, out.size() - output.used, 0};
    status = gcomp_encoder_finish(encoder_, &finish_out);
    if (status != GCOMP_OK) {
      return status;
    }

    out.resize(output.used + finish_out.used);
    return GCOMP_OK;
  }

  // Helper: decode data to a vector
  gcomp_status_t decode_data(const uint8_t * data, size_t len,
      std::vector<uint8_t> & out, size_t expected_len = 0) {
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "deflate", nullptr, &decoder_);
    if (status != GCOMP_OK) {
      return status;
    }

    size_t out_size = (expected_len > 0) ? expected_len * 2 : len * 10 + 1024;
    out.resize(out_size);
    gcomp_buffer_t input = {data, len, 0};
    gcomp_buffer_t output = {out.data(), out.size(), 0};

    status = gcomp_decoder_update(decoder_, &input, &output);
    if (status != GCOMP_OK) {
      return status;
    }

    gcomp_buffer_t finish_out = {
        out.data() + output.used, out.size() - output.used, 0};
    status = gcomp_decoder_finish(decoder_, &finish_out);
    if (status != GCOMP_OK) {
      return status;
    }

    out.resize(output.used + finish_out.used);
    return GCOMP_OK;
  }

  gcomp_registry_t * registry_ = nullptr;
  gcomp_encoder_t * encoder_ = nullptr;
  gcomp_decoder_t * decoder_ = nullptr;
};

//
// Basic encoder creation tests
//

TEST_F(DeflateEncoderTest, CreateEncoderSuccess) {
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "deflate", nullptr, &encoder_);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder_, nullptr);
}

TEST_F(DeflateEncoderTest, CreateEncoderWithOptions) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 6), GCOMP_OK);

  gcomp_status_t status =
      gcomp_encoder_create(registry_, "deflate", opts, &encoder_);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder_, nullptr);

  gcomp_options_destroy(opts);
}

//
// Round-trip tests: encode then decode
//

TEST_F(DeflateEncoderTest, RoundTrip_HelloWorld) {
  const char * input_str = "Hello, World!";
  const uint8_t * input = (const uint8_t *)input_str;
  size_t input_len = strlen(input_str);

  std::vector<uint8_t> compressed;
  ASSERT_EQ(encode_data(input, input_len, compressed), GCOMP_OK);
  EXPECT_GT(compressed.size(), 0u);

  // Clean up encoder before creating decoder
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(
                compressed.data(), compressed.size(), decompressed, input_len),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input_len);
  EXPECT_EQ(memcmp(decompressed.data(), input, input_len), 0);
}

TEST_F(DeflateEncoderTest, RoundTrip_EmptyInput) {
  std::vector<uint8_t> compressed;
  ASSERT_EQ(encode_data(nullptr, 0, compressed), GCOMP_OK);

  // Clean up encoder before creating decoder
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(compressed.data(), compressed.size(), decompressed, 0),
      GCOMP_OK);

  EXPECT_EQ(decompressed.size(), 0u);
}

TEST_F(DeflateEncoderTest, RoundTrip_SingleByte) {
  uint8_t input[] = {0x42};

  std::vector<uint8_t> compressed;
  ASSERT_EQ(encode_data(input, sizeof(input), compressed), GCOMP_OK);

  // Clean up encoder before creating decoder
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(compressed.data(), compressed.size(), decompressed, 1),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), 1u);
  EXPECT_EQ(decompressed[0], 0x42);
}

TEST_F(DeflateEncoderTest, RoundTrip_RepeatedPattern) {
  // Repeated pattern should compress well
  std::vector<uint8_t> input(1000);
  for (size_t i = 0; i < input.size(); i++) {
    input[i] = (uint8_t)(i % 10);
  }

  std::vector<uint8_t> compressed;
  ASSERT_EQ(encode_data(input.data(), input.size(), compressed), GCOMP_OK);

  // Clean up encoder before creating decoder
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(compressed.data(), compressed.size(), decompressed,
                input.size()),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);

  // Should compress since there's a pattern
  // (compression ratio depends on level)
}

TEST_F(DeflateEncoderTest, RoundTrip_LargeInput) {
  // 64KB of random-ish data
  std::vector<uint8_t> input(65536);
  unsigned seed = 12345;
  for (size_t i = 0; i < input.size(); i++) {
    seed = seed * 1103515245u + 12345u;
    input[i] = (uint8_t)(seed >> 16);
  }

  std::vector<uint8_t> compressed;
  ASSERT_EQ(encode_data(input.data(), input.size(), compressed), GCOMP_OK);

  // Clean up encoder before creating decoder
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(compressed.data(), compressed.size(), decompressed,
                input.size()),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(DeflateEncoderTest, RoundTrip_PastWindowWrap_CompressibleData) {
  // Regression: the match finder measured hash-entry staleness against the
  // encoding position instead of the window fill position, so the newest
  // `lookahead` bytes of "history" had already been overwritten by future
  // data.  Matches against that stale region emitted distances that decode
  // to the wrong bytes.
  //
  // Triggering it needs BOTH of these, which is why the pre-existing 64KB
  // random-data round trip never caught it:
  //   * more than one window (32KB) of input, so the buffer wraps twice, and
  //   * data compressible enough to actually produce long-distance matches.
  //     Pseudo-random bytes expand rather than match, and never exercise the
  //     match finder at all.
  const char * words[] = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta",
      "eta", "theta", "iota", "kappa", "lambda", "mu", "nu", "xi", "omicron"};
  std::vector<uint8_t> input;
  input.reserve(300000);
  unsigned seed = 9876;
  while (input.size() < 300000) {
    seed = seed * 1103515245u + 12345u;
    const char * w = words[(seed >> 16) % (sizeof(words) / sizeof(*words))];
    while (*w) {
      input.push_back((uint8_t)*w++);
    }
    input.push_back((uint8_t)' ');
  }

  std::vector<uint8_t> compressed;
  ASSERT_EQ(encode_data(input.data(), input.size(), compressed), GCOMP_OK);

  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(compressed.data(), compressed.size(), decompressed,
                input.size()),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

//
// Level 0 (stored) tests
//

TEST_F(DeflateEncoderTest, Level0_Stored_HelloWorld) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 0), GCOMP_OK);

  const char * input_str = "Hello, World!";
  const uint8_t * input = (const uint8_t *)input_str;
  size_t input_len = strlen(input_str);

  std::vector<uint8_t> compressed;
  ASSERT_EQ(encode_data(input, input_len, compressed, opts), GCOMP_OK);

  gcomp_options_destroy(opts);

  // Clean up encoder before creating decoder
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(
                compressed.data(), compressed.size(), decompressed, input_len),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input_len);
  EXPECT_EQ(memcmp(decompressed.data(), input, input_len), 0);
}

TEST_F(DeflateEncoderTest, Level0_Stored_LargeInput) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 0), GCOMP_OK);

  // 128KB to test multiple stored blocks
  std::vector<uint8_t> input(128 * 1024);
  for (size_t i = 0; i < input.size(); i++) {
    input[i] = (uint8_t)(i & 0xFF);
  }

  std::vector<uint8_t> compressed;
  ASSERT_EQ(
      encode_data(input.data(), input.size(), compressed, opts), GCOMP_OK);

  gcomp_options_destroy(opts);

  // Clean up encoder before creating decoder
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(compressed.data(), compressed.size(), decompressed,
                input.size()),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

//
// Different compression levels
//

TEST_F(DeflateEncoderTest, AllLevels_RoundTrip) {
  const char * input_str = "The quick brown fox jumps over the lazy dog. "
                           "Pack my box with five dozen liquor jugs. "
                           "How vexingly quick daft zebras jump!";
  const uint8_t * input = (const uint8_t *)input_str;
  size_t input_len = strlen(input_str);

  for (int level = 0; level <= 9; level++) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", level), GCOMP_OK);

    std::vector<uint8_t> compressed;
    ASSERT_EQ(encode_data(input, input_len, compressed, opts), GCOMP_OK)
        << "Failed at level " << level;

    gcomp_options_destroy(opts);

    // Clean up encoder before creating decoder
    gcomp_encoder_destroy(encoder_);
    encoder_ = nullptr;

    std::vector<uint8_t> decompressed;
    ASSERT_EQ(decode_data(compressed.data(), compressed.size(), decompressed,
                  input_len),
        GCOMP_OK)
        << "Failed to decode at level " << level;

    ASSERT_EQ(decompressed.size(), input_len)
        << "Size mismatch at level " << level;
    EXPECT_EQ(memcmp(decompressed.data(), input, input_len), 0)
        << "Data mismatch at level " << level;

    // Clean up decoder before next iteration
    gcomp_decoder_destroy(decoder_);
    decoder_ = nullptr;
  }
}

//
// Streaming tests (chunked encoding)
//

TEST_F(DeflateEncoderTest, ChunkedEncoding_SmallChunks) {
  const char * input_str =
      "This is a test of chunked encoding. "
      "The encoder should handle multiple update calls correctly.";
  const uint8_t * input = (const uint8_t *)input_str;
  size_t input_len = strlen(input_str);

  ASSERT_EQ(
      gcomp_encoder_create(registry_, "deflate", nullptr, &encoder_), GCOMP_OK);

  std::vector<uint8_t> compressed(input_len * 2 + 1024);
  size_t total_out = 0;

  // Feed input in small chunks
  size_t chunk_size = 10;
  for (size_t i = 0; i < input_len; i += chunk_size) {
    size_t this_chunk =
        (i + chunk_size <= input_len) ? chunk_size : (input_len - i);
    gcomp_buffer_t in_buf = {input + i, this_chunk, 0};
    gcomp_buffer_t out_buf = {
        compressed.data() + total_out, compressed.size() - total_out, 0};

    ASSERT_EQ(gcomp_encoder_update(encoder_, &in_buf, &out_buf), GCOMP_OK);
    EXPECT_EQ(in_buf.used, this_chunk);
    total_out += out_buf.used;
  }

  // Finish
  gcomp_buffer_t finish_out = {
      compressed.data() + total_out, compressed.size() - total_out, 0};
  ASSERT_EQ(gcomp_encoder_finish(encoder_, &finish_out), GCOMP_OK);
  total_out += finish_out.used;

  compressed.resize(total_out);

  // Clean up encoder before creating decoder
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  // Decode and verify
  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(
                compressed.data(), compressed.size(), decompressed, input_len),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input_len);
  EXPECT_EQ(memcmp(decompressed.data(), input, input_len), 0);
}

TEST_F(DeflateEncoderTest, ChunkedEncoding_SmallInputChunks) {
  // Test that the encoder can handle input fed in very small chunks (1 byte at
  // a time). This stress-tests the streaming behavior during update().
  //
  // Note: The current encoder's finish() doesn't support incremental output
  // across multiple calls with small buffers - it needs enough space to write
  // the entire final block. This test focuses on small INPUT chunks instead.
  const char * input_str =
      "This is a test of small input chunk encoding. "
      "The encoder must handle input fed one byte at a time correctly. "
      "Each update call receives a single byte of input.";
  const uint8_t * input = (const uint8_t *)input_str;
  size_t input_len = strlen(input_str);

  ASSERT_EQ(
      gcomp_encoder_create(registry_, "deflate", nullptr, &encoder_), GCOMP_OK);

  std::vector<uint8_t> compressed;
  compressed.reserve(input_len * 2);

  // Feed input one byte at a time
  uint8_t update_out[256] = {};
  for (size_t i = 0; i < input_len; i++) {
    gcomp_buffer_t in_buf = {input + i, 1, 0};
    gcomp_buffer_t out_buf = {update_out, sizeof(update_out), 0};

    gcomp_status_t status = gcomp_encoder_update(encoder_, &in_buf, &out_buf);
    ASSERT_EQ(status, GCOMP_OK) << "Update failed at byte " << i;
    ASSERT_EQ(in_buf.used, 1u) << "Encoder did not consume byte " << i;

    // Collect any output
    for (size_t j = 0; j < out_buf.used; j++) {
      compressed.push_back(update_out[j]);
    }
  }

  // Finish with a buffer large enough to complete in one call
  uint8_t finish_out[512] = {};
  gcomp_buffer_t finish_buf = {finish_out, sizeof(finish_out), 0};
  gcomp_status_t status = gcomp_encoder_finish(encoder_, &finish_buf);
  ASSERT_EQ(status, GCOMP_OK) << "Finish failed";

  for (size_t i = 0; i < finish_buf.used; i++) {
    compressed.push_back(finish_out[i]);
  }

  ASSERT_GT(compressed.size(), 0u) << "No compressed output produced";

  // Clean up encoder before creating decoder
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  // Decode and verify
  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(
                compressed.data(), compressed.size(), decompressed, input_len),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input_len);
  EXPECT_EQ(memcmp(decompressed.data(), input, input_len), 0)
      << "Decompressed data doesn't match original";
}

//
// Memory tests
//

TEST_F(DeflateEncoderTest, Memory_CreateDestroyNoLeak) {
  for (int i = 0; i < 4; i++) {
    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(
        gcomp_encoder_create(registry_, "deflate", nullptr, &enc), GCOMP_OK);
    ASSERT_NE(enc, nullptr);
    gcomp_encoder_destroy(enc);
  }
}

TEST_F(DeflateEncoderTest, Memory_CreateDestroyAllLevels) {
  for (int level = 0; level <= 9; level++) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", level), GCOMP_OK);

    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(gcomp_encoder_create(registry_, "deflate", opts, &enc), GCOMP_OK);
    ASSERT_NE(enc, nullptr);

    gcomp_encoder_destroy(enc);
    gcomp_options_destroy(opts);
  }
}

//
// Error handling tests
//

TEST_F(DeflateEncoderTest, Error_NullEncoder) {
  uint8_t buf[8] = {};
  gcomp_buffer_t input = {buf, sizeof(buf), 0};
  gcomp_buffer_t output = {buf, sizeof(buf), 0};

  EXPECT_EQ(
      gcomp_encoder_update(nullptr, &input, &output), GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_encoder_finish(nullptr, &output), GCOMP_ERR_INVALID_ARG);
}

TEST_F(DeflateEncoderTest, Error_NullBuffers) {
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "deflate", nullptr, &encoder_), GCOMP_OK);

  uint8_t buf[8] = {};
  gcomp_buffer_t valid_buf = {buf, sizeof(buf), 0};

  EXPECT_EQ(gcomp_encoder_update(encoder_, nullptr, &valid_buf),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_encoder_update(encoder_, &valid_buf, nullptr),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_encoder_finish(encoder_, nullptr), GCOMP_ERR_INVALID_ARG);
}

//
// Strategy tests
//

TEST_F(DeflateEncoderTest, Strategy_Default_RoundTrip) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_string(opts, "deflate.strategy", "default"), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 6), GCOMP_OK);

  const char * input_str = "Hello, World! This is a test with repeated words. "
                           "Hello again! World, here we go.";
  const uint8_t * input = (const uint8_t *)input_str;
  size_t input_len = strlen(input_str);

  std::vector<uint8_t> compressed;
  ASSERT_EQ(encode_data(input, input_len, compressed, opts), GCOMP_OK);

  gcomp_options_destroy(opts);
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(
                compressed.data(), compressed.size(), decompressed, input_len),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input_len);
  EXPECT_EQ(memcmp(decompressed.data(), input, input_len), 0);
}

TEST_F(DeflateEncoderTest, Strategy_Fixed_RoundTrip) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_string(opts, "deflate.strategy", "fixed"), GCOMP_OK);
  // Use level 6 (which would normally use dynamic Huffman)
  ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 6), GCOMP_OK);

  const char * input_str = "The quick brown fox jumps over the lazy dog. "
                           "Pack my box with five dozen liquor jugs.";
  const uint8_t * input = (const uint8_t *)input_str;
  size_t input_len = strlen(input_str);

  std::vector<uint8_t> compressed;
  ASSERT_EQ(encode_data(input, input_len, compressed, opts), GCOMP_OK);

  gcomp_options_destroy(opts);
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(
                compressed.data(), compressed.size(), decompressed, input_len),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input_len);
  EXPECT_EQ(memcmp(decompressed.data(), input, input_len), 0);
}

TEST_F(DeflateEncoderTest, Strategy_HuffmanOnly_RoundTrip) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_string(opts, "deflate.strategy", "huffman_only"),
      GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 6), GCOMP_OK);

  const char * input_str = "Hello, World! This is a test.";
  const uint8_t * input = (const uint8_t *)input_str;
  size_t input_len = strlen(input_str);

  std::vector<uint8_t> compressed;
  ASSERT_EQ(encode_data(input, input_len, compressed, opts), GCOMP_OK);

  gcomp_options_destroy(opts);
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(
                compressed.data(), compressed.size(), decompressed, input_len),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input_len);
  EXPECT_EQ(memcmp(decompressed.data(), input, input_len), 0);
}

TEST_F(DeflateEncoderTest, Strategy_HuffmanOnly_NoCompression) {
  // Huffman-only should produce larger output for repetitive data
  // because it doesn't use LZ77 back-references
  gcomp_options_t * opts_huffman = nullptr;
  gcomp_options_t * opts_default = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts_huffman), GCOMP_OK);
  ASSERT_EQ(gcomp_options_create(&opts_default), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_string(
                opts_huffman, "deflate.strategy", "huffman_only"),
      GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_string(opts_default, "deflate.strategy", "default"),
      GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_int64(opts_huffman, "deflate.level", 6), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_int64(opts_default, "deflate.level", 6), GCOMP_OK);

  // Repetitive data that compresses well with LZ77
  std::vector<uint8_t> input(1000);
  for (size_t i = 0; i < input.size(); i++) {
    input[i] = (uint8_t)(i % 10);
  }

  // Compress with huffman_only
  std::vector<uint8_t> compressed_huffman;
  ASSERT_EQ(
      encode_data(input.data(), input.size(), compressed_huffman, opts_huffman),
      GCOMP_OK);
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  // Compress with default strategy
  std::vector<uint8_t> compressed_default;
  ASSERT_EQ(
      encode_data(input.data(), input.size(), compressed_default, opts_default),
      GCOMP_OK);

  gcomp_options_destroy(opts_huffman);
  gcomp_options_destroy(opts_default);
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  // Huffman-only should produce larger output for repetitive data
  EXPECT_GT(compressed_huffman.size(), compressed_default.size());

  // Verify both decompress correctly
  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(compressed_huffman.data(), compressed_huffman.size(),
                decompressed, input.size()),
      GCOMP_OK);
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(DeflateEncoderTest, Strategy_RLE_RoundTrip) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_string(opts, "deflate.strategy", "rle"), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 6), GCOMP_OK);

  // Data with runs of repeated bytes (good for RLE)
  std::vector<uint8_t> input;
  for (int i = 0; i < 100; i++) {
    for (int j = 0; j < 10; j++) {
      input.push_back((uint8_t)i);
    }
  }

  std::vector<uint8_t> compressed;
  ASSERT_EQ(
      encode_data(input.data(), input.size(), compressed, opts), GCOMP_OK);

  gcomp_options_destroy(opts);
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(compressed.data(), compressed.size(), decompressed,
                input.size()),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(DeflateEncoderTest, Strategy_RLE_CompressesRuns) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_string(opts, "deflate.strategy", "rle"), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 6), GCOMP_OK);

  // Data with long runs of the same byte
  std::vector<uint8_t> input(1000, 'A'); // 1000 'A' characters

  std::vector<uint8_t> compressed;
  ASSERT_EQ(
      encode_data(input.data(), input.size(), compressed, opts), GCOMP_OK);

  gcomp_options_destroy(opts);
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  // RLE should compress this very well
  EXPECT_LT(compressed.size(), input.size());

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(compressed.data(), compressed.size(), decompressed,
                input.size()),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(DeflateEncoderTest, Strategy_Filtered_RoundTrip) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_string(opts, "deflate.strategy", "filtered"), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 6), GCOMP_OK);

  // Simulate PNG-like filtered data (small differences)
  std::vector<uint8_t> input(1000);
  uint8_t prev = 0;
  for (size_t i = 0; i < input.size(); i++) {
    // Small differences like PNG filter output
    int diff = (int)(i % 7) - 3; // -3 to +3
    input[i] = (uint8_t)(prev + diff);
    prev = input[i];
  }

  std::vector<uint8_t> compressed;
  ASSERT_EQ(
      encode_data(input.data(), input.size(), compressed, opts), GCOMP_OK);

  gcomp_options_destroy(opts);
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(compressed.data(), compressed.size(), decompressed,
                input.size()),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(DeflateEncoderTest, Strategy_InvalidFallsBackToDefault) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  // Invalid strategy should silently fall back to default
  ASSERT_EQ(gcomp_options_set_string(opts, "deflate.strategy", "invalid_xyz"),
      GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 6), GCOMP_OK);

  const char * input_str = "Hello, World!";
  const uint8_t * input = (const uint8_t *)input_str;
  size_t input_len = strlen(input_str);

  std::vector<uint8_t> compressed;
  ASSERT_EQ(encode_data(input, input_len, compressed, opts), GCOMP_OK);

  gcomp_options_destroy(opts);
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(
                compressed.data(), compressed.size(), decompressed, input_len),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input_len);
  EXPECT_EQ(memcmp(decompressed.data(), input, input_len), 0);
}

TEST_F(DeflateEncoderTest, Strategy_Default_LargeInput) {
  // Test default strategy with larger input - same data as RoundTrip_LargeInput
  // to verify strategy option doesn't break basic functionality
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_string(opts, "deflate.strategy", "default"), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 6), GCOMP_OK);

  // 64KB of random-ish data (same pattern as RoundTrip_LargeInput)
  std::vector<uint8_t> input(65536);
  unsigned seed = 12345;
  for (size_t i = 0; i < input.size(); i++) {
    seed = seed * 1103515245u + 12345u;
    input[i] = (uint8_t)(seed >> 16);
  }

  std::vector<uint8_t> compressed;
  ASSERT_EQ(
      encode_data(input.data(), input.size(), compressed, opts), GCOMP_OK);

  gcomp_options_destroy(opts);
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(compressed.data(), compressed.size(), decompressed,
                input.size()),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(DeflateEncoderTest, Strategy_HuffmanOnly_SizeProgression) {
  // Test huffman_only with progressively larger sizes to find the breakpoint
  const size_t test_sizes[] = {100, 500, 1000, 2000, 4000, 8000};

  for (size_t test_size : test_sizes) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(
        gcomp_options_set_string(opts, "deflate.strategy", "huffman_only"),
        GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 6), GCOMP_OK);

    // Simple incrementing pattern
    std::vector<uint8_t> input(test_size);
    for (size_t i = 0; i < input.size(); i++) {
      input[i] = (uint8_t)(i & 0xFF);
    }

    std::vector<uint8_t> compressed;
    ASSERT_EQ(
        encode_data(input.data(), input.size(), compressed, opts), GCOMP_OK)
        << "Failed to encode with size: " << test_size;

    gcomp_options_destroy(opts);
    gcomp_encoder_destroy(encoder_);
    encoder_ = nullptr;

    std::vector<uint8_t> decompressed;
    ASSERT_EQ(decode_data(compressed.data(), compressed.size(), decompressed,
                  input.size()),
        GCOMP_OK)
        << "Failed to decode with size: " << test_size;

    ASSERT_EQ(decompressed.size(), input.size())
        << "Size mismatch for size: " << test_size;
    EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0)
        << "Data mismatch for size: " << test_size;

    gcomp_decoder_destroy(decoder_);
    decoder_ = nullptr;
  }
}

TEST_F(DeflateEncoderTest, Strategy_AllStrategies_SmallInput) {
  // Test all strategies with small input (256 bytes)
  const char * strategies[] = {
      "default", "filtered", "huffman_only", "rle", "fixed"};

  // Simple incrementing pattern
  std::vector<uint8_t> input(256);
  for (size_t i = 0; i < input.size(); i++) {
    input[i] = (uint8_t)i;
  }

  for (const char * strategy : strategies) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(
        gcomp_options_set_string(opts, "deflate.strategy", strategy), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 6), GCOMP_OK);

    std::vector<uint8_t> compressed;
    ASSERT_EQ(
        encode_data(input.data(), input.size(), compressed, opts), GCOMP_OK)
        << "Failed to encode with strategy: " << strategy;

    gcomp_options_destroy(opts);
    gcomp_encoder_destroy(encoder_);
    encoder_ = nullptr;

    std::vector<uint8_t> decompressed;
    ASSERT_EQ(decode_data(compressed.data(), compressed.size(), decompressed,
                  input.size()),
        GCOMP_OK)
        << "Failed to decode with strategy: " << strategy;

    ASSERT_EQ(decompressed.size(), input.size())
        << "Size mismatch for strategy: " << strategy;
    EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0)
        << "Data mismatch for strategy: " << strategy;

    gcomp_decoder_destroy(decoder_);
    decoder_ = nullptr;
  }
}

TEST_F(DeflateEncoderTest, Strategy_HuffmanOnly_LargerSimple) {
  // Test with larger simple pattern to check if size matters
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_string(opts, "deflate.strategy", "huffman_only"),
      GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 6), GCOMP_OK);

  // 32KB of simple incrementing pattern
  std::vector<uint8_t> input(32 * 1024);
  for (size_t i = 0; i < input.size(); i++) {
    input[i] = (uint8_t)(i & 0xFF);
  }

  std::vector<uint8_t> compressed;
  ASSERT_EQ(
      encode_data(input.data(), input.size(), compressed, opts), GCOMP_OK);

  gcomp_options_destroy(opts);
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(compressed.data(), compressed.size(), decompressed,
                input.size()),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(DeflateEncoderTest, Strategy_HuffmanOnly_MixedPattern_Fixed) {
  // Test with mixed pattern using fixed Huffman (level 3) to isolate
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_string(opts, "deflate.strategy", "huffman_only"),
      GCOMP_OK);
  // Level 3 uses fixed Huffman codes
  ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 3), GCOMP_OK);

  // 4KB of mixed data
  std::vector<uint8_t> input(4 * 1024);
  unsigned seed = 54321;
  for (size_t i = 0; i < input.size(); i++) {
    seed = seed * 1103515245u + 12345u;
    if ((i / 100) % 2 == 0) {
      input[i] = (uint8_t)(seed >> 16);
    }
    else {
      input[i] = (uint8_t)(i % 10);
    }
  }

  std::vector<uint8_t> compressed;
  ASSERT_EQ(
      encode_data(input.data(), input.size(), compressed, opts), GCOMP_OK);

  gcomp_options_destroy(opts);
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(compressed.data(), compressed.size(), decompressed,
                input.size()),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(DeflateEncoderTest, Strategy_HuffmanOnly_MixedPattern_Dynamic) {
  // Test with mixed pattern using dynamic Huffman (level 6)
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_string(opts, "deflate.strategy", "huffman_only"),
      GCOMP_OK);
  // Level 6 uses dynamic Huffman codes
  ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 6), GCOMP_OK);

  // 4KB of mixed data
  std::vector<uint8_t> input(4 * 1024);
  unsigned seed = 54321;
  for (size_t i = 0; i < input.size(); i++) {
    seed = seed * 1103515245u + 12345u;
    if ((i / 100) % 2 == 0) {
      input[i] = (uint8_t)(seed >> 16);
    }
    else {
      input[i] = (uint8_t)(i % 10);
    }
  }

  std::vector<uint8_t> compressed;
  ASSERT_EQ(
      encode_data(input.data(), input.size(), compressed, opts), GCOMP_OK);

  gcomp_options_destroy(opts);
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(compressed.data(), compressed.size(), decompressed,
                input.size()),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(DeflateEncoderTest, Strategy_Default_MixedPattern_Dynamic) {
  // Test default strategy with same mixed pattern
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(
      gcomp_options_set_string(opts, "deflate.strategy", "default"), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 6), GCOMP_OK);

  // Same 4KB of mixed data
  std::vector<uint8_t> input(4 * 1024);
  unsigned seed = 54321;
  for (size_t i = 0; i < input.size(); i++) {
    seed = seed * 1103515245u + 12345u;
    if ((i / 100) % 2 == 0) {
      input[i] = (uint8_t)(seed >> 16);
    }
    else {
      input[i] = (uint8_t)(i % 10);
    }
  }


  std::vector<uint8_t> compressed;
  ASSERT_EQ(
      encode_data(input.data(), input.size(), compressed, opts), GCOMP_OK);

  gcomp_options_destroy(opts);
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(compressed.data(), compressed.size(), decompressed,
                input.size()),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

//
// Incremental finish() output tests (T5.9)
//
// These tests verify that encoder finish() correctly supports small output
// buffers by buffering output internally and copying incrementally.
//

TEST_F(DeflateEncoderTest, Finish_OneByteOutputBuffer) {
  // Test finish() with 1-byte output buffer - the encoder should buffer
  // internally and copy out one byte at a time
  const char * input_str = "Hello, World! This tests incremental finish.";
  const uint8_t * input = (const uint8_t *)input_str;
  size_t input_len = strlen(input_str);

  ASSERT_EQ(
      gcomp_encoder_create(registry_, "deflate", nullptr, &encoder_), GCOMP_OK);

  // First, feed all input and collect any output during update
  std::vector<uint8_t> compressed;
  compressed.reserve(input_len * 2);

  uint8_t update_out[256] = {};
  gcomp_buffer_t in_buf = {input, input_len, 0};
  gcomp_buffer_t out_buf = {update_out, sizeof(update_out), 0};

  gcomp_status_t status = gcomp_encoder_update(encoder_, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(in_buf.used, input_len);

  for (size_t i = 0; i < out_buf.used; i++) {
    compressed.push_back(update_out[i]);
  }

  // Now call finish() with 1-byte output buffer repeatedly until done
  uint8_t one_byte_buf[1] = {};
  int iterations = 0;
  const int max_iterations = 10000; // Safety limit

  while (iterations < max_iterations) {
    gcomp_buffer_t finish_buf = {one_byte_buf, 1, 0};
    status = gcomp_encoder_finish(encoder_, &finish_buf);

    if (finish_buf.used > 0) {
      compressed.push_back(one_byte_buf[0]);
    }

    if (status == GCOMP_OK) {
      // Finished successfully
      break;
    }

    ASSERT_EQ(status, GCOMP_ERR_LIMIT)
        << "Unexpected status: " << status << " at iteration " << iterations;
    iterations++;
  }

  ASSERT_LT(iterations, max_iterations)
      << "finish() did not complete in reasonable iterations";
  ASSERT_GT(compressed.size(), 0u) << "No compressed output produced";

  // Clean up encoder before creating decoder
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  // Verify the output decompresses correctly
  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(
                compressed.data(), compressed.size(), decompressed, input_len),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input_len);
  EXPECT_EQ(memcmp(decompressed.data(), input, input_len), 0)
      << "Decompressed data doesn't match original";
}

TEST_F(DeflateEncoderTest, Finish_SmallOutputBuffer_16Bytes) {
  // Test finish() with 16-byte output buffer
  const char * input_str = "The quick brown fox jumps over the lazy dog. "
                           "Pack my box with five dozen liquor jugs.";
  const uint8_t * input = (const uint8_t *)input_str;
  size_t input_len = strlen(input_str);

  ASSERT_EQ(
      gcomp_encoder_create(registry_, "deflate", nullptr, &encoder_), GCOMP_OK);

  // Feed all input
  std::vector<uint8_t> compressed;
  compressed.reserve(input_len * 2);

  uint8_t update_out[512] = {};
  gcomp_buffer_t in_buf = {input, input_len, 0};
  gcomp_buffer_t out_buf = {update_out, sizeof(update_out), 0};

  ASSERT_EQ(gcomp_encoder_update(encoder_, &in_buf, &out_buf), GCOMP_OK);

  for (size_t i = 0; i < out_buf.used; i++) {
    compressed.push_back(update_out[i]);
  }

  // Call finish() with 16-byte output buffer
  uint8_t small_buf[16] = {};
  int iterations = 0;
  const int max_iterations = 1000;

  while (iterations < max_iterations) {
    gcomp_buffer_t finish_buf = {small_buf, sizeof(small_buf), 0};
    gcomp_status_t status = gcomp_encoder_finish(encoder_, &finish_buf);

    for (size_t i = 0; i < finish_buf.used; i++) {
      compressed.push_back(small_buf[i]);
    }

    if (status == GCOMP_OK) {
      break;
    }

    ASSERT_EQ(status, GCOMP_ERR_LIMIT)
        << "Unexpected status at iteration " << iterations;
    iterations++;
  }

  ASSERT_LT(iterations, max_iterations);

  // Clean up and verify
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(
                compressed.data(), compressed.size(), decompressed, input_len),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input_len);
  EXPECT_EQ(memcmp(decompressed.data(), input, input_len), 0);
}

TEST_F(DeflateEncoderTest, Finish_VaryingBufferSizes) {
  // Test finish() with varying buffer sizes each call
  const char * input_str =
      "This test uses different buffer sizes for each finish call.";
  const uint8_t * input = (const uint8_t *)input_str;
  size_t input_len = strlen(input_str);

  ASSERT_EQ(
      gcomp_encoder_create(registry_, "deflate", nullptr, &encoder_), GCOMP_OK);

  // Feed all input
  std::vector<uint8_t> compressed;
  uint8_t update_out[512] = {};
  gcomp_buffer_t in_buf = {input, input_len, 0};
  gcomp_buffer_t out_buf = {update_out, sizeof(update_out), 0};

  ASSERT_EQ(gcomp_encoder_update(encoder_, &in_buf, &out_buf), GCOMP_OK);

  for (size_t i = 0; i < out_buf.used; i++) {
    compressed.push_back(update_out[i]);
  }

  // Call finish() with varying buffer sizes: 1, 3, 7, 2, 5, 11, 4, 8, ...
  const size_t buffer_sizes[] = {1, 3, 7, 2, 5, 11, 4, 8, 13, 6, 9, 15, 10};
  size_t size_index = 0;
  uint8_t var_buf[16] = {};
  int iterations = 0;
  const int max_iterations = 1000;

  while (iterations < max_iterations) {
    size_t buf_size = buffer_sizes[size_index % 13];
    gcomp_buffer_t finish_buf = {var_buf, buf_size, 0};
    gcomp_status_t status = gcomp_encoder_finish(encoder_, &finish_buf);

    for (size_t i = 0; i < finish_buf.used; i++) {
      compressed.push_back(var_buf[i]);
    }

    if (status == GCOMP_OK) {
      break;
    }

    ASSERT_EQ(status, GCOMP_ERR_LIMIT);
    size_index++;
    iterations++;
  }

  ASSERT_LT(iterations, max_iterations);

  // Clean up and verify
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(
                compressed.data(), compressed.size(), decompressed, input_len),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input_len);
  EXPECT_EQ(memcmp(decompressed.data(), input, input_len), 0);
}

TEST_F(DeflateEncoderTest, Finish_SmallBuffer_MatchesSingleCall) {
  // Verify that incremental finish produces the same output as single-call
  const char * input_str = "Test data for comparing incremental vs single call";
  const uint8_t * input = (const uint8_t *)input_str;
  size_t input_len = strlen(input_str);

  // First: encode with large buffer (single-call finish)
  std::vector<uint8_t> compressed_single;
  {
    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(
        gcomp_encoder_create(registry_, "deflate", nullptr, &enc), GCOMP_OK);

    uint8_t out[1024] = {};
    gcomp_buffer_t in_buf = {input, input_len, 0};
    gcomp_buffer_t out_buf = {out, sizeof(out), 0};
    ASSERT_EQ(gcomp_encoder_update(enc, &in_buf, &out_buf), GCOMP_OK);

    for (size_t i = 0; i < out_buf.used; i++) {
      compressed_single.push_back(out[i]);
    }

    gcomp_buffer_t finish_buf = {out, sizeof(out), 0};
    ASSERT_EQ(gcomp_encoder_finish(enc, &finish_buf), GCOMP_OK);

    for (size_t i = 0; i < finish_buf.used; i++) {
      compressed_single.push_back(out[i]);
    }

    gcomp_encoder_destroy(enc);
  }

  // Second: encode with small buffer (incremental finish)
  std::vector<uint8_t> compressed_incremental;
  {
    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(
        gcomp_encoder_create(registry_, "deflate", nullptr, &enc), GCOMP_OK);

    uint8_t out[1024] = {};
    gcomp_buffer_t in_buf = {input, input_len, 0};
    gcomp_buffer_t out_buf = {out, sizeof(out), 0};
    ASSERT_EQ(gcomp_encoder_update(enc, &in_buf, &out_buf), GCOMP_OK);

    for (size_t i = 0; i < out_buf.used; i++) {
      compressed_incremental.push_back(out[i]);
    }

    // Finish with 1-byte buffer
    uint8_t one_byte[1] = {};
    int iterations = 0;
    while (iterations < 10000) {
      gcomp_buffer_t finish_buf = {one_byte, 1, 0};
      gcomp_status_t status = gcomp_encoder_finish(enc, &finish_buf);

      if (finish_buf.used > 0) {
        compressed_incremental.push_back(one_byte[0]);
      }

      if (status == GCOMP_OK) {
        break;
      }
      iterations++;
    }

    gcomp_encoder_destroy(enc);
  }

  // Compare: both should produce identical output
  ASSERT_EQ(compressed_single.size(), compressed_incremental.size())
      << "Size mismatch between single-call and incremental finish";
  EXPECT_EQ(memcmp(compressed_single.data(), compressed_incremental.data(),
                compressed_single.size()),
      0)
      << "Data mismatch between single-call and incremental finish";
}

TEST_F(DeflateEncoderTest, Finish_SmallBuffer_Level0_Stored) {
  // Test incremental finish with level 0 (stored blocks)
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", 0), GCOMP_OK);

  const char * input_str =
      "Testing level 0 stored blocks with small finish buffer";
  const uint8_t * input = (const uint8_t *)input_str;
  size_t input_len = strlen(input_str);

  ASSERT_EQ(
      gcomp_encoder_create(registry_, "deflate", opts, &encoder_), GCOMP_OK);
  gcomp_options_destroy(opts);

  // Feed all input
  std::vector<uint8_t> compressed;
  uint8_t update_out[256] = {};
  gcomp_buffer_t in_buf = {input, input_len, 0};
  gcomp_buffer_t out_buf = {update_out, sizeof(update_out), 0};

  ASSERT_EQ(gcomp_encoder_update(encoder_, &in_buf, &out_buf), GCOMP_OK);

  for (size_t i = 0; i < out_buf.used; i++) {
    compressed.push_back(update_out[i]);
  }

  // Finish with 1-byte buffer
  uint8_t one_byte[1] = {};
  int iterations = 0;

  while (iterations < 10000) {
    gcomp_buffer_t finish_buf = {one_byte, 1, 0};
    gcomp_status_t status = gcomp_encoder_finish(encoder_, &finish_buf);

    if (finish_buf.used > 0) {
      compressed.push_back(one_byte[0]);
    }

    if (status == GCOMP_OK) {
      break;
    }

    ASSERT_EQ(status, GCOMP_ERR_LIMIT);
    iterations++;
  }

  ASSERT_LT(iterations, 10000);

  // Clean up and verify
  gcomp_encoder_destroy(encoder_);
  encoder_ = nullptr;

  std::vector<uint8_t> decompressed;
  ASSERT_EQ(decode_data(
                compressed.data(), compressed.size(), decompressed, input_len),
      GCOMP_OK);

  ASSERT_EQ(decompressed.size(), input_len);
  EXPECT_EQ(memcmp(decompressed.data(), input, input_len), 0);
}

TEST_F(DeflateEncoderTest, Finish_SmallBuffer_AllLevels) {
  // Test incremental finish across all compression levels
  const char * input_str = "Test data for all levels with incremental finish";
  const uint8_t * input = (const uint8_t *)input_str;
  size_t input_len = strlen(input_str);

  for (int level = 0; level <= 9; level++) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", level), GCOMP_OK);

    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(gcomp_encoder_create(registry_, "deflate", opts, &enc), GCOMP_OK);
    gcomp_options_destroy(opts);

    // Feed input
    std::vector<uint8_t> compressed;
    uint8_t out[256] = {};
    gcomp_buffer_t in_buf = {input, input_len, 0};
    gcomp_buffer_t out_buf = {out, sizeof(out), 0};

    ASSERT_EQ(gcomp_encoder_update(enc, &in_buf, &out_buf), GCOMP_OK)
        << "Update failed at level " << level;

    for (size_t i = 0; i < out_buf.used; i++) {
      compressed.push_back(out[i]);
    }

    // Incremental finish with 8-byte buffer
    uint8_t small_buf[8] = {};
    int iterations = 0;

    while (iterations < 1000) {
      gcomp_buffer_t finish_buf = {small_buf, sizeof(small_buf), 0};
      gcomp_status_t status = gcomp_encoder_finish(enc, &finish_buf);

      for (size_t i = 0; i < finish_buf.used; i++) {
        compressed.push_back(small_buf[i]);
      }

      if (status == GCOMP_OK) {
        break;
      }

      ASSERT_EQ(status, GCOMP_ERR_LIMIT)
          << "Unexpected status at level " << level;
      iterations++;
    }

    ASSERT_LT(iterations, 1000) << "Too many iterations at level " << level;

    gcomp_encoder_destroy(enc);

    // Verify
    std::vector<uint8_t> decompressed;
    ASSERT_EQ(decode_data(compressed.data(), compressed.size(), decompressed,
                  input_len),
        GCOMP_OK)
        << "Decode failed at level " << level;

    ASSERT_EQ(decompressed.size(), input_len)
        << "Size mismatch at level " << level;
    EXPECT_EQ(memcmp(decompressed.data(), input, input_len), 0)
        << "Data mismatch at level " << level;

    gcomp_decoder_destroy(decoder_);
    decoder_ = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Length and distance code tables
// ---------------------------------------------------------------------------

extern "C" {
uint32_t gcomp_deflate_length_code(uint32_t length);
uint32_t gcomp_deflate_distance_code(uint32_t distance);
}

namespace {

// The base tables of RFC 1951 section 3.2.5, transcribed here from the spec
// rather than from the encoder, so that a typo in one is not copied into the
// other.
const uint16_t kSpecLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17,
    19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint16_t kSpecDistanceBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49,
    65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577};

// The definition, stated as a search: the code is the one whose base is the
// largest that does not exceed the value.  The encoder answers by table; these
// say what the table has to contain.
uint32_t SpecLengthCode(uint32_t length) {
  uint32_t code = 0;
  for (uint32_t i = 0; i < 29; i++) {
    if (length >= kSpecLengthBase[i]) {
      code = 257 + i;
    }
  }
  return code;
}

uint32_t SpecDistanceCode(uint32_t distance) {
  uint32_t code = 0;
  for (uint32_t i = 0; i < 30; i++) {
    if (distance >= kSpecDistanceBase[i]) {
      code = i;
    }
  }
  return code;
}

} // namespace

// Every length the format allows, all 256 of them.
TEST(DeflateEncodeCodeTables, LengthCodeMatchesTheSpecForEveryLength) {
  for (uint32_t length = 3; length <= 258; length++) {
    EXPECT_EQ(gcomp_deflate_length_code(length), SpecLengthCode(length))
        << "length " << length;
  }
}

// Every distance the format allows, all 32768 of them.  The table is split at
// 256 and indexed by (distance - 1) >> 7 above that, so the whole range has to
// be walked to show the split is where the encoder thinks it is.
TEST(DeflateEncodeCodeTables, DistanceCodeMatchesTheSpecForEveryDistance) {
  for (uint32_t distance = 1; distance <= 32768; distance++) {
    EXPECT_EQ(gcomp_deflate_distance_code(distance), SpecDistanceCode(distance))
        << "distance " << distance;
  }
}

// The extra-bits arithmetic elsewhere in the encoder computes
// `value - base[code]`, which is only in range if the value really does fall
// inside that code's span.
TEST(DeflateEncodeCodeTables, EveryValueFallsInsideItsOwnCodeSpan) {
  for (uint32_t length = 3; length <= 258; length++) {
    uint32_t code = gcomp_deflate_length_code(length);
    ASSERT_GE(code, 257u);
    ASSERT_LE(code, 285u);
    EXPECT_GE(length, kSpecLengthBase[code - 257]) << "length " << length;
  }
  for (uint32_t distance = 1; distance <= 32768; distance++) {
    uint32_t code = gcomp_deflate_distance_code(distance);
    ASSERT_LE(code, 29u);
    EXPECT_GE(distance, kSpecDistanceBase[code]) << "distance " << distance;
  }
}

// Out of range in either direction is rejected rather than indexed.
TEST(DeflateEncodeCodeTables, OutOfRangeValuesReturnZero) {
  EXPECT_EQ(gcomp_deflate_length_code(0u), 0u);
  EXPECT_EQ(gcomp_deflate_length_code(2u), 0u);
  EXPECT_EQ(gcomp_deflate_length_code(259u), 0u);
  EXPECT_EQ(gcomp_deflate_length_code(0xFFFFFFFFu), 0u);
  EXPECT_EQ(gcomp_deflate_distance_code(0u), 0u);
  EXPECT_EQ(gcomp_deflate_distance_code(32769u), 0u);
  EXPECT_EQ(gcomp_deflate_distance_code(0xFFFFFFFFu), 0u);
}

// ---------------------------------------------------------------------------
// Lazy matching under the "filtered" strategy
// ---------------------------------------------------------------------------

namespace {

// Bytes shaped like PNG filter output: mostly small values, with short runs
// and near-repeats at short distance, which is what lazy matching is for.
std::vector<uint8_t> FilteredLookingBytes(size_t n) {
  std::vector<uint8_t> v;
  v.reserve(n);
  uint32_t seed = 20260916u;
  while (v.size() < n) {
    seed = seed * 1103515245u + 12345u;
    uint32_t r = seed >> 16;
    size_t run = (r % 9) + 1;
    uint8_t value = static_cast<uint8_t>((r >> 4) % 7u);
    for (size_t i = 0; i < run && v.size() < n; i++) {
      v.push_back(value);
    }
    if ((r & 0x3F) == 0) {
      // An occasional literal that breaks the run, so a match at p is short
      // and a match at p+1 is long - the case the deferral exists to catch.
      seed = seed * 1103515245u + 12345u;
      v.push_back(static_cast<uint8_t>(seed >> 24));
    }
  }
  v.resize(n);
  return v;
}

size_t EncodeWithLevel(gcomp_registry_t * reg, const char * strategy, int level,
    const std::vector<uint8_t> & in, std::vector<uint8_t> & out) {
  gcomp_options_t * opts = nullptr;
  EXPECT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_string(opts, "deflate.strategy", strategy),
      GCOMP_OK);
  if (level > 0) {
    EXPECT_EQ(gcomp_options_set_int64(opts, "deflate.level", level), GCOMP_OK);
  }
  out.assign(in.size() * 2 + 1024, 0);
  size_t used = out.size();
  gcomp_status_t s = gcomp_encode_buffer(reg, "deflate", opts, in.data(),
      in.size(), out.data(), out.size(), &used);
  gcomp_options_destroy(opts);
  EXPECT_EQ(s, GCOMP_OK);
  out.resize(used);
  return used;
}

size_t EncodeWith(gcomp_registry_t * reg, const char * strategy,
    const std::vector<uint8_t> & in, std::vector<uint8_t> & out) {
  return EncodeWithLevel(reg, strategy, 0, in, out);
}

} // namespace

// Lazy matching has to earn its cost. On data shaped like PNG filter output it
// is worth several percent.
//
// DEFAULT now defers matches from level 4 up, the same place zlib switches
// from deflate_fast to deflate_slow, so the two strategies differ only at
// levels 1 to 3 - which is where this asks the question.  FILTERED defers at
// every level; DEFAULT at those levels takes what it finds.
TEST_F(DeflateEncoderTest, FilteredBeatsDefaultAtTheFastLevels) {
  std::vector<uint8_t> in = FilteredLookingBytes(200000);
  for (int level = 1; level <= 3; level++) {
    std::vector<uint8_t> a;
    std::vector<uint8_t> b;
    size_t plain = EncodeWithLevel(registry_, "default", level, in, a);
    size_t lazy = EncodeWithLevel(registry_, "filtered", level, in, b);
    EXPECT_LT(lazy, plain)
        << "level " << level << ": filtered " << lazy << " vs default "
        << plain;
  }
}

// And from level 4 up they are the same encoder.  That is a consequence of
// giving DEFAULT the only thing FILTERED had - this file's own measurements
// put chain depth at 0.1 points across a factor of eight and deferral at 1.7 -
// so it is recorded here rather than left to be discovered.  The option stays
// because it still means something at levels 1 to 3, and because it is a
// documented name that callers may already pass.
TEST_F(DeflateEncoderTest, FilteredMatchesDefaultAtTheSlowLevels) {
  std::vector<uint8_t> in = FilteredLookingBytes(200000);
  for (int level = 4; level <= 9; level++) {
    std::vector<uint8_t> a;
    std::vector<uint8_t> b;
    size_t plain = EncodeWithLevel(registry_, "default", level, in, a);
    size_t lazy = EncodeWithLevel(registry_, "filtered", level, in, b);
    ASSERT_EQ(plain, lazy) << "level " << level;
    EXPECT_EQ(memcmp(a.data(), b.data(), plain), 0) << "level " << level;
  }
}

// Deferring is what a higher level buys here, so the levels that defer have to
// come out smaller than the levels that do not.
TEST_F(DeflateEncoderTest, DefaultImprovesWhenDeferralTurnsOn) {
  std::vector<uint8_t> in = FilteredLookingBytes(200000);
  std::vector<uint8_t> fast;
  std::vector<uint8_t> slow;
  size_t at3 = EncodeWithLevel(registry_, "default", 3, in, fast);
  size_t at4 = EncodeWithLevel(registry_, "default", 4, in, slow);
  EXPECT_LT(at4, at3) << "level 4 " << at4 << " vs level 3 " << at3;
}

// A match held back must be emitted exactly once, whether the stream ends
// while it is held or another match displaces it. Every length here is a
// different place for the stream to end relative to a held match.
TEST_F(DeflateEncoderTest, FilteredRoundTripsAtEveryTailLength) {
  std::vector<uint8_t> base = FilteredLookingBytes(4096);
  for (size_t n = 0; n <= 600; n++) {
    std::vector<uint8_t> in(base.begin(), base.begin() + n);
    std::vector<uint8_t> enc;
    EncodeWith(registry_, "filtered", in, enc);

    std::vector<uint8_t> back;
    ASSERT_EQ(decode_data(enc.data(), enc.size(), back, n), GCOMP_OK)
        << "length " << n;
    ASSERT_EQ(back.size(), n) << "length " << n;
    if (n > 0) {
      EXPECT_EQ(memcmp(back.data(), in.data(), n), 0) << "length " << n;
    }
    gcomp_decoder_destroy(decoder_);
    decoder_ = nullptr;
  }
}

// The deferral lives in the encoder state so that it survives a call boundary.
// Reusing an encoder must not carry a held match into the next stream.
TEST_F(DeflateEncoderTest, FilteredResetDropsAHeldMatch) {
  std::vector<uint8_t> in = FilteredLookingBytes(50000);
  std::vector<uint8_t> first;
  std::vector<uint8_t> second;
  size_t a = EncodeWith(registry_, "filtered", in, first);
  size_t b = EncodeWith(registry_, "filtered", in, second);
  ASSERT_EQ(a, b);
  EXPECT_EQ(memcmp(first.data(), second.data(), a), 0);
}

// ---------------------------------------------------------------------------
// Headers that describe a code at all
// ---------------------------------------------------------------------------

namespace {

// The code-length alphabet of RFC 1951 section 3.2.7 is limited to seven bits.
// Reaching that limit needs a symbol distribution skewed enough that the tree
// wants deeper codes, which the length-limiting step then has to flatten. This
// shape does it: many short runs of a few values, with rare interruptions.
std::vector<uint8_t> SkewedRunData(size_t n) {
  std::vector<uint8_t> v;
  v.reserve(n + 16);
  uint32_t seed = 20260916u;
  while (v.size() < n) {
    seed = seed * 1103515245u + 12345u;
    uint32_t r = seed >> 16;
    size_t run = (r % 9) + 1;
    uint8_t value = static_cast<uint8_t>((r >> 4) % 7u);
    for (size_t i = 0; i < run; i++) {
      v.push_back(value);
    }
    if ((r & 0x3F) == 0) {
      seed = seed * 1103515245u + 12345u;
      v.push_back(static_cast<uint8_t>(seed >> 24));
    }
  }
  v.resize(n);
  return v;
}

// Walk a raw deflate stream's first block header and return the Kraft sum of
// its code-length alphabet, scaled so that a complete code sums to 128.
// Returns 0 if the first block is not a dynamic one.
unsigned CodeLengthKraftSum(const std::vector<uint8_t> & data) {
  static const int kOrder[19] = {
      16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  size_t bit = 0;
  auto get = [&](int count) {
    unsigned value = 0;
    for (int i = 0; i < count; i++) {
      unsigned b = (data[bit >> 3] >> (bit & 7)) & 1u;
      value |= b << i;
      bit++;
    }
    return value;
  };
  get(1);                    // BFINAL
  if (get(2) != 2) {
    return 0;                // not a dynamic block
  }
  get(5);                    // HLIT
  get(5);                    // HDIST
  unsigned hclen = get(4) + 4;
  unsigned lengths[19] = {0};
  for (unsigned i = 0; i < hclen; i++) {
    lengths[kOrder[i]] = get(3);
  }
  unsigned kraft = 0;
  for (int i = 0; i < 19; i++) {
    if (lengths[i]) {
      kraft += 1u << (7 - lengths[i]);
    }
  }
  return kraft;
}

} // namespace

// A set of code lengths whose Kraft sum falls short does not describe a code:
// RFC 1951 section 3.2.2 builds the code by handing out every available code
// word, and zlib refuses such a header with "invalid code lengths set".  The
// length-limiting step used to leave one behind, because lengthening a symbol
// halves its share and so steps past the target as often as it lands on it.
TEST_F(DeflateEncoderTest, CodeLengthAlphabetIsAlwaysComplete) {
  for (size_t n = 120000; n <= 120040; n++) {
    std::vector<uint8_t> in = SkewedRunData(n);
    std::vector<uint8_t> enc;
    EncodeWith(registry_, "filtered", in, enc);
    unsigned kraft = CodeLengthKraftSum(enc);
    if (kraft == 0) {
      continue; // stored or fixed block, nothing to check
    }
    EXPECT_EQ(kraft, 128u) << "length " << n << ": code-length alphabet is "
                           << (kraft < 128u ? "under" : "over")
                           << "-subscribed at " << kraft << "/128";
  }
}

// The whole stream, not just the header: what comes out has to decode back.
TEST_F(DeflateEncoderTest, SkewedRunDataRoundTrips) {
  for (size_t n = 120000; n <= 120020; n++) {
    std::vector<uint8_t> in = SkewedRunData(n);
    std::vector<uint8_t> enc;
    EncodeWith(registry_, "filtered", in, enc);
    std::vector<uint8_t> back;
    ASSERT_EQ(decode_data(enc.data(), enc.size(), back, n), GCOMP_OK)
        << "length " << n;
    ASSERT_EQ(back.size(), n) << "length " << n;
    EXPECT_EQ(memcmp(back.data(), in.data(), n), 0) << "length " << n;
    gcomp_decoder_destroy(decoder_);
    decoder_ = nullptr;
  }
}

// The RLE strategy reads the byte before the current position to find a run at
// distance 1.  A refill can fill the window entirely with lookahead, and the
// byte before the position is then unemitted data rather than history.  Taking
// it from there emitted a distance-1 match against a byte that was never
// written, so a run carried one byte past its end.  The small windows are
// where the lookahead reaches the window size often enough to show it.
TEST_F(DeflateEncoderTest, RleDoesNotMatchAgainstUnwrittenHistory) {
  const size_t n = 40000;
  std::vector<uint8_t> in(n);
  for (size_t i = 0; i < n; i++) {
    in[i] = static_cast<uint8_t>((i / 1000) & 0xFF);
  }
  for (uint64_t window_bits = 8; window_bits <= 12; window_bits++) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_string(opts, "deflate.strategy", "rle"),
        GCOMP_OK);
    ASSERT_EQ(
        gcomp_options_set_uint64(opts, "deflate.window_bits", window_bits),
        GCOMP_OK);
    std::vector<uint8_t> enc(n * 2 + 1024);
    size_t used = enc.size();
    ASSERT_EQ(gcomp_encode_buffer(registry_, "deflate", opts, in.data(), n,
                  enc.data(), enc.size(), &used),
        GCOMP_OK);
    gcomp_options_destroy(opts);
    enc.resize(used);

    std::vector<uint8_t> back;
    ASSERT_EQ(decode_data(enc.data(), enc.size(), back, n), GCOMP_OK)
        << "window_bits " << window_bits;
    ASSERT_EQ(back.size(), n) << "window_bits " << window_bits;
    for (size_t i = 0; i < n; i++) {
      ASSERT_EQ(back[i], in[i])
          << "window_bits " << window_bits << ", first wrong byte at " << i;
    }
    gcomp_decoder_destroy(decoder_);
    decoder_ = nullptr;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
