/**
 * @file test_zlib_oracle.cpp
 *
 * Cross-checks the zlib container against the real zlib, through Python.
 *
 * Round-tripping against ourselves proves the two halves agree with each
 * other; it does not prove either agrees with RFC 1950.  These do: streams we
 * produce are read by zlib, streams zlib produces are read by us, and the
 * header bytes are compared directly, because a header can be wrong in a way
 * that our own decoder is equally wrong about.
 *
 * Skipped, not failed, when Python with zlib is not available -- as the
 * integration oracle tests are, and for the same reason.
 *
 * Environment:
 *   GCOMP_SKIP_ORACLE_TESTS=1  skip these
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../common/test_helpers.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zlib.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define popen _popen
#define pclose _pclose
#define unlink _unlink
#else
#include <unistd.h>
#endif

namespace {

const char * PythonCommand() {
#ifdef _WIN32
  return (system("python3 --version >NUL 2>&1") == 0) ? "python3" : "python";
#else
  return "python3";
#endif
}

bool HasPythonZlib() {
  if (const char * skip = std::getenv("GCOMP_SKIP_ORACLE_TESTS")) {
    if (skip[0] == '1') {
      return false;
    }
  }
  std::string cmd = std::string(PythonCommand()) + " -c \"import zlib\"";
#ifdef _WIN32
  cmd += " >NUL 2>&1";
#else
  cmd += " >/dev/null 2>&1";
#endif
  return system(cmd.c_str()) == 0;
}

std::string WriteTemp(const std::vector<uint8_t> & data) {
#ifdef _WIN32
  char name[L_tmpnam];
  if (!tmpnam(name)) {
    return "";
  }
  std::ofstream f(name, std::ios::binary);
  if (!f) {
    return "";
  }
  f.write((const char *)data.data(), (std::streamsize)data.size());
  f.close();
  return name;
#else
  char name[] = "/tmp/gcomp_zlib_oracle_XXXXXX";
  int fd = mkstemp(name);
  if (fd < 0) {
    return "";
  }
  if (!data.empty()) {
    if (write(fd, data.data(), data.size()) < 0) {
      close(fd);
      unlink(name);
      return "";
    }
  }
  close(fd);
  return name;
#endif
}

std::vector<uint8_t> RunPython(const std::string & script,
    const std::vector<uint8_t> & input, bool * ok) {
  *ok = false;
  std::string in_path = WriteTemp(input);
  if (in_path.empty()) {
    return {};
  }
  std::string out_path = WriteTemp({});
  if (out_path.empty()) {
    unlink(in_path.c_str());
    return {};
  }

  // Keep the script on one line: quoting a multi-line program through the
  // shell is exactly the kind of thing that fails differently on Windows.
  std::string cmd = std::string(PythonCommand()) + " -c \"import zlib,sys;" +
      "src=open(r'" + in_path + "','rb').read();" + script +
      "open(r'" + out_path + "','wb').write(dst)\"";
#ifdef _WIN32
  cmd += " >NUL 2>&1";
#else
  cmd += " >/dev/null 2>&1";
#endif

  int rc = system(cmd.c_str());
  std::vector<uint8_t> result;
  if (rc == 0) {
    std::ifstream f(out_path, std::ios::binary);
    if (f) {
      result.assign(std::istreambuf_iterator<char>(f),
          std::istreambuf_iterator<char>());
      *ok = true;
    }
  }
  unlink(in_path.c_str());
  unlink(out_path.c_str());
  return result;
}

class ZlibOracleTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
    available_ = HasPythonZlib();
  }

  std::vector<uint8_t> Encode(
      const std::vector<uint8_t> & data, gcomp_options_t * opts) {
    gcomp_encoder_t * encoder = nullptr;
    EXPECT_EQ(
        gcomp_encoder_create(registry_, "zlib", opts, &encoder), GCOMP_OK);
    if (!encoder) {
      return {};
    }
    std::vector<uint8_t> out(data.size() + data.size() / 2 + 4096);
    gcomp_buffer_t in_buf = {
        const_cast<uint8_t *>(data.data()), data.size(), 0};
    gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
    while (in_buf.used < in_buf.size) {
      size_t bi = in_buf.used;
      size_t bo = out_buf.used;
      EXPECT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
      if (in_buf.used == bi && out_buf.used == bo) {
        ADD_FAILURE() << "update() made no progress";
        break;
      }
    }
    while (gcomp_encoder_finish(encoder, &out_buf) == GCOMP_ERR_LIMIT) {
    }
    out.resize(out_buf.used);
    gcomp_encoder_destroy(encoder);
    return out;
  }

  std::vector<uint8_t> Decode(
      const std::vector<uint8_t> & stream, size_t expected) {
    gcomp_decoder_t * decoder = nullptr;
    EXPECT_EQ(
        gcomp_decoder_create(registry_, "zlib", nullptr, &decoder), GCOMP_OK);
    if (!decoder) {
      return {};
    }
    std::vector<uint8_t> out(expected + 4096);
    gcomp_buffer_t in_buf = {
        const_cast<uint8_t *>(stream.data()), stream.size(), 0};
    gcomp_buffer_t out_buf = {out.data(), out.size(), 0};
    EXPECT_EQ(gcomp_decoder_update(decoder, &in_buf, &out_buf), GCOMP_OK);
    EXPECT_EQ(gcomp_decoder_finish(decoder, &out_buf), GCOMP_OK);
    out.resize(out_buf.used);
    gcomp_decoder_destroy(decoder);
    return out;
  }

  static std::vector<uint8_t> Sample(size_t n, unsigned seed) {
    std::vector<uint8_t> v(n);
    if (n == 0) {
      return v; // v.data() is null here, and pointer arithmetic on it is UB.
    }
    static const char * words[] = {"alpha ", "beta ", "gamma ", "delta ",
        "epsilon ", "zeta "};
    unsigned s = seed;
    size_t p = 0;
    while (p < n / 2) {
      s = s * 1103515245u + 12345u;
      const char * w = words[(s >> 16) % 6];
      size_t l = strlen(w);
      if (p + l > n / 2) {
        l = n / 2 - p;
      }
      if (l > 0) {
        memcpy(v.data() + p, w, l);
      }
      p += l;
    }
    while (p < n) {
      s = s * 1103515245u + 12345u;
      v[p++] = (uint8_t)(s >> 16);
    }
    return v;
  }

  gcomp_registry_t * registry_ = nullptr;
  bool available_ = false;
};

/// zlib must be able to read everything we write, at every level.
TEST_F(ZlibOracleTest, ZlibReadsWhatWeWrite) {
  if (!available_) {
    GTEST_SKIP() << "Python zlib not available";
  }
  const std::vector<uint8_t> data = Sample(50000, 13);

  for (int64_t level = 0; level <= 9; level++) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", level), GCOMP_OK);
    std::vector<uint8_t> stream = Encode(data, opts);
    gcomp_options_destroy(opts);
    ASSERT_FALSE(stream.empty());

    bool ok = false;
    std::vector<uint8_t> back =
        RunPython("dst=zlib.decompress(src);", stream, &ok);
    ASSERT_TRUE(ok) << "zlib refused our level " << level << " stream";
    EXPECT_EQ(back, data) << "level=" << level;
  }
}

/// And at every window size, since CINFO is what tells zlib how much history
/// to keep.
TEST_F(ZlibOracleTest, ZlibReadsEveryWindowSizeWeWrite) {
  if (!available_) {
    GTEST_SKIP() << "Python zlib not available";
  }
  const std::vector<uint8_t> data = Sample(50000, 17);

  for (uint64_t wbits = 9; wbits <= 15; wbits++) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(opts, "deflate.window_bits", wbits),
        GCOMP_OK);
    std::vector<uint8_t> stream = Encode(data, opts);
    gcomp_options_destroy(opts);

    bool ok = false;
    std::vector<uint8_t> back =
        RunPython("dst=zlib.decompress(src);", stream, &ok);
    ASSERT_TRUE(ok) << "zlib refused our window_bits=" << wbits;
    EXPECT_EQ(back, data) << "window_bits=" << wbits;
  }
}

/// We must be able to read everything zlib writes.
TEST_F(ZlibOracleTest, WeReadWhatZlibWrites) {
  if (!available_) {
    GTEST_SKIP() << "Python zlib not available";
  }
  const std::vector<uint8_t> data = Sample(50000, 19);

  for (int level = 0; level <= 9; level++) {
    bool ok = false;
    std::vector<uint8_t> stream = RunPython(
        "dst=zlib.compress(src," + std::to_string(level) + ");", data, &ok);
    ASSERT_TRUE(ok) << "python could not compress at level " << level;
    ASSERT_FALSE(stream.empty());
    EXPECT_EQ(Decode(stream, data.size()), data) << "level=" << level;
  }
}

/// Including the sizes where framing is most of the stream.
TEST_F(ZlibOracleTest, WeReadZlibsAwkwardSizes) {
  if (!available_) {
    GTEST_SKIP() << "Python zlib not available";
  }
  const size_t sizes[] = {0, 1, 2, 3, 255, 256, 1023, 65536};
  for (size_t n : sizes) {
    std::vector<uint8_t> data = Sample(n, (unsigned)n + 3);
    bool ok = false;
    std::vector<uint8_t> stream = RunPython("dst=zlib.compress(src,6);", data,
        &ok);
    ASSERT_TRUE(ok) << "python could not compress " << n << " bytes";
    EXPECT_EQ(Decode(stream, n), data) << "n=" << n;
  }
}

/**
 * The header bytes themselves, compared with zlib's.
 *
 * This is the check a round trip cannot make: CINFO, FLEVEL and FCHECK could
 * all be wrong together and our own decoder would not care.  zlib picks the
 * same two bytes for the same settings, so they can simply be compared.
 */
TEST_F(ZlibOracleTest, OurHeaderBytesAreTheOnesZlibWouldWrite) {
  if (!available_) {
    GTEST_SKIP() << "Python zlib not available";
  }
  const std::vector<uint8_t> data = Sample(4000, 23);

  for (int level = 0; level <= 9; level++) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_int64(opts, "deflate.level", level), GCOMP_OK);
    std::vector<uint8_t> ours = Encode(data, opts);
    gcomp_options_destroy(opts);

    bool ok = false;
    std::vector<uint8_t> theirs = RunPython(
        "dst=zlib.compress(src," + std::to_string(level) + ");", data, &ok);
    ASSERT_TRUE(ok);
    ASSERT_GE(ours.size(), 2u);
    ASSERT_GE(theirs.size(), 2u);

    EXPECT_EQ(ours[0], theirs[0]) << "CMF differs at level " << level;
    EXPECT_EQ(ours[1], theirs[1]) << "FLG differs at level " << level;
  }
}

/**
 * The trailer too.  The Adler-32 is over the *input*, so it cannot depend on
 * how well either side compressed -- which makes it comparable even at levels
 * where our deflate and zlib's disagree byte for byte about everything in
 * between.
 */
TEST_F(ZlibOracleTest, OurTrailerIsTheOneZlibWouldWrite) {
  if (!available_) {
    GTEST_SKIP() << "Python zlib not available";
  }
  const size_t sizes[] = {0, 1, 1000, 50000};
  for (size_t n : sizes) {
    std::vector<uint8_t> data = Sample(n, (unsigned)n + 7);
    std::vector<uint8_t> ours = Encode(data, nullptr);
    bool ok = false;
    std::vector<uint8_t> theirs =
        RunPython("dst=zlib.compress(src,6);", data, &ok);
    ASSERT_TRUE(ok);
    ASSERT_GE(ours.size(), 4u);
    ASSERT_GE(theirs.size(), 4u);
    EXPECT_EQ(std::vector<uint8_t>(ours.end() - 4, ours.end()),
        std::vector<uint8_t>(theirs.end() - 4, theirs.end()))
        << "Adler-32 trailer differs at n=" << n;
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
