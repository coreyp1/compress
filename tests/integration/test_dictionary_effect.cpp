/**
 * @file test_dictionary_effect.cpp
 *
 * A dictionary that is accepted but not used looks exactly like one that
 * works: the stream round-trips either way, the dictionary ID is written, and
 * every existing test passes. The only thing that tells the two apart is
 * whether the output actually gets smaller.
 *
 * So this measures. The case is the strongest one there is - the payload is
 * the dictionary, byte for byte - where a working dictionary turns the whole
 * input into one long match and a few bytes of framing. The reference
 * implementation puts 4096 such bytes in 21 bytes; anything in the hundreds
 * means the dictionary content is not reaching the match finder.
 *
 * ## What this found, and what it did not
 *
 * The mechanism is sound: with content whose four-byte prefixes are mostly
 * distinct, a dictionary works at every level, taking 4096 bytes to 18.
 *
 * What it does not do is help at the lowest levels on data with very few
 * distinct prefixes. With a twelve-word vocabulary, 4096 bytes measured:
 *
 * | level | no dictionary | with dictionary |
 * | --- | --- | --- |
 * | 1 | 803 | 739 |
 * | 3 (the default) | 750 | 657 |
 * | 6 | 731 | 23 |
 * | 9 | 731 | 18 |
 *
 * That is the hash chain running out of attempts, not the dictionary failing
 * to load: levels 1 to 8 walk a bounded chain (four candidates at level 1,
 * sixteen at level 3), and on data where thousands of positions share a hash,
 * all of them are recent and none is in the dictionary. Level 6 has sixty-four
 * and gets there; level 9 searches a tree instead.
 *
 * libzstd does better here because it builds a search structure dedicated to
 * the dictionary for exactly these levels. We do not, and until we do, a
 * caller whose data is highly repetitive should ask for level 6 or above when
 * passing a dictionary. That is recorded rather than asserted - a test that
 * demanded the present behaviour would have to be deleted to improve it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

/// Content whose four-byte prefixes are almost all distinct.
std::vector<uint8_t> distinct_prefixes(size_t len, uint32_t seed) {
  std::vector<uint8_t> v(len);
  uint32_t s = seed;
  for (size_t i = 0; i < len; i++) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    v[i] = (uint8_t)(s >> 24);
  }
  return v;
}

/// Encode with the given dictionary, and check it reads back.
size_t encode_with_dict(const char * method, const std::vector<uint8_t> & input,
    const std::vector<uint8_t> * dict, int64_t level) {
  gcomp_options_t * o = nullptr;
  EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
  if (std::strcmp(method, "zstd") == 0) {
    EXPECT_EQ(gcomp_options_set_int64(o, "zstd.level", level), GCOMP_OK);
  }
  if (dict) {
    EXPECT_EQ(gcomp_options_set_bytes(o,
                  std::string(method).append(".dictionary").c_str(),
                  dict->data(), dict->size()),
        GCOMP_OK);
  }

  size_t bound = 0;
  EXPECT_EQ(gcomp_encode_bound(nullptr, method, o, input.size(), &bound),
      GCOMP_OK);
  std::vector<uint8_t> out(bound ? bound : 1);
  size_t written = 0;
  EXPECT_EQ(gcomp_encode_buffer(nullptr, method, o, input.data(), input.size(),
                out.data(), out.size(), &written),
      GCOMP_OK);

  // A small stream is worth nothing if it does not decode back, and a
  // dictionary is the easiest way to produce one that does not: the decoder
  // has to load the same bytes as history before the first block.
  EXPECT_EQ(gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 0),
      GCOMP_OK);
  std::vector<uint8_t> back(input.size() + 1024);
  size_t produced = 0;
  EXPECT_EQ(gcomp_decode_buffer(nullptr, method, o, out.data(), written,
                back.data(), back.size(), &produced),
      GCOMP_OK)
      << method << " level " << level << ": dictionary stream did not decode";
  EXPECT_EQ(produced, input.size()) << method << " level " << level;
  if (produced == input.size() && produced > 0) {
    EXPECT_EQ(std::memcmp(back.data(), input.data(), produced), 0)
        << method << " level " << level;
  }

  gcomp_options_destroy(o);
  return written;
}

/**
 * @brief The dictionary reaches the match finder, at every level.
 *
 * Payload identical to the dictionary: one long match, and almost nothing
 * else. The threshold is deliberately loose - a fortieth of the input - because
 * the point is to tell "the dictionary is being searched" from "it is not",
 * and those differ by a factor of two hundred, not by a few percent.
 */
TEST(DictionaryEffect, ZstdUsesTheDictionaryAtEveryLevel) {
  const std::vector<uint8_t> dict = distinct_prefixes(4096, 12345);
  const std::vector<uint8_t> payload = dict;

  for (int64_t level : {1, 3, 6, 9, 12, 19}) {
    const size_t without = encode_with_dict("zstd", payload, nullptr, level);
    const size_t with = encode_with_dict("zstd", payload, &dict, level);

    EXPECT_LT(with, payload.size() / 40)
        << "zstd level " << level << ": " << with
        << " bytes for a payload identical to its dictionary; the dictionary "
           "is not reaching the match finder";
    EXPECT_LT(with * 10, without)
        << "zstd level " << level << ": " << with << " with a dictionary "
        << "against " << without << " without";
  }
}

/// The same for LZ4, which indexes its dictionary for every independent block.
TEST(DictionaryEffect, Lz4UsesTheDictionary) {
  const std::vector<uint8_t> dict = distinct_prefixes(4096, 999);
  const std::vector<uint8_t> payload = dict;

  const size_t without = encode_with_dict("lz4", payload, nullptr, 0);
  const size_t with = encode_with_dict("lz4", payload, &dict, 0);

  EXPECT_LT(with, payload.size() / 10)
      << "lz4: " << with << " bytes for a payload identical to its dictionary";
  EXPECT_LT(with * 4, without) << "lz4: " << with << " against " << without;
}

/**
 * @brief A dictionary that is only part of the payload still pays.
 *
 * The realistic shape: many small records drawn from a shared vocabulary,
 * where the dictionary holds what they have in common and each record is
 * mostly not in it.
 */
TEST(DictionaryEffect, HelpsASmallRecord) {
  const std::vector<uint8_t> dict = distinct_prefixes(8192, 4242);
  // A record built out of pieces of the dictionary, in a different order.
  std::vector<uint8_t> record;
  for (int i = 0; i < 8; i++) {
    size_t at = (size_t)(i * 887) % (dict.size() - 64);
    record.insert(record.end(), dict.begin() + at, dict.begin() + at + 64);
  }

  for (int64_t level : {1, 3, 9}) {
    const size_t without = encode_with_dict("zstd", record, nullptr, level);
    const size_t with = encode_with_dict("zstd", record, &dict, level);
    EXPECT_LT(with, without)
        << "zstd level " << level << ": a dictionary holding every byte of the "
        << "record did not make it smaller (" << with << " against " << without
        << ")";
  }
}

/**
 * @brief The decoder needs the same dictionary, and says so when it has none.
 *
 * A stream encoded against a dictionary and decoded without one must not
 * quietly produce different bytes.
 */
TEST(DictionaryEffect, DecodingWithoutTheDictionaryFails) {
  const std::vector<uint8_t> dict = distinct_prefixes(4096, 31337);
  const std::vector<uint8_t> payload = dict;

  gcomp_options_t * o = nullptr;
  ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bytes(
                o, "zstd.dictionary", dict.data(), dict.size()),
      GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(o, "zstd.dictionary_id", 0xABCDu),
      GCOMP_OK);

  void * enc = nullptr;
  size_t enc_len = 0;
  ASSERT_EQ(gcomp_encode_alloc(nullptr, "zstd", o, payload.data(),
                payload.size(), &enc, &enc_len),
      GCOMP_OK);
  gcomp_options_destroy(o);

  // peek says which dictionary is wanted, which is how a reader fails usefully.
  gcomp_stream_info_t info;
  ASSERT_EQ(
      gcomp_peek(nullptr, "zstd", nullptr, enc, enc_len, &info, nullptr),
      GCOMP_OK);
  EXPECT_TRUE(info.has_dictionary);
  EXPECT_EQ(info.dictionary_id, 0xABCDu);

  void * dec = nullptr;
  size_t dec_len = 0;
  gcomp_status_t s =
      gcomp_decode_alloc(nullptr, "zstd", nullptr, enc, enc_len, &dec, &dec_len);
  EXPECT_NE(s, GCOMP_OK)
      << "decoded a dictionary stream without the dictionary, producing "
      << dec_len << " bytes";
  gcomp_buffer_free(nullptr, dec);
  gcomp_buffer_free(nullptr, enc);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
