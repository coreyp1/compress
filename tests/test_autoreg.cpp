/**
 * @file test_autoreg.cpp
 *
 * Unit tests for auto-registration in the Ghoti.io Compress library.
 * Tests that methods auto-register when the library is loaded.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>

/**
 * Test that deflate is automatically registered with the default registry.
 *
 * This test verifies that when the compress library is linked, the deflate
 * method is automatically registered with the default registry before main()
 * runs (via constructor functions).
 */
TEST(AutoRegistrationTest, DeflateAutoRegistered) {
  gcomp_registry_t * reg = gcomp_registry_default();
  ASSERT_NE(reg, nullptr);

  // Deflate should already be registered due to auto-registration
  const gcomp_method_t * method = gcomp_registry_find(reg, "deflate");
  ASSERT_NE(method, nullptr) << "deflate should be auto-registered";
  EXPECT_STREQ(method->name, "deflate");
  EXPECT_TRUE(method->capabilities & GCOMP_CAP_ENCODE);
  EXPECT_TRUE(method->capabilities & GCOMP_CAP_DECODE);
}

/**
 * Test that explicit registration is idempotent with auto-registration.
 *
 * Since deflate is already auto-registered, calling the explicit registration
 * function should succeed (or be a no-op) and not cause any issues.
 */
TEST(AutoRegistrationTest, ExplicitRegistrationIdempotent) {
  gcomp_registry_t * reg = gcomp_registry_default();
  ASSERT_NE(reg, nullptr);

  // Call explicit registration (deflate is already auto-registered)
  gcomp_status_t status = gcomp_method_deflate_register(reg);
  EXPECT_EQ(status, GCOMP_OK);

  // Verify deflate is still findable
  const gcomp_method_t * method = gcomp_registry_find(reg, "deflate");
  ASSERT_NE(method, nullptr);
  EXPECT_STREQ(method->name, "deflate");
}

/**
 * Test that explicit registration works with a custom registry.
 *
 * Auto-registration only registers with the default registry. Methods can
 * still be explicitly registered with custom registries.
 */
TEST(AutoRegistrationTest, ExplicitRegistrationCustomRegistry) {
  gcomp_registry_t * custom_reg = nullptr;
  gcomp_status_t status = gcomp_registry_create(nullptr, &custom_reg);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(custom_reg, nullptr);

  // Custom registry should be empty initially
  const gcomp_method_t * before = gcomp_registry_find(custom_reg, "deflate");
  EXPECT_EQ(before, nullptr)
      << "Custom registry should not have deflate until registered";

  // Explicitly register deflate with custom registry
  status = gcomp_method_deflate_register(custom_reg);
  EXPECT_EQ(status, GCOMP_OK);

  // Now it should be findable
  const gcomp_method_t * after = gcomp_registry_find(custom_reg, "deflate");
  ASSERT_NE(after, nullptr);
  EXPECT_STREQ(after->name, "deflate");

  gcomp_registry_destroy(custom_reg);
}

/**
 * Test that the auto-registered method has all expected properties.
 *
 * Verifies that the auto-registered deflate method has the correct
 * ABI version, capabilities, and function pointers.
 */
TEST(AutoRegistrationTest, DeflateMethodProperties) {
  gcomp_registry_t * reg = gcomp_registry_default();
  const gcomp_method_t * method = gcomp_registry_find(reg, "deflate");
  ASSERT_NE(method, nullptr);

  // Check ABI version
  EXPECT_EQ(method->abi_version, 1);

  // Check size
  EXPECT_EQ(method->size, sizeof(gcomp_method_t));

  // Check capabilities
  EXPECT_TRUE(method->capabilities & GCOMP_CAP_ENCODE);
  EXPECT_TRUE(method->capabilities & GCOMP_CAP_DECODE);

  // Check function pointers are set
  EXPECT_NE(method->create_encoder, nullptr);
  EXPECT_NE(method->create_decoder, nullptr);
  EXPECT_NE(method->destroy_encoder, nullptr);
  EXPECT_NE(method->destroy_decoder, nullptr);
  EXPECT_NE(method->get_schema, nullptr);
}

/**
 * Test that auto-registered deflate can be used for encoding/decoding.
 *
 * Creates an encoder and decoder using the auto-registered method and
 * verifies they work correctly.
 */
TEST(AutoRegistrationTest, DeflateUsableAfterAutoRegistration) {
  gcomp_registry_t * reg = gcomp_registry_default();

  // Create encoder using auto-registered deflate
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(reg, "deflate", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(encoder, nullptr);

  // Create decoder using auto-registered deflate
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(reg, "deflate", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(decoder, nullptr);

  // Clean up
  gcomp_encoder_destroy(encoder);
  gcomp_decoder_destroy(decoder);
}

/**
 * Test round-trip compression/decompression with auto-registered deflate.
 */
TEST(AutoRegistrationTest, DeflateRoundTrip) {
  gcomp_registry_t * reg = gcomp_registry_default();

  // Test data
  const char * input_str = "Hello, auto-registration test!";
  size_t input_len = strlen(input_str);
  const uint8_t * input = reinterpret_cast<const uint8_t *>(input_str);

  // Compress
  uint8_t compressed[256];
  size_t compressed_len = 0;
  gcomp_status_t status = gcomp_encode_buffer(reg, "deflate", nullptr, input,
      input_len, compressed, sizeof(compressed), &compressed_len);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_GT(compressed_len, 0u);

  // Decompress
  uint8_t decompressed[256];
  size_t decompressed_len = 0;
  status = gcomp_decode_buffer(reg, "deflate", nullptr, compressed,
      compressed_len, decompressed, sizeof(decompressed), &decompressed_len);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decompressed_len, input_len);
  EXPECT_EQ(memcmp(decompressed, input, input_len), 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
