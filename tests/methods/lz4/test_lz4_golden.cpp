/**
 * @file test_lz4_golden.cpp
 *
 * Decoder tests against stored LZ4 frames produced elsewhere.
 *
 * This is the one form of cross-implementation test that needs nothing
 * installed: the frames were produced by a real LZ4 and are checked into the
 * tree.  test_lz4_spec_oracle.cpp covers far more ground, but needs python3,
 * and its strongest tests need liblz4 to be present.  These run anywhere.
 *
 * This file replaces the cross-tool half of the old test_lz4_oracle.cpp, in
 * which 18 of 19 tests needed a Python module or a CLI that is not commonly
 * installed and skipped silently when they were absent.  The one test that
 * did run was this one, and it could not fail: it printed a warning for each
 * mismatched vector and then asserted only that at least one had passed,
 * on the stated grounds that the vectors "were created by hand and may need
 * verification against an actual lz4 library".
 *
 * They had in fact been verified, as the header of golden_vectors.h says, and
 * all ten were re-checked against liblz4 before this file was written.  The
 * comment was wrong, and it had quietly disarmed the only lz4 cross-check
 * that ran at all.  Every vector is now a hard assertion.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "data/golden_vectors.h"
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lz4.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <gtest/gtest.h>
#include <vector>

namespace {

class Lz4GoldenTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  std::vector<uint8_t> decode(
      const std::vector<uint8_t> & data, size_t expected, bool * ok_out) {
    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      *ok_out = false;
      return {};
    }
    gcomp_options_set_uint64(opts, "limits.max_output_bytes", 16u << 20);
    gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 1000000);

    std::vector<uint8_t> out(expected + 4096);
    size_t used = out.size();
    gcomp_status_t s = gcomp_decode_buffer(registry_, "lz4", opts, data.data(),
        data.size(), out.data(), out.size(), &used);
    gcomp_options_destroy(opts);
    *ok_out = (s == GCOMP_OK);
    if (s != GCOMP_OK) {
      return {};
    }
    out.resize(used);
    return out;
  }

  gcomp_registry_t * registry_ = nullptr;
};

TEST_F(Lz4GoldenTest, EveryStoredFrameDecodesExactly) {
  ASSERT_GT(lz4_golden_vectors_count, 0u) << "no golden vectors are present";

  for (size_t i = 0; i < lz4_golden_vectors_count; i++) {
    const lz4_golden_vector_t & vec = lz4_golden_vectors[i];
    std::vector<uint8_t> frame(
        vec.compressed, vec.compressed + vec.compressed_len);

    bool ok = false;
    std::vector<uint8_t> out = decode(frame, vec.expected_len, &ok);

    ASSERT_TRUE(ok) << vec.name << ": our decoder refused a frame that liblz4 "
                                   "reads";
    ASSERT_EQ(out.size(), vec.expected_len) << vec.name;
    if (vec.expected_len) {
      ASSERT_EQ(memcmp(out.data(), vec.expected, vec.expected_len), 0)
          << vec.name;
    }
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
