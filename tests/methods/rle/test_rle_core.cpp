/**
 * @file test_rle_core.cpp
 *
 * Unit tests for RLE core: rle_emit_literal and rle_emit_repeat
 * with bounds enforcement.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/rle.h>
#include <gtest/gtest.h>

// Access RLE core API (internal to the library; we link the library)
// The core is compiled into the library. We need to call rle_emit_literal
// and rle_emit_repeat. They are not in the public rle.h. So we have two
// options: (1) export them for testing (GCOMP_INTERNAL_API or test-only
// symbol), or (2) test via the decoder path that uses the core.
// Task says "Create unit tests for RLE core" - so we need to test the
// primitives. Checking if there's a pattern in the codebase for testing
// internal APIs...
// In the Makefile we have -DGCOMP_TEST_BUILD. So we can declare the
// core functions extern in the test and they might be exported. Let me
// check method exports - stream_internal has GCOMP_INTERNAL_API for
// set_error. So internal API is exported when GCOMP_TEST_BUILD. The
// rle_core functions are in rle_core.c and not marked GCOMP_INTERNAL_API.
// So they are not exported from the shared library. We have two options:
// 1) Add GCOMP_INTERNAL_API to rle_emit_literal and rle_emit_repeat in
//    rle_core.c and declare them in a header that the test can include, or
//    the test includes a test-only header. Actually the test can't include
//    rle_internal.h from the build because that's in src/. The TEST_INCLUDE
//    has -I src/ so we can #include "methods/rle/rle_core.h" and the
//    library exports the symbols when linked. But rle_core.h is not in
//    the public API and the .c file doesn't have GCOMP_INTERNAL_API on
//    the functions - so the symbols will be in the library (all non-static
//    symbols are exported in a shared lib by default on Linux). So we
//    can include the core header. Let me check - compress build uses
//    -fPIC and builds a .so. By default all non-static symbols are
//    exported. So rle_emit_literal and rle_emit_repeat will be exported.
//    We need to include the header. The test include path has -I src/ so
//    we can do #include "methods/rle/rle_core.h" - but that might not
//    work because include is usually include/ and src/. Let me check
//    TEST_INCLUDE: -I include/ -I src/ -I tests/common/ -I
//    tests/methods/deflate/. So we have -I src/. So from src/, the path
//    to rle_core.h would be methods/rle/rle_core.h. So in the test we
//    can #include "methods/rle/rle_core.h".
extern "C" {
#include "methods/rle/rle_core.h"
}

class RleCoreTest : public ::testing::Test {
protected:
  static const size_t BUF_SIZE = 1024;
  uint8_t output_buf_[BUF_SIZE];
  size_t output_used_;

  void SetUp() override {
    output_used_ = 0;
    memset(output_buf_, 0xCC, BUF_SIZE);
  }
};

TEST_F(RleCoreTest, EmitLiteralEmpty) {
  const uint8_t data[] = {'x'};
  gcomp_status_t s =
      rle_emit_literal(output_buf_, BUF_SIZE, &output_used_, data, 0, 0);
  EXPECT_EQ(s, GCOMP_OK);
  EXPECT_EQ(output_used_, 0u);
}

TEST_F(RleCoreTest, EmitLiteralSingleByte) {
  const uint8_t data[] = {0xAB};
  gcomp_status_t s =
      rle_emit_literal(output_buf_, BUF_SIZE, &output_used_, data, 1, 0);
  EXPECT_EQ(s, GCOMP_OK);
  EXPECT_EQ(output_used_, 1u);
  EXPECT_EQ(output_buf_[0], 0xAB);
}

TEST_F(RleCoreTest, EmitLiteralMultipleBytes) {
  const uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
  gcomp_status_t s =
      rle_emit_literal(output_buf_, BUF_SIZE, &output_used_, data, 5, 0);
  EXPECT_EQ(s, GCOMP_OK);
  EXPECT_EQ(output_used_, 5u);
  EXPECT_TRUE(test_helpers_buffers_equal(data, 5, output_buf_, 5));
}

TEST_F(RleCoreTest, EmitLiteralThenLiteral) {
  const uint8_t a[] = {0x11, 0x22};
  const uint8_t b[] = {0x33, 0x44, 0x55};
  gcomp_status_t s =
      rle_emit_literal(output_buf_, BUF_SIZE, &output_used_, a, 2, 0);
  ASSERT_EQ(s, GCOMP_OK);
  s = rle_emit_literal(output_buf_, BUF_SIZE, &output_used_, b, 3, 0);
  EXPECT_EQ(s, GCOMP_OK);
  EXPECT_EQ(output_used_, 5u);
  EXPECT_EQ(output_buf_[0], 0x11);
  EXPECT_EQ(output_buf_[1], 0x22);
  EXPECT_EQ(output_buf_[2], 0x33);
  EXPECT_EQ(output_buf_[3], 0x44);
  EXPECT_EQ(output_buf_[4], 0x55);
}

TEST_F(RleCoreTest, EmitLiteralExceedsBuffer) {
  const uint8_t data[100] = {0};
  output_used_ = BUF_SIZE - 50;
  gcomp_status_t s =
      rle_emit_literal(output_buf_, BUF_SIZE, &output_used_, data, 100, 0);
  EXPECT_EQ(s, GCOMP_ERR_LIMIT);
  EXPECT_EQ(output_used_, BUF_SIZE - 50u); // unchanged
}

TEST_F(RleCoreTest, EmitLiteralExceedsMaxOutputBytes) {
  const uint8_t data[] = {0x01, 0x02};
  gcomp_status_t s =
      rle_emit_literal(output_buf_, BUF_SIZE, &output_used_, data, 2, 1);
  EXPECT_EQ(s, GCOMP_ERR_LIMIT);
  EXPECT_EQ(output_used_, 0u);
}

TEST_F(RleCoreTest, EmitRepeatEmpty) {
  gcomp_status_t s =
      rle_emit_repeat(output_buf_, BUF_SIZE, &output_used_, 0x42, 0, 0);
  EXPECT_EQ(s, GCOMP_OK);
  EXPECT_EQ(output_used_, 0u);
}

TEST_F(RleCoreTest, EmitRepeatSingleByte) {
  gcomp_status_t s =
      rle_emit_repeat(output_buf_, BUF_SIZE, &output_used_, 0x42, 1, 0);
  EXPECT_EQ(s, GCOMP_OK);
  EXPECT_EQ(output_used_, 1u);
  EXPECT_EQ(output_buf_[0], 0x42);
}

TEST_F(RleCoreTest, EmitRepeatMultiple) {
  gcomp_status_t s =
      rle_emit_repeat(output_buf_, BUF_SIZE, &output_used_, 0xAA, 10, 0);
  EXPECT_EQ(s, GCOMP_OK);
  EXPECT_EQ(output_used_, 10u);
  for (size_t i = 0; i < 10; i++) {
    EXPECT_EQ(output_buf_[i], 0xAA) << "i=" << i;
  }
}

TEST_F(RleCoreTest, EmitRepeatExceedsBuffer) {
  output_used_ = BUF_SIZE - 10;
  gcomp_status_t s =
      rle_emit_repeat(output_buf_, BUF_SIZE, &output_used_, 0x00, 20, 0);
  EXPECT_EQ(s, GCOMP_ERR_LIMIT);
  EXPECT_EQ(output_used_, BUF_SIZE - 10u);
}

TEST_F(RleCoreTest, EmitRepeatExceedsMaxOutputBytes) {
  gcomp_status_t s =
      rle_emit_repeat(output_buf_, BUF_SIZE, &output_used_, 0x00, 5, 3);
  EXPECT_EQ(s, GCOMP_ERR_LIMIT);
  EXPECT_EQ(output_used_, 0u);
}

TEST_F(RleCoreTest, EmitLiteralNullOutput) {
  const uint8_t data[] = {0x01};
  size_t used = 0;
  gcomp_status_t s = rle_emit_literal(nullptr, BUF_SIZE, &used, data, 1, 0);
  EXPECT_EQ(s, GCOMP_ERR_INVALID_ARG);
}

TEST_F(RleCoreTest, EmitLiteralNullOutputUsed) {
  const uint8_t data[] = {0x01};
  gcomp_status_t s =
      rle_emit_literal(output_buf_, BUF_SIZE, nullptr, data, 1, 0);
  EXPECT_EQ(s, GCOMP_ERR_INVALID_ARG);
}

TEST_F(RleCoreTest, EmitLiteralNullData) {
  size_t used = 0;
  gcomp_status_t s =
      rle_emit_literal(output_buf_, BUF_SIZE, &used, nullptr, 1, 0);
  EXPECT_EQ(s, GCOMP_ERR_INVALID_ARG);
}

TEST_F(RleCoreTest, EmitRepeatNullOutput) {
  size_t used = 0;
  gcomp_status_t s = rle_emit_repeat(nullptr, BUF_SIZE, &used, 0x00, 1, 0);
  EXPECT_EQ(s, GCOMP_ERR_INVALID_ARG);
}

TEST_F(RleCoreTest, LiteralThenRepeat) {
  const uint8_t lit[] = {0x01, 0x02};
  gcomp_status_t s =
      rle_emit_literal(output_buf_, BUF_SIZE, &output_used_, lit, 2, 0);
  ASSERT_EQ(s, GCOMP_OK);
  s = rle_emit_repeat(output_buf_, BUF_SIZE, &output_used_, 0xFF, 3, 0);
  EXPECT_EQ(s, GCOMP_OK);
  EXPECT_EQ(output_used_, 5u);
  EXPECT_EQ(output_buf_[0], 0x01);
  EXPECT_EQ(output_buf_[1], 0x02);
  EXPECT_EQ(output_buf_[2], 0xFF);
  EXPECT_EQ(output_buf_[3], 0xFF);
  EXPECT_EQ(output_buf_[4], 0xFF);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
