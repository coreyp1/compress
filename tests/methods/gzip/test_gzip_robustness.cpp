/**
 * @file test_gzip_robustness.cpp
 *
 * State machine robustness tests for the gzip encoder and decoder.
 *
 * These tests verify that the gzip implementation handles unexpected or
 * edge-case API call sequences without crashing, leaking memory, or
 * triggering undefined behavior.
 *
 * Test categories:
 * - finish() before any update()
 * - update() after finish() returned success
 * - Multiple finish() calls
 * - destroy() without calling finish()
 * - update() with zero-size buffers
 * - reset() mid-stream
 *
 * Run these tests under valgrind and ASan+UBSan to verify memory safety.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/gzip.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <vector>

//
// Test fixture
//

class GzipRobustnessTest : public ::testing::Test {
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

// ============================================================================
// Encoder Robustness Tests
// ============================================================================

//
// Test: finish() before any update()
//
// The encoder should handle finish() being called immediately after creation
// without any update() calls. This produces a valid gzip stream with empty
// content.
//

TEST_F(GzipRobustnessTest, EncoderFinishBeforeUpdate) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(encoder, nullptr);

  // Call finish() without any prior update()
  std::vector<uint8_t> out_buf(1024);
  gcomp_buffer_t output = {out_buf.data(), out_buf.size(), 0};

  status = gcomp_encoder_finish(encoder, &output);
  EXPECT_EQ(status, GCOMP_OK);

  // Should produce a valid (minimal) gzip stream
  EXPECT_GE(output.used, 10u + 8u); // At least header + trailer

  gcomp_encoder_destroy(encoder);
}

//
// Test: update() after finish() returned success
//
// After finish() completes successfully, the encoder is in DONE state.
// Calling update() in this state should either:
// - Return OK but not consume any input (graceful no-op), or
// - Return an error indicating the stream is complete
//
// Either behavior is acceptable as long as it doesn't crash.
//

TEST_F(GzipRobustnessTest, EncoderUpdateAfterFinish) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Complete a normal encode
  const char * input = "Hello, World!";
  size_t input_len = strlen(input);
  std::vector<uint8_t> out_buf(1024);

  gcomp_buffer_t in_buf = {const_cast<char *>(input), input_len, 0};
  gcomp_buffer_t output = {out_buf.data(), out_buf.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &output);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(in_buf.used, input_len);

  status = gcomp_encoder_finish(encoder, &output);
  ASSERT_EQ(status, GCOMP_OK);

  // Now try to call update() again - should not crash
  const char * extra = "Extra data";
  gcomp_buffer_t extra_in = {const_cast<char *>(extra), strlen(extra), 0};

  // This may return OK or an error - both are acceptable
  // The key assertion is: no crash
  (void)gcomp_encoder_update(encoder, &extra_in, &output);

  gcomp_encoder_destroy(encoder);
}

//
// Test: Multiple finish() calls
//
// Calling finish() multiple times after the stream is complete should be
// safe and return OK (idempotent behavior).
//

TEST_F(GzipRobustnessTest, EncoderMultipleFinishCalls) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> out_buf(1024);
  gcomp_buffer_t output = {out_buf.data(), out_buf.size(), 0};

  // First finish
  status = gcomp_encoder_finish(encoder, &output);
  ASSERT_EQ(status, GCOMP_OK);
  size_t first_size = output.used;

  // Second finish - should be safe
  status = gcomp_encoder_finish(encoder, &output);
  EXPECT_EQ(status, GCOMP_OK);
  // Should not produce additional output
  EXPECT_EQ(output.used, first_size);

  // Third finish - still safe
  status = gcomp_encoder_finish(encoder, &output);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(output.used, first_size);

  gcomp_encoder_destroy(encoder);
}

//
// Test: destroy() without calling finish()
//
// It should be safe to destroy an encoder at any point without calling
// finish(). This tests cleanup of partially-used state.
//

TEST_F(GzipRobustnessTest, EncoderDestroyWithoutFinish) {
  // Test 1: Destroy immediately after creation
  {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
    ASSERT_EQ(status, GCOMP_OK);
    gcomp_encoder_destroy(encoder); // Should not crash or leak
  }

  // Test 2: Destroy after partial update
  {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
    ASSERT_EQ(status, GCOMP_OK);

    const char * input = "Partial data";
    std::vector<uint8_t> out_buf(1024);
    gcomp_buffer_t in_buf = {const_cast<char *>(input), strlen(input), 0};
    gcomp_buffer_t output = {out_buf.data(), out_buf.size(), 0};

    status = gcomp_encoder_update(encoder, &in_buf, &output);
    ASSERT_EQ(status, GCOMP_OK);

    gcomp_encoder_destroy(encoder); // Should not crash or leak
  }

  // Test 3: Destroy during header output (tiny buffer forces partial header)
  {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
    ASSERT_EQ(status, GCOMP_OK);

    const char * input = "Data";
    uint8_t tiny_out[3];
    gcomp_buffer_t in_buf = {const_cast<char *>(input), strlen(input), 0};
    gcomp_buffer_t output = {tiny_out, sizeof(tiny_out), 0};

    // This will likely only write part of the header
    status = gcomp_encoder_update(encoder, &in_buf, &output);
    ASSERT_EQ(status, GCOMP_OK);

    gcomp_encoder_destroy(encoder); // Should not crash or leak
  }
}

//
// Test: update() with zero-size buffers
//
// Calling update() with empty input and/or output buffers should be handled
// gracefully.
//

TEST_F(GzipRobustnessTest, EncoderUpdateZeroSizeBuffers) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> out_buf(1024);

  // Test 1: Zero-size input
  {
    gcomp_buffer_t empty_in = {nullptr, 0, 0};
    gcomp_buffer_t output = {out_buf.data(), out_buf.size(), 0};

    status = gcomp_encoder_update(encoder, &empty_in, &output);
    EXPECT_EQ(status, GCOMP_OK);
  }

  // Test 2: Zero-size output (but valid input)
  {
    const char * input = "Test";
    gcomp_buffer_t in_buf = {const_cast<char *>(input), strlen(input), 0};
    uint8_t dummy;
    gcomp_buffer_t zero_out = {&dummy, 0, 0};

    status = gcomp_encoder_update(encoder, &in_buf, &zero_out);
    EXPECT_EQ(status, GCOMP_OK);
    // Input may or may not be consumed depending on internal buffering
  }

  // Test 3: Both zero-size
  {
    gcomp_buffer_t empty_in = {nullptr, 0, 0};
    uint8_t dummy;
    gcomp_buffer_t zero_out = {&dummy, 0, 0};

    status = gcomp_encoder_update(encoder, &empty_in, &zero_out);
    EXPECT_EQ(status, GCOMP_OK);
  }

  gcomp_encoder_destroy(encoder);
}

//
// Test: reset() mid-stream
//
// Calling reset() while encoding is in progress should reset the encoder
// to a fresh state, ready to encode new data.
//

TEST_F(GzipRobustnessTest, EncoderResetMidStream) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Start encoding some data
  const char * input1 = "First chunk of data";
  std::vector<uint8_t> out_buf(1024);
  gcomp_buffer_t in_buf = {const_cast<char *>(input1), strlen(input1), 0};
  gcomp_buffer_t output = {out_buf.data(), out_buf.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &output);
  ASSERT_EQ(status, GCOMP_OK);

  // Reset mid-stream
  status = gcomp_encoder_reset(encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Now encode different data
  const char * input2 = "Different data after reset";
  output.used = 0; // Reset output buffer
  gcomp_buffer_t in_buf2 = {const_cast<char *>(input2), strlen(input2), 0};

  status = gcomp_encoder_update(encoder, &in_buf2, &output);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &output);
  ASSERT_EQ(status, GCOMP_OK);

  // Verify the output is valid gzip that decodes to input2
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> decompressed(1024);
  gcomp_buffer_t dec_in = {out_buf.data(), output.used, 0};
  gcomp_buffer_t dec_out = {decompressed.data(), decompressed.size(), 0};

  status = gcomp_decoder_update(decoder, &dec_in, &dec_out);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_decoder_finish(decoder, &dec_out);
  ASSERT_EQ(status, GCOMP_OK);

  // Verify decoded content matches input2
  EXPECT_EQ(dec_out.used, strlen(input2));
  EXPECT_EQ(memcmp(decompressed.data(), input2, strlen(input2)), 0);

  gcomp_decoder_destroy(decoder);
  gcomp_encoder_destroy(encoder);
}

//
// Test: reset() immediately after creation
//

TEST_F(GzipRobustnessTest, EncoderResetImmediatelyAfterCreation) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Reset immediately (should be no-op but safe)
  status = gcomp_encoder_reset(encoder);
  EXPECT_EQ(status, GCOMP_OK);

  // Should still work normally
  const char * input = "Test data";
  std::vector<uint8_t> out_buf(1024);
  gcomp_buffer_t in_buf = {const_cast<char *>(input), strlen(input), 0};
  gcomp_buffer_t output = {out_buf.data(), out_buf.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &output);
  EXPECT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &output);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_encoder_destroy(encoder);
}

//
// Test: reset() after finish()
//

TEST_F(GzipRobustnessTest, EncoderResetAfterFinish) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Complete first encode
  const char * input1 = "First stream";
  std::vector<uint8_t> out_buf(1024);
  gcomp_buffer_t in_buf = {const_cast<char *>(input1), strlen(input1), 0};
  gcomp_buffer_t output = {out_buf.data(), out_buf.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &output);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &output);
  ASSERT_EQ(status, GCOMP_OK);

  // Reset after complete stream
  status = gcomp_encoder_reset(encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Encode second stream
  const char * input2 = "Second stream";
  output.used = 0;
  gcomp_buffer_t in_buf2 = {const_cast<char *>(input2), strlen(input2), 0};

  status = gcomp_encoder_update(encoder, &in_buf2, &output);
  EXPECT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &output);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_encoder_destroy(encoder);
}

// ============================================================================
// Decoder Robustness Tests
// ============================================================================

//
// Test: finish() before any update()
//
// Calling finish() without providing any input should return an error
// indicating the stream is truncated (in HEADER stage).
//

TEST_F(GzipRobustnessTest, DecoderFinishBeforeUpdate) {
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(decoder, nullptr);

  std::vector<uint8_t> out_buf(1024);
  gcomp_buffer_t output = {out_buf.data(), out_buf.size(), 0};

  // Call finish() without any prior update() - should fail (truncated)
  status = gcomp_decoder_finish(decoder, &output);
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);

  gcomp_decoder_destroy(decoder);
}

//
// Test: update() after finish() returned success
//
// After a complete gzip stream is decoded, the decoder is in DONE state.
// Calling update() should be handled gracefully.
//

TEST_F(GzipRobustnessTest, DecoderUpdateAfterFinish) {
  // First, create valid compressed data
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  const char * input = "Hello, World!";
  std::vector<uint8_t> compressed(1024);
  gcomp_buffer_t enc_in = {const_cast<char *>(input), strlen(input), 0};
  gcomp_buffer_t enc_out = {compressed.data(), compressed.size(), 0};

  status = gcomp_encoder_update(encoder, &enc_in, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  gcomp_encoder_destroy(encoder);

  // Now decode
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> decompressed(1024);
  gcomp_buffer_t dec_in = {compressed.data(), enc_out.used, 0};
  gcomp_buffer_t dec_out = {decompressed.data(), decompressed.size(), 0};

  status = gcomp_decoder_update(decoder, &dec_in, &dec_out);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_decoder_finish(decoder, &dec_out);
  ASSERT_EQ(status, GCOMP_OK);

  // Now try to call update() again - should not crash
  uint8_t extra_data[] = {0x1F, 0x8B, 0x08}; // Looks like start of gzip
  gcomp_buffer_t extra_in = {extra_data, sizeof(extra_data), 0};

  // This may return OK (no-op) or error - both acceptable as long as no crash
  status = gcomp_decoder_update(decoder, &extra_in, &dec_out);

  gcomp_decoder_destroy(decoder);
}

//
// Test: Multiple finish() calls
//

TEST_F(GzipRobustnessTest, DecoderMultipleFinishCalls) {
  // Create valid compressed data
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  const char * input = "Test";
  std::vector<uint8_t> compressed(1024);
  gcomp_buffer_t enc_in = {const_cast<char *>(input), strlen(input), 0};
  gcomp_buffer_t enc_out = {compressed.data(), compressed.size(), 0};

  status = gcomp_encoder_update(encoder, &enc_in, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  gcomp_encoder_destroy(encoder);

  // Decode
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> decompressed(1024);
  gcomp_buffer_t dec_in = {compressed.data(), enc_out.used, 0};
  gcomp_buffer_t dec_out = {decompressed.data(), decompressed.size(), 0};

  status = gcomp_decoder_update(decoder, &dec_in, &dec_out);
  ASSERT_EQ(status, GCOMP_OK);

  // First finish
  status = gcomp_decoder_finish(decoder, &dec_out);
  ASSERT_EQ(status, GCOMP_OK);
  size_t first_size = dec_out.used;

  // Second finish - should still return OK
  status = gcomp_decoder_finish(decoder, &dec_out);
  EXPECT_EQ(status, GCOMP_OK);
  EXPECT_EQ(dec_out.used, first_size);

  // Third finish
  status = gcomp_decoder_finish(decoder, &dec_out);
  EXPECT_EQ(status, GCOMP_OK);

  gcomp_decoder_destroy(decoder);
}

//
// Test: destroy() without calling finish()
//

TEST_F(GzipRobustnessTest, DecoderDestroyWithoutFinish) {
  // Create valid compressed data
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  const char * input = "Some test data for destroy test";
  std::vector<uint8_t> compressed(1024);
  gcomp_buffer_t enc_in = {const_cast<char *>(input), strlen(input), 0};
  gcomp_buffer_t enc_out = {compressed.data(), compressed.size(), 0};

  status = gcomp_encoder_update(encoder, &enc_in, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  gcomp_encoder_destroy(encoder);

  // Test 1: Destroy immediately after creation
  {
    gcomp_decoder_t * decoder = nullptr;
    status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
    ASSERT_EQ(status, GCOMP_OK);
    gcomp_decoder_destroy(decoder); // Should not crash or leak
  }

  // Test 2: Destroy after partial header parse
  {
    gcomp_decoder_t * decoder = nullptr;
    status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
    ASSERT_EQ(status, GCOMP_OK);

    std::vector<uint8_t> decompressed(1024);
    // Feed only first 5 bytes of header
    gcomp_buffer_t dec_in = {compressed.data(), 5, 0};
    gcomp_buffer_t dec_out = {decompressed.data(), decompressed.size(), 0};

    status = gcomp_decoder_update(decoder, &dec_in, &dec_out);
    ASSERT_EQ(status, GCOMP_OK);

    gcomp_decoder_destroy(decoder); // Should not crash or leak
  }

  // Test 3: Destroy mid-body decode
  {
    gcomp_decoder_t * decoder = nullptr;
    status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
    ASSERT_EQ(status, GCOMP_OK);

    std::vector<uint8_t> decompressed(1024);
    // Feed header + some body, but not all
    size_t partial_len = enc_out.used > 20 ? 20 : enc_out.used;
    gcomp_buffer_t dec_in = {compressed.data(), partial_len, 0};
    gcomp_buffer_t dec_out = {decompressed.data(), decompressed.size(), 0};

    status = gcomp_decoder_update(decoder, &dec_in, &dec_out);
    ASSERT_EQ(status, GCOMP_OK);

    gcomp_decoder_destroy(decoder); // Should not crash or leak
  }

  // Test 4: Destroy after complete decode but before finish()
  {
    gcomp_decoder_t * decoder = nullptr;
    status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
    ASSERT_EQ(status, GCOMP_OK);

    std::vector<uint8_t> decompressed(1024);
    gcomp_buffer_t dec_in = {compressed.data(), enc_out.used, 0};
    gcomp_buffer_t dec_out = {decompressed.data(), decompressed.size(), 0};

    status = gcomp_decoder_update(decoder, &dec_in, &dec_out);
    ASSERT_EQ(status, GCOMP_OK);

    // Don't call finish()
    gcomp_decoder_destroy(decoder); // Should not crash or leak
  }
}

//
// Test: update() with zero-size buffers
//

TEST_F(GzipRobustnessTest, DecoderUpdateZeroSizeBuffers) {
  // Create valid compressed data
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  const char * input = "Test data";
  std::vector<uint8_t> compressed(1024);
  gcomp_buffer_t enc_in = {const_cast<char *>(input), strlen(input), 0};
  gcomp_buffer_t enc_out = {compressed.data(), compressed.size(), 0};

  status = gcomp_encoder_update(encoder, &enc_in, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  gcomp_encoder_destroy(encoder);

  // Test with decoder
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> decompressed(1024);

  // Test 1: Zero-size input
  {
    gcomp_buffer_t empty_in = {nullptr, 0, 0};
    gcomp_buffer_t dec_out = {decompressed.data(), decompressed.size(), 0};

    status = gcomp_decoder_update(decoder, &empty_in, &dec_out);
    EXPECT_EQ(status, GCOMP_OK);
  }

  // Test 2: Zero-size output
  {
    gcomp_buffer_t dec_in = {compressed.data(), enc_out.used, 0};
    uint8_t dummy;
    gcomp_buffer_t zero_out = {&dummy, 0, 0};

    status = gcomp_decoder_update(decoder, &dec_in, &zero_out);
    EXPECT_EQ(status, GCOMP_OK);
  }

  // Test 3: Both zero-size
  {
    gcomp_buffer_t empty_in = {nullptr, 0, 0};
    uint8_t dummy;
    gcomp_buffer_t zero_out = {&dummy, 0, 0};

    status = gcomp_decoder_update(decoder, &empty_in, &zero_out);
    EXPECT_EQ(status, GCOMP_OK);
  }

  gcomp_decoder_destroy(decoder);
}

//
// Test: reset() mid-stream
//

TEST_F(GzipRobustnessTest, DecoderResetMidStream) {
  // Create valid compressed data for two different inputs
  std::vector<uint8_t> compressed1(1024), compressed2(1024);

  // Compress input1
  {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
    ASSERT_EQ(status, GCOMP_OK);

    const char * input1 = "First stream data";
    gcomp_buffer_t enc_in = {const_cast<char *>(input1), strlen(input1), 0};
    gcomp_buffer_t enc_out = {compressed1.data(), compressed1.size(), 0};

    status = gcomp_encoder_update(encoder, &enc_in, &enc_out);
    ASSERT_EQ(status, GCOMP_OK);
    status = gcomp_encoder_finish(encoder, &enc_out);
    ASSERT_EQ(status, GCOMP_OK);
    compressed1.resize(enc_out.used);
    gcomp_encoder_destroy(encoder);
  }

  // Compress input2
  {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
    ASSERT_EQ(status, GCOMP_OK);

    const char * input2 = "Second different stream";
    gcomp_buffer_t enc_in = {const_cast<char *>(input2), strlen(input2), 0};
    gcomp_buffer_t enc_out = {compressed2.data(), compressed2.size(), 0};

    status = gcomp_encoder_update(encoder, &enc_in, &enc_out);
    ASSERT_EQ(status, GCOMP_OK);
    status = gcomp_encoder_finish(encoder, &enc_out);
    ASSERT_EQ(status, GCOMP_OK);
    compressed2.resize(enc_out.used);
    gcomp_encoder_destroy(encoder);
  }

  // Now test reset mid-stream
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> decompressed(1024);

  // Start decoding first stream (partial)
  {
    gcomp_buffer_t dec_in = {compressed1.data(), compressed1.size() / 2, 0};
    gcomp_buffer_t dec_out = {decompressed.data(), decompressed.size(), 0};

    status = gcomp_decoder_update(decoder, &dec_in, &dec_out);
    ASSERT_EQ(status, GCOMP_OK);
  }

  // Reset mid-stream
  status = gcomp_decoder_reset(decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Decode second stream completely
  {
    gcomp_buffer_t dec_in = {compressed2.data(), compressed2.size(), 0};
    gcomp_buffer_t dec_out = {decompressed.data(), decompressed.size(), 0};

    status = gcomp_decoder_update(decoder, &dec_in, &dec_out);
    ASSERT_EQ(status, GCOMP_OK);

    status = gcomp_decoder_finish(decoder, &dec_out);
    ASSERT_EQ(status, GCOMP_OK);

    // Verify decoded content matches input2
    const char * input2 = "Second different stream";
    EXPECT_EQ(dec_out.used, strlen(input2));
    EXPECT_EQ(memcmp(decompressed.data(), input2, strlen(input2)), 0);
  }

  gcomp_decoder_destroy(decoder);
}

//
// Test: reset() immediately after creation
//

TEST_F(GzipRobustnessTest, DecoderResetImmediatelyAfterCreation) {
  gcomp_decoder_t * decoder = nullptr;
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Reset immediately (should be no-op but safe)
  status = gcomp_decoder_reset(decoder);
  EXPECT_EQ(status, GCOMP_OK);

  // Should still work normally
  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  const char * input = "Test data";
  std::vector<uint8_t> compressed(1024);
  gcomp_buffer_t enc_in = {const_cast<char *>(input), strlen(input), 0};
  gcomp_buffer_t enc_out = {compressed.data(), compressed.size(), 0};

  status = gcomp_encoder_update(encoder, &enc_in, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  gcomp_encoder_destroy(encoder);

  std::vector<uint8_t> decompressed(1024);
  gcomp_buffer_t dec_in = {compressed.data(), enc_out.used, 0};
  gcomp_buffer_t dec_out = {decompressed.data(), decompressed.size(), 0};

  status = gcomp_decoder_update(decoder, &dec_in, &dec_out);
  EXPECT_EQ(status, GCOMP_OK);

  status = gcomp_decoder_finish(decoder, &dec_out);
  EXPECT_EQ(status, GCOMP_OK);

  EXPECT_EQ(dec_out.used, strlen(input));
  EXPECT_EQ(memcmp(decompressed.data(), input, strlen(input)), 0);

  gcomp_decoder_destroy(decoder);
}

//
// Test: reset() after finish() (for decoder reuse)
//

TEST_F(GzipRobustnessTest, DecoderResetAfterFinish) {
  // Create compressed data
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  const char * input = "Test data for reset after finish";
  std::vector<uint8_t> compressed(1024);
  gcomp_buffer_t enc_in = {const_cast<char *>(input), strlen(input), 0};
  gcomp_buffer_t enc_out = {compressed.data(), compressed.size(), 0};

  status = gcomp_encoder_update(encoder, &enc_in, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  gcomp_encoder_destroy(encoder);

  // Decode first time
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> decompressed(1024);
  gcomp_buffer_t dec_in = {compressed.data(), enc_out.used, 0};
  gcomp_buffer_t dec_out = {decompressed.data(), decompressed.size(), 0};

  status = gcomp_decoder_update(decoder, &dec_in, &dec_out);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_decoder_finish(decoder, &dec_out);
  ASSERT_EQ(status, GCOMP_OK);

  // Reset after finish
  status = gcomp_decoder_reset(decoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Decode again
  dec_in.used = 0;
  dec_out.used = 0;

  status = gcomp_decoder_update(decoder, &dec_in, &dec_out);
  EXPECT_EQ(status, GCOMP_OK);

  status = gcomp_decoder_finish(decoder, &dec_out);
  EXPECT_EQ(status, GCOMP_OK);

  EXPECT_EQ(dec_out.used, strlen(input));
  EXPECT_EQ(memcmp(decompressed.data(), input, strlen(input)), 0);

  gcomp_decoder_destroy(decoder);
}

// ============================================================================
// Edge Case Tests
// ============================================================================

//
// Test: Finish with small but adequate buffer
//
// This test verifies that the encoder handles small output buffers during
// finish() gracefully when the buffer is large enough to hold the trailer.
// (Note: 1-byte buffer streaming is covered in test_gzip_streaming.cpp)
//

TEST_F(GzipRobustnessTest, EncoderFinishWithSmallBuffer) {
  gcomp_encoder_t * encoder = nullptr;
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Feed some input with a normal-sized output buffer
  const char * input = "Hello, World!";
  std::vector<uint8_t> out_buf(2048);
  gcomp_buffer_t in_buf = {const_cast<char *>(input), strlen(input), 0};
  gcomp_buffer_t output = {out_buf.data(), out_buf.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &output);
  ASSERT_EQ(status, GCOMP_OK);

  // Finish with a 32-byte buffer (small but adequate)
  std::vector<uint8_t> finish_output(32);
  gcomp_buffer_t finish_buf = {finish_output.data(), finish_output.size(), 0};

  status = gcomp_encoder_finish(encoder, &finish_buf);
  EXPECT_EQ(status, GCOMP_OK);

  // Combined output should be valid gzip
  size_t total = output.used + finish_buf.used;
  EXPECT_GE(total, 18u); // At least header (10) + trailer (8)

  gcomp_encoder_destroy(encoder);
}

//
// Test: Decoder with header field at buffer boundary
//

TEST_F(GzipRobustnessTest, DecoderHeaderFieldBoundary) {
  // Create gzip with optional fields
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_options_set_string(opts, "gzip.name", "test.txt");
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_string(opts, "gzip.comment", "A test comment");
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "gzip", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  const char * input = "Test data";
  std::vector<uint8_t> compressed(2048);
  gcomp_buffer_t enc_in = {const_cast<char *>(input), strlen(input), 0};
  gcomp_buffer_t enc_out = {compressed.data(), compressed.size(), 0};

  status = gcomp_encoder_update(encoder, &enc_in, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(encoder, &enc_out);
  ASSERT_EQ(status, GCOMP_OK);
  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);

  // Decode one byte at a time to test header parsing at boundaries
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> decompressed(1024);
  gcomp_buffer_t dec_out = {decompressed.data(), decompressed.size(), 0};

  for (size_t i = 0; i < enc_out.used; i++) {
    gcomp_buffer_t dec_in = {compressed.data() + i, 1, 0};
    status = gcomp_decoder_update(decoder, &dec_in, &dec_out);
    ASSERT_EQ(status, GCOMP_OK) << "Failed at byte " << i;
  }

  status = gcomp_decoder_finish(decoder, &dec_out);
  EXPECT_EQ(status, GCOMP_OK);

  EXPECT_EQ(dec_out.used, strlen(input));
  EXPECT_EQ(memcmp(decompressed.data(), input, strlen(input)), 0);

  gcomp_decoder_destroy(decoder);
}

//
// Test: NULL pointer handling
//

TEST_F(GzipRobustnessTest, NullPointerHandling) {
  // Encoder
  {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "gzip", nullptr, &encoder);
    ASSERT_EQ(status, GCOMP_OK);

    // NULL input buffer
    std::vector<uint8_t> out_buf(1024);
    gcomp_buffer_t output = {out_buf.data(), out_buf.size(), 0};
    status = gcomp_encoder_update(encoder, nullptr, &output);
    EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

    // NULL output buffer
    uint8_t in_byte = 0;
    gcomp_buffer_t in_buf = {&in_byte, 1, 0};
    status = gcomp_encoder_update(encoder, &in_buf, nullptr);
    EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

    // NULL output in finish
    status = gcomp_encoder_finish(encoder, nullptr);
    EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

    gcomp_encoder_destroy(encoder);
  }

  // Decoder
  {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "gzip", nullptr, &decoder);
    ASSERT_EQ(status, GCOMP_OK);

    // NULL input buffer
    std::vector<uint8_t> out_buf(1024);
    gcomp_buffer_t output = {out_buf.data(), out_buf.size(), 0};
    status = gcomp_decoder_update(decoder, nullptr, &output);
    EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

    // NULL output buffer
    uint8_t in_byte = 0x1F;
    gcomp_buffer_t in_buf = {&in_byte, 1, 0};
    status = gcomp_decoder_update(decoder, &in_buf, nullptr);
    EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

    gcomp_decoder_destroy(decoder);
  }

  // Destroy with NULL
  gcomp_encoder_destroy(nullptr); // Should not crash
  gcomp_decoder_destroy(nullptr); // Should not crash
}

//
// Test: Reset with NULL encoder/decoder state
//

TEST_F(GzipRobustnessTest, ResetWithNullState) {
  // Test reset on NULL encoder
  gcomp_status_t status = gcomp_encoder_reset(nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);

  // Test reset on NULL decoder
  status = gcomp_decoder_reset(nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
