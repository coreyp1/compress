/**
 * @file test_rle_register.cpp
 *
 * Unit tests for RLE method registration.
 *
 * - Explicit registration to custom registry
 * - Method found after registration
 * - Memory cleanup (verify with valgrind)
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/rle.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>

class RleRegisterTest : public ::testing::Test {
protected:
  void SetUp() override {
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

TEST(RleDefaultRegistryTest, AutoRegistered) {
  gcomp_registry_t * reg = gcomp_registry_default();
  ASSERT_NE(reg, nullptr);

  const gcomp_method_t * method = gcomp_registry_find(reg, "rle");
  EXPECT_NE(method, nullptr);
  if (method) {
    EXPECT_STREQ(method->name, "rle");
    EXPECT_TRUE(method->capabilities & GCOMP_CAP_ENCODE);
    EXPECT_TRUE(method->capabilities & GCOMP_CAP_DECODE);
  }
}

TEST_F(RleRegisterTest, ExplicitRegistration) {
  gcomp_status_t status = gcomp_method_rle_register(registry_);
  EXPECT_EQ(status, GCOMP_OK);

  const gcomp_method_t * method = gcomp_registry_find(registry_, "rle");
  EXPECT_NE(method, nullptr);
  if (method) {
    EXPECT_STREQ(method->name, "rle");
    EXPECT_TRUE(method->capabilities & GCOMP_CAP_ENCODE);
    EXPECT_TRUE(method->capabilities & GCOMP_CAP_DECODE);
  }
}

TEST_F(RleRegisterTest, RegistrationWithNullRegistry) {
  gcomp_status_t status = gcomp_method_rle_register(nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(RleRegisterTest, MethodCapabilities) {
  gcomp_method_rle_register(registry_);

  const gcomp_method_t * method = gcomp_registry_find(registry_, "rle");
  ASSERT_NE(method, nullptr);

  EXPECT_TRUE(method->capabilities & GCOMP_CAP_ENCODE);
  EXPECT_TRUE(method->capabilities & GCOMP_CAP_DECODE);
  EXPECT_NE(method->create_encoder, nullptr);
  EXPECT_NE(method->create_decoder, nullptr);
  EXPECT_NE(method->destroy_encoder, nullptr);
  EXPECT_NE(method->destroy_decoder, nullptr);
  EXPECT_NE(method->get_schema, nullptr);
}

TEST_F(RleRegisterTest, MethodSchema) {
  gcomp_method_rle_register(registry_);

  const gcomp_method_t * method = gcomp_registry_find(registry_, "rle");
  ASSERT_NE(method, nullptr);
  ASSERT_NE(method->get_schema, nullptr);

  const gcomp_method_schema_t * schema = method->get_schema();
  ASSERT_NE(schema, nullptr);
  EXPECT_GT(schema->num_options, 0u);
  EXPECT_NE(schema->options, nullptr);

  bool found_format = false;
  bool found_max_output = false;
  for (size_t i = 0; i < schema->num_options; i++) {
    const char * key = schema->options[i].key;
    if (key) {
      if (strcmp(key, "rle.format") == 0)
        found_format = true;
      if (strcmp(key, "limits.max_output_bytes") == 0)
        found_max_output = true;
    }
  }

  EXPECT_TRUE(found_format) << "rle.format not found in schema";
  EXPECT_TRUE(found_max_output)
      << "limits.max_output_bytes not found in schema";
}

TEST_F(RleRegisterTest, MemoryCleanupOnDestroy) {
  gcomp_method_rle_register(registry_);
  EXPECT_NE(gcomp_registry_find(registry_, "rle"), nullptr);

  gcomp_registry_destroy(registry_);
  registry_ = nullptr;
}

TEST_F(RleRegisterTest, EncoderDecoderCreation) {
  gcomp_method_rle_register(registry_);

  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "rle", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);
  if (encoder) {
    gcomp_encoder_destroy(encoder);
  }

  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "rle", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(decoder, nullptr);
  if (decoder) {
    gcomp_decoder_destroy(decoder);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
