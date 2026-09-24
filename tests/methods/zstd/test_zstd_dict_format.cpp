/**
 * @file test_zstd_dict_format.cpp
 *
 * Reading a *formatted* Zstandard dictionary — the kind `zstd --train` and
 * libzstd's `ZDICT_trainFromBuffer` produce.
 *
 * RFC 8878 section 5 allows two shapes. A **content-only** dictionary is just
 * bytes, which is what this library's own trainer writes and what every method
 * here can use. A **formatted** one begins with the magic number `0xEC30A437`,
 * a `Dictionary_ID`, and pre-trained Huffman and FSE tables before its
 * content.
 *
 * The parser for the second existed and had never worked. All three of its
 * calls to `zstd_fse_build_decoding_table()` passed NULL for the
 * `max_symbol_out` parameter, which that function refuses outright, so every
 * formatted dictionary failed at the first FSE table with
 * `GCOMP_ERR_INVALID_ARG` — reported to the caller as `GCOMP_ERR_CORRUPT`, so
 * the dictionary looked malformed rather than unread.
 *
 * Nothing noticed because our encoder writes content-only dictionaries and no
 * test had ever handed this one of the other kind. The first `zstd --train`
 * dictionary did, immediately.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <temp_file.h>
#include <unistd.h>
#include <vector>

namespace {

bool skip_oracle() {
  const char * e = std::getenv("GCOMP_SKIP_ORACLE_TESTS");
  return e && std::string(e) == "1";
}

bool has_pyzstd_train() {
  // Double quotes: cmd.exe passes single quotes through, and python then
  // evaluates a string literal and succeeds whether pyzstd is there or not.
  return std::system("python3 -c \"import pyzstd; pyzstd.train_dict\" "
                     ">" GCOMP_TEST_NULL_DEVICE " 2>&1") == 0;
}

std::string temp_path(const char * tag) {
  // The name this replaces was "/tmp/...%d_%p" of a stack address: the
  // same value on every call from the same frame, in a directory named
  // outright rather than asked for.  cutil creates the file as it names
  // it, under gcu_path_temp_dir().
  //
  // Forward slashes, because these names are pasted into Python string
  // literals, where a Windows path's backslashes are escape sequences.
  return gcomp_test::toPosixPath(
      gcomp_test::uniqueTempPath("gcomp_fmtdict", std::string("_") + tag));
}

std::vector<uint8_t> read_file(const std::string & p) {
  return gcomp_test::readWholeFile(p);
}

/// One record of the shape a dictionary is actually for: short and repetitive.
std::string record(int i) {
  char buf[256];
  std::snprintf(buf, sizeof(buf),
      "{\"ts\":\"2026-09-20T%02d:%02d:00Z\",\"user\":\"alice\","
      "\"method\":\"GET\",\"path\":\"/api/v1/users\",\"status\":200,"
      "\"n\":%d}",
      i % 24, i % 60, i);
  return std::string(buf);
}

} // namespace

/**
 * @brief A dictionary the reference trained is usable here.
 *
 * Both halves matter. It has to be *accepted* - the bug made it
 * GCOMP_ERR_CORRUPT - and it has to actually help, because a parser that
 * silently discarded the content would accept it and change nothing.
 */
TEST(ZstdDictFormat, AFormattedDictionaryFromTheReferenceIsUsable) {
  if (skip_oracle()) {
    GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
  }
  if (!has_pyzstd_train()) {
    GTEST_SKIP() << "pyzstd with train_dict not available";
  }

  const std::string dict_path = temp_path("dict");
  const std::string script_path = temp_path("py");

  // The script goes to a file rather than into the shell command. Nesting
  // three levels of quoting inside a C string literal is how the first
  // version of this failed, and it failed silently: system() returned 256 and
  // the only symptom was a dictionary that was not there.
  {
    FILE * f = std::fopen(script_path.c_str(), "w");
    ASSERT_NE(f, nullptr);
    // Written without a single per-cent sign: this is a C format string
    // producing a Python program, and `%` means something in both. The first
    // version tried to escape its way through and produced a syntax error
    // that surfaced only as system() returning 256.
    std::fprintf(f,
        "import pyzstd\n"
        "s = []\n"
        "for i in range(3000):\n"
        "    h = str(i // 60).zfill(2)\n"
        "    m = str(i).zfill(2)[-2:]\n"
        "    s.append(('{\"ts\":\"2026-09-20T' + h + ':' + m + ':00Z\",'\n"
        "              '\"user\":\"alice\",\"method\":\"GET\",'\n"
        "              '\"path\":\"/api/v1/users\",\"status\":200,'\n"
        "              '\"n\":' + str(i) + '}').encode())\n"
        "d = pyzstd.train_dict(s, 8192)\n"
        "open('%s', 'wb').write(d.dict_content)\n",
        dict_path.c_str());
    std::fclose(f);
  }

  char cmd[768];
  std::snprintf(cmd, sizeof(cmd), "python3 %s", script_path.c_str());
  const int rc = std::system(cmd);
  std::remove(script_path.c_str());
  ASSERT_EQ(rc, 0) << "the reference dictionary trainer did not run";

  const std::vector<uint8_t> dict = read_file(dict_path);
  std::remove(dict_path.c_str());
  ASSERT_GT(dict.size(), 64u);
  // The magic that says this is a formatted dictionary, not raw content.
  ASSERT_EQ(dict[0], 0x37u);
  ASSERT_EQ(dict[1], 0xA4u);
  ASSERT_EQ(dict[2], 0x30u);
  ASSERT_EQ(dict[3], 0xECu);

  size_t with_dict = 0;
  size_t without_dict = 0;

  for (int i = 5000; i < 5100; i++) {
    const std::string rec = record(i);
    const uint8_t * in = (const uint8_t *)rec.data();

    for (int use = 0; use < 2; use++) {
      gcomp_options_t * o = nullptr;
      ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_int64(o, "zstd.level", 3), GCOMP_OK);
      if (use) {
        ASSERT_EQ(gcomp_options_set_bytes(
                      o, "zstd.dictionary", dict.data(), dict.size()),
            GCOMP_OK);
      }
      size_t bound = 0;
      ASSERT_EQ(gcomp_encode_bound(nullptr, "zstd", o, rec.size(), &bound),
          GCOMP_OK);
      std::vector<uint8_t> out(bound);
      size_t w = 0;
      ASSERT_EQ(gcomp_encode_buffer(nullptr, "zstd", o, in, rec.size(),
                    out.data(), out.size(), &w),
          GCOMP_OK)
          << (use ? "with" : "without")
          << " a formatted dictionary: the encoder refused it";

      if (use) {
        with_dict += w;
        // And it reads back, which is what says the content reached the
        // decoder's history and not only the encoder's.
        ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 0),
            GCOMP_OK);
        std::vector<uint8_t> back(rec.size() + 1024);
        size_t produced = 0;
        ASSERT_EQ(gcomp_decode_buffer(nullptr, "zstd", o, out.data(), w,
                      back.data(), back.size(), &produced),
            GCOMP_OK);
        ASSERT_EQ(produced, rec.size());
        EXPECT_EQ(std::memcmp(back.data(), in, rec.size()), 0);
      }
      else {
        without_dict += w;
      }
      gcomp_options_destroy(o);
    }
  }

  // A parser that accepted the dictionary and threw its content away would
  // pass everything above. The whole point is the size.
  EXPECT_LT(with_dict * 2, without_dict)
      << with_dict << " bytes with a trained dictionary against "
      << without_dict << " without; it was accepted but is not being used";
}

/// Say so if the reference is missing.
TEST(ZstdDictFormat, OracleIsActuallyAvailable) {
  if (skip_oracle()) {
    GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
  }
  ASSERT_TRUE(has_pyzstd_train())
      << "pyzstd with train_dict was not found, so nothing here handed this "
         "library a formatted dictionary produced by the reference. Install "
         "it (apt install python3-pyzstd), or set GCOMP_SKIP_ORACLE_TESTS=1.";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
