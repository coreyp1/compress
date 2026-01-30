/**
 * @file test_state_machine.cpp
 *
 * Tests for state machine robustness - calling API functions in unexpected
 * order, edge cases with buffer sizes, etc.
 *
 * Part of T5.6: State machine robustness testing.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <string>

class StateMachineTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry = gcomp_registry_default();
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  gcomp_registry_t * registry;
};

// =============================================================================
// Encoder State Machine Tests
// =============================================================================

TEST_F(StateMachineTest, Encoder_FinishBeforeAnyUpdate) {
  // finish() called without any prior update() calls
  // This should work - produces an empty compressed stream

  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t s =
      gcomp_encoder_create(registry, "deflate", nullptr, &encoder);
  ASSERT_EQ(GCOMP_OK, s);
  ASSERT_NE(nullptr, encoder);

  uint8_t output[256] = {0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  // Call finish() without any update() - should work
  s = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);
  EXPECT_GT(out_buf.used, 0u); // Should have at least empty block header

  gcomp_encoder_destroy(encoder);
}

TEST_F(StateMachineTest, Encoder_MultipleFinishCalls) {
  // Multiple finish() calls should be safe
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t s =
      gcomp_encoder_create(registry, "deflate", nullptr, &encoder);
  ASSERT_EQ(GCOMP_OK, s);

  const char * input = "test data";
  gcomp_buffer_t in_buf = {input, strlen(input), 0};
  uint8_t output[256] = {0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  s = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);

  // First finish
  s = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);
  size_t first_used = out_buf.used;

  // Second finish - should be OK and idempotent
  s = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);
  // Output shouldn't change after already finished
  EXPECT_EQ(first_used, out_buf.used);

  // Third finish
  s = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StateMachineTest, Encoder_UpdateAfterFinish) {
  // update() after finish() has returned success
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t s =
      gcomp_encoder_create(registry, "deflate", nullptr, &encoder);
  ASSERT_EQ(GCOMP_OK, s);

  const char * input = "test data";
  gcomp_buffer_t in_buf = {input, strlen(input), 0};
  uint8_t output[256] = {0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  s = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);

  s = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);

  // Try update() after finish() - encoder is in DONE state
  // This should return OK but not process any data
  in_buf.used = 0;
  size_t prev_out = out_buf.used;
  s = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);
  // No additional output expected
  EXPECT_EQ(prev_out, out_buf.used);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StateMachineTest, Encoder_DestroyWithoutFinish) {
  // destroy() without calling finish() - should be safe (no crash, no leak)
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t s =
      gcomp_encoder_create(registry, "deflate", nullptr, &encoder);
  ASSERT_EQ(GCOMP_OK, s);

  const char * input = "test data that wasn't finished";
  gcomp_buffer_t in_buf = {input, strlen(input), 0};
  uint8_t output[256] = {0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  s = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);

  // Don't call finish(), just destroy
  gcomp_encoder_destroy(encoder);
  // Should not crash or leak (verified by valgrind)
}

TEST_F(StateMachineTest, Encoder_ZeroSizeInputBuffer) {
  // update() with zero-size input buffer
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t s =
      gcomp_encoder_create(registry, "deflate", nullptr, &encoder);
  ASSERT_EQ(GCOMP_OK, s);

  uint8_t dummy_input = 0;
  gcomp_buffer_t in_buf = {&dummy_input, 0, 0}; // size = 0
  uint8_t output[256] = {0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  // Should handle gracefully
  s = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);

  s = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StateMachineTest, Encoder_ZeroSizeOutputBuffer) {
  // update() with zero-size output buffer
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t s =
      gcomp_encoder_create(registry, "deflate", nullptr, &encoder);
  ASSERT_EQ(GCOMP_OK, s);

  const char * input = "test data";
  gcomp_buffer_t in_buf = {input, strlen(input), 0};
  uint8_t dummy_output = 0;
  gcomp_buffer_t out_buf = {&dummy_output, 0, 0}; // size = 0

  // Should handle gracefully - may not make progress
  s = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);

  // Even finish with zero-size output should not crash
  // (may return ERR_LIMIT if it needs to output data)
  s = gcomp_encoder_finish(encoder, &out_buf);
  // Accept either OK or ERR_LIMIT
  EXPECT_TRUE(s == GCOMP_OK || s == GCOMP_ERR_LIMIT);

  gcomp_encoder_destroy(encoder);
}

// NOTE: Passing NULL data pointer with non-zero size is undefined behavior
// per C semantics. The library does not add defensive checks for this case
// as it would impact performance for valid use cases. Callers must ensure
// that buffer data pointers are valid when size > 0.
// This test is disabled because it would trigger UB/crash.
// TEST_F(StateMachineTest, Encoder_NullDataPointerNonZeroSize) { ... }

TEST_F(StateMachineTest, Encoder_EmptyDataNullPointer) {
  // A NULL data pointer is acceptable when size = 0 (empty buffer)
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t s =
      gcomp_encoder_create(registry, "deflate", nullptr, &encoder);
  ASSERT_EQ(GCOMP_OK, s);

  gcomp_buffer_t in_buf = {nullptr, 0, 0}; // data = NULL, size = 0 is OK
  uint8_t output[256] = {0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  // This should be safe - no data to read
  s = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);

  s = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);

  gcomp_encoder_destroy(encoder);
}

TEST_F(StateMachineTest, Encoder_ResetAndReuse) {
  // Test reset functionality
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t s =
      gcomp_encoder_create(registry, "deflate", nullptr, &encoder);
  ASSERT_EQ(GCOMP_OK, s);

  // First compression
  const char * input1 = "first test data";
  gcomp_buffer_t in_buf1 = {input1, strlen(input1), 0};
  uint8_t output1[256] = {0};
  gcomp_buffer_t out_buf1 = {output1, sizeof(output1), 0};

  s = gcomp_encoder_update(encoder, &in_buf1, &out_buf1);
  EXPECT_EQ(GCOMP_OK, s);
  s = gcomp_encoder_finish(encoder, &out_buf1);
  EXPECT_EQ(GCOMP_OK, s);
  size_t first_size = out_buf1.used;

  // Reset
  s = gcomp_encoder_reset(encoder);
  EXPECT_EQ(GCOMP_OK, s);

  // Second compression - same input should produce same output
  gcomp_buffer_t in_buf2 = {input1, strlen(input1), 0};
  uint8_t output2[256] = {0};
  gcomp_buffer_t out_buf2 = {output2, sizeof(output2), 0};

  s = gcomp_encoder_update(encoder, &in_buf2, &out_buf2);
  EXPECT_EQ(GCOMP_OK, s);
  s = gcomp_encoder_finish(encoder, &out_buf2);
  EXPECT_EQ(GCOMP_OK, s);

  // Should produce identical output
  EXPECT_EQ(first_size, out_buf2.used);
  EXPECT_EQ(0, memcmp(output1, output2, first_size));

  gcomp_encoder_destroy(encoder);
}

// =============================================================================
// Decoder State Machine Tests
// =============================================================================

TEST_F(StateMachineTest, Decoder_FinishBeforeAnyUpdate) {
  // finish() called without any prior update() calls
  // This should return error (incomplete stream)
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t s =
      gcomp_decoder_create(registry, "deflate", nullptr, &decoder);
  ASSERT_EQ(GCOMP_OK, s);
  ASSERT_NE(nullptr, decoder);

  uint8_t output[256] = {0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  // Call finish() without any update() - should fail (no data to decode)
  s = gcomp_decoder_finish(decoder, &out_buf);
  EXPECT_EQ(GCOMP_ERR_CORRUPT, s);

  gcomp_decoder_destroy(decoder);
}

TEST_F(StateMachineTest, Decoder_MultipleFinishCalls) {
  // Multiple finish() calls should be safe after successful completion
  // First, create valid compressed data
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t s =
      gcomp_encoder_create(registry, "deflate", nullptr, &encoder);
  ASSERT_EQ(GCOMP_OK, s);

  const char * input = "test data for multi-finish";
  gcomp_buffer_t in_buf = {input, strlen(input), 0};
  uint8_t compressed[256] = {0};
  gcomp_buffer_t comp_buf = {compressed, sizeof(compressed), 0};

  s = gcomp_encoder_update(encoder, &in_buf, &comp_buf);
  ASSERT_EQ(GCOMP_OK, s);
  s = gcomp_encoder_finish(encoder, &comp_buf);
  ASSERT_EQ(GCOMP_OK, s);
  gcomp_encoder_destroy(encoder);

  // Now decode
  gcomp_decoder_t * decoder = nullptr;
  s = gcomp_decoder_create(registry, "deflate", nullptr, &decoder);
  ASSERT_EQ(GCOMP_OK, s);

  gcomp_buffer_t dec_in = {compressed, comp_buf.used, 0};
  uint8_t decompressed[256] = {0};
  gcomp_buffer_t dec_out = {decompressed, sizeof(decompressed), 0};

  s = gcomp_decoder_update(decoder, &dec_in, &dec_out);
  EXPECT_EQ(GCOMP_OK, s);

  // First finish
  s = gcomp_decoder_finish(decoder, &dec_out);
  EXPECT_EQ(GCOMP_OK, s);

  // Second finish - should be OK and idempotent
  s = gcomp_decoder_finish(decoder, &dec_out);
  EXPECT_EQ(GCOMP_OK, s);

  // Third finish
  s = gcomp_decoder_finish(decoder, &dec_out);
  EXPECT_EQ(GCOMP_OK, s);

  gcomp_decoder_destroy(decoder);
}

TEST_F(StateMachineTest, Decoder_UpdateAfterFinish) {
  // update() after finish() has returned success
  // First create valid compressed data
  uint8_t compressed[256];
  size_t compressed_size;
  {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t s =
        gcomp_encoder_create(registry, "deflate", nullptr, &encoder);
    ASSERT_EQ(GCOMP_OK, s);

    const char * input = "test";
    gcomp_buffer_t in_buf = {input, strlen(input), 0};
    gcomp_buffer_t out_buf = {compressed, sizeof(compressed), 0};
    s = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    ASSERT_EQ(GCOMP_OK, s);
    s = gcomp_encoder_finish(encoder, &out_buf);
    ASSERT_EQ(GCOMP_OK, s);
    compressed_size = out_buf.used;
    gcomp_encoder_destroy(encoder);
  }

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t s =
      gcomp_decoder_create(registry, "deflate", nullptr, &decoder);
  ASSERT_EQ(GCOMP_OK, s);

  gcomp_buffer_t dec_in = {compressed, compressed_size, 0};
  uint8_t decompressed[256] = {0};
  gcomp_buffer_t dec_out = {decompressed, sizeof(decompressed), 0};

  s = gcomp_decoder_update(decoder, &dec_in, &dec_out);
  EXPECT_EQ(GCOMP_OK, s);
  s = gcomp_decoder_finish(decoder, &dec_out);
  EXPECT_EQ(GCOMP_OK, s);

  // Try update() after finish()
  // Decoder is in DONE state - should be OK but no-op
  dec_in.used = 0; // Reset to "unread"
  size_t prev_out = dec_out.used;
  s = gcomp_decoder_update(decoder, &dec_in, &dec_out);
  EXPECT_EQ(GCOMP_OK, s);
  EXPECT_EQ(prev_out, dec_out.used); // No additional output

  gcomp_decoder_destroy(decoder);
}

TEST_F(StateMachineTest, Decoder_DestroyWithoutFinish) {
  // destroy() without calling finish() - should be safe
  uint8_t compressed[256];
  size_t compressed_size;
  {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t s =
        gcomp_encoder_create(registry, "deflate", nullptr, &encoder);
    ASSERT_EQ(GCOMP_OK, s);

    const char * input = "test data for destroy test";
    gcomp_buffer_t in_buf = {input, strlen(input), 0};
    gcomp_buffer_t out_buf = {compressed, sizeof(compressed), 0};
    s = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    ASSERT_EQ(GCOMP_OK, s);
    s = gcomp_encoder_finish(encoder, &out_buf);
    ASSERT_EQ(GCOMP_OK, s);
    compressed_size = out_buf.used;
    gcomp_encoder_destroy(encoder);
  }

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t s =
      gcomp_decoder_create(registry, "deflate", nullptr, &decoder);
  ASSERT_EQ(GCOMP_OK, s);

  gcomp_buffer_t dec_in = {compressed, compressed_size, 0};
  uint8_t decompressed[256] = {0};
  gcomp_buffer_t dec_out = {decompressed, sizeof(decompressed), 0};

  // Partial update
  dec_in.size = compressed_size / 2; // Only process half
  s = gcomp_decoder_update(decoder, &dec_in, &dec_out);
  EXPECT_EQ(GCOMP_OK, s);

  // Don't call finish(), just destroy
  gcomp_decoder_destroy(decoder);
  // Should not crash or leak
}

TEST_F(StateMachineTest, Decoder_ZeroSizeInputBuffer) {
  // update() with zero-size input buffer
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t s =
      gcomp_decoder_create(registry, "deflate", nullptr, &decoder);
  ASSERT_EQ(GCOMP_OK, s);

  uint8_t dummy_input = 0;
  gcomp_buffer_t in_buf = {&dummy_input, 0, 0}; // size = 0
  uint8_t output[256] = {0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  // Should handle gracefully - just no progress
  s = gcomp_decoder_update(decoder, &in_buf, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);
  EXPECT_EQ(0u, out_buf.used);

  gcomp_decoder_destroy(decoder);
}

TEST_F(StateMachineTest, Decoder_ZeroSizeOutputBuffer) {
  // Decoder with zero-size output buffer should handle gracefully
  uint8_t compressed[256];
  size_t compressed_size;
  {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t s =
        gcomp_encoder_create(registry, "deflate", nullptr, &encoder);
    ASSERT_EQ(GCOMP_OK, s);

    const char * input = "test data";
    gcomp_buffer_t in_buf = {input, strlen(input), 0};
    gcomp_buffer_t out_buf = {compressed, sizeof(compressed), 0};
    s = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    ASSERT_EQ(GCOMP_OK, s);
    s = gcomp_encoder_finish(encoder, &out_buf);
    ASSERT_EQ(GCOMP_OK, s);
    compressed_size = out_buf.used;
    gcomp_encoder_destroy(encoder);
  }

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t s =
      gcomp_decoder_create(registry, "deflate", nullptr, &decoder);
  ASSERT_EQ(GCOMP_OK, s);

  gcomp_buffer_t dec_in = {compressed, compressed_size, 0};
  uint8_t dummy_output = 0;
  gcomp_buffer_t dec_out = {&dummy_output, 0, 0}; // size = 0

  // Should handle gracefully - progress may stall
  s = gcomp_decoder_update(decoder, &dec_in, &dec_out);
  EXPECT_EQ(GCOMP_OK, s);

  gcomp_decoder_destroy(decoder);
}

TEST_F(StateMachineTest, Decoder_ResetAndReuse) {
  // Test reset functionality
  uint8_t compressed[256];
  size_t compressed_size;
  {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t s =
        gcomp_encoder_create(registry, "deflate", nullptr, &encoder);
    ASSERT_EQ(GCOMP_OK, s);

    const char * input = "reset test data";
    gcomp_buffer_t in_buf = {input, strlen(input), 0};
    gcomp_buffer_t out_buf = {compressed, sizeof(compressed), 0};
    s = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    ASSERT_EQ(GCOMP_OK, s);
    s = gcomp_encoder_finish(encoder, &out_buf);
    ASSERT_EQ(GCOMP_OK, s);
    compressed_size = out_buf.used;
    gcomp_encoder_destroy(encoder);
  }

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t s =
      gcomp_decoder_create(registry, "deflate", nullptr, &decoder);
  ASSERT_EQ(GCOMP_OK, s);

  // First decode
  gcomp_buffer_t dec_in1 = {compressed, compressed_size, 0};
  uint8_t output1[256] = {0};
  gcomp_buffer_t dec_out1 = {output1, sizeof(output1), 0};
  s = gcomp_decoder_update(decoder, &dec_in1, &dec_out1);
  EXPECT_EQ(GCOMP_OK, s);
  s = gcomp_decoder_finish(decoder, &dec_out1);
  EXPECT_EQ(GCOMP_OK, s);
  size_t first_size = dec_out1.used;

  // Reset
  s = gcomp_decoder_reset(decoder);
  EXPECT_EQ(GCOMP_OK, s);

  // Second decode - same input should produce same output
  gcomp_buffer_t dec_in2 = {compressed, compressed_size, 0};
  uint8_t output2[256] = {0};
  gcomp_buffer_t dec_out2 = {output2, sizeof(output2), 0};
  s = gcomp_decoder_update(decoder, &dec_in2, &dec_out2);
  EXPECT_EQ(GCOMP_OK, s);
  s = gcomp_decoder_finish(decoder, &dec_out2);
  EXPECT_EQ(GCOMP_OK, s);

  EXPECT_EQ(first_size, dec_out2.used);
  EXPECT_EQ(0, memcmp(output1, output2, first_size));

  gcomp_decoder_destroy(decoder);
}

// =============================================================================
// Null Pointer Tests
// =============================================================================

TEST_F(StateMachineTest, Encoder_NullPointers) {
  // Various null pointer scenarios - should return errors, not crash
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t s;

  // Create with null output pointer
  s = gcomp_encoder_create(registry, "deflate", nullptr, nullptr);
  EXPECT_EQ(GCOMP_ERR_INVALID_ARG, s);

  // Create with null registry
  s = gcomp_encoder_create(nullptr, "deflate", nullptr, &encoder);
  EXPECT_EQ(GCOMP_ERR_INVALID_ARG, s);

  // Create valid encoder for further tests
  s = gcomp_encoder_create(registry, "deflate", nullptr, &encoder);
  ASSERT_EQ(GCOMP_OK, s);

  // Update with null input
  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};
  s = gcomp_encoder_update(encoder, nullptr, &out_buf);
  EXPECT_EQ(GCOMP_ERR_INVALID_ARG, s);

  // Update with null output
  const char * input = "test";
  gcomp_buffer_t in_buf = {input, strlen(input), 0};
  s = gcomp_encoder_update(encoder, &in_buf, nullptr);
  EXPECT_EQ(GCOMP_ERR_INVALID_ARG, s);

  // Finish with null output
  s = gcomp_encoder_finish(encoder, nullptr);
  EXPECT_EQ(GCOMP_ERR_INVALID_ARG, s);

  gcomp_encoder_destroy(encoder);

  // Destroy null encoder - should be safe
  gcomp_encoder_destroy(nullptr);
}

TEST_F(StateMachineTest, Decoder_NullPointers) {
  // Various null pointer scenarios
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t s;

  // Create with null output pointer
  s = gcomp_decoder_create(registry, "deflate", nullptr, nullptr);
  EXPECT_EQ(GCOMP_ERR_INVALID_ARG, s);

  // Create with null registry
  s = gcomp_decoder_create(nullptr, "deflate", nullptr, &decoder);
  EXPECT_EQ(GCOMP_ERR_INVALID_ARG, s);

  // Create valid decoder for further tests
  s = gcomp_decoder_create(registry, "deflate", nullptr, &decoder);
  ASSERT_EQ(GCOMP_OK, s);

  // Update with null input
  uint8_t output[256];
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};
  s = gcomp_decoder_update(decoder, nullptr, &out_buf);
  EXPECT_EQ(GCOMP_ERR_INVALID_ARG, s);

  // Update with null output
  uint8_t input[10];
  gcomp_buffer_t in_buf = {input, sizeof(input), 0};
  s = gcomp_decoder_update(decoder, &in_buf, nullptr);
  EXPECT_EQ(GCOMP_ERR_INVALID_ARG, s);

  // Finish with null output
  s = gcomp_decoder_finish(decoder, nullptr);
  EXPECT_EQ(GCOMP_ERR_INVALID_ARG, s);

  gcomp_decoder_destroy(decoder);

  // Destroy null decoder - should be safe
  gcomp_decoder_destroy(nullptr);
}

// =============================================================================
// Level 0 (Stored) Specific Tests
// =============================================================================

TEST_F(StateMachineTest, Encoder_Level0_FinishBeforeUpdate) {
  // Level 0 encoder: finish() before any update()
  gcomp_options_t * opts = nullptr;
  gcomp_status_t s = gcomp_options_create(&opts);
  ASSERT_EQ(GCOMP_OK, s);
  s = gcomp_options_set_int64(opts, "deflate.level", 0);
  ASSERT_EQ(GCOMP_OK, s);

  gcomp_encoder_t * encoder = nullptr;
  s = gcomp_encoder_create(registry, "deflate", opts, &encoder);
  ASSERT_EQ(GCOMP_OK, s);

  uint8_t output[256] = {0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  // finish() without update() - should produce empty stored block
  s = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

TEST_F(StateMachineTest, Encoder_Level0_MultipleFinishCalls) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t s = gcomp_options_create(&opts);
  ASSERT_EQ(GCOMP_OK, s);
  s = gcomp_options_set_int64(opts, "deflate.level", 0);
  ASSERT_EQ(GCOMP_OK, s);

  gcomp_encoder_t * encoder = nullptr;
  s = gcomp_encoder_create(registry, "deflate", opts, &encoder);
  ASSERT_EQ(GCOMP_OK, s);

  const char * input = "stored block test";
  gcomp_buffer_t in_buf = {input, strlen(input), 0};
  uint8_t output[256] = {0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  s = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);

  // Multiple finish calls
  s = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);
  s = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

// =============================================================================
// All Compression Levels
// =============================================================================

class StateMachineLevelTest : public ::testing::TestWithParam<int> {
protected:
  void SetUp() override {
    registry = gcomp_registry_default();
  }
  gcomp_registry_t * registry;
};

TEST_P(StateMachineLevelTest, FinishBeforeUpdate) {
  int level = GetParam();

  gcomp_options_t * opts = nullptr;
  gcomp_status_t s = gcomp_options_create(&opts);
  ASSERT_EQ(GCOMP_OK, s);
  s = gcomp_options_set_int64(opts, "deflate.level", level);
  ASSERT_EQ(GCOMP_OK, s);

  gcomp_encoder_t * encoder = nullptr;
  s = gcomp_encoder_create(registry, "deflate", opts, &encoder);
  ASSERT_EQ(GCOMP_OK, s);

  uint8_t output[256] = {0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  s = gcomp_encoder_finish(encoder, &out_buf);
  EXPECT_EQ(GCOMP_OK, s);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

TEST_P(StateMachineLevelTest, ResetBetweenCompressions) {
  int level = GetParam();

  gcomp_options_t * opts = nullptr;
  gcomp_status_t s = gcomp_options_create(&opts);
  ASSERT_EQ(GCOMP_OK, s);
  s = gcomp_options_set_int64(opts, "deflate.level", level);
  ASSERT_EQ(GCOMP_OK, s);

  gcomp_encoder_t * encoder = nullptr;
  s = gcomp_encoder_create(registry, "deflate", opts, &encoder);
  ASSERT_EQ(GCOMP_OK, s);

  // First round
  const char * input = "test data for level test";
  gcomp_buffer_t in_buf = {input, strlen(input), 0};
  uint8_t output1[512] = {0};
  gcomp_buffer_t out_buf1 = {output1, sizeof(output1), 0};

  s = gcomp_encoder_update(encoder, &in_buf, &out_buf1);
  EXPECT_EQ(GCOMP_OK, s);
  s = gcomp_encoder_finish(encoder, &out_buf1);
  EXPECT_EQ(GCOMP_OK, s);

  size_t first_size = out_buf1.used;

  // Reset
  s = gcomp_encoder_reset(encoder);
  EXPECT_EQ(GCOMP_OK, s);

  // Second round with same input
  in_buf.used = 0;
  uint8_t output2[512] = {0};
  gcomp_buffer_t out_buf2 = {output2, sizeof(output2), 0};

  s = gcomp_encoder_update(encoder, &in_buf, &out_buf2);
  EXPECT_EQ(GCOMP_OK, s);
  s = gcomp_encoder_finish(encoder, &out_buf2);
  EXPECT_EQ(GCOMP_OK, s);

  // Same input should produce same output
  EXPECT_EQ(first_size, out_buf2.used);
  EXPECT_EQ(0, memcmp(output1, output2, first_size));

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);
}

INSTANTIATE_TEST_SUITE_P(AllLevels, StateMachineLevelTest,
    ::testing::Values(0, 1, 2, 3, 4, 5, 6, 7, 8, 9),
    [](const ::testing::TestParamInfo<int> & info) {
      return "Level" + std::to_string(info.param);
    });

// =============================================================================
// Rapid Create/Destroy Cycles (Stress)
// =============================================================================

TEST_F(StateMachineTest, Encoder_RapidCreateDestroy) {
  // Create and destroy encoders rapidly - stress test for leaks
  for (int i = 0; i < 100; i++) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t s =
        gcomp_encoder_create(registry, "deflate", nullptr, &encoder);
    ASSERT_EQ(GCOMP_OK, s);
    gcomp_encoder_destroy(encoder);
  }
}

TEST_F(StateMachineTest, Decoder_RapidCreateDestroy) {
  // Create and destroy decoders rapidly
  for (int i = 0; i < 100; i++) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t s =
        gcomp_decoder_create(registry, "deflate", nullptr, &decoder);
    ASSERT_EQ(GCOMP_OK, s);
    gcomp_decoder_destroy(decoder);
  }
}

TEST_F(StateMachineTest, Encoder_RapidResetCycles) {
  // Create encoder, then reset repeatedly
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t s =
      gcomp_encoder_create(registry, "deflate", nullptr, &encoder);
  ASSERT_EQ(GCOMP_OK, s);

  for (int i = 0; i < 100; i++) {
    const char * input = "test";
    gcomp_buffer_t in_buf = {input, strlen(input), 0};
    uint8_t output[256] = {0};
    gcomp_buffer_t out_buf = {output, sizeof(output), 0};

    s = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    EXPECT_EQ(GCOMP_OK, s);
    s = gcomp_encoder_finish(encoder, &out_buf);
    EXPECT_EQ(GCOMP_OK, s);
    s = gcomp_encoder_reset(encoder);
    EXPECT_EQ(GCOMP_OK, s);
  }

  gcomp_encoder_destroy(encoder);
}

TEST_F(StateMachineTest, Decoder_RapidResetCycles) {
  // First create valid compressed data
  uint8_t compressed[256];
  size_t compressed_size;
  {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t s =
        gcomp_encoder_create(registry, "deflate", nullptr, &encoder);
    ASSERT_EQ(GCOMP_OK, s);

    const char * input = "test data for rapid reset";
    gcomp_buffer_t in_buf = {input, strlen(input), 0};
    gcomp_buffer_t out_buf = {compressed, sizeof(compressed), 0};
    s = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    ASSERT_EQ(GCOMP_OK, s);
    s = gcomp_encoder_finish(encoder, &out_buf);
    ASSERT_EQ(GCOMP_OK, s);
    compressed_size = out_buf.used;
    gcomp_encoder_destroy(encoder);
  }

  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t s =
      gcomp_decoder_create(registry, "deflate", nullptr, &decoder);
  ASSERT_EQ(GCOMP_OK, s);

  for (int i = 0; i < 100; i++) {
    gcomp_buffer_t dec_in = {compressed, compressed_size, 0};
    uint8_t output[256] = {0};
    gcomp_buffer_t dec_out = {output, sizeof(output), 0};

    s = gcomp_decoder_update(decoder, &dec_in, &dec_out);
    EXPECT_EQ(GCOMP_OK, s);
    s = gcomp_decoder_finish(decoder, &dec_out);
    EXPECT_EQ(GCOMP_OK, s);
    s = gcomp_decoder_reset(decoder);
    EXPECT_EQ(GCOMP_OK, s);
  }

  gcomp_decoder_destroy(decoder);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
