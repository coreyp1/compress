/**
 * @file test_gzip_register.cpp
 *
 * Unit tests for gzip method registration in the Ghoti.io Compress library.
 *
 * These tests verify:
 * - Explicit registration to custom registry
 * - Registration fails gracefully when deflate not registered
 * - Method found after registration
 * - Memory cleanup (verify with valgrind)
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/gzip.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>

//
// Test fixture that creates a fresh registry for each test
//

class GzipRegisterTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create a fresh registry for each test
    gcomp_status_t status = gcomp_registry_create(nullptr, &registry_);
    ASSERT_EQ(status, GCOMP_OK);
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    if (registry_ != nullptr) {
      gcomp_registry_destroy(registry_);
      registry_ = nullptr;
    }
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Tests using the default registry (with auto-registration)
//

TEST(GzipDefaultRegistryTest, AutoRegistered) {
  // The gzip method should be auto-registered in the default registry
  gcomp_registry_t * reg = gcomp_registry_default();
  ASSERT_NE(reg, nullptr);

  const gcomp_method_t * method = gcomp_registry_find(reg, "gzip");
  EXPECT_NE(method, nullptr);
  if (method) {
    EXPECT_STREQ(method->name, "gzip");
    EXPECT_TRUE(method->capabilities & GCOMP_CAP_ENCODE);
    EXPECT_TRUE(method->capabilities & GCOMP_CAP_DECODE);
  }
}

TEST(GzipDefaultRegistryTest, DeflateAlsoRegistered) {
  // The deflate method should also be auto-registered (gzip depends on it)
  gcomp_registry_t * reg = gcomp_registry_default();
  ASSERT_NE(reg, nullptr);

  const gcomp_method_t * deflate = gcomp_registry_find(reg, "deflate");
  EXPECT_NE(deflate, nullptr);
  if (deflate) {
    EXPECT_STREQ(deflate->name, "deflate");
  }
}

//
// Tests using a custom registry (explicit registration)
//

TEST_F(GzipRegisterTest, ExplicitRegistrationWithDeflate) {
  // Register deflate first, then gzip
  gcomp_status_t status = gcomp_method_deflate_register(registry_);
  EXPECT_EQ(status, GCOMP_OK);

  status = gcomp_method_gzip_register(registry_);
  EXPECT_EQ(status, GCOMP_OK);

  // Both methods should be found
  const gcomp_method_t * deflate = gcomp_registry_find(registry_, "deflate");
  EXPECT_NE(deflate, nullptr);

  const gcomp_method_t * gzip = gcomp_registry_find(registry_, "gzip");
  EXPECT_NE(gzip, nullptr);
  if (gzip) {
    EXPECT_STREQ(gzip->name, "gzip");
    EXPECT_TRUE(gzip->capabilities & GCOMP_CAP_ENCODE);
    EXPECT_TRUE(gzip->capabilities & GCOMP_CAP_DECODE);
  }
}

TEST_F(GzipRegisterTest, RegistrationWithNullRegistry) {
  // Registering with NULL registry should return error
  gcomp_status_t status = gcomp_method_gzip_register(nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(GzipRegisterTest, EncoderCreationFailsWithoutDeflate) {
  // Register gzip WITHOUT registering deflate first
  gcomp_status_t status = gcomp_method_gzip_register(registry_);
  EXPECT_EQ(status, GCOMP_OK); // Registration itself succeeds

  // Try to create an encoder - should fail because deflate is not registered
  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(encoder, nullptr);
}

TEST_F(GzipRegisterTest, DecoderCreationFailsWithoutDeflate) {
  // Register gzip WITHOUT registering deflate first
  gcomp_status_t status = gcomp_method_gzip_register(registry_);
  EXPECT_EQ(status, GCOMP_OK); // Registration itself succeeds

  // Try to create a decoder - should fail because deflate is not registered
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(decoder, nullptr);
}

TEST_F(GzipRegisterTest, MethodCapabilities) {
  // Register both methods
  gcomp_method_deflate_register(registry_);
  gcomp_method_gzip_register(registry_);

  const gcomp_method_t * method = gcomp_registry_find(registry_, "gzip");
  ASSERT_NE(method, nullptr);

  // Verify capabilities
  EXPECT_TRUE(method->capabilities & GCOMP_CAP_ENCODE);
  EXPECT_TRUE(method->capabilities & GCOMP_CAP_DECODE);

  // Verify vtable hooks are set
  EXPECT_NE(method->create_encoder, nullptr);
  EXPECT_NE(method->create_decoder, nullptr);
  EXPECT_NE(method->destroy_encoder, nullptr);
  EXPECT_NE(method->destroy_decoder, nullptr);
  EXPECT_NE(method->get_schema, nullptr);
}

TEST_F(GzipRegisterTest, MethodSchema) {
  // Register both methods
  gcomp_method_deflate_register(registry_);
  gcomp_method_gzip_register(registry_);

  const gcomp_method_t * method = gcomp_registry_find(registry_, "gzip");
  ASSERT_NE(method, nullptr);
  ASSERT_NE(method->get_schema, nullptr);

  const gcomp_method_schema_t * schema = method->get_schema();
  ASSERT_NE(schema, nullptr);
  EXPECT_GT(schema->num_options, 0u);
  EXPECT_NE(schema->options, nullptr);

  // Verify we can find some expected gzip options
  bool found_mtime = false;
  bool found_os = false;
  bool found_name = false;
  bool found_concat = false;

  for (size_t i = 0; i < schema->num_options; i++) {
    const char * key = schema->options[i].key;
    if (key) {
      if (strcmp(key, "gzip.mtime") == 0)
        found_mtime = true;
      if (strcmp(key, "gzip.os") == 0)
        found_os = true;
      if (strcmp(key, "gzip.name") == 0)
        found_name = true;
      if (strcmp(key, "gzip.concat") == 0)
        found_concat = true;
    }
  }

  EXPECT_TRUE(found_mtime) << "gzip.mtime not found in schema";
  EXPECT_TRUE(found_os) << "gzip.os not found in schema";
  EXPECT_TRUE(found_name) << "gzip.name not found in schema";
  EXPECT_TRUE(found_concat) << "gzip.concat not found in schema";
}

TEST_F(GzipRegisterTest, DuplicateRegistration) {
  // Register deflate and gzip
  gcomp_method_deflate_register(registry_);
  gcomp_method_gzip_register(registry_);

  // Registering again should be idempotent (return OK or error, but not crash)
  gcomp_status_t status = gcomp_method_gzip_register(registry_);
  // Either GCOMP_OK (idempotent) or GCOMP_ERR_INVALID_ARG (duplicate) is
  // acceptable
  EXPECT_TRUE(status == GCOMP_OK || status == GCOMP_ERR_INVALID_ARG);

  // Method should still be found
  const gcomp_method_t * method = gcomp_registry_find(registry_, "gzip");
  EXPECT_NE(method, nullptr);
}

//
// Test memory cleanup (run with valgrind to verify no leaks)
//

TEST_F(GzipRegisterTest, MemoryCleanupOnDestroy) {
  // Register methods
  gcomp_method_deflate_register(registry_);
  gcomp_method_gzip_register(registry_);

  // Verify registration
  EXPECT_NE(gcomp_registry_find(registry_, "gzip"), nullptr);

  // Destroy and nullify (TearDown will handle cleanup, but let's be explicit)
  gcomp_registry_destroy(registry_);
  registry_ = nullptr;

  // If we get here without crashing, and valgrind shows no leaks, we're good
}

TEST_F(GzipRegisterTest, MultipleRegistriesIndependent) {
  // Create a second registry
  gcomp_registry_t * registry2 = nullptr;
  gcomp_status_t status = gcomp_registry_create(nullptr, &registry2);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(registry2, nullptr);

  // Register only in first registry
  gcomp_method_deflate_register(registry_);
  gcomp_method_gzip_register(registry_);

  // First registry should have gzip
  EXPECT_NE(gcomp_registry_find(registry_, "gzip"), nullptr);

  // Second registry should NOT have gzip
  EXPECT_EQ(gcomp_registry_find(registry2, "gzip"), nullptr);

  // Clean up second registry
  gcomp_registry_destroy(registry2);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
