/**
 * @file test_passthru.cpp
 *
 * Comprehensive tests for the pass-thru (no-op) compression method.
 * Validates encoder/decoder creation, update/finish behavior, buffer usage,
 * round-trip, buffer wrappers, callback API, and infrastructure.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "passthru_method.h"
#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <vector>

using namespace gcomp_test;

class PassthruTest : public ::testing::Test {
protected:
  void SetUp() override {
    gcomp_status_t status = gcomp_registry_create(nullptr, &registry_);
    ASSERT_EQ(status, GCOMP_OK);
    ASSERT_NE(registry_, nullptr);
    passthru_method_ = create_passthru_method("passthru");
    status = gcomp_registry_register(registry_, &passthru_method_);
    ASSERT_EQ(status, GCOMP_OK);
  }

  void TearDown() override {
    if (encoder_) {
      gcomp_encoder_destroy(encoder_);
      encoder_ = nullptr;
    }
    if (decoder_) {
      gcomp_decoder_destroy(decoder_);
      decoder_ = nullptr;
    }
    if (registry_) {
      gcomp_registry_destroy(registry_);
      registry_ = nullptr;
    }
  }

  gcomp_registry_t * registry_ = nullptr;
  gcomp_encoder_t * encoder_ = nullptr;
  gcomp_decoder_t * decoder_ = nullptr;
  gcomp_method_t passthru_method_;
};

// --- Encoder creation and registration ---
TEST_F(PassthruTest, EncoderCreateSuccess) {
  gcomp_status_t status =
      gcomp_encoder_create(registry_, "passthru", nullptr, &encoder_);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(encoder_, nullptr);
  ASSERT_NE(encoder_->method, nullptr);
  ASSERT_EQ(strcmp(encoder_->method->name, "passthru"), 0);
  ASSERT_NE(encoder_->update_fn, nullptr);
  ASSERT_NE(encoder_->finish_fn, nullptr);
}

TEST_F(PassthruTest, DecoderCreateSuccess) {
  gcomp_status_t status =
      gcomp_decoder_create(registry_, "passthru", nullptr, &decoder_);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(decoder_, nullptr);
  ASSERT_NE(decoder_->method, nullptr);
  ASSERT_EQ(strcmp(decoder_->method->name, "passthru"), 0);
  ASSERT_NE(decoder_->update_fn, nullptr);
  ASSERT_NE(decoder_->finish_fn, nullptr);
}

// --- Encoder update: single call, complete data ---
TEST_F(PassthruTest, EncoderUpdate_SingleCallComplete) {
  ASSERT_EQ(gcomp_encoder_create(registry_, "passthru", nullptr, &encoder_),
      GCOMP_OK);
  const uint8_t input[] = {'H', 'e', 'l', 'l', 'o'};
  uint8_t output[32] = {};
  gcomp_buffer_t in_buf = {input, sizeof(input), 0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  gcomp_status_t status = gcomp_encoder_update(encoder_, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(in_buf.used, sizeof(input));
  ASSERT_EQ(out_buf.used, sizeof(input));
  ASSERT_EQ(memcmp(input, output, sizeof(input)), 0);
}

// --- Encoder update: chunked input ---
TEST_F(PassthruTest, EncoderUpdate_ChunkedInput) {
  ASSERT_EQ(gcomp_encoder_create(registry_, "passthru", nullptr, &encoder_),
      GCOMP_OK);
  const uint8_t input[] = {'A', 'B', 'C', 'D', 'E', 'F'};
  uint8_t output[32] = {};
  size_t total_out = 0;
  for (size_t i = 0; i < sizeof(input); i++) {
    gcomp_buffer_t in_buf = {input + i, 1, 0};
    gcomp_buffer_t out_buf = {
        output + total_out, sizeof(output) - total_out, 0};
    gcomp_status_t status = gcomp_encoder_update(encoder_, &in_buf, &out_buf);
    ASSERT_EQ(status, GCOMP_OK);
    total_out += out_buf.used;
  }
  ASSERT_EQ(total_out, sizeof(input));
  ASSERT_EQ(memcmp(input, output, sizeof(input)), 0);
}

// --- Encoder update: partial output (output smaller than input) ---
TEST_F(PassthruTest, EncoderUpdate_PartialOutput) {
  ASSERT_EQ(gcomp_encoder_create(registry_, "passthru", nullptr, &encoder_),
      GCOMP_OK);
  const uint8_t input[] = {'1', '2', '3', '4', '5'};
  uint8_t output[2] = {};
  gcomp_buffer_t in_buf = {input, sizeof(input), 0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  gcomp_status_t status = gcomp_encoder_update(encoder_, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(in_buf.used, 2u);
  ASSERT_EQ(out_buf.used, 2u);
  ASSERT_EQ(output[0], '1');
  ASSERT_EQ(output[1], '2');
  // Second update to drain rest
  out_buf.used = 0;
  out_buf.size = 8;
  uint8_t out2[8] = {};
  out_buf.data = out2;
  status = gcomp_encoder_update(encoder_, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(in_buf.used, 5u);
  ASSERT_EQ(out_buf.used, 3u);
  ASSERT_EQ(out2[0], '3');
  ASSERT_EQ(out2[1], '4');
  ASSERT_EQ(out2[2], '5');
}

// --- Encoder update: empty input ---
TEST_F(PassthruTest, EncoderUpdate_EmptyInput) {
  ASSERT_EQ(gcomp_encoder_create(registry_, "passthru", nullptr, &encoder_),
      GCOMP_OK);
  const uint8_t input[] = {'x'};
  uint8_t output[8] = {};
  gcomp_buffer_t in_buf = {input, 0, 0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  gcomp_status_t status = gcomp_encoder_update(encoder_, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(in_buf.used, 0u);
  ASSERT_EQ(out_buf.used, 0u);
}

// --- Encoder update: large input ---
TEST_F(PassthruTest, EncoderUpdate_LargeInput) {
  ASSERT_EQ(gcomp_encoder_create(registry_, "passthru", nullptr, &encoder_),
      GCOMP_OK);
  const size_t N = 64 * 1024;
  std::vector<uint8_t> input(N);
  for (size_t i = 0; i < N; i++) {
    input[i] = static_cast<uint8_t>(i & 0xFF);
  }
  std::vector<uint8_t> output(N + 1024);
  gcomp_buffer_t in_buf = {input.data(), N, 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  gcomp_status_t status = gcomp_encoder_update(encoder_, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(in_buf.used, N);
  ASSERT_EQ(out_buf.used, N);
  ASSERT_EQ(memcmp(input.data(), output.data(), N), 0);
}

// --- Encoder finish ---
TEST_F(PassthruTest, EncoderFinish_ReturnsOk) {
  ASSERT_EQ(gcomp_encoder_create(registry_, "passthru", nullptr, &encoder_),
      GCOMP_OK);
  uint8_t output[8] = {};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};
  gcomp_status_t status = gcomp_encoder_finish(encoder_, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(out_buf.used, 0u);
}

// --- Decoder update: same scenarios ---
TEST_F(PassthruTest, DecoderUpdate_SingleCallComplete) {
  ASSERT_EQ(gcomp_decoder_create(registry_, "passthru", nullptr, &decoder_),
      GCOMP_OK);
  const uint8_t input[] = {'H', 'e', 'l', 'l', 'o'};
  uint8_t output[32] = {};
  gcomp_buffer_t in_buf = {input, sizeof(input), 0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  gcomp_status_t status = gcomp_decoder_update(decoder_, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(in_buf.used, sizeof(input));
  ASSERT_EQ(out_buf.used, sizeof(input));
  ASSERT_EQ(memcmp(input, output, sizeof(input)), 0);
}

TEST_F(PassthruTest, DecoderUpdate_PartialOutput) {
  ASSERT_EQ(gcomp_decoder_create(registry_, "passthru", nullptr, &decoder_),
      GCOMP_OK);
  const uint8_t input[] = {'a', 'b', 'c', 'd'};
  uint8_t output[2] = {};
  gcomp_buffer_t in_buf = {input, sizeof(input), 0};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};

  gcomp_status_t status = gcomp_decoder_update(decoder_, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(in_buf.used, 2u);
  ASSERT_EQ(out_buf.used, 2u);
  ASSERT_EQ(output[0], 'a');
  ASSERT_EQ(output[1], 'b');
}

TEST_F(PassthruTest, DecoderFinish_ReturnsOk) {
  ASSERT_EQ(gcomp_decoder_create(registry_, "passthru", nullptr, &decoder_),
      GCOMP_OK);
  uint8_t output[8] = {};
  gcomp_buffer_t out_buf = {output, sizeof(output), 0};
  gcomp_status_t status = gcomp_decoder_finish(decoder_, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(out_buf.used, 0u);
}

// --- Round-trip via raw update/finish ---
TEST_F(PassthruTest, RoundTrip_RawStream) {
  const uint8_t original[] = {
      'H', 'e', 'l', 'l', 'o', ',', ' ', 'W', 'o', 'r', 'l', 'd', '!'};
  const size_t orig_len = sizeof(original);
  std::vector<uint8_t> encoded(orig_len + 256);
  std::vector<uint8_t> decoded(orig_len + 256);

  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(registry_, "passthru", nullptr, &enc), GCOMP_OK);
  gcomp_buffer_t in_buf = {original, orig_len, 0};
  gcomp_buffer_t out_buf = {encoded.data(), encoded.size(), 0};
  gcomp_status_t status = gcomp_encoder_update(enc, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_encoder_finish(enc, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  gcomp_encoder_destroy(enc);

  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(registry_, "passthru", nullptr, &dec), GCOMP_OK);
  gcomp_buffer_t in2 = {encoded.data(), out_buf.used, 0};
  gcomp_buffer_t out2 = {decoded.data(), decoded.size(), 0};
  status = gcomp_decoder_update(dec, &in2, &out2);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_decoder_finish(dec, &out2);
  ASSERT_EQ(status, GCOMP_OK);
  gcomp_decoder_destroy(dec);

  ASSERT_EQ(out2.used, orig_len);
  ASSERT_EQ(memcmp(original, decoded.data(), orig_len), 0);
}

// --- Buffer wrappers with pass-thru ---
TEST_F(PassthruTest, BufferWrappers_EncodeDecode) {
  const uint8_t input[] = {'p', 'a', 's', 's'};
  const size_t input_size = sizeof(input);
  uint8_t encoded[64];
  size_t encoded_size = 0;
  uint8_t decoded[64];
  size_t decoded_size = 0;

  gcomp_status_t status = gcomp_encode_buffer(registry_, "passthru", nullptr,
      input, input_size, encoded, sizeof(encoded), &encoded_size);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(encoded_size, input_size);

  status = gcomp_decode_buffer(registry_, "passthru", nullptr, encoded,
      encoded_size, decoded, sizeof(decoded), &decoded_size);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded_size, input_size);
  ASSERT_EQ(memcmp(input, decoded, input_size), 0);
}

// --- Callback API with pass-thru (minimal: encode then decode) ---
TEST_F(PassthruTest, CallbackApi_EncodeDecode) {
  const uint8_t input[] = {'c', 'b', ' ', 't', 'e', 's', 't'};
  const size_t input_size = sizeof(input);
  struct Ctx {
    const uint8_t * data;
    size_t size;
    size_t offset;
  };
  auto read_fn = [](void * ctx, uint8_t * dst, size_t cap,
                     size_t * out_n) -> gcomp_status_t {
    Ctx * c = static_cast<Ctx *>(ctx);
    if (c->offset >= c->size) {
      *out_n = 0;
      return GCOMP_OK;
    }
    size_t n = c->size - c->offset;
    if (n > cap)
      n = cap;
    memcpy(dst, c->data + c->offset, n);
    c->offset += n;
    *out_n = n;
    return GCOMP_OK;
  };
  auto write_fn = [](void * ctx, const uint8_t * src, size_t n,
                      size_t * out_n) -> gcomp_status_t {
    std::vector<uint8_t> * v = static_cast<std::vector<uint8_t> *>(ctx);
    v->insert(v->end(), src, src + n);
    *out_n = n;
    return GCOMP_OK;
  };

  Ctx read_ctx = {input, input_size, 0};
  std::vector<uint8_t> out_vec;
  gcomp_status_t status = gcomp_encode_stream_cb(
      registry_, "passthru", nullptr, read_fn, &read_ctx, write_fn, &out_vec);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(out_vec.size(), input_size);
  ASSERT_EQ(memcmp(input, out_vec.data(), input_size), 0);

  Ctx decode_read = {out_vec.data(), out_vec.size(), 0};
  std::vector<uint8_t> decoded;
  status = gcomp_decode_stream_cb(registry_, "passthru", nullptr, read_fn,
      &decode_read, write_fn, &decoded);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decoded.size(), input_size);
  ASSERT_EQ(memcmp(input, decoded.data(), input_size), 0);
}

// --- Memory: multiple create/destroy cycles ---
TEST_F(PassthruTest, Memory_MultipleCreateDestroy) {
  for (int i = 0; i < 10; i++) {
    gcomp_encoder_t * enc = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "passthru", nullptr, &enc);
    ASSERT_EQ(status, GCOMP_OK);
    ASSERT_NE(enc, nullptr);
    gcomp_encoder_destroy(enc);
  }
  for (int i = 0; i < 10; i++) {
    gcomp_decoder_t * dec = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "passthru", nullptr, &dec);
    ASSERT_EQ(status, GCOMP_OK);
    ASSERT_NE(dec, nullptr);
    gcomp_decoder_destroy(dec);
  }
}

// --- Error handling: NULL arguments (core checks; method defense-in-depth) ---
TEST_F(PassthruTest, EncoderUpdate_NullInput) {
  ASSERT_EQ(gcomp_encoder_create(registry_, "passthru", nullptr, &encoder_),
      GCOMP_OK);
  uint8_t out[8] = {};
  gcomp_buffer_t out_buf = {out, sizeof(out), 0};
  gcomp_status_t status = gcomp_encoder_update(encoder_, nullptr, &out_buf);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

TEST_F(PassthruTest, EncoderUpdate_NullOutput) {
  ASSERT_EQ(gcomp_encoder_create(registry_, "passthru", nullptr, &encoder_),
      GCOMP_OK);
  uint8_t in[] = {'x'};
  gcomp_buffer_t in_buf = {in, 1, 0};
  gcomp_status_t status = gcomp_encoder_update(encoder_, &in_buf, nullptr);
  EXPECT_EQ(status, GCOMP_ERR_INVALID_ARG);
}

// --- Edge cases: 1-byte buffer ---
TEST_F(PassthruTest, EdgeCase_OneByteBuffer) {
  ASSERT_EQ(gcomp_encoder_create(registry_, "passthru", nullptr, &encoder_),
      GCOMP_OK);
  const uint8_t input[] = {'X'};
  uint8_t output[1] = {};
  gcomp_buffer_t in_buf = {input, 1, 0};
  gcomp_buffer_t out_buf = {output, 1, 0};
  gcomp_status_t status = gcomp_encoder_update(encoder_, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(in_buf.used, 1u);
  ASSERT_EQ(out_buf.used, 1u);
  ASSERT_EQ(output[0], 'X');
}

// --- Finish called multiple times (should be safe) ---
TEST_F(PassthruTest, EdgeCase_FinishMultipleTimes) {
  ASSERT_EQ(gcomp_encoder_create(registry_, "passthru", nullptr, &encoder_),
      GCOMP_OK);
  uint8_t out[8] = {};
  gcomp_buffer_t out_buf = {out, sizeof(out), 0};
  ASSERT_EQ(gcomp_encoder_finish(encoder_, &out_buf), GCOMP_OK);
  ASSERT_EQ(gcomp_encoder_finish(encoder_, &out_buf), GCOMP_OK);
}

TEST_F(PassthruTest, EdgeCase_DecoderFinishMultipleTimes) {
  ASSERT_EQ(gcomp_decoder_create(registry_, "passthru", nullptr, &decoder_),
      GCOMP_OK);
  uint8_t out[8] = {};
  gcomp_buffer_t out_buf = {out, sizeof(out), 0};
  ASSERT_EQ(gcomp_decoder_finish(decoder_, &out_buf), GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_finish(decoder_, &out_buf), GCOMP_OK);
}

// --- Options passed (passthru ignores but API accepts) ---
TEST_F(PassthruTest, OptionsPassedToEncoder) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_int64(opts, "dummy", 99), GCOMP_OK);

  gcomp_status_t status =
      gcomp_encoder_create(registry_, "passthru", opts, &encoder_);
  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_NE(encoder_, nullptr);

  gcomp_options_destroy(opts);
}

// --- Registry can find and use pass-thru ---
TEST_F(PassthruTest, RegistryFindPassthru) {
  const gcomp_method_t * m = gcomp_registry_find(registry_, "passthru");
  ASSERT_NE(m, nullptr);
  ASSERT_EQ(strcmp(m->name, "passthru"), 0);
  ASSERT_TRUE(m->capabilities & GCOMP_CAP_ENCODE);
  ASSERT_TRUE(m->capabilities & GCOMP_CAP_DECODE);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
