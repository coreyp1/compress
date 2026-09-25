/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Compress.
 *
 * Ghoti.io Compress is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Compress is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file test_golden_provenance.cpp
 *
 * Do the golden vectors mean what their files say they mean?
 *
 * Three `golden_vectors.h` files hold committed compressed bytes and the output
 * they are supposed to decode to, and the decoder suites check this library
 * against them. Their whole value is that this project did not write them - a
 * vector that came from somewhere else can catch a misreading of the
 * specification that our own encoder and decoder share, and nothing else in the
 * suite can.
 *
 * That value rests entirely on a sentence in a comment:
 *
 *   deflate  "generated using Python's zlib module with known inputs"
 *   gzip     "generated using Python's gzip/zlib modules with known inputs"
 *   zstd     "minimal valid Zstandard frames (RFC 8878 / zstd format spec)"
 *
 * **Nothing ever checked those sentences.** If a vector was in fact produced by
 * this library, or typed by hand and never confirmed, then a decoder suite that
 * passes against it is a round trip wearing a disguise: it looks like an
 * external check and is not one. That failure is invisible - the tests pass
 * either way, and they pass for the wrong reason.
 *
 * So each vector is handed to the reference here, and the reference's answer is
 * compared with the committed `expected`. This does not regenerate anything and
 * changes no fixture; it turns a comment into a check.
 *
 * zstd's file is the interesting one and is treated as such. It claims no tool
 * at all - the vectors are read out of the format specification by hand - so
 * this is the first time anything external has seen them, which makes it the
 * most valuable of the three rather than the weakest.
 *
 * lz4's vectors are checked the same way in test_lz4_spec_oracle.cpp, where the
 * liblz4 loader already lives; duplicating that loader here to keep the four
 * files together would be two copies of one piece of logic, which drift.
 *
 * The references are the pinned ones when this runs under `make check-oracle`.
 * See documentation/testing/oracles.md.
 */

#include "../methods/deflate/data/golden_vectors.h"
#include "../methods/gzip/data/golden_vectors.h"
#include "../methods/zstd/data/golden_vectors.h"

#include <gtest/gtest.h>
#include <temp_file.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool skip_oracle() {
  const char * e = std::getenv("GCOMP_SKIP_ORACLE_TESTS");
  return e && std::string(e) == "1";
}

/**
 * @brief Run @p command, giving it nothing, and collect its stdout as bytes.
 *
 * Binary output through popen, so no text-mode translation and no assumption
 * that the answer is printable: two of these vectors decode to bytes that are
 * not.
 *
 * @param command The shell command to run.
 * @param out Receives stdout.
 * @return False if the command could not be started or exited non-zero.
 */
bool capture(const std::string & command, std::vector<uint8_t> * out) {
  out->clear();
  FILE * pipe = popen(command.c_str(), "r");
  if (!pipe) {
    return false;
  }
  uint8_t buffer[4096];
  size_t got = 0;
  while ((got = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
    out->insert(out->end(), buffer, buffer + got);
  }
  return pclose(pipe) == 0;
}

/// Write @p data to a temporary file and return its path, or "" on failure.
std::string spill(const uint8_t * data, size_t len) {
  const std::string path = gcomp_test::uniqueTempPath("golden", ".bin");
  if (path.empty()) {
    return path;
  }
  std::vector<uint8_t> bytes(data, data + len);
  if (!gcomp_test::writeWholeFile(path, bytes)) {
    return std::string();
  }
  return path;
}

/**
 * @brief Decode one vector with a reference and compare with what is committed.
 *
 * @param what A label for the failure message.
 * @param compressed The committed compressed bytes.
 * @param compressed_len How many.
 * @param expected The committed decoded bytes; may be NULL when empty.
 * @param expected_len How many.
 * @param recipe A printf pattern taking one path, producing a shell command
 *   that writes the decoded bytes to stdout.
 */
void check_one(const char * what, const uint8_t * compressed,
    size_t compressed_len, const uint8_t * expected, size_t expected_len,
    const char * recipe) {
  SCOPED_TRACE(what);
  const std::string path = spill(compressed, compressed_len);
  ASSERT_FALSE(path.empty()) << "could not write a temporary file";

  char command[1024];
  std::snprintf(command, sizeof(command), recipe,
      gcomp_test::toPosixPath(path).c_str());

  std::vector<uint8_t> got;
  const bool ran = capture(command, &got);
  std::remove(path.c_str());

  ASSERT_TRUE(ran) << "the reference refused these bytes, which the file says "
                      "it produced. Either the vector is not what the comment "
                      "claims, or it is corrupt.";
  ASSERT_EQ(got.size(), expected_len)
      << "the reference decoded " << got.size() << " bytes and the committed "
      << "expectation is " << expected_len;
  if (expected_len > 0) {
    EXPECT_EQ(std::memcmp(got.data(), expected, expected_len), 0)
        << "the reference and the committed expectation disagree on the bytes";
  }
}

// Each recipe reads the file named by %s and writes the decoded bytes to
// stdout. Spelled here rather than inside the loops so that the three
// references are visible side by side, and so that a change to one cannot
// silently apply to another.
const char * const kDeflateRecipe =
    "python3 -c 'import sys,zlib; "
    "sys.stdout.buffer.write(zlib.decompress(open(sys.argv[1],\"rb\").read(),"
    " -15))' '%s'";
const char * const kGzipRecipe =
    "python3 -c 'import sys,gzip; "
    "sys.stdout.buffer.write(gzip.decompress(open(sys.argv[1],\"rb\").read()))'"
    " '%s'";
const char * const kZstdRecipe = "zstd -d -c '%s' 2>" GCOMP_TEST_NULL_DEVICE;

} // namespace

/**
 * @brief Say so if no reference ran.
 *
 * Every test below needs python3 and the zstd CLI. Without them each would
 * report nothing rather than failing, which is the state this whole file exists
 * to remove one level up.
 */
TEST(GoldenProvenance, OracleIsActuallyAvailable) {
  if (skip_oracle()) {
    GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
  }
  std::vector<uint8_t> out;
  EXPECT_TRUE(capture("python3 -c 'import zlib, gzip'", &out))
      << "python3 with the zlib and gzip modules was not found, so none of the "
         "committed deflate or gzip vectors were checked against the reference "
         "their file names. Install it, or set GCOMP_SKIP_ORACLE_TESTS=1 to say "
         "the gap is intentional.";
  EXPECT_TRUE(capture("zstd --version >" GCOMP_TEST_NULL_DEVICE " 2>&1", &out))
      << "the zstd CLI was not found, so the hand-built zstd vectors were not "
         "checked against any implementation at all. Install it, or set "
         "GCOMP_SKIP_ORACLE_TESTS=1 to say the gap is intentional.";
}

/// Every committed deflate vector decodes in zlib to its committed expectation.
TEST(GoldenProvenance, DeflateVectorsAreWhatZlibSaysTheyAre) {
  if (skip_oracle()) {
    GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
  }
  ASSERT_GT(g_golden_vectors_count, 0u) << "no deflate golden vectors present";
  for (size_t i = 0; i < g_golden_vectors_count; i++) {
    const gcomp_golden_vector_t & vec = g_golden_vectors[i];
    check_one(vec.name, vec.compressed, vec.compressed_len, vec.expected,
        vec.expected_len, kDeflateRecipe);
  }
}

/// Every committed gzip vector decodes in Python's gzip to its expectation.
TEST(GoldenProvenance, GzipVectorsAreWhatPythonSaysTheyAre) {
  if (skip_oracle()) {
    GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
  }
  ASSERT_GT(g_gzip_golden_vectors_count, 0u) << "no gzip golden vectors present";
  for (size_t i = 0; i < g_gzip_golden_vectors_count; i++) {
    const gcomp_gzip_golden_vector_t & vec = g_gzip_golden_vectors[i];
    check_one(vec.name, vec.compressed, vec.compressed_len, vec.expected,
        vec.expected_len, kGzipRecipe);
  }
}

/**
 * @brief The hand-built zstd frames decode in the real zstd.
 *
 * These two are not attributed to any tool: the file says they were read out of
 * the format specification. So unlike the other two this is not confirming an
 * attribution, it is the first external reading of them - and a frame assembled
 * by hand from a specification is exactly the thing a second implementation is
 * worth asking about.
 *
 * The vectors are loose arrays rather than a table, so they are named here.
 */
TEST(GoldenProvenance, ZstdHandBuiltFramesDecodeInZstd) {
  if (skip_oracle()) {
    GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
  }
  check_one("empty", zstd_v1_empty_compressed,
      sizeof(zstd_v1_empty_compressed), nullptr, 0, kZstdRecipe);
  check_one("hello", zstd_v2_hello_compressed,
      sizeof(zstd_v2_hello_compressed), zstd_v2_hello_expected,
      sizeof(zstd_v2_hello_expected), kZstdRecipe);
}

/**
 * @brief The two deflate vectors that are not in the table.
 *
 * Vectors 7 and 8 sit outside `g_golden_vectors` because their expected output
 * is generated at run time rather than committed - 256 bytes of high-entropy
 * data in a stored block, and 260 through a dynamic Huffman block. So there is
 * no committed expectation to compare against, and what is checkable is the
 * claim the file does make: that these are deflate streams which decode to that
 * many bytes.
 *
 * They are here because the compiler asked for them. This file would not build
 * without touching `golden_v8_compressed_ptr`, which only
 * test_deflate_decoder.cpp had ever referenced - an unused-variable error that
 * turned out to be pointing at the two vectors nothing was checking the
 * provenance of.
 */
TEST(GoldenProvenance, TheTwoUntabledDeflateVectorsDecodeInZlib) {
  if (skip_oracle()) {
    GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
  }
  struct Special {
    const char * name;
    const uint8_t * compressed;
    size_t compressed_len;
    size_t expected_len;
  };
  const Special specials[] = {
      {"v7_stored_high_entropy", golden_v7_compressed_ptr,
          golden_v7_compressed_len, golden_v7_expected_len},
      {"v8_dynamic_huffman", golden_v8_compressed_ptr,
          golden_v8_compressed_len, golden_v8_expected_len},
  };
  for (const Special & one : specials) {
    SCOPED_TRACE(one.name);
    const std::string path = spill(one.compressed, one.compressed_len);
    ASSERT_FALSE(path.empty());
    char command[1024];
    std::snprintf(command, sizeof(command), kDeflateRecipe,
        gcomp_test::toPosixPath(path).c_str());
    std::vector<uint8_t> got;
    const bool ran = capture(command, &got);
    std::remove(path.c_str());
    ASSERT_TRUE(ran) << "zlib refused these bytes";
    EXPECT_EQ(got.size(), one.expected_len)
        << "zlib decoded " << got.size() << " bytes; the file says "
        << one.expected_len;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
