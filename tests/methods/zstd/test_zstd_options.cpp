/**
 * @file test_zstd_options.cpp
 *
 * Tests for zstd option parsing and validation.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
#include <cstdlib>
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>
#include <vector>

class ZstdOptionsTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    method_ = gcomp_registry_find(registry_, "zstd");
  }
  gcomp_registry_t * registry_ = nullptr;
  const gcomp_method_t * method_ = nullptr;
};

//
// Schema Introspection Tests
//

// Memcheck keeps validity and addressability bits for every allocated byte,
// so a test that asks for a 2 GB window costs several gigabytes more here
// than it does outside Valgrind, and the run is killed before it reports
// anything. The window size is the point of those tests, so there is nothing
// smaller for them to ask for: they skip instead, and the ordinary run and
// the ASan run still cover them. The Makefile's valgrind targets set this.
static bool UnderValgrind() {
  const char * vg = std::getenv("GCOMP_UNDER_VALGRIND");
  return vg && vg[0] == '1';
}

static const char kValgrindWindowSkip[] =
    "a 2 GB window does not fit under Memcheck's shadow memory; this test "
    "runs outside valgrind";

TEST_F(ZstdOptionsTest, SchemaAvailable) {
  ASSERT_NE(method_, nullptr);
  ASSERT_NE(method_->get_schema, nullptr);
  const gcomp_method_schema_t * schema = method_->get_schema();
  ASSERT_NE(schema, nullptr);
  EXPECT_GT(schema->num_options, 0u);
}

TEST_F(ZstdOptionsTest, SchemaContainsLevelOption) {
  ASSERT_NE(method_->get_schema, nullptr);
  const gcomp_method_schema_t * schema = method_->get_schema();
  ASSERT_NE(schema, nullptr);

  bool found = false;
  for (size_t i = 0; i < schema->num_options; i++) {
    if (strcmp(schema->options[i].key, "zstd.level") == 0) {
      found = true;
      EXPECT_EQ(schema->options[i].type, GCOMP_OPT_INT64);
      break;
    }
  }
  EXPECT_TRUE(found) << "zstd.level option not found in schema";
}

TEST_F(ZstdOptionsTest, SchemaContainsChecksumOption) {
  ASSERT_NE(method_->get_schema, nullptr);
  const gcomp_method_schema_t * schema = method_->get_schema();
  ASSERT_NE(schema, nullptr);

  bool found = false;
  for (size_t i = 0; i < schema->num_options; i++) {
    if (strcmp(schema->options[i].key, "zstd.checksum") == 0) {
      found = true;
      EXPECT_EQ(schema->options[i].type, GCOMP_OPT_BOOL);
      break;
    }
  }
  EXPECT_TRUE(found) << "zstd.checksum option not found in schema";
}

TEST_F(ZstdOptionsTest, SchemaContainsWindowLogOption) {
  ASSERT_NE(method_->get_schema, nullptr);
  const gcomp_method_schema_t * schema = method_->get_schema();
  ASSERT_NE(schema, nullptr);

  bool found = false;
  for (size_t i = 0; i < schema->num_options; i++) {
    if (strcmp(schema->options[i].key, "zstd.window_log") == 0) {
      found = true;
      EXPECT_EQ(schema->options[i].type, GCOMP_OPT_UINT64);
      break;
    }
  }
  EXPECT_TRUE(found) << "zstd.window_log option not found in schema";
}

TEST_F(ZstdOptionsTest, SchemaContainsConcatOption) {
  ASSERT_NE(method_->get_schema, nullptr);
  const gcomp_method_schema_t * schema = method_->get_schema();
  ASSERT_NE(schema, nullptr);

  bool found = false;
  for (size_t i = 0; i < schema->num_options; i++) {
    if (strcmp(schema->options[i].key, "zstd.concat") == 0) {
      found = true;
      EXPECT_EQ(schema->options[i].type, GCOMP_OPT_BOOL);
      break;
    }
  }
  EXPECT_TRUE(found) << "zstd.concat option not found in schema";
}

TEST_F(ZstdOptionsTest, SchemaContainsLimitOptions) {
  ASSERT_NE(method_->get_schema, nullptr);
  const gcomp_method_schema_t * schema = method_->get_schema();
  ASSERT_NE(schema, nullptr);

  const char * limit_keys[] = {"limits.max_output_bytes",
      "limits.max_window_bytes", "limits.max_memory_bytes",
      "limits.max_expansion_ratio"};

  for (const char * key : limit_keys) {
    bool found = false;
    for (size_t i = 0; i < schema->num_options; i++) {
      if (strcmp(schema->options[i].key, key) == 0) {
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found) << key << " option not found in schema";
  }
}

//
// Default Value Tests
//

TEST_F(ZstdOptionsTest, DefaultLevelIs3) {
  // Create encoder with no options, verify default behavior
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);
  // Encoder creation succeeds with default level 3
  gcomp_encoder_destroy(enc);
}

TEST_F(ZstdOptionsTest, DefaultChecksumIsFalse) {
  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", nullptr, &enc), GCOMP_OK);

  // Encode minimal data and check flag
  const char data[] = "test";
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {(void *)data, strlen(data), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);

  // Frame header byte 4 bit 2 is checksum flag (0 = disabled by default)
  EXPECT_FALSE(out[4] & 0x04) << "Checksum should be disabled by default";
  gcomp_encoder_destroy(enc);
}

//
// Level Validation Tests
//

TEST_F(ZstdOptionsTest, ValidLevelMin) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_int64(opts, "zstd.level", 1);

  gcomp_encoder_t * enc = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdOptionsTest, ValidLevelMax) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_int64(opts, "zstd.level", 22);

  gcomp_encoder_t * enc = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdOptionsTest, InvalidLevelZero) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_int64(opts, "zstd.level", 0);

  gcomp_encoder_t * enc = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc),
      GCOMP_ERR_INVALID_ARG);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdOptionsTest, InvalidLevelNegative) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_int64(opts, "zstd.level", -1);

  gcomp_encoder_t * enc = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc),
      GCOMP_ERR_INVALID_ARG);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdOptionsTest, InvalidLevelTooHigh) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_int64(opts, "zstd.level", 23);

  gcomp_encoder_t * enc = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc),
      GCOMP_ERR_INVALID_ARG);
  gcomp_options_destroy(opts);
}

//
// Window Log Validation Tests
//

TEST_F(ZstdOptionsTest, ValidWindowLogMin) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "zstd.window_log", 10);

  gcomp_encoder_t * enc = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

// The window size in the frame header is a promise that a sequence may point
// that far back, so the encoder has to hold that much history to be able to
// make one.  A 2 GB window therefore costs 2 GB of buffer plus its position
// tables, and the default memory limit of 256 MiB refuses it - which is the
// limit doing its job, not the option being invalid.
TEST_F(ZstdOptionsTest, ValidWindowLogMax) {
  if (UnderValgrind()) {
    GTEST_SKIP() << kValgrindWindowSkip;
  }

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "zstd.window_log", 31);
  gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 0); // unlimited

  gcomp_encoder_t * enc = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdOptionsTest, LargeWindowLogNeedsTheMemoryToBackIt) {
  // This one asks to be *refused*, and still costs the memory: the encoder
  // allocates the window, the block buffer and the match finder's tables and
  // only then calls gcomp_memory_check_limit(), so the 2 GB is allocated
  // before limits.max_memory_bytes turns it down. Outside Valgrind that is
  // invisible - Linux does not back an untouched mapping with pages - which
  // is why nothing noticed. Under Memcheck it is real, and it is why this
  // test is skipped here rather than only the one above.
  if (UnderValgrind()) {
    GTEST_SKIP() << kValgrindWindowSkip;
  }

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "zstd.window_log", 31);

  gcomp_encoder_t * enc = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc),
      GCOMP_ERR_LIMIT);
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

// Declaring the content size caps what the window can be worth: a stream of
// ten bytes has no use for a megabyte of history, and paying for one would be
// the option's meaning leaking into the encoder's footprint.
TEST_F(ZstdOptionsTest, ASmallDeclaredContentSizeDoesNotPayForALargeWindow) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "zstd.window_log", 30);
  gcomp_options_set_uint64(opts, "zstd.content_size", 64);

  gcomp_encoder_t * enc = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdOptionsTest, ValidWindowLogAuto) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "zstd.window_log", 0); // 0 = auto

  gcomp_encoder_t * enc = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdOptionsTest, InvalidWindowLogTooSmall) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "zstd.window_log", 9); // min is 10

  gcomp_encoder_t * enc = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc),
      GCOMP_ERR_INVALID_ARG);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdOptionsTest, InvalidWindowLogTooLarge) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "zstd.window_log", 32); // max is 31

  gcomp_encoder_t * enc = nullptr;
  EXPECT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc),
      GCOMP_ERR_INVALID_ARG);
  gcomp_options_destroy(opts);
}

//
// Checksum Option Tests
//

TEST_F(ZstdOptionsTest, ChecksumEnabled) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "zstd.checksum", true);

  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);

  const char data[] = "test data for checksum";
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {(void *)data, strlen(data), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);

  // Verify checksum flag is set in frame header
  EXPECT_TRUE(out[4] & 0x04) << "Checksum flag should be set";

  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

//
// Limits Options Tests
//

TEST_F(ZstdOptionsTest, MaxMemoryLimitRespected) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  // Set a very small memory limit that should fail
  gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 1024);

  gcomp_encoder_t * enc = nullptr;
  // Should fail because minimum allocations exceed 1KB
  EXPECT_EQ(
      gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_ERR_LIMIT);
  gcomp_options_destroy(opts);
}

//
// Content Size Option Tests
//

TEST_F(ZstdOptionsTest, ContentSizeInHeaderSmall) {
  // Small content size (< 256) with single_segment uses fcs_flag=0 per spec
  // This test verifies small content sizes are handled correctly
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "zstd.content_size", 100);

  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);

  std::vector<uint8_t> data(100, 'X');
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {data.data(), data.size(), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);

  // Per spec: small content size with single_segment uses fcs_flag=0
  // Frame header descriptor byte: bits 6-7 = FCS_Flag, bit 5 = Single_Segment
  uint8_t fhd = out[4];
  uint8_t fcs_flag = (fhd >> 6) & 0x03;
  bool single_segment = (fhd & 0x20) != 0;

  // Either FCS_Flag is set, OR Single_Segment is set (which implies content
  // size)
  EXPECT_TRUE(fcs_flag != 0 || single_segment)
      << "Content size should be present (via FCS_Flag or Single_Segment)";

  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(ZstdOptionsTest, ContentSizeInHeaderLarge) {
  // Larger content size forces FCS_Flag > 0
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "zstd.content_size", 1000);

  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);

  std::vector<uint8_t> data(1000, 'X');
  std::vector<uint8_t> out(2048);
  gcomp_buffer_t in = {data.data(), data.size(), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);

  // Frame header descriptor byte 4: bits 6-7 = FCS_Flag
  uint8_t fcs_flag = (out[4] >> 6) & 0x03;
  EXPECT_GE(fcs_flag, 1u) << "FCS_Flag should be >= 1 for content_size >= 256";

  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

//
// Combined Options Tests
//

TEST_F(ZstdOptionsTest, MultipleOptionsWork) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_int64(opts, "zstd.level", 5);
  gcomp_options_set_bool(opts, "zstd.checksum", true);
  gcomp_options_set_uint64(opts, "zstd.window_log", 18);

  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(registry_, "zstd", opts, &enc), GCOMP_OK);

  const char data[] = "Test with multiple options";
  std::vector<uint8_t> out(256);
  gcomp_buffer_t in = {(void *)data, strlen(data), 0};
  gcomp_buffer_t ob = {out.data(), out.size(), 0};
  EXPECT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
  EXPECT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
  EXPECT_GT(ob.used, 0u);

  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
