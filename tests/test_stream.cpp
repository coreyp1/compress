/**
 * @file test_stream.cpp
 *
 * Unit tests for the stream API in the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>

// Include internal header for testing
#include "../src/core/stream_internal.h"

// Mock update/finish functions for testing
static gcomp_status_t mock_encoder_update(
    gcomp_encoder_t * encoder, gcomp_buffer_t * input, gcomp_buffer_t * output) {
  (void)encoder;
  (void)input;
  (void)output;
  return GCOMP_OK;
}

static gcomp_status_t mock_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  (void)encoder;
  (void)output;
  return GCOMP_OK;
}

static gcomp_status_t mock_decoder_update(
    gcomp_decoder_t * decoder, gcomp_buffer_t * input, gcomp_buffer_t * output) {
  (void)decoder;
  (void)input;
  (void)output;
  return GCOMP_OK;
}

static gcomp_status_t mock_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output) {
  (void)decoder;
  (void)output;
  return GCOMP_OK;
}

// Mock create_encoder that sets up the encoder properly
static gcomp_status_t mock_create_encoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t ** encoder_out) {
  (void)registry;
  (void)options;

  // The encoder structure is already allocated by gcomp_encoder_create
  // We just need to set up the function pointers
  if (encoder_out && *encoder_out) {
    gcomp_encoder_t * encoder = *encoder_out;
    encoder->update_fn = mock_encoder_update;
    encoder->finish_fn = mock_encoder_finish;
  }

  return GCOMP_OK;
}

// Mock create_decoder that sets up the decoder properly
static gcomp_status_t mock_create_decoder(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t ** decoder_out) {
  (void)registry;
  (void)options;

  // The decoder structure is already allocated by gcomp_decoder_create
  // We just need to set up the function pointers
  if (decoder_out && *decoder_out) {
    gcomp_decoder_t * decoder = *decoder_out;
    decoder->update_fn = mock_decoder_update;
    decoder->finish_fn = mock_decoder_finish;
  }

  return GCOMP_OK;
}

static void mock_destroy_encoder(gcomp_encoder_t * encoder) {
  (void)encoder;
  // No-op for mock
}

static void mock_destroy_decoder(gcomp_decoder_t * decoder) {
  (void)decoder;
  // No-op for mock
}

// Create a mock method for testing
static gcomp_method_t create_mock_method(
    const char * name, gcomp_capabilities_t caps) {
  gcomp_method_t method = {};
  method.abi_version = 1;
  method.size = sizeof(gcomp_method_t);
  method.name = name;
  method.capabilities = caps;
  method.create_encoder = mock_create_encoder;
  method.create_decoder = mock_create_decoder;
  method.destroy_encoder = mock_destroy_encoder;
  method.destroy_decoder = mock_destroy_decoder;
  return method;
}

class StreamTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create a fresh registry for each test
    gcomp_registry_create(nullptr, &registry_);
    ASSERT_NE(registry_, nullptr);

    // Register a mock method that supports both encode and decode
    // Store as member variable so it stays in scope
    method_ = create_mock_method("test_method",
        static_cast<gcomp_capabilities_t>(GCOMP_CAP_ENCODE | GCOMP_CAP_DECODE));
    ASSERT_EQ(gcomp_registry_register(registry_, &method_), GCOMP_OK);
  }

  void TearDown() override {
    if (encoder_ != nullptr) {
      gcomp_encoder_destroy(encoder_);
      encoder_ = nullptr;
    }
    if (decoder_ != nullptr) {
      gcomp_decoder_destroy(decoder_);
      decoder_ = nullptr;
    }
    if (registry_ != nullptr) {
      gcomp_registry_destroy(registry_);
      registry_ = nullptr;
    }
  }

  gcomp_registry_t * registry_ = nullptr;
  gcomp_encoder_t * encoder_ = nullptr;
  gcomp_decoder_t * decoder_ = nullptr;
  gcomp_method_t method_; // Store method so pointer remains valid
};

// Test gcomp_encoder_create()
TEST_F(StreamTest, EncoderCreateSuccess) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status = gcomp_encoder_create(
      registry_, "test_method", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  // Clean up
  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, EncoderCreateNullRegistry) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(nullptr, "test_method", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(encoder, nullptr);
}

TEST_F(StreamTest, EncoderCreateNullMethodName) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, nullptr, nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(encoder, nullptr);
}

TEST_F(StreamTest, EncoderCreateNullEncoderOut) {
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "test_method", nullptr, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(StreamTest, EncoderCreateInvalidMethodName) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "nonexistent", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(encoder, nullptr);
}

TEST_F(StreamTest, EncoderCreateMethodWithoutEncodeCapability) {
  // Register a method that only supports decode
  gcomp_method_t decode_only = create_mock_method("decode_only", GCOMP_CAP_DECODE);
  ASSERT_EQ(gcomp_registry_register(registry_, &decode_only), GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "decode_only", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(encoder, nullptr);
}

TEST_F(StreamTest, EncoderCreateMethodWithoutCreateFunction) {
  // Register a method with NULL create_encoder
  gcomp_method_t method = create_mock_method("no_create", GCOMP_CAP_ENCODE);
  method.create_encoder = nullptr;
  ASSERT_EQ(gcomp_registry_register(registry_, &method), GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "no_create", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(encoder, nullptr);
}

// Test gcomp_decoder_create()
TEST_F(StreamTest, DecoderCreateSuccess) {
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status = gcomp_decoder_create(
      registry_, "test_method", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(decoder, nullptr);

  // Clean up
  gcomp_decoder_destroy(decoder);
}

TEST_F(StreamTest, DecoderCreateNullRegistry) {
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(nullptr, "test_method", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(decoder, nullptr);
}

TEST_F(StreamTest, DecoderCreateNullMethodName) {
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, nullptr, nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(decoder, nullptr);
}

TEST_F(StreamTest, DecoderCreateNullDecoderOut) {
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "test_method", nullptr, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(StreamTest, DecoderCreateInvalidMethodName) {
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "nonexistent", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(decoder, nullptr);
}

TEST_F(StreamTest, DecoderCreateMethodWithoutDecodeCapability) {
  // Register a method that only supports encode
  gcomp_method_t encode_only = create_mock_method("encode_only", GCOMP_CAP_ENCODE);
  ASSERT_EQ(gcomp_registry_register(registry_, &encode_only), GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "encode_only", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(decoder, nullptr);
}

TEST_F(StreamTest, DecoderCreateMethodWithoutCreateFunction) {
  // Register a method with NULL create_decoder
  gcomp_method_t method = create_mock_method("no_create", GCOMP_CAP_DECODE);
  method.create_decoder = nullptr;
  ASSERT_EQ(gcomp_registry_register(registry_, &method), GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "no_create", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(decoder, nullptr);
}

// Test gcomp_encoder_destroy()
TEST_F(StreamTest, EncoderDestroyNullPointer) {
  // Should not crash
  gcomp_encoder_destroy(nullptr);
}

TEST_F(StreamTest, EncoderDestroyCleanup) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);
  ASSERT_NE(encoder, nullptr);

  // Destroy should clean up
  gcomp_encoder_destroy(encoder);
  // If we get here without crashing, cleanup worked
}

// Test gcomp_decoder_destroy()
TEST_F(StreamTest, DecoderDestroyNullPointer) {
  // Should not crash
  gcomp_decoder_destroy(nullptr);
}

TEST_F(StreamTest, DecoderDestroyCleanup) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "test_method", nullptr, &decoder),
      GCOMP_OK);
  ASSERT_NE(decoder, nullptr);

  // Destroy should clean up
  gcomp_decoder_destroy(decoder);
  // If we get here without crashing, cleanup worked
}

// Test encoder/decoder creation with options
TEST_F(StreamTest, EncoderCreateWithOptions) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "test_method", opts, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

TEST_F(StreamTest, DecoderCreateWithOptions) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_NE(opts, nullptr);

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "test_method", opts, &decoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(decoder, nullptr);

  gcomp_decoder_destroy(decoder);
  gcomp_options_destroy(opts);
}

// Test update/finish with NULL function pointers (should return INTERNAL error)
TEST_F(StreamTest, EncoderUpdateWithoutFunction) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);
  ASSERT_NE(encoder, nullptr);

  // Manually clear the update function to test error path
  encoder->update_fn = nullptr;

  gcomp_buffer_t input = {};
  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_encoder_update(encoder, &input, &output);
  EXPECT_EQ(status, GCOMP_ERR_INTERNAL);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, EncoderFinishWithoutFunction) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);
  ASSERT_NE(encoder, nullptr);

  // Manually clear the finish function to test error path
  encoder->finish_fn = nullptr;

  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_encoder_finish(encoder, &output);
  EXPECT_EQ(status, GCOMP_ERR_INTERNAL);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, DecoderUpdateWithoutFunction) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "test_method", nullptr, &decoder),
      GCOMP_OK);
  ASSERT_NE(decoder, nullptr);

  // Manually clear the update function to test error path
  decoder->update_fn = nullptr;

  gcomp_buffer_t input = {};
  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_decoder_update(decoder, &input, &output);
  EXPECT_EQ(status, GCOMP_ERR_INTERNAL);

  gcomp_decoder_destroy(decoder);
}

TEST_F(StreamTest, DecoderFinishWithoutFunction) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "test_method", nullptr, &decoder),
      GCOMP_OK);
  ASSERT_NE(decoder, nullptr);

  // Manually clear the finish function to test error path
  decoder->finish_fn = nullptr;

  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_decoder_finish(decoder, &output);
  EXPECT_EQ(status, GCOMP_ERR_INTERNAL);

  gcomp_decoder_destroy(decoder);
}

// Test update/finish with NULL arguments
TEST_F(StreamTest, EncoderUpdateNullEncoder) {
  gcomp_buffer_t input = {};
  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_encoder_update(nullptr, &input, &output);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(StreamTest, EncoderUpdateNullInput) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);

  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_encoder_update(encoder, nullptr, &output);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, EncoderUpdateNullOutput) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);

  gcomp_buffer_t input = {};
  gcomp_status_t status = gcomp_encoder_update(encoder, &input, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, EncoderFinishNullEncoder) {
  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_encoder_finish(nullptr, &output);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(StreamTest, EncoderFinishNullOutput) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);

  gcomp_status_t status = gcomp_encoder_finish(encoder, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, DecoderUpdateNullDecoder) {
  gcomp_buffer_t input = {};
  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_decoder_update(nullptr, &input, &output);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(StreamTest, DecoderUpdateNullInput) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "test_method", nullptr, &decoder),
      GCOMP_OK);

  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_decoder_update(decoder, nullptr, &output);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_decoder_destroy(decoder);
}

TEST_F(StreamTest, DecoderUpdateNullOutput) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "test_method", nullptr, &decoder),
      GCOMP_OK);

  gcomp_buffer_t input = {};
  gcomp_status_t status = gcomp_decoder_update(decoder, &input, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_decoder_destroy(decoder);
}

TEST_F(StreamTest, DecoderFinishNullDecoder) {
  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_decoder_finish(nullptr, &output);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(StreamTest, DecoderFinishNullOutput) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "test_method", nullptr, &decoder),
      GCOMP_OK);

  gcomp_status_t status = gcomp_decoder_finish(decoder, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  gcomp_decoder_destroy(decoder);
}

// Test that update/finish functions are called correctly
TEST_F(StreamTest, EncoderUpdateCallsFunction) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);
  ASSERT_NE(encoder, nullptr);
  ASSERT_NE(encoder->update_fn, nullptr);

  gcomp_buffer_t input = {};
  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_encoder_update(encoder, &input, &output);
  // Mock function returns GCOMP_OK
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, EncoderFinishCallsFunction) {
  gcomp_encoder_t * encoder = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "test_method", nullptr, &encoder),
      GCOMP_OK);
  ASSERT_NE(encoder, nullptr);
  ASSERT_NE(encoder->finish_fn, nullptr);

  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_encoder_finish(encoder, &output);
  // Mock function returns GCOMP_OK
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StreamTest, DecoderUpdateCallsFunction) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "test_method", nullptr, &decoder),
      GCOMP_OK);
  ASSERT_NE(decoder, nullptr);
  ASSERT_NE(decoder->update_fn, nullptr);

  gcomp_buffer_t input = {};
  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_decoder_update(decoder, &input, &output);
  // Mock function returns GCOMP_OK
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_decoder_destroy(decoder);
}

TEST_F(StreamTest, DecoderFinishCallsFunction) {
  gcomp_decoder_t * decoder = nullptr;
  ASSERT_EQ(gcomp_decoder_create(registry_, "test_method", nullptr, &decoder),
      GCOMP_OK);
  ASSERT_NE(decoder, nullptr);
  ASSERT_NE(decoder->finish_fn, nullptr);

  gcomp_buffer_t output = {};
  gcomp_status_t status = gcomp_decoder_finish(decoder, &output);
  // Mock function returns GCOMP_OK
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_decoder_destroy(decoder);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
