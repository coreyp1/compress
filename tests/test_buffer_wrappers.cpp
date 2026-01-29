/**
 * @file test_buffer_wrappers.cpp
 *
 * Unit tests for buffer-to-buffer convenience wrappers in the Ghoti.io
 * Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "passthru_method.h"
#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <gtest/gtest.h>

using namespace gcomp_test;

class BufferWrappersTest : public ::testing::Test {
protected:
  void SetUp() override {
    gcomp_status_t status = gcomp_registry_create(nullptr, &registry_);
    ASSERT_EQ(status, GCOMP_OK);
    ASSERT_NE(registry_, nullptr);

    // Register pass-thru method (store as member to keep it alive)
    passthru_method_ = create_passthru_method("passthru");
    status = gcomp_registry_register(registry_, &passthru_method_);
    ASSERT_EQ(status, GCOMP_OK);
  }

  void TearDown() override {
    if (registry_) {
      gcomp_registry_destroy(registry_);
      registry_ = nullptr;
    }
  }

  gcomp_registry_t * registry_;
  gcomp_method_t passthru_method_; // Keep method alive for registry
};

// Test gcomp_encode_buffer() - basic encoding
TEST_F(BufferWrappersTest, EncodeBuffer_Basic) {
  const uint8_t input[] = {'H', 'e', 'l', 'l', 'o'};
  const size_t input_size = sizeof(input);
  uint8_t output[1024];
  size_t output_size = 0;

  gcomp_status_t status = gcomp_encode_buffer(registry_, "passthru", nullptr,
      input, input_size, output, sizeof(output), &output_size);

  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(output_size, input_size);
  ASSERT_EQ(memcmp(input, output, input_size), 0);
}

// Test gcomp_encode_buffer() - NULL pointer handling
TEST_F(BufferWrappersTest, EncodeBuffer_NullPointers) {
  const uint8_t input[] = {'H', 'e', 'l', 'l', 'o'};
  uint8_t output[1024];
  size_t output_size = 0;

  // NULL method_name
  gcomp_status_t status = gcomp_encode_buffer(registry_, nullptr, nullptr,
      input, sizeof(input), output, sizeof(output), &output_size);
  ASSERT_EQ(status, GCOMP_ERR_INVALID_ARG);

  // NULL input_data
  status = gcomp_encode_buffer(registry_, "passthru", nullptr, nullptr,
      sizeof(input), output, sizeof(output), &output_size);
  ASSERT_EQ(status, GCOMP_ERR_INVALID_ARG);

  // NULL output_data
  status = gcomp_encode_buffer(registry_, "passthru", nullptr, input,
      sizeof(input), nullptr, sizeof(output), &output_size);
  ASSERT_EQ(status, GCOMP_ERR_INVALID_ARG);

  // NULL output_size_out
  status = gcomp_encode_buffer(registry_, "passthru", nullptr, input,
      sizeof(input), output, sizeof(output), nullptr);
  ASSERT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

// Test gcomp_encode_buffer() - empty input
TEST_F(BufferWrappersTest, EncodeBuffer_EmptyInput) {
  const uint8_t * input = nullptr;
  uint8_t output[1024];
  size_t output_size = 0;

  gcomp_status_t status = gcomp_encode_buffer(registry_, "passthru", nullptr,
      input, 0, output, sizeof(output), &output_size);

  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(output_size, 0);
}

// Test gcomp_encode_buffer() - output buffer too small
TEST_F(BufferWrappersTest, EncodeBuffer_OutputTooSmall) {
  const uint8_t input[] = {'H', 'e', 'l', 'l', 'o'};
  uint8_t output[2]; // Too small
  size_t output_size = 0;

  gcomp_status_t status = gcomp_encode_buffer(registry_, "passthru", nullptr,
      input, sizeof(input), output, sizeof(output), &output_size);

  ASSERT_EQ(status, GCOMP_ERR_LIMIT);
}

// Test gcomp_encode_buffer() - invalid method
TEST_F(BufferWrappersTest, EncodeBuffer_InvalidMethod) {
  const uint8_t input[] = {'H', 'e', 'l', 'l', 'o'};
  uint8_t output[1024];
  size_t output_size = 0;

  gcomp_status_t status = gcomp_encode_buffer(registry_, "nonexistent", nullptr,
      input, sizeof(input), output, sizeof(output), &output_size);

  ASSERT_EQ(status, GCOMP_ERR_UNSUPPORTED);
}

// Test gcomp_encode_buffer() - use default registry
TEST_F(BufferWrappersTest, EncodeBuffer_DefaultRegistry) {
  // Register method in default registry
  gcomp_registry_t * default_reg = gcomp_registry_default();
  ASSERT_NE(default_reg, nullptr);

  gcomp_method_t passthru = create_passthru_method("passthru_default");
  gcomp_status_t status = gcomp_registry_register(default_reg, &passthru);
  ASSERT_EQ(status, GCOMP_OK);

  const uint8_t input[] = {'H', 'e', 'l', 'l', 'o'};
  uint8_t output[1024];
  size_t output_size = 0;

  // Use NULL registry to use default
  status = gcomp_encode_buffer(nullptr, "passthru_default", nullptr, input,
      sizeof(input), output, sizeof(output), &output_size);

  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(output_size, sizeof(input));
  ASSERT_EQ(memcmp(input, output, sizeof(input)), 0);
}

// Test gcomp_decode_buffer() - basic decoding
TEST_F(BufferWrappersTest, DecodeBuffer_Basic) {
  const uint8_t input[] = {'H', 'e', 'l', 'l', 'o'};
  const size_t input_size = sizeof(input);
  uint8_t output[1024];
  size_t output_size = 0;

  gcomp_status_t status = gcomp_decode_buffer(registry_, "passthru", nullptr,
      input, input_size, output, sizeof(output), &output_size);

  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(output_size, input_size);
  ASSERT_EQ(memcmp(input, output, input_size), 0);
}

// Test gcomp_decode_buffer() - NULL pointer handling
TEST_F(BufferWrappersTest, DecodeBuffer_NullPointers) {
  const uint8_t input[] = {'H', 'e', 'l', 'l', 'o'};
  uint8_t output[1024];
  size_t output_size = 0;

  // NULL method_name
  gcomp_status_t status = gcomp_decode_buffer(registry_, nullptr, nullptr,
      input, sizeof(input), output, sizeof(output), &output_size);
  ASSERT_EQ(status, GCOMP_ERR_INVALID_ARG);

  // NULL input_data
  status = gcomp_decode_buffer(registry_, "passthru", nullptr, nullptr,
      sizeof(input), output, sizeof(output), &output_size);
  ASSERT_EQ(status, GCOMP_ERR_INVALID_ARG);

  // NULL output_data
  status = gcomp_decode_buffer(registry_, "passthru", nullptr, input,
      sizeof(input), nullptr, sizeof(output), &output_size);
  ASSERT_EQ(status, GCOMP_ERR_INVALID_ARG);

  // NULL output_size_out
  status = gcomp_decode_buffer(registry_, "passthru", nullptr, input,
      sizeof(input), output, sizeof(output), nullptr);
  ASSERT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

// Test gcomp_decode_buffer() - empty input
TEST_F(BufferWrappersTest, DecodeBuffer_EmptyInput) {
  const uint8_t * input = nullptr;
  uint8_t output[1024];
  size_t output_size = 0;

  gcomp_status_t status = gcomp_decode_buffer(registry_, "passthru", nullptr,
      input, 0, output, sizeof(output), &output_size);

  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(output_size, 0);
}

// Test gcomp_decode_buffer() - output buffer too small
TEST_F(BufferWrappersTest, DecodeBuffer_OutputTooSmall) {
  const uint8_t input[] = {'H', 'e', 'l', 'l', 'o'};
  uint8_t output[2]; // Too small
  size_t output_size = 0;

  gcomp_status_t status = gcomp_decode_buffer(registry_, "passthru", nullptr,
      input, sizeof(input), output, sizeof(output), &output_size);

  ASSERT_EQ(status, GCOMP_ERR_LIMIT);
}

// Test gcomp_decode_buffer() - invalid method
TEST_F(BufferWrappersTest, DecodeBuffer_InvalidMethod) {
  const uint8_t input[] = {'H', 'e', 'l', 'l', 'o'};
  uint8_t output[1024];
  size_t output_size = 0;

  gcomp_status_t status = gcomp_decode_buffer(registry_, "nonexistent", nullptr,
      input, sizeof(input), output, sizeof(output), &output_size);

  ASSERT_EQ(status, GCOMP_ERR_UNSUPPORTED);
}

// Test round-trip - encode then decode matches input
TEST_F(BufferWrappersTest, RoundTrip) {
  const uint8_t input[] = {
      'H', 'e', 'l', 'l', 'o', ',', ' ', 'W', 'o', 'r', 'l', 'd', '!'};
  const size_t input_size = sizeof(input);

  // Encode
  uint8_t encoded[1024];
  size_t encoded_size = 0;
  gcomp_status_t status = gcomp_encode_buffer(registry_, "passthru", nullptr,
      input, input_size, encoded, sizeof(encoded), &encoded_size);
  ASSERT_EQ(status, GCOMP_OK);

  // Decode
  uint8_t decoded[1024];
  size_t decoded_size = 0;
  status = gcomp_decode_buffer(registry_, "passthru", nullptr, encoded,
      encoded_size, decoded, sizeof(decoded), &decoded_size);
  ASSERT_EQ(status, GCOMP_OK);

  // Verify
  ASSERT_EQ(decoded_size, input_size);
  ASSERT_EQ(memcmp(input, decoded, input_size), 0);
}

// Test large input
TEST_F(BufferWrappersTest, LargeInput) {
  // Create a 64KB input
  const size_t input_size = 64 * 1024;
  uint8_t * input = new uint8_t[input_size];
  for (size_t i = 0; i < input_size; i++) {
    input[i] = (uint8_t)(i & 0xFF);
  }

  uint8_t * output = new uint8_t[input_size + 1024];
  size_t output_size = 0;

  gcomp_status_t status = gcomp_encode_buffer(registry_, "passthru", nullptr,
      input, input_size, output, input_size + 1024, &output_size);

  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(output_size, input_size);
  ASSERT_EQ(memcmp(input, output, input_size), 0);

  delete[] input;
  delete[] output;
}

// Test with options
TEST_F(BufferWrappersTest, WithOptions) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  // Set some option (passthru ignores it, but we test the API)
  status = gcomp_options_set_int64(opts, "test.option", 42);
  ASSERT_EQ(status, GCOMP_OK);

  const uint8_t input[] = {'H', 'e', 'l', 'l', 'o'};
  uint8_t output[1024];
  size_t output_size = 0;

  status = gcomp_encode_buffer(registry_, "passthru", opts, input,
      sizeof(input), output, sizeof(output), &output_size);

  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(output_size, sizeof(input));

  gcomp_options_destroy(opts);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
