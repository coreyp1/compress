/**
 * @file test_callback_api.cpp
 *
 * Unit tests for callback-based streaming API in the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "passthru_method.h"
#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <vector>

using namespace gcomp_test;

// Context for simple in-memory read callback
struct ReadContext {
  const uint8_t * data;
  size_t size;
  size_t offset;
  bool partial_reads;   // If true, return partial reads
  size_t max_read_size; // Maximum bytes to read per call (0 = unlimited)
};

// Simple read callback that reads from memory
static gcomp_status_t simple_read_cb(
    void * ctx, uint8_t * dst, size_t cap, size_t * out_n) {
  ReadContext * read_ctx = static_cast<ReadContext *>(ctx);
  if (!read_ctx || !dst || !out_n) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (read_ctx->offset >= read_ctx->size) {
    *out_n = 0; // EOF
    return GCOMP_OK;
  }

  size_t to_read = read_ctx->size - read_ctx->offset;
  if (read_ctx->max_read_size > 0 && to_read > read_ctx->max_read_size) {
    to_read = read_ctx->max_read_size;
  }

  if (to_read > cap) {
    to_read = cap;
  }

  if (read_ctx->partial_reads && to_read > 1) {
    // Return partial read (half of available)
    to_read = to_read / 2;
    if (to_read == 0) {
      to_read = 1;
    }
  }

  memcpy(dst, read_ctx->data + read_ctx->offset, to_read);
  read_ctx->offset += to_read;
  *out_n = to_read;
  return GCOMP_OK;
}

// Context for simple in-memory write callback
struct WriteContext {
  std::vector<uint8_t> * buffer;
  bool partial_writes;   // If true, accept partial writes
  size_t max_write_size; // Maximum bytes to write per call (0 = unlimited)
  gcomp_status_t error_status; // Error to return (GCOMP_OK = no error)
};

// Simple write callback that writes to memory
static gcomp_status_t simple_write_cb(
    void * ctx, const uint8_t * src, size_t n, size_t * out_n) {
  WriteContext * write_ctx = static_cast<WriteContext *>(ctx);
  if (!write_ctx || !src || !out_n) {
    return GCOMP_ERR_INVALID_ARG;
  }

  if (write_ctx->error_status != GCOMP_OK) {
    return write_ctx->error_status;
  }

  size_t to_write = n;
  if (write_ctx->max_write_size > 0 && to_write > write_ctx->max_write_size) {
    to_write = write_ctx->max_write_size;
  }

  if (write_ctx->partial_writes && to_write > 1) {
    // Accept partial write (half of requested)
    to_write = to_write / 2;
    if (to_write == 0) {
      to_write = 1;
    }
  }

  if (write_ctx->buffer) {
    write_ctx->buffer->insert(write_ctx->buffer->end(), src, src + to_write);
  }

  *out_n = to_write;
  return GCOMP_OK;
}

class CallbackApiTest : public ::testing::Test {
protected:
  void SetUp() override {
    gcomp_status_t status = gcomp_registry_create(nullptr, &registry_);
    ASSERT_EQ(status, GCOMP_OK);
    ASSERT_NE(registry_, nullptr);

    // Register pass-through method (store as member to keep it alive)
    passthru_method_ = create_passthru_method("passthru");
    status = gcomp_registry_register(registry_, &passthru_method_);
    ASSERT_EQ(status, GCOMP_OK);
  }

  void TearDown() override {
    if (registry_) {
      gcomp_registry_destroy(registry_);
      registry_ = nullptr;
    }
  }

  gcomp_registry_t * registry_;
  gcomp_method_t passthru_method_; // Keep method alive for registry
};

// Test basic encode with callbacks
TEST_F(CallbackApiTest, EncodeStreamCb_Basic) {
  const uint8_t input[] = {
      'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};
  const size_t input_size = sizeof(input);

  ReadContext read_ctx = {input, input_size, 0, false, 0};
  std::vector<uint8_t> output;
  WriteContext write_ctx = {&output, false, 0, GCOMP_OK};

  gcomp_status_t status = gcomp_encode_stream_cb(registry_, "passthru", nullptr,
      simple_read_cb, &read_ctx, simple_write_cb, &write_ctx);

  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(output.size(), input_size);
  ASSERT_EQ(memcmp(input, output.data(), input_size), 0);
}

// Test basic decode with callbacks
TEST_F(CallbackApiTest, DecodeStreamCb_Basic) {
  const uint8_t input[] = {
      'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};
  const size_t input_size = sizeof(input);

  ReadContext read_ctx = {input, input_size, 0, false, 0};
  std::vector<uint8_t> output;
  WriteContext write_ctx = {&output, false, 0, GCOMP_OK};

  gcomp_status_t status = gcomp_decode_stream_cb(registry_, "passthru", nullptr,
      simple_read_cb, &read_ctx, simple_write_cb, &write_ctx);

  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(output.size(), input_size);
  ASSERT_EQ(memcmp(input, output.data(), input_size), 0);
}

// Test NULL pointer handling
TEST_F(CallbackApiTest, EncodeStreamCb_NullPointers) {
  const uint8_t input[] = {'H', 'e', 'l', 'l', 'o'};
  ReadContext read_ctx = {input, sizeof(input), 0, false, 0};
  std::vector<uint8_t> output;
  WriteContext write_ctx = {&output, false, 0, GCOMP_OK};

  // NULL method_name
  gcomp_status_t status = gcomp_encode_stream_cb(registry_, nullptr, nullptr,
      simple_read_cb, &read_ctx, simple_write_cb, &write_ctx);
  ASSERT_EQ(status, GCOMP_ERR_INVALID_ARG);

  // NULL read_cb
  status = gcomp_encode_stream_cb(registry_, "passthru", nullptr, nullptr,
      &read_ctx, simple_write_cb, &write_ctx);
  ASSERT_EQ(status, GCOMP_ERR_INVALID_ARG);

  // NULL write_cb
  status = gcomp_encode_stream_cb(registry_, "passthru", nullptr,
      simple_read_cb, &read_ctx, nullptr, &write_ctx);
  ASSERT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

// Test partial read handling
TEST_F(CallbackApiTest, EncodeStreamCb_PartialReads) {
  const uint8_t input[] = {
      'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};
  const size_t input_size = sizeof(input);

  ReadContext read_ctx = {
      input, input_size, 0, true, 0}; // Enable partial reads
  std::vector<uint8_t> output;
  WriteContext write_ctx = {&output, false, 0, GCOMP_OK};

  gcomp_status_t status = gcomp_encode_stream_cb(registry_, "passthru", nullptr,
      simple_read_cb, &read_ctx, simple_write_cb, &write_ctx);

  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(output.size(), input_size);
  ASSERT_EQ(memcmp(input, output.data(), input_size), 0);
}

// Test partial write handling
TEST_F(CallbackApiTest, EncodeStreamCb_PartialWrites) {
  const uint8_t input[] = {
      'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};
  const size_t input_size = sizeof(input);

  ReadContext read_ctx = {input, input_size, 0, false, 0};
  std::vector<uint8_t> output;
  WriteContext write_ctx = {
      &output, true, 0, GCOMP_OK}; // Enable partial writes

  gcomp_status_t status = gcomp_encode_stream_cb(registry_, "passthru", nullptr,
      simple_read_cb, &read_ctx, simple_write_cb, &write_ctx);

  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(output.size(), input_size);
  ASSERT_EQ(memcmp(input, output.data(), input_size), 0);
}

// Test error handling from read callback
TEST_F(CallbackApiTest, EncodeStreamCb_ReadError) {
  // Read callback that returns error
  auto error_read_cb = [](void * ctx, uint8_t * dst, size_t cap,
                           size_t * out_n) -> gcomp_status_t {
    (void)ctx;
    (void)dst;
    (void)cap;
    (void)out_n;
    return GCOMP_ERR_IO;
  };

  std::vector<uint8_t> output;
  WriteContext write_ctx = {&output, false, 0, GCOMP_OK};

  gcomp_status_t status = gcomp_encode_stream_cb(registry_, "passthru", nullptr,
      error_read_cb, nullptr, simple_write_cb, &write_ctx);

  ASSERT_EQ(status, GCOMP_ERR_IO);
}

// Test error handling from write callback
TEST_F(CallbackApiTest, EncodeStreamCb_WriteError) {
  const uint8_t input[] = {'H', 'e', 'l', 'l', 'o'};
  const size_t input_size = sizeof(input);

  ReadContext read_ctx = {input, input_size, 0, false, 0};
  std::vector<uint8_t> output;
  WriteContext write_ctx = {&output, false, 0, GCOMP_ERR_IO}; // Return error

  gcomp_status_t status = gcomp_encode_stream_cb(registry_, "passthru", nullptr,
      simple_read_cb, &read_ctx, simple_write_cb, &write_ctx);

  ASSERT_EQ(status, GCOMP_ERR_IO);
}

// Test EOF handling (read callback returns 0 bytes)
TEST_F(CallbackApiTest, EncodeStreamCb_EOF) {
  // Empty input
  ReadContext read_ctx = {nullptr, 0, 0, false, 0};
  std::vector<uint8_t> output;
  WriteContext write_ctx = {&output, false, 0, GCOMP_OK};

  gcomp_status_t status = gcomp_encode_stream_cb(registry_, "passthru", nullptr,
      simple_read_cb, &read_ctx, simple_write_cb, &write_ctx);

  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(output.size(), 0u);
}

// Test large data streams
TEST_F(CallbackApiTest, EncodeStreamCb_LargeData) {
  // Create large input (1 MiB)
  const size_t large_size = 1024 * 1024;
  std::vector<uint8_t> input(large_size);
  for (size_t i = 0; i < large_size; ++i) {
    input[i] = static_cast<uint8_t>(i & 0xFF);
  }

  ReadContext read_ctx = {input.data(), large_size, 0, false, 0};
  std::vector<uint8_t> output;
  WriteContext write_ctx = {&output, false, 0, GCOMP_OK};

  gcomp_status_t status = gcomp_encode_stream_cb(registry_, "passthru", nullptr,
      simple_read_cb, &read_ctx, simple_write_cb, &write_ctx);

  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(output.size(), large_size);
  ASSERT_EQ(memcmp(input.data(), output.data(), large_size), 0);
}

// Test round-trip encoding/decoding
TEST_F(CallbackApiTest, RoundTrip) {
  const uint8_t original[] = {
      'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};
  const size_t original_size = sizeof(original);

  // Encode
  ReadContext encode_read_ctx = {original, original_size, 0, false, 0};
  std::vector<uint8_t> encoded;
  WriteContext encode_write_ctx = {&encoded, false, 0, GCOMP_OK};

  gcomp_status_t status = gcomp_encode_stream_cb(registry_, "passthru", nullptr,
      simple_read_cb, &encode_read_ctx, simple_write_cb, &encode_write_ctx);
  ASSERT_EQ(status, GCOMP_OK);

  // Decode
  ReadContext decode_read_ctx = {encoded.data(), encoded.size(), 0, false, 0};
  std::vector<uint8_t> decoded;
  WriteContext decode_write_ctx = {&decoded, false, 0, GCOMP_OK};

  status = gcomp_decode_stream_cb(registry_, "passthru", nullptr,
      simple_read_cb, &decode_read_ctx, simple_write_cb, &decode_write_ctx);
  ASSERT_EQ(status, GCOMP_OK);

  ASSERT_EQ(decoded.size(), original_size);
  ASSERT_EQ(memcmp(original, decoded.data(), original_size), 0);
}

// Test with default registry
TEST_F(CallbackApiTest, EncodeStreamCb_DefaultRegistry) {
  // Register method in default registry
  gcomp_registry_t * default_reg = gcomp_registry_default();
  ASSERT_NE(default_reg, nullptr);

  gcomp_method_t passthru = create_passthru_method("passthru_default");
  gcomp_status_t status = gcomp_registry_register(default_reg, &passthru);
  ASSERT_EQ(status, GCOMP_OK);

  const uint8_t input[] = {'H', 'e', 'l', 'l', 'o'};
  const size_t input_size = sizeof(input);

  ReadContext read_ctx = {input, input_size, 0, false, 0};
  std::vector<uint8_t> output;
  WriteContext write_ctx = {&output, false, 0, GCOMP_OK};

  // Use NULL registry to use default
  status = gcomp_encode_stream_cb(nullptr, "passthru_default", nullptr,
      simple_read_cb, &read_ctx, simple_write_cb, &write_ctx);

  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(output.size(), input_size);
  ASSERT_EQ(memcmp(input, output.data(), input_size), 0);
}

// Test invalid method name
TEST_F(CallbackApiTest, EncodeStreamCb_InvalidMethod) {
  const uint8_t input[] = {'H', 'e', 'l', 'l', 'o'};
  ReadContext read_ctx = {input, sizeof(input), 0, false, 0};
  std::vector<uint8_t> output;
  WriteContext write_ctx = {&output, false, 0, GCOMP_OK};

  gcomp_status_t status = gcomp_encode_stream_cb(registry_, "nonexistent",
      nullptr, simple_read_cb, &read_ctx, simple_write_cb, &write_ctx);

  ASSERT_EQ(status, GCOMP_ERR_UNSUPPORTED);
}

// Test with limited read size
TEST_F(CallbackApiTest, EncodeStreamCb_LimitedReadSize) {
  const uint8_t input[] = {
      'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};
  const size_t input_size = sizeof(input);

  ReadContext read_ctx = {
      input, input_size, 0, false, 3}; // Max 3 bytes per read
  std::vector<uint8_t> output;
  WriteContext write_ctx = {&output, false, 0, GCOMP_OK};

  gcomp_status_t status = gcomp_encode_stream_cb(registry_, "passthru", nullptr,
      simple_read_cb, &read_ctx, simple_write_cb, &write_ctx);

  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(output.size(), input_size);
  ASSERT_EQ(memcmp(input, output.data(), input_size), 0);
}

// Test with limited write size
TEST_F(CallbackApiTest, EncodeStreamCb_LimitedWriteSize) {
  const uint8_t input[] = {
      'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};
  const size_t input_size = sizeof(input);

  ReadContext read_ctx = {input, input_size, 0, false, 0};
  std::vector<uint8_t> output;
  WriteContext write_ctx = {
      &output, false, 2, GCOMP_OK}; // Max 2 bytes per write

  gcomp_status_t status = gcomp_encode_stream_cb(registry_, "passthru", nullptr,
      simple_read_cb, &read_ctx, simple_write_cb, &write_ctx);

  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(output.size(), input_size);
  ASSERT_EQ(memcmp(input, output.data(), input_size), 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
