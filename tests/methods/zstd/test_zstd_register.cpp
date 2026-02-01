/**
 * @file test_zstd_register.cpp
 *
 * Unit tests for Zstd method registration in the Ghoti.io Compress library.
 *
 * These tests verify:
 * - Explicit registration to custom registry
 * - Method found after registration
 * - Duplicate registration handling
 * - Memory cleanup (verify with valgrind)
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>

//
// Test fixture that creates a fresh registry for each test
//

class ZstdRegisterTest : public ::testing::Test {
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

TEST(ZstdDefaultRegistryTest, AutoRegistered) {
  // The zstd method should be auto-registered in the default registry
  gcomp_registry_t * reg = gcomp_registry_default();
  ASSERT_NE(reg, nullptr);

  const gcomp_method_t * method = gcomp_registry_find(reg, "zstd");
  EXPECT_NE(method, nullptr);
  if (method) {
    EXPECT_STREQ(method->name, "zstd");
    EXPECT_TRUE(method->capabilities & GCOMP_CAP_ENCODE);
    EXPECT_TRUE(method->capabilities & GCOMP_CAP_DECODE);
  }
}

//
// Tests using a custom registry (explicit registration)
//

TEST_F(ZstdRegisterTest, ExplicitRegistration) {
  // Register zstd
  gcomp_status_t status = gcomp_method_zstd_register(registry_);
  EXPECT_EQ(status, GCOMP_OK);

  // Method should be found
  const gcomp_method_t * zstd = gcomp_registry_find(registry_, "zstd");
  EXPECT_NE(zstd, nullptr);
  if (zstd) {
    EXPECT_STREQ(zstd->name, "zstd");
    EXPECT_TRUE(zstd->capabilities & GCOMP_CAP_ENCODE);
    EXPECT_TRUE(zstd->capabilities & GCOMP_CAP_DECODE);
  }
}

TEST_F(ZstdRegisterTest, RegistrationWithNullRegistry) {
  // Registering with NULL registry should return error
  gcomp_status_t status = gcomp_method_zstd_register(nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(ZstdRegisterTest, MethodNotFoundBeforeRegistration) {
  // zstd should not be found before explicit registration
  const gcomp_method_t * zstd = gcomp_registry_find(registry_, "zstd");
  EXPECT_EQ(zstd, nullptr);
}

TEST_F(ZstdRegisterTest, MethodCapabilities) {
  // Register zstd
  gcomp_method_zstd_register(registry_);

  const gcomp_method_t * method = gcomp_registry_find(registry_, "zstd");
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

TEST_F(ZstdRegisterTest, MethodSchema) {
  // Register zstd
  gcomp_method_zstd_register(registry_);

  const gcomp_method_t * method = gcomp_registry_find(registry_, "zstd");
  ASSERT_NE(method, nullptr);
  ASSERT_NE(method->get_schema, nullptr);

  const gcomp_method_schema_t * schema = method->get_schema();
  ASSERT_NE(schema, nullptr);
  EXPECT_GT(schema->num_options, 0u);
  EXPECT_NE(schema->options, nullptr);

  // Verify we can find expected zstd options
  bool found_level = false;
  bool found_checksum = false;
  bool found_window_log = false;
  bool found_content_size = false;
  bool found_concat = false;

  for (size_t i = 0; i < schema->num_options; i++) {
    const char * key = schema->options[i].key;
    if (key) {
      if (strcmp(key, "zstd.level") == 0)
        found_level = true;
      if (strcmp(key, "zstd.checksum") == 0)
        found_checksum = true;
      if (strcmp(key, "zstd.window_log") == 0)
        found_window_log = true;
      if (strcmp(key, "zstd.content_size") == 0)
        found_content_size = true;
      if (strcmp(key, "zstd.concat") == 0)
        found_concat = true;
    }
  }

  EXPECT_TRUE(found_level) << "zstd.level not found in schema";
  EXPECT_TRUE(found_checksum) << "zstd.checksum not found in schema";
  EXPECT_TRUE(found_window_log) << "zstd.window_log not found in schema";
  EXPECT_TRUE(found_content_size) << "zstd.content_size not found in schema";
  EXPECT_TRUE(found_concat) << "zstd.concat not found in schema";
}

TEST_F(ZstdRegisterTest, LimitOptionsInSchema) {
  // Register zstd
  gcomp_method_zstd_register(registry_);

  const gcomp_method_t * method = gcomp_registry_find(registry_, "zstd");
  ASSERT_NE(method, nullptr);
  ASSERT_NE(method->get_schema, nullptr);

  const gcomp_method_schema_t * schema = method->get_schema();
  ASSERT_NE(schema, nullptr);

  // Verify limit options are present
  bool found_max_output = false;
  bool found_max_window = false;
  bool found_max_memory = false;
  bool found_max_expansion = false;

  for (size_t i = 0; i < schema->num_options; i++) {
    const char * key = schema->options[i].key;
    if (key) {
      if (strcmp(key, "limits.max_output_bytes") == 0)
        found_max_output = true;
      if (strcmp(key, "limits.max_window_bytes") == 0)
        found_max_window = true;
      if (strcmp(key, "limits.max_memory_bytes") == 0)
        found_max_memory = true;
      if (strcmp(key, "limits.max_expansion_ratio") == 0)
        found_max_expansion = true;
    }
  }

  EXPECT_TRUE(found_max_output) << "limits.max_output_bytes not found";
  EXPECT_TRUE(found_max_window) << "limits.max_window_bytes not found";
  EXPECT_TRUE(found_max_memory) << "limits.max_memory_bytes not found";
  EXPECT_TRUE(found_max_expansion) << "limits.max_expansion_ratio not found";
}

TEST_F(ZstdRegisterTest, DuplicateRegistration) {
  // Register zstd
  gcomp_method_zstd_register(registry_);

  // Registering again should be idempotent (return OK or error, but not crash)
  gcomp_status_t status = gcomp_method_zstd_register(registry_);
  // Either GCOMP_OK (idempotent) or GCOMP_ERR_INVALID_ARG (duplicate) is
  // acceptable
  EXPECT_TRUE(status == GCOMP_OK || status == GCOMP_ERR_INVALID_ARG);

  // Method should still be found
  const gcomp_method_t * method = gcomp_registry_find(registry_, "zstd");
  EXPECT_NE(method, nullptr);
}

TEST_F(ZstdRegisterTest, EncoderCreationAfterRegistration) {
  // Register zstd
  gcomp_status_t status = gcomp_method_zstd_register(registry_);
  ASSERT_EQ(status, GCOMP_OK);

  // Try to create an encoder - should succeed
  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "zstd", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  if (encoder) {
    gcomp_encoder_destroy(encoder);
  }
}

TEST_F(ZstdRegisterTest, DecoderCreationAfterRegistration) {
  // Register zstd
  gcomp_status_t status = gcomp_method_zstd_register(registry_);
  ASSERT_EQ(status, GCOMP_OK);

  // Try to create a decoder - should succeed
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "zstd", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(decoder, nullptr);

  if (decoder) {
    gcomp_decoder_destroy(decoder);
  }
}

TEST_F(ZstdRegisterTest, EncoderCreationWithoutRegistration) {
  // Try to create encoder without registration - should fail
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "zstd", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(encoder, nullptr);
}

TEST_F(ZstdRegisterTest, DecoderCreationWithoutRegistration) {
  // Try to create decoder without registration - should fail
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "zstd", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_ERR_UNSUPPORTED);
  EXPECT_EQ(decoder, nullptr);
}

//
// Test memory cleanup (run with valgrind to verify no leaks)
//

TEST_F(ZstdRegisterTest, MemoryCleanupOnDestroy) {
  // Register zstd
  gcomp_method_zstd_register(registry_);

  // Verify registration
  EXPECT_NE(gcomp_registry_find(registry_, "zstd"), nullptr);

  // Destroy and nullify (TearDown will handle cleanup, but let's be explicit)
  gcomp_registry_destroy(registry_);
  registry_ = nullptr;

  // If we get here without crashing, and valgrind shows no leaks, we're good
}

TEST_F(ZstdRegisterTest, MultipleRegistriesIndependent) {
  // Create a second registry
  gcomp_registry_t * registry2 = nullptr;
  gcomp_status_t status = gcomp_registry_create(nullptr, &registry2);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(registry2, nullptr);

  // Register only in first registry
  gcomp_method_zstd_register(registry_);

  // First registry should have zstd
  EXPECT_NE(gcomp_registry_find(registry_, "zstd"), nullptr);

  // Second registry should NOT have zstd
  EXPECT_EQ(gcomp_registry_find(registry2, "zstd"), nullptr);

  // Clean up second registry
  gcomp_registry_destroy(registry2);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
