/**
 * @file test_buffer_wrappers.cpp
 *
 * Unit tests for buffer-to-buffer convenience wrappers in the Ghoti.io
 * Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <gtest/gtest.h>

// Include internal header for testing
#include "../src/core/stream_internal.h"

// Pass-through encoder update: copies input to output
static gcomp_status_t passthru_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  (void)encoder;

  if (!input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  size_t to_copy = input->size - input->used;
  size_t available = output->size - output->used;

  if (to_copy > available) {
    to_copy = available;
  }

  if (to_copy > 0 && input->data != NULL && output->data != NULL) {
    memcpy((uint8_t *)output->data + output->used,
        (const uint8_t *)input->data + input->used, to_copy);
    input->used += to_copy;
    output->used += to_copy;
  }

  return GCOMP_OK;
}

// Pass-through encoder finish: no-op
static gcomp_status_t passthru_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  (void)encoder;
  (void)output;
  return GCOMP_OK;
}

// Pass-through decoder update: copies input to output (same as encoder)
static gcomp_status_t passthru_decoder_update(gcomp_decoder_t * decoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  (void)decoder;

  if (!input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  size_t to_copy = input->size - input->used;
  size_t available = output->size - output->used;

  if (to_copy > available) {
    to_copy = available;
  }

  if (to_copy > 0 && input->data != NULL && output->data != NULL) {
    memcpy((uint8_t *)output->data + output->used,
        (const uint8_t *)input->data + input->used, to_copy);
    input->used += to_copy;
    output->used += to_copy;
  }

  return GCOMP_OK;
}

// Pass-through decoder finish: no-op
static gcomp_status_t passthru_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  (void)decoder;
  (void)output;
  return GCOMP_OK;
}

// Mock create_encoder that sets up the encoder properly
static gcomp_status_t passthru_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  (void)registry;
  (void)options;

  // The encoder structure is already allocated by gcomp_encoder_create
  // We just need to set up the function pointers
  if (encoder_out && *encoder_out) {
    gcomp_encoder_t * encoder = *encoder_out;
    encoder->update_fn = passthru_encoder_update;
    encoder->finish_fn = passthru_encoder_finish;
  }

  return GCOMP_OK;
}

// Mock create_decoder that sets up the decoder properly
static gcomp_status_t passthru_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  (void)registry;
  (void)options;

  // The decoder structure is already allocated by gcomp_decoder_create
  // We just need to set up the function pointers
  if (decoder_out && *decoder_out) {
    gcomp_decoder_t * decoder = *decoder_out;
    decoder->update_fn = passthru_decoder_update;
    decoder->finish_fn = passthru_decoder_finish;
  }

  return GCOMP_OK;
}

static void passthru_destroy_encoder(gcomp_encoder_t * encoder) {
  (void)encoder;
  // No-op for pass-through
}

static void passthru_destroy_decoder(gcomp_decoder_t * decoder) {
  (void)decoder;
  // No-op for pass-through
}

// Create a pass-through method for testing
static gcomp_method_t create_passthru_method(const char * name) {
  gcomp_method_t method = {};
  method.abi_version = 1;
  method.size = sizeof(gcomp_method_t);
  method.name = name;
  method.capabilities =
      static_cast<gcomp_capabilities_t>(GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE);
  method.create_encoder = passthru_create_encoder;
  method.create_decoder = passthru_create_decoder;
  method.destroy_encoder = passthru_destroy_encoder;
  method.destroy_decoder = passthru_destroy_decoder;
  return method;
}

class BufferWrappersTest : public ::testing::Test {
protected:
  void SetUp() override {
    gcomp_status_t status = gcomp_registry_create(nullptr, &registry_);
    ASSERT_EQ(status, GCOMP_OK);
    ASSERT_NE(registry_, nullptr);

    // Register pass-through method (store as member to keep it alive)
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
