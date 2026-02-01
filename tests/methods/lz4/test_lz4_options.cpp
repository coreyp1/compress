/**
 * @file test_lz4_options.cpp
 *
 * Unit tests for LZ4 options parsing and validation in the Ghoti.io Compress
 * library.
 *
 * These tests verify:
 * - LZ4-specific options are parsed correctly
 * - Default values are applied
 * - Invalid block size values are rejected
 * - Limits options are read correctly
 * - Schema introspection returns correct keys and types
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lz4.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <vector>

//
// Test fixture
//

class Lz4OptionsTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Default Values Tests
//

TEST_F(Lz4OptionsTest, EncoderWithNoOptions) {
  // Encoder creation with no options should use defaults
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "lz4", nullptr, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  if (encoder) {
    gcomp_encoder_destroy(encoder);
  }
}

TEST_F(Lz4OptionsTest, DecoderWithNoOptions) {
  // Decoder creation with no options should use defaults
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "lz4", nullptr, &decoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(decoder, nullptr);

  if (decoder) {
    gcomp_decoder_destroy(decoder);
  }
}

//
// Block Size Validation Tests
//

TEST_F(Lz4OptionsTest, ValidBlockSize64KB) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_uint64(opts, "lz4.block_size", 65536);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  if (encoder) {
    gcomp_encoder_destroy(encoder);
  }
  gcomp_options_destroy(opts);
}

TEST_F(Lz4OptionsTest, ValidBlockSize256KB) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_uint64(opts, "lz4.block_size", 262144);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  if (encoder) {
    gcomp_encoder_destroy(encoder);
  }
  gcomp_options_destroy(opts);
}

TEST_F(Lz4OptionsTest, ValidBlockSize1MB) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_uint64(opts, "lz4.block_size", 1048576);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  if (encoder) {
    gcomp_encoder_destroy(encoder);
  }
  gcomp_options_destroy(opts);
}

TEST_F(Lz4OptionsTest, ValidBlockSize4MB) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_uint64(opts, "lz4.block_size", 4194304);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  if (encoder) {
    gcomp_encoder_destroy(encoder);
  }
  gcomp_options_destroy(opts);
}

TEST_F(Lz4OptionsTest, InvalidBlockSizeRejected) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Invalid block size (not one of the allowed values)
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 100000);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(encoder, nullptr);

  gcomp_options_destroy(opts);
}

TEST_F(Lz4OptionsTest, TooSmallBlockSizeRejected) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Too small block size
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 1024);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(encoder, nullptr);

  gcomp_options_destroy(opts);
}

TEST_F(Lz4OptionsTest, TooLargeBlockSizeRejected) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Too large block size
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 16 * 1024 * 1024);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(encoder, nullptr);

  gcomp_options_destroy(opts);
}

//
// Checksum Options Tests
//

TEST_F(Lz4OptionsTest, BlockChecksumEnabled) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_bool(opts, "lz4.block_checksum", 1);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  if (encoder) {
    gcomp_encoder_destroy(encoder);
  }
  gcomp_options_destroy(opts);
}

TEST_F(Lz4OptionsTest, ContentChecksumEnabled) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_bool(opts, "lz4.content_checksum", 1);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  if (encoder) {
    gcomp_encoder_destroy(encoder);
  }
  gcomp_options_destroy(opts);
}

//
// Block Mode Tests
//

TEST_F(Lz4OptionsTest, IndependentBlocksTrue) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_bool(opts, "lz4.independent_blocks", 1);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  if (encoder) {
    gcomp_encoder_destroy(encoder);
  }
  gcomp_options_destroy(opts);
}

TEST_F(Lz4OptionsTest, IndependentBlocksFalse) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_bool(opts, "lz4.independent_blocks", 0);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  if (encoder) {
    gcomp_encoder_destroy(encoder);
  }
  gcomp_options_destroy(opts);
}

//
// Decoder Options Tests
//

TEST_F(Lz4OptionsTest, ConcatEnabled) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_bool(opts, "lz4.concat", 1);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "lz4", opts, &decoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(decoder, nullptr);

  if (decoder) {
    gcomp_decoder_destroy(decoder);
  }
  gcomp_options_destroy(opts);
}

TEST_F(Lz4OptionsTest, ConcatDisabled) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_bool(opts, "lz4.concat", 0);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "lz4", opts, &decoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(decoder, nullptr);

  if (decoder) {
    gcomp_decoder_destroy(decoder);
  }
  gcomp_options_destroy(opts);
}

//
// Limits Options Tests
//

TEST_F(Lz4OptionsTest, MaxOutputBytesOption) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_uint64(opts, "limits.max_output_bytes", 1024);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "lz4", opts, &decoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(decoder, nullptr);

  if (decoder) {
    gcomp_decoder_destroy(decoder);
  }
  gcomp_options_destroy(opts);
}

TEST_F(Lz4OptionsTest, MaxExpansionRatioOption) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 100);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "lz4", opts, &decoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(decoder, nullptr);

  if (decoder) {
    gcomp_decoder_destroy(decoder);
  }
  gcomp_options_destroy(opts);
}

TEST_F(Lz4OptionsTest, MaxBlockBytesOption) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_uint64(opts, "limits.max_block_bytes", 1048576);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "lz4", opts, &decoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(decoder, nullptr);

  if (decoder) {
    gcomp_decoder_destroy(decoder);
  }
  gcomp_options_destroy(opts);
}

TEST_F(Lz4OptionsTest, MaxMemoryBytesOption) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status =
      gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 128 * 1024);
  ASSERT_EQ(status, GCOMP_OK);

  // May succeed or fail depending on required memory, just shouldn't crash
  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  // Either succeeds or returns GCOMP_ERR_MEMORY
  EXPECT_TRUE(status == GCOMP_OK || status == GCOMP_ERR_MEMORY);

  if (encoder) {
    gcomp_encoder_destroy(encoder);
  }
  gcomp_options_destroy(opts);
}

//
// Schema Introspection Tests
//

TEST_F(Lz4OptionsTest, SchemaHasCorrectNumOptions) {
  const gcomp_method_t * method = gcomp_registry_find(registry_, "lz4");
  ASSERT_NE(method, nullptr);
  ASSERT_NE(method->get_schema, nullptr);

  const gcomp_method_schema_t * schema = method->get_schema();
  ASSERT_NE(schema, nullptr);

  // Should have all the lz4.* and limits.* options
  EXPECT_GE(schema->num_options, 10u);
}

TEST_F(Lz4OptionsTest, SchemaOptionTypes) {
  const gcomp_method_t * method = gcomp_registry_find(registry_, "lz4");
  ASSERT_NE(method, nullptr);
  ASSERT_NE(method->get_schema, nullptr);

  const gcomp_method_schema_t * schema = method->get_schema();
  ASSERT_NE(schema, nullptr);

  for (size_t i = 0; i < schema->num_options; i++) {
    const gcomp_option_schema_t * opt = &schema->options[i];
    ASSERT_NE(opt->key, nullptr);

    // Check that bool options have bool type
    if (strstr(opt->key, "checksum") || strstr(opt->key, "independent") ||
        strcmp(opt->key, "lz4.concat") == 0) {
      EXPECT_EQ(opt->type, GCOMP_OPT_BOOL)
          << "Option " << opt->key << " should be bool";
    }

    // Check that numeric options have uint64 type
    if (strstr(opt->key, "size") || strstr(opt->key, "bytes") ||
        strstr(opt->key, "ratio") || strstr(opt->key, "dictionary_id")) {
      EXPECT_EQ(opt->type, GCOMP_OPT_UINT64)
          << "Option " << opt->key << " should be uint64";
    }
  }
}

TEST_F(Lz4OptionsTest, SchemaHasHelpStrings) {
  const gcomp_method_t * method = gcomp_registry_find(registry_, "lz4");
  ASSERT_NE(method, nullptr);
  ASSERT_NE(method->get_schema, nullptr);

  const gcomp_method_schema_t * schema = method->get_schema();
  ASSERT_NE(schema, nullptr);

  for (size_t i = 0; i < schema->num_options; i++) {
    const gcomp_option_schema_t * opt = &schema->options[i];
    EXPECT_NE(opt->help, nullptr) << "Option " << opt->key << " lacks help";
    if (opt->help) {
      EXPECT_GT(strlen(opt->help), 0u)
          << "Option " << opt->key << " has empty help";
    }
  }
}

TEST_F(Lz4OptionsTest, SchemaUnknownKeyPolicy) {
  const gcomp_method_t * method = gcomp_registry_find(registry_, "lz4");
  ASSERT_NE(method, nullptr);
  ASSERT_NE(method->get_schema, nullptr);

  const gcomp_method_schema_t * schema = method->get_schema();
  ASSERT_NE(schema, nullptr);

  // LZ4 is not a wrapper, so unknown keys should error
  EXPECT_EQ(schema->unknown_key_policy, GCOMP_UNKNOWN_KEY_ERROR);
}

//
// Combined Options Tests
//

TEST_F(Lz4OptionsTest, MultipleOptionsAtOnce) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  // Set multiple options
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 65536);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.block_checksum", 1);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.content_checksum", 1);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.independent_blocks", 0);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_NE(encoder, nullptr);

  if (encoder) {
    gcomp_encoder_destroy(encoder);
  }
  gcomp_options_destroy(opts);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
