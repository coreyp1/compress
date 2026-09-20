/**
 * @file test_dict_train.cpp
 *
 * Training a dictionary from samples.
 *
 * A dictionary is only worth anything if it holds what the messages have in
 * common, and "it produced some bytes" says nothing about that. So the test is
 * a measurement, on samples the trainer never saw: a held-out split has to
 * compress substantially better with the dictionary than without.
 *
 * The comparison that keeps it honest is against `zstd --train`, through
 * pyzstd. Ours only has to be in the same class - the algorithms differ in
 * their parameter search and there is no byte-exact answer to check against.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/dict.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

/// Log lines: short, highly patterned, and the shape dictionaries are for.
std::string record(uint32_t & s) {
  static const char * users[] = {
      "alice", "bob", "carol", "dave", "erin", "frank"};
  static const char * paths[] = {"/api/v1/users", "/api/v1/orders",
      "/api/v1/items", "/health", "/api/v1/sessions/refresh"};
  static const char * agents[] = {"Mozilla/5.0 (X11; Linux x86_64)",
      "curl/8.5.0", "Go-http-client/2.0"};
  s = s * 1103515245u + 12345u;
  char buf[512];
  std::snprintf(buf, sizeof(buf),
      "{\"ts\":\"2026-09-20T%02u:%02u:%02uZ\",\"user\":\"%s\","
      "\"method\":\"%s\",\"path\":\"%s\",\"status\":%u,\"bytes\":%u,"
      "\"agent\":\"%s\",\"trace\":\"%08x\"}",
      (s >> 3) % 24u, (s >> 7) % 60u, (s >> 11) % 60u, users[(s >> 5) % 6u],
      ((s >> 9) & 1u) ? "GET" : "POST", paths[(s >> 13) % 5u],
      ((s >> 17) % 5u) ? 200u : 500u, (s >> 6) % 65536u, agents[(s >> 19) % 3u],
      s);
  return std::string(buf);
}

/// Total compressed size of a set of records, one frame each.
size_t compressed_total(const std::vector<std::string> & recs,
    const std::vector<uint8_t> * dict, int64_t level) {
  size_t total = 0;
  for (const std::string & r : recs) {
    gcomp_options_t * o = nullptr;
    EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_int64(o, "zstd.level", level), GCOMP_OK);
    if (dict) {
      EXPECT_EQ(gcomp_options_set_bytes(
                    o, "zstd.dictionary", dict->data(), dict->size()),
          GCOMP_OK);
    }
    size_t bound = 0;
    EXPECT_EQ(gcomp_encode_bound(nullptr, "zstd", o, r.size(), &bound),
        GCOMP_OK);
    std::vector<uint8_t> out(bound);
    size_t w = 0;
    EXPECT_EQ(gcomp_encode_buffer(nullptr, "zstd", o, r.data(), r.size(),
                  out.data(), out.size(), &w),
        GCOMP_OK);
    gcomp_options_destroy(o);
    total += w;
  }
  return total;
}

/**
 * @brief How many samples to use.
 *
 * Training scans every segment position in the corpus once per dictionary
 * segment chosen, which is cheap normally and expensive under Memcheck: this
 * file took 243 seconds there, second only to the match finder's. The shape
 * being tested is "a dictionary learned from these helps on those", and that
 * holds at a tenth of the samples - so under Memcheck it runs a tenth of them
 * and still exercises every path.
 *
 * GCOMP_UNDER_VALGRIND is set by the Makefile's valgrind targets.
 */
size_t scale(size_t n) {
  const char * v = std::getenv("GCOMP_UNDER_VALGRIND");
  return (v && v[0] == '1') ? ((n / 10u) ? (n / 10u) : 1u) : n;
}

struct Split {
  std::vector<std::string> train;
  std::vector<std::string> held;
};

Split make_split(size_t n_train, size_t n_held) {
  Split s;
  uint32_t seed = 12345u;
  for (size_t i = 0; i < n_train; i++) {
    s.train.push_back(record(seed));
  }
  for (size_t i = 0; i < n_held; i++) {
    s.held.push_back(record(seed));
  }
  return s;
}

std::vector<uint8_t> train(const std::vector<std::string> & samples,
    size_t capacity, gcomp_options_t * options) {
  std::vector<const void *> ptrs;
  std::vector<size_t> sizes;
  for (const std::string & r : samples) {
    ptrs.push_back(r.data());
    sizes.push_back(r.size());
  }
  std::vector<uint8_t> dict(capacity);
  size_t out = 0;
  const gcomp_status_t s = gcomp_dict_train(nullptr, "zstd", options,
      ptrs.data(), sizes.data(), ptrs.size(), dict.data(), dict.size(), &out);
  EXPECT_EQ(s, GCOMP_OK);
  dict.resize(s == GCOMP_OK ? out : 0);
  return dict;
}

} // namespace

/**
 * @brief A trained dictionary makes held-out samples much smaller.
 *
 * Held out, because a dictionary that merely memorised its training set would
 * look perfect on it and be worthless in use. The threshold is deliberately
 * coarse - half the size - since the effect being measured is a factor of
 * three, not a few percent.
 */
TEST(DictTrain, HelpsOnSamplesItNeverSaw) {
  const Split split = make_split(scale(3000), scale(1000));
  const std::vector<uint8_t> dict = train(split.train, 16384, nullptr);
  ASSERT_GT(dict.size(), scale(4096)) << "the trainer produced almost nothing";

  for (int64_t level : {1, 3, 9, 19}) {
    const size_t without = compressed_total(split.held, nullptr, level);
    const size_t with = compressed_total(split.held, &dict, level);
    EXPECT_LT(with * 2, without)
        << "level " << level << ": " << with << " bytes with a dictionary "
        << "against " << without << " without - trained on 3000 samples and "
        << "measured on 1000 it never saw";
  }
}

/**
 * @brief Short samples are the point, and used to be refused.
 *
 * A segment cannot straddle two samples, so a segment size larger than the
 * samples picks nothing: 4000 log lines of about 170 bytes got
 * GCOMP_ERR_UNSUPPORTED from the 256-byte default, which is precisely the case
 * dictionaries exist for. The segment size now clamps to the median sample
 * length.
 */
TEST(DictTrain, WorksOnSamplesShorterThanTheDefaultSegment) {
  std::vector<std::string> samples;
  uint32_t seed = 999u;
  for (size_t i = 0; i < scale(2000); i++) {
    // Around 60 bytes: far below the 256-byte default segment.
    const std::string r = record(seed);
    samples.push_back(r.substr(0, 60));
  }
  const std::vector<uint8_t> dict = train(samples, 8192, nullptr);
  EXPECT_GT(dict.size(), 0u)
      << "samples shorter than the default segment produced no dictionary";

  std::vector<std::string> held;
  for (size_t i = 0; i < scale(300); i++) {
    held.push_back(record(seed).substr(0, 60));
  }
  const size_t without = compressed_total(held, nullptr, 3);
  const size_t with = compressed_total(held, &dict, 3);
  EXPECT_LT(with, without)
      << with << " against " << without
      << ": a dictionary trained on short samples did not help";
}

/// The knobs move the result, and bad arguments are refused.
TEST(DictTrain, RefusesWhatItCannotTrainOn) {
  const Split split = make_split(scale(200), 0);
  std::vector<const void *> ptrs;
  std::vector<size_t> sizes;
  for (const std::string & r : split.train) {
    ptrs.push_back(r.data());
    sizes.push_back(r.size());
  }
  std::vector<uint8_t> dict(4096);
  size_t out = 0;

  EXPECT_EQ(gcomp_dict_train(nullptr, "zstd", nullptr, nullptr, sizes.data(),
                ptrs.size(), dict.data(), dict.size(), &out),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_dict_train(nullptr, "zstd", nullptr, ptrs.data(),
                sizes.data(), 0, dict.data(), dict.size(), &out),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_dict_train(nullptr, "no-such-method", nullptr, ptrs.data(),
                sizes.data(), ptrs.size(), dict.data(), dict.size(), &out),
      GCOMP_ERR_UNSUPPORTED);

  // Samples with nothing in them at all.
  const char * empty = "";
  const void * eptr[1] = {empty};
  const size_t esize[1] = {0};
  EXPECT_EQ(gcomp_dict_train(nullptr, "zstd", nullptr, eptr, esize, 1,
                dict.data(), dict.size(), &out),
      GCOMP_ERR_UNSUPPORTED);
}

/**
 * @brief The dictionary works in the other methods too.
 *
 * The content is method-independent - it is history, and deflate and LZ4 take
 * history as raw bytes just as Zstandard does with a content-only dictionary.
 */
TEST(DictTrain, TheSameDictionaryHelpsDeflateAndLz4) {
  const Split split = make_split(scale(3000), scale(500));
  const std::vector<uint8_t> dict = train(split.train, 16384, nullptr);
  ASSERT_GT(dict.size(), scale(4096));

  for (const char * method : {"zlib", "lz4"}) {
    size_t with = 0, without = 0;
    for (const std::string & r : split.held) {
      for (int use = 0; use < 2; use++) {
        gcomp_options_t * o = nullptr;
        ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
        if (use) {
          ASSERT_EQ(gcomp_options_set_bytes(o,
                        (std::string(method) + ".dictionary").c_str(),
                        dict.data(), dict.size()),
              GCOMP_OK);
        }
        size_t bound = 0;
        ASSERT_EQ(gcomp_encode_bound(nullptr, method, o, r.size(), &bound),
            GCOMP_OK);
        std::vector<uint8_t> out(bound);
        size_t w = 0;
        ASSERT_EQ(gcomp_encode_buffer(nullptr, method, o, r.data(), r.size(),
                      out.data(), out.size(), &w),
            GCOMP_OK)
            << method;
        gcomp_options_destroy(o);
        (use ? with : without) += w;
      }
    }
    EXPECT_LT(with, without)
        << method << ": " << with << " with the dictionary against " << without
        << " without";
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
