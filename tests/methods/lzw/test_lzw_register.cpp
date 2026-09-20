/**
 * @file test_lzw_register.cpp
 *
 * Unit tests for LZW method registration.
 *
 * - Explicit registration to custom registry
 * - Method found after registration
 * - Memory cleanup (verify with valgrind)
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lzw.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>

class LzwRegisterTest : public ::testing::Test {
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

TEST(LzwDefaultRegistryTest, AutoRegistered) {
  gcomp_registry_t * reg = gcomp_registry_default();
  ASSERT_NE(reg, nullptr);

  const gcomp_method_t * method = gcomp_registry_find(reg, "lzw");
  EXPECT_NE(method, nullptr);
  if (method) {
    EXPECT_STREQ(method->name, "lzw");
    EXPECT_TRUE(method->capabilities & GCOMP_CAP_ENCODE);
    EXPECT_TRUE(method->capabilities & GCOMP_CAP_DECODE);
  }
}

TEST_F(LzwRegisterTest, ExplicitRegistration) {
  gcomp_status_t status = gcomp_method_lzw_register(registry_);
  EXPECT_EQ(status, GCOMP_OK);

  const gcomp_method_t * method = gcomp_registry_find(registry_, "lzw");
  EXPECT_NE(method, nullptr);
  if (method) {
    EXPECT_STREQ(method->name, "lzw");
    EXPECT_TRUE(method->capabilities & GCOMP_CAP_ENCODE);
    EXPECT_TRUE(method->capabilities & GCOMP_CAP_DECODE);
  }
}

TEST_F(LzwRegisterTest, RegistrationWithNullRegistry) {
  gcomp_status_t status = gcomp_method_lzw_register(nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(LzwRegisterTest, MethodCapabilities) {
  gcomp_method_lzw_register(registry_);

  const gcomp_method_t * method = gcomp_registry_find(registry_, "lzw");
  ASSERT_NE(method, nullptr);

  EXPECT_TRUE(method->capabilities & GCOMP_CAP_ENCODE);
  EXPECT_TRUE(method->capabilities & GCOMP_CAP_DECODE);
  EXPECT_NE(method->create_encoder, nullptr);
  EXPECT_NE(method->create_decoder, nullptr);
  EXPECT_NE(method->destroy_encoder, nullptr);
  EXPECT_NE(method->destroy_decoder, nullptr);
  EXPECT_NE(method->get_schema, nullptr);
}

TEST_F(LzwRegisterTest, MethodSchema) {
  gcomp_method_lzw_register(registry_);

  const gcomp_method_t * method = gcomp_registry_find(registry_, "lzw");
  ASSERT_NE(method, nullptr);
  ASSERT_NE(method->get_schema, nullptr);

  const gcomp_method_schema_t * schema = method->get_schema();
  ASSERT_NE(schema, nullptr);
  EXPECT_GT(schema->num_options, 0u);
  EXPECT_NE(schema->options, nullptr);

  bool found_format = false;
  bool found_max_code_bits = false;
  for (size_t i = 0; i < schema->num_options; i++) {
    const char * key = schema->options[i].key;
    if (key) {
      if (strcmp(key, "lzw.format") == 0)
        found_format = true;
      if (strcmp(key, "lzw.max_code_bits") == 0)
        found_max_code_bits = true;
    }
  }

  EXPECT_TRUE(found_format) << "lzw.format not found in schema";
  EXPECT_TRUE(found_max_code_bits) << "lzw.max_code_bits not found in schema";
}

TEST_F(LzwRegisterTest, MemoryCleanupOnDestroy) {
  gcomp_method_lzw_register(registry_);
  EXPECT_NE(gcomp_registry_find(registry_, "lzw"), nullptr);

  gcomp_registry_destroy(registry_);
  registry_ = nullptr;
}

TEST_F(LzwRegisterTest, EncoderDecoderCreation) {
  gcomp_method_lzw_register(registry_);

  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "lzw", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);
  if (encoder) {
    gcomp_encoder_destroy(encoder);
  }

  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "lzw", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(decoder, nullptr);
  if (decoder) {
    gcomp_decoder_destroy(decoder);
  }
}

/**
 * @brief The schema states the width range, so a bad one is refused by name.
 *
 * The range 9 to 12 was enforced by the decoder and by
 * lzw_core_encoder_init() but never declared, so gcomp_options_validate() -
 * which gcomp_encoder_create() and gcomp_decoder_create() run over a caller's
 * options - had nothing to check against, and the failure arrived later from
 * inside the method, naming a limit the schema had not mentioned.
 *
 * TIFF 6.0 section 13 caps a code at twelve bits; below nine there is no room
 * for the 256 literals plus Clear and End_of_Information.
 *
 * Validation happens at create, not at set: an options object is not bound to
 * a method until it is handed to one, so there is no schema to consult when a
 * value is stored.
 */
TEST(LzwRegister, TheCodeWidthRangeIsDeclaredAndEnforced) {
  gcomp_registry_t * reg = gcomp_registry_default();
  ASSERT_NE(reg, nullptr);
  const gcomp_method_t * method = gcomp_registry_find(reg, "lzw");
  ASSERT_NE(method, nullptr);

  // Declared, so introspection can report it.
  const gcomp_option_schema_t * schema = nullptr;
  ASSERT_EQ(
      gcomp_method_get_option_schema(method, "lzw.max_code_bits", &schema),
      GCOMP_OK);
  ASSERT_NE(schema, nullptr);
  EXPECT_TRUE(schema->has_min)
      << "lzw.max_code_bits declares no minimum, so a caller cannot discover "
         "the range the method will enforce";
  EXPECT_TRUE(schema->has_max);
  EXPECT_EQ(schema->min_uint, 9u);
  EXPECT_EQ(schema->max_uint, 12u);

  // Enforced, on both sides, wherever an encoder is created.
  for (uint64_t bits = 9; bits <= 12; bits++) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(opts, "lzw.max_code_bits", bits),
        GCOMP_OK);
    gcomp_encoder_t * enc = nullptr;
    EXPECT_EQ(gcomp_encoder_create(reg, "lzw", opts, &enc), GCOMP_OK)
        << "lzw.max_code_bits " << bits << " is valid and was refused";
    if (enc) {
      gcomp_encoder_destroy(enc);
    }
    gcomp_options_destroy(opts);
  }

  for (uint64_t bits : {(uint64_t)0, (uint64_t)8, (uint64_t)13, (uint64_t)64}) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(opts, "lzw.max_code_bits", bits),
        GCOMP_OK);
    gcomp_encoder_t * enc = nullptr;
    EXPECT_NE(gcomp_encoder_create(reg, "lzw", opts, &enc), GCOMP_OK)
        << "lzw.max_code_bits " << bits
        << " is outside 9..12 and an encoder was created for it";
    if (enc) {
      gcomp_encoder_destroy(enc);
    }
    gcomp_options_destroy(opts);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
