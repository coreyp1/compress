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
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
