/**
 * @file test_lzw_encoder.cpp
 *
 * Encoder tests for LZW: round-trip as golden check (GIF and TIFF),
 * invalid arguments, and output limit behavior.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lzw.h>
#include <ghoti.io/compress/method.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <vector>

static void encode_then_decode(gcomp_registry_t * reg, const char * format,
    const uint8_t * input, size_t input_len) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "lzw.format", format);

  gcomp_encoder_t * enc = nullptr;
  gcomp_status_t s = gcomp_encoder_create(reg, "lzw", opts, &enc);
  ASSERT_EQ(s, GCOMP_OK);
  ASSERT_NE(enc, nullptr);

  std::vector<uint8_t> encoded(input_len * 2 + 128);
  gcomp_buffer_t in_buf = {const_cast<uint8_t *>(input), input_len, 0};
  gcomp_buffer_t out_buf = {encoded.data(), encoded.size(), 0};

  s = gcomp_encoder_update(enc, &in_buf, &out_buf);
  ASSERT_EQ(s, GCOMP_OK);
  s = gcomp_encoder_finish(enc, &out_buf);
  ASSERT_EQ(s, GCOMP_OK);
  size_t encoded_len = out_buf.used;
  gcomp_encoder_destroy(enc);

  gcomp_decoder_t * dec = nullptr;
  s = gcomp_decoder_create(reg, "lzw", opts, &dec);
  ASSERT_EQ(s, GCOMP_OK);
  ASSERT_NE(dec, nullptr);

  std::vector<uint8_t> decoded(input_len + 64);
  gcomp_buffer_t enc_in = {encoded.data(), encoded_len, 0};
  gcomp_buffer_t dec_out = {decoded.data(), decoded.size(), 0};

  s = gcomp_decoder_update(dec, &enc_in, &dec_out);
  ASSERT_EQ(s, GCOMP_OK);
  s = gcomp_decoder_finish(dec, &dec_out);
  ASSERT_EQ(s, GCOMP_OK);
  gcomp_decoder_destroy(dec);
  gcomp_options_destroy(opts);

  EXPECT_EQ(dec_out.used, input_len) << "format=" << format;
  if (input_len > 0 && input) {
    EXPECT_TRUE(test_helpers_buffers_equal(
        input, input_len, decoded.data(), dec_out.used))
        << "format=" << format;
  }
}

class LzwEncoderTest : public ::testing::Test {
protected:
  void SetUp() override {
    gcomp_registry_create(nullptr, &reg_);
    gcomp_method_lzw_register(reg_);
  }
  void TearDown() override {
    if (reg_) {
      gcomp_registry_destroy(reg_);
      reg_ = nullptr;
    }
  }
  gcomp_registry_t * reg_ = nullptr;
};

TEST_F(LzwEncoderTest, GifEncodeDecodeEmpty) {
  encode_then_decode(reg_, "gif", nullptr, 0);
}

TEST_F(LzwEncoderTest, GifEncodeDecodeSingleByte) {
  const uint8_t one[] = {0x41};
  encode_then_decode(reg_, "gif", one, sizeof(one));
}

TEST_F(LzwEncoderTest, GifEncodeDecodeShort) {
  const uint8_t data[] = "hello";
  encode_then_decode(reg_, "gif", data, sizeof(data) - 1);
}

TEST_F(LzwEncoderTest, TiffEncodeDecodeEmpty) {
  encode_then_decode(reg_, "tiff", nullptr, 0);
}

TEST_F(LzwEncoderTest, TiffEncodeDecodeShort) {
  const uint8_t data[] = "world";
  encode_then_decode(reg_, "tiff", data, sizeof(data) - 1);
}

TEST_F(LzwEncoderTest, InvalidArgNullRegistry) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_encoder_t * enc = nullptr;
  gcomp_status_t s = gcomp_encoder_create(nullptr, "lzw", opts, &enc);
  EXPECT_NE(s, GCOMP_OK);
  EXPECT_EQ(enc, nullptr);
  gcomp_options_destroy(opts);
}

TEST_F(LzwEncoderTest, InvalidArgNullOutputData) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_encoder_t * enc = nullptr;
  gcomp_status_t s = gcomp_encoder_create(reg_, "lzw", opts, &enc);
  ASSERT_EQ(s, GCOMP_OK);
  ASSERT_NE(enc, nullptr);

  uint8_t in[] = {0x41};
  gcomp_buffer_t in_buf = {in, 1, 0};
  gcomp_buffer_t out_buf = {nullptr, 64, 0};

  s = gcomp_encoder_update(enc, &in_buf, &out_buf);
  EXPECT_NE(s, GCOMP_OK);
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

TEST_F(LzwEncoderTest, LimitMaxMemoryBytesBelowBaselineFails) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "lzw.format", "gif");
  gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 1000);

  gcomp_encoder_t * enc = nullptr;
  gcomp_status_t s = gcomp_encoder_create(reg_, "lzw", opts, &enc);
  EXPECT_EQ(s, GCOMP_ERR_LIMIT);
  EXPECT_EQ(enc, nullptr);
  gcomp_options_destroy(opts);
}

TEST_F(LzwEncoderTest, LimitMaxMemoryBytesSufficientSucceeds) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "lzw.format", "gif");
  gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 256 * 1024);

  gcomp_encoder_t * enc = nullptr;
  gcomp_status_t s = gcomp_encoder_create(reg_, "lzw", opts, &enc);
  ASSERT_EQ(s, GCOMP_OK);
  ASSERT_NE(enc, nullptr);
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
}

// The hash table is charged against limits.max_memory_bytes on top of the
// dictionary, so there is a band of limits that the linear encoder fits inside
// and the hash encoder does not. Nothing reached that rejection before this
// test: the two existing limit tests sit below the dictionary baseline (which
// is refused earlier, before the hash table is ever built) and above the whole
// requirement. 32 KiB is inside the band - the assertions below say so rather
// than assuming it - and reaching it is what exercises the hash table's own
// teardown path.
TEST_F(LzwEncoderTest, TheHashTableCountsAgainstTheMemoryLimitToo) {
  const uint64_t limit = 32 * 1024;

  // Below the band's top: the dictionary alone fits, so the baseline check
  // passes and we get past it.
  gcomp_options_t * linear_opts = nullptr;
  gcomp_options_create(&linear_opts);
  gcomp_options_set_string(linear_opts, "lzw.format", "gif");
  gcomp_options_set_string(linear_opts, "lzw.encoder_lookup", "linear");
  gcomp_options_set_uint64(linear_opts, "limits.max_memory_bytes", limit);
  gcomp_encoder_t * linear_enc = nullptr;
  ASSERT_EQ(gcomp_encoder_create(reg_, "lzw", linear_opts, &linear_enc),
      GCOMP_OK);
  ASSERT_NE(linear_enc, nullptr);
  gcomp_encoder_destroy(linear_enc);
  gcomp_options_destroy(linear_opts);

  // Same limit, hash lookup: the dictionary still fits, the hash table does
  // not, and the rejection must come from the second check rather than being
  // silently allowed through.
  gcomp_options_t * hash_opts = nullptr;
  gcomp_options_create(&hash_opts);
  gcomp_options_set_string(hash_opts, "lzw.format", "gif");
  gcomp_options_set_string(hash_opts, "lzw.encoder_lookup", "hash");
  gcomp_options_set_uint64(hash_opts, "limits.max_memory_bytes", limit);
  gcomp_encoder_t * hash_enc = nullptr;
  EXPECT_EQ(
      gcomp_encoder_create(reg_, "lzw", hash_opts, &hash_enc), GCOMP_ERR_LIMIT);
  EXPECT_EQ(hash_enc, nullptr);
  gcomp_options_destroy(hash_opts);
}

// Encode with given encoder_lookup mode; return encoded size (buffer in
// encoded_out).
static size_t encode_with_lookup(gcomp_registry_t * reg, const char * lookup,
    const uint8_t * input, size_t input_len,
    std::vector<uint8_t> * encoded_out) {
  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "lzw.format", "gif");
  gcomp_options_set_string(opts, "lzw.encoder_lookup", lookup);

  gcomp_encoder_t * enc = nullptr;
  gcomp_status_t s = gcomp_encoder_create(reg, "lzw", opts, &enc);
  if (s != GCOMP_OK || enc == nullptr) {
    gcomp_options_destroy(opts);
    return 0;
  }

  encoded_out->resize(input_len * 2 + 128);
  gcomp_buffer_t in_buf = {const_cast<uint8_t *>(input), input_len, 0};
  gcomp_buffer_t out_buf = {encoded_out->data(), encoded_out->size(), 0};

  s = gcomp_encoder_update(enc, &in_buf, &out_buf);
  if (s != GCOMP_OK) {
    gcomp_encoder_destroy(enc);
    gcomp_options_destroy(opts);
    return 0;
  }
  s = gcomp_encoder_finish(enc, &out_buf);
  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(opts);
  return (s == GCOMP_OK) ? out_buf.used : 0;
}

TEST_F(LzwEncoderTest, EncoderLookupLinearVsHashEquivalence) {
  const uint8_t data[] = "hello";
  const size_t len = sizeof(data) - 1;

  std::vector<uint8_t> encoded_linear, encoded_hash;
  size_t len_linear =
      encode_with_lookup(reg_, "linear", data, len, &encoded_linear);
  size_t len_hash = encode_with_lookup(reg_, "hash", data, len, &encoded_hash);

  ASSERT_GT(len_linear, 0u) << "linear encode failed";
  ASSERT_GT(len_hash, 0u) << "hash encode failed";

  gcomp_options_t * opts = nullptr;
  gcomp_options_create(&opts);
  gcomp_options_set_string(opts, "lzw.format", "gif");

  std::vector<uint8_t> decoded_linear(len + 64), decoded_hash(len + 64);
  gcomp_buffer_t dec_out_linear = {
      decoded_linear.data(), decoded_linear.size(), 0};
  gcomp_buffer_t dec_out_hash = {decoded_hash.data(), decoded_hash.size(), 0};

  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t s = gcomp_decoder_create(reg_, "lzw", opts, &dec);
  ASSERT_EQ(s, GCOMP_OK);
  gcomp_buffer_t enc_in = {encoded_linear.data(), len_linear, 0};
  s = gcomp_decoder_update(dec, &enc_in, &dec_out_linear);
  ASSERT_EQ(s, GCOMP_OK);
  s = gcomp_decoder_finish(dec, &dec_out_linear);
  ASSERT_EQ(s, GCOMP_OK);
  gcomp_decoder_destroy(dec);

  dec = nullptr;
  s = gcomp_decoder_create(reg_, "lzw", opts, &dec);
  ASSERT_EQ(s, GCOMP_OK);
  enc_in.data = encoded_hash.data();
  enc_in.size = len_hash;
  enc_in.used = 0;
  s = gcomp_decoder_update(dec, &enc_in, &dec_out_hash);
  ASSERT_EQ(s, GCOMP_OK);
  s = gcomp_decoder_finish(dec, &dec_out_hash);
  ASSERT_EQ(s, GCOMP_OK);
  gcomp_decoder_destroy(dec);
  gcomp_options_destroy(opts);

  EXPECT_EQ(dec_out_linear.used, len);
  EXPECT_EQ(dec_out_hash.used, len);
  EXPECT_TRUE(test_helpers_buffers_equal(
      data, len, decoded_linear.data(), dec_out_linear.used));
  EXPECT_TRUE(test_helpers_buffers_equal(
      data, len, decoded_hash.data(), dec_out_hash.used));
  EXPECT_TRUE(test_helpers_buffers_equal(decoded_linear.data(),
      dec_out_linear.used, decoded_hash.data(), dec_out_hash.used));
}


// Encode with lzw.encoder_lookup set to `mode`, or with no options at all
// when `mode` is nullptr.
static std::vector<uint8_t> encode_buffer_with_lookup(
    gcomp_registry_t * reg, const char * mode, const std::vector<uint8_t> & in) {
  gcomp_options_t * opts = nullptr;
  if (mode) {
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      return {};
    }
    gcomp_options_set_string(opts, "lzw.encoder_lookup", mode);
  }

  std::vector<uint8_t> out(in.size() * 2 + 4096);
  size_t used = out.size();
  gcomp_status_t s = gcomp_encode_buffer(
      reg, "lzw", opts, in.data(), in.size(), out.data(), out.size(), &used);

  if (opts) {
    gcomp_options_destroy(opts);
  }
  if (s != GCOMP_OK) {
    return {};
  }
  out.resize(used);
  return out;
}

// Data with enough repeated substrings to fill the code table, so the two
// lookup paths have to agree over a long run of dictionary entries.
static std::vector<uint8_t> lookup_test_data(size_t n, unsigned seed) {
  static const char * kFragments[] = {"alpha", "beta", "gamma", "alpha",
      "delta", "beta", "epsilon", "alphabet", "gammaray", "de", "ep", "al"};
  const size_t kNum = sizeof(kFragments) / sizeof(kFragments[0]);

  std::vector<uint8_t> out;
  out.reserve(n + 16);
  unsigned x = seed;
  while (out.size() < n) {
    x = x * 1103515245u + 12345u;
    const char * f = kFragments[(x >> 16) % kNum];
    for (const char * c = f; *c; c++) {
      out.push_back((uint8_t)*c);
    }
    if (((x >> 8) & 7) == 0) {
      out.push_back((uint8_t)(' ' + ((x >> 3) & 31)));
    }
  }
  out.resize(n);
  return out;
}

// lzw.encoder_lookup declares "hash" as its default, but the encoder only
// honoured that when a caller passed the option explicitly -- so encoding
// with no options ran the linear scan, which was 98.9% of the encoder's
// instructions.  A default that does not match its declaration is invisible
// unless something checks it, and the two paths produce identical bytes, so
// nothing else could have noticed.
TEST_F(LzwEncoderTest, DefaultLookupIsTheOneTheSchemaDeclares) {
  const gcomp_method_t * method = gcomp_registry_find(reg_, "lzw");
  ASSERT_NE(method, nullptr);

  const gcomp_option_schema_t * schema = nullptr;
  ASSERT_EQ(
      gcomp_method_get_option_schema(method, "lzw.encoder_lookup", &schema),
      GCOMP_OK);
  ASSERT_NE(schema, nullptr);
  ASSERT_TRUE(schema->has_default)
      << "lzw.encoder_lookup no longer declares a default";

  const char * declared = schema->default_value.str;
  ASSERT_NE(declared, nullptr);

  // Comparing output cannot tell the two lookups apart -- they are required
  // to produce identical bytes, and do.  What distinguishes them is that the
  // hash table is allocated and charged against limits.max_memory_bytes, so a
  // limit that admits the dictionary but not the hash table accepts one and
  // rejects the other.  Setting only that limit leaves encoder_lookup at its
  // default, and the outcome then says which default actually took effect.
  const uint64_t limit = 32 * 1024;

  gcomp_options_t * implicit_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&implicit_opts), GCOMP_OK);
  gcomp_options_set_string(implicit_opts, "lzw.format", "gif");
  gcomp_options_set_uint64(implicit_opts, "limits.max_memory_bytes", limit);
  gcomp_encoder_t * implicit_enc = nullptr;
  gcomp_status_t implicit_status =
      gcomp_encoder_create(reg_, "lzw", implicit_opts, &implicit_enc);
  if (implicit_enc) {
    gcomp_encoder_destroy(implicit_enc);
  }
  gcomp_options_destroy(implicit_opts);

  gcomp_options_t * declared_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&declared_opts), GCOMP_OK);
  gcomp_options_set_string(declared_opts, "lzw.format", "gif");
  gcomp_options_set_string(declared_opts, "lzw.encoder_lookup", declared);
  gcomp_options_set_uint64(declared_opts, "limits.max_memory_bytes", limit);
  gcomp_encoder_t * declared_enc = nullptr;
  gcomp_status_t declared_status =
      gcomp_encoder_create(reg_, "lzw", declared_opts, &declared_enc);
  if (declared_enc) {
    gcomp_encoder_destroy(declared_enc);
  }
  gcomp_options_destroy(declared_opts);

  EXPECT_EQ(implicit_status, declared_status)
      << "leaving lzw.encoder_lookup unset did not behave like setting it to "
      << declared << ", which the schema declares as the default";

  // And the output still has to be identical either way.
  std::vector<uint8_t> data = lookup_test_data(64 * 1024, 20260917u);
  std::vector<uint8_t> implicit_out =
      encode_buffer_with_lookup(reg_, nullptr, data);
  std::vector<uint8_t> declared_out =
      encode_buffer_with_lookup(reg_, declared, data);
  ASSERT_FALSE(implicit_out.empty());
  ASSERT_EQ(implicit_out.size(), declared_out.size());
  EXPECT_EQ(
      memcmp(implicit_out.data(), declared_out.data(), implicit_out.size()), 0);
}

TEST_F(LzwEncoderTest, HashAndLinearLookupsAgreeByteForByte) {
  // The hash table is an index over the same dictionary the linear scan walks,
  // so it must not change a single bit of the output at any size.
  static const size_t kSizes[] = {0, 1, 2, 17, 255, 256, 1024, 4095, 4096,
      4097, 16384, 65536, 200000};

  for (size_t n : kSizes) {
    std::vector<uint8_t> data = lookup_test_data(n, (unsigned)(n * 2654435761u));

    std::vector<uint8_t> hashed = encode_buffer_with_lookup(reg_, "hash", data);
    std::vector<uint8_t> linear =
        encode_buffer_with_lookup(reg_, "linear", data);

    ASSERT_EQ(hashed.size(), linear.size()) << "n=" << n;
    ASSERT_EQ(memcmp(hashed.data(), linear.data(), hashed.size()), 0)
        << "n=" << n;

    // And both must decode back to the input.
    std::vector<uint8_t> back(n + 1024);
    size_t back_used = back.size();
    ASSERT_EQ(gcomp_decode_buffer(reg_, "lzw", nullptr, hashed.data(),
                  hashed.size(), back.data(), back.size(), &back_used),
        GCOMP_OK)
        << "n=" << n;
    ASSERT_EQ(back_used, n) << "n=" << n;
    if (n) {
      ASSERT_EQ(memcmp(back.data(), data.data(), n), 0) << "n=" << n;
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
