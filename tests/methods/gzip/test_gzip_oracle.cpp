/**
 * @file test_gzip_oracle.cpp
 *
 * Cross-tool validation ("oracle") tests for the gzip method.
 *
 * These tests compare our gzip implementation against external tools
 * (Python gzip module, system gzip CLI) to verify correctness.
 * Tests are skipped gracefully when external tools are not available.
 *
 * Environment variables:
 *   GCOMP_SKIP_ORACLE_TESTS - Set to "1" to skip all oracle tests
 *   GCOMP_ORACLE_VERBOSE - Set to "1" for verbose output
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../common/test_helpers.h"
#include "data/golden_vectors.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ghoti.io/compress/compress.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#define popen _popen
#define pclose _pclose
#else
#include <unistd.h>
#endif
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/gzip.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <gtest/gtest.h>
#include <random>
#include <sstream>
#include <string>
#include <temp_file.h>
#include <vector>

// Check if oracle tests should be skipped
static bool shouldSkipOracleTests() {
  const char * env = std::getenv("GCOMP_SKIP_ORACLE_TESTS");
  return env && std::string(env) == "1";
}

static bool isVerbose() {
  const char * env = std::getenv("GCOMP_ORACLE_VERBOSE");
  return env && std::string(env) == "1";
}

// Get the Python command name (python3 on Unix, python on Windows)
static const char * getPythonCommand() {
#ifdef _WIN32
  static const char * cmd = nullptr;
  if (!cmd) {
    if (system("python3 --version >NUL 2>&1") == 0) {
      cmd = "python3";
    }
    else {
      cmd = "python";
    }
  }
  return cmd;
#else
  return "python3";
#endif
}

// Check if Python 3 with gzip is available
static bool hasPythonGzip() {
  std::string cmd = std::string(getPythonCommand()) + " -c \"import gzip\"";
#ifdef _WIN32
  cmd += " >NUL 2>&1";
#else
  cmd += " >/dev/null 2>&1";
#endif
  return system(cmd.c_str()) == 0;
}

// Check if gzip CLI is available
static bool hasGzipCli() {
#ifdef _WIN32
  int result = system("gzip --version >NUL 2>&1");
#else
  int result = system("gzip --version >/dev/null 2>&1");
#endif
  return result == 0;
}

// Check if gunzip CLI is available
static bool hasGunzipCli() {
#ifdef _WIN32
  int result = system("gunzip --version >NUL 2>&1");
#else
  int result = system("gunzip --version >/dev/null 2>&1");
#endif
  return result == 0;
}

class GzipOracleTest : public ::testing::Test {
protected:
  void SetUp() override {
    if (shouldSkipOracleTests()) {
      GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
    }

    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);

    has_python_gzip_ = hasPythonGzip();
    has_gzip_cli_ = hasGzipCli();
    has_gunzip_cli_ = hasGunzipCli();

    if (isVerbose()) {
      std::cout << "Python gzip available: "
                << (has_python_gzip_ ? "yes" : "no") << std::endl;
      std::cout << "gzip CLI available: " << (has_gzip_cli_ ? "yes" : "no")
                << std::endl;
      std::cout << "gunzip CLI available: " << (has_gunzip_cli_ ? "yes" : "no")
                << std::endl;
    }
  }



  // Helper to run a command and capture stdout
  std::vector<uint8_t> runCommandGetOutput(const std::string & cmd) {
#ifdef _WIN32
    FILE * pipe = popen(cmd.c_str(), "rb");
#else
    FILE * pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) {
      return {};
    }
    std::vector<uint8_t> result;
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
      result.insert(result.end(), buffer, buffer + n);
    }
    pclose(pipe);
    return result;
  }

  // Use Python gzip to compress data
  std::vector<uint8_t> pythonGzipCompress(
      const std::vector<uint8_t> & data, int level = 6) {
    if (!has_python_gzip_) {
      return {};
    }

    gcomp_test::TempFile tmp("gcomp_gzip_oracle", ".bin");
    if (!tmp.valid() || !tmp.write(data)) {
      return {};
    }

    // Python wants forward slashes in the literal below whatever the host
    // uses; cutil copies unchanged on POSIX, so this is not an #ifdef.
    std::string escaped_path = gcomp_test::toPosixPath(tmp.path());
    std::stringstream cmd;
    cmd << getPythonCommand() << " -c \""
        << "import gzip,sys;"
        << "data = open('" << escaped_path << "', 'rb').read();"
        << "sys.stdout.buffer.write(gzip.compress(data, compresslevel=" << level
        << "));"
        << "\"";

    // No unlink: ~TempFile does it, on every path out of this function.
    return runCommandGetOutput(cmd.str());
  }

  // Use Python gzip to decompress data
  std::vector<uint8_t> pythonGzipDecompress(const std::vector<uint8_t> & data) {
    if (!has_python_gzip_ || data.empty()) {
      return {};
    }

    gcomp_test::TempFile tmp("gcomp_gzip_oracle", ".gz");
    if (!tmp.valid() || !tmp.write(data)) {
      return {};
    }

    // Python wants forward slashes in the literal below whatever the host
    // uses; cutil copies unchanged on POSIX, so this is not an #ifdef.
    std::string escaped_path = gcomp_test::toPosixPath(tmp.path());
    std::stringstream cmd;
    cmd << getPythonCommand() << " -c \""
        << "import gzip,sys;"
        << "data = open('" << escaped_path << "', 'rb').read();"
        << "sys.stdout.buffer.write(gzip.decompress(data));"
        << "\"";

    // No unlink: ~TempFile does it, on every path out of this function.
    return runCommandGetOutput(cmd.str());
  }

  // Use gzip CLI to compress data
  std::vector<uint8_t> gzipCliCompress(
      const std::vector<uint8_t> & data, int level = 6) {
    if (!has_gzip_cli_) {
      return {};
    }

    gcomp_test::TempFile tmp("gcomp_gzip_oracle", ".bin");
    if (!tmp.valid() || !tmp.write(data)) {
      return {};
    }

    std::stringstream cmd;
    cmd << "gzip -" << level << " -c \"" << tmp.path() << "\"";

    // No unlink: ~TempFile does it, on every path out of this function.
    return runCommandGetOutput(cmd.str());
  }

  // Use gunzip CLI to decompress data
  std::vector<uint8_t> gunzipCliDecompress(const std::vector<uint8_t> & data) {
    if (!has_gunzip_cli_ || data.empty()) {
      return {};
    }

    gcomp_test::TempFile tmp("gcomp_gzip_oracle", ".gz");
    if (!tmp.valid() || !tmp.write(data)) {
      return {};
    }

    std::stringstream cmd;
    cmd << "gunzip -c \"" << tmp.path() << "\"";

    // No unlink: ~TempFile does it, on every path out of this function.
    return runCommandGetOutput(cmd.str());
  }

  // Compress with our library
  std::vector<uint8_t> gcompCompress(
      const std::vector<uint8_t> & data, int level = 6) {
    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      return {};
    }
    if (gcomp_options_set_int64(opts, "deflate.level", level) != GCOMP_OK) {
      gcomp_options_destroy(opts);
      return {};
    }

    size_t comp_capacity = (data.size() * 12 / 10) + 1024;
    std::vector<uint8_t> compressed(std::max(comp_capacity, size_t(1024)));
    size_t comp_size = 0;

    gcomp_status_t status =
        gcomp_encode_buffer(registry_, "gzip", opts, data.data(), data.size(),
            compressed.data(), compressed.size(), &comp_size);
    gcomp_options_destroy(opts);

    if (status != GCOMP_OK) {
      return {};
    }
    compressed.resize(comp_size);
    return compressed;
  }

  // Decompress with our library
  std::vector<uint8_t> gcompDecompress(
      const std::vector<uint8_t> & data, size_t expected_size = 0) {
    if (data.empty()) {
      return {};
    }

    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      return {};
    }
    if (gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 0) !=
        GCOMP_OK) {
      gcomp_options_destroy(opts);
      return {};
    }

    size_t decomp_capacity =
        expected_size > 0 ? expected_size + 1024 : data.size() * 100 + 1024;
    std::vector<uint8_t> decompressed(decomp_capacity);
    size_t decomp_size = 0;

    gcomp_status_t status =
        gcomp_decode_buffer(registry_, "gzip", opts, data.data(), data.size(),
            decompressed.data(), decompressed.size(), &decomp_size);
    gcomp_options_destroy(opts);

    if (status != GCOMP_OK) {
      return {};
    }
    decompressed.resize(decomp_size);
    return decompressed;
  }

  // Generate test data
  std::vector<uint8_t> generateTextData(size_t size) {
    std::vector<uint8_t> data(size);
    const char * words[] = {"hello", "world", "test", "data", "compression",
        "gzip", "deflate", "oracle"};
    size_t pos = 0;
    size_t word_idx = 0;
    while (pos < size) {
      const char * word = words[word_idx % 8];
      size_t word_len = strlen(word);
      for (size_t i = 0; i < word_len && pos < size; i++) {
        data[pos++] = word[i];
      }
      if (pos < size) {
        data[pos++] = ' ';
      }
      word_idx++;
    }
    return data;
  }

  std::vector<uint8_t> generateRandomData(size_t size, unsigned int seed = 42) {
    std::vector<uint8_t> data(size);
    std::mt19937 gen(seed);
    std::uniform_int_distribution<> dis(0, 255);
    for (size_t i = 0; i < size; i++) {
      data[i] = static_cast<uint8_t>(dis(gen));
    }
    return data;
  }

  std::vector<uint8_t> generateRepeatedPattern(size_t size) {
    std::vector<uint8_t> data(size);
    const uint8_t pattern[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
    for (size_t i = 0; i < size; i++) {
      data[i] = pattern[i % sizeof(pattern)];
    }
    return data;
  }

  gcomp_registry_t * registry_ = nullptr;
  bool has_python_gzip_ = false;
  bool has_gzip_cli_ = false;
  bool has_gunzip_cli_ = false;
};

//
// Tests: Golden vectors (pre-generated external gzip data)
//

TEST_F(GzipOracleTest, OracleIsActuallyAvailable) {
  // A skipped oracle test and an absent one look identical in the summary
  // line. If no oracle at all can run, say so as a failure rather than
  // reporting a green suite that checked nothing against a reference.
  //
  // Three references gate tests here and they can go missing independently,
  // so each is reported rather than stopping at the first.
  EXPECT_TRUE(has_python_gzip_)
      << "python3 with the gzip module was not found; the tests that read our "
         "output with it, and write input for us with it, are all skipped.";
  EXPECT_TRUE(has_gzip_cli_)
      << "the gzip CLI was not found; nothing checks that the system gzip can "
         "read what we write.";
  EXPECT_TRUE(has_gunzip_cli_)
      << "the gunzip CLI was not found; nothing checks that we can read what "
         "the system gzip writes.\n"
         "Install what is missing, or set GCOMP_SKIP_ORACLE_TESTS=1 to say the "
         "gap is intentional.";
}

TEST_F(GzipOracleTest, GoldenVectors_Decompress) {
  for (size_t i = 0; i < g_gzip_golden_vectors_count; i++) {
    const auto & vec = g_gzip_golden_vectors[i];
    std::vector<uint8_t> decompressed =
        gcompDecompress(std::vector<uint8_t>(vec.compressed,
                            vec.compressed + vec.compressed_len),
            vec.expected_len);

    ASSERT_EQ(decompressed.size(), vec.expected_len)
        << "Size mismatch for vector: " << vec.name;

    if (vec.expected_len > 0) {
      ASSERT_EQ(memcmp(decompressed.data(), vec.expected, vec.expected_len), 0)
          << "Data mismatch for vector: " << vec.name;
    }

    if (isVerbose()) {
      std::cout << "Vector " << vec.name << ": " << vec.compressed_len << " -> "
                << decompressed.size() << " bytes OK" << std::endl;
    }
  }
}

TEST_F(GzipOracleTest, GoldenVectors_ConcatDecompress) {
  // Concatenated vector needs gzip.concat option
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bool(opts, "gzip.concat", 1), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 0),
      GCOMP_OK);

  const auto & vec = g_gzip_concat_vector;
  size_t decomp_capacity = vec.expected_len + 1024;
  std::vector<uint8_t> decompressed(decomp_capacity);
  size_t decomp_size = 0;

  gcomp_status_t status = gcomp_decode_buffer(registry_, "gzip", opts,
      vec.compressed, vec.compressed_len, decompressed.data(),
      decompressed.size(), &decomp_size);
  gcomp_options_destroy(opts);

  ASSERT_EQ(status, GCOMP_OK);
  ASSERT_EQ(decomp_size, vec.expected_len);
  ASSERT_EQ(memcmp(decompressed.data(), vec.expected, vec.expected_len), 0);

  if (isVerbose()) {
    std::cout << "Concat vector: " << vec.compressed_len << " -> "
              << decomp_size << " bytes OK" << std::endl;
  }
}

//
// Tests: Our encoder, Python decoder
//

TEST_F(GzipOracleTest, OurEncoder_PythonDecoder_TextData) {
  if (!has_python_gzip_) {
    GTEST_SKIP() << "Python gzip not available";
  }

  std::vector<uint8_t> original = generateTextData(10 * 1024);
  std::vector<uint8_t> compressed = gcompCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonGzipDecompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";

  if (isVerbose()) {
    std::cout << "Text data: " << original.size() << " -> " << compressed.size()
              << " bytes (" << (100 * compressed.size() / original.size())
              << "%)" << std::endl;
  }
}

TEST_F(GzipOracleTest, OurEncoder_PythonDecoder_RandomData) {
  if (!has_python_gzip_) {
    GTEST_SKIP() << "Python gzip not available";
  }

  std::vector<uint8_t> original = generateRandomData(10 * 1024);
  std::vector<uint8_t> compressed = gcompCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonGzipDecompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(GzipOracleTest, OurEncoder_PythonDecoder_RepeatedPattern) {
  if (!has_python_gzip_) {
    GTEST_SKIP() << "Python gzip not available";
  }

  std::vector<uint8_t> original = generateRepeatedPattern(10 * 1024);
  std::vector<uint8_t> compressed = gcompCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonGzipDecompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(GzipOracleTest, OurEncoder_PythonDecoder_AllLevels) {
  if (!has_python_gzip_) {
    GTEST_SKIP() << "Python gzip not available";
  }

  std::vector<uint8_t> original = generateTextData(4 * 1024);

  for (int level = 0; level <= 9; level++) {
    std::vector<uint8_t> compressed = gcompCompress(original, level);
    ASSERT_FALSE(compressed.empty()) << "Compression failed at level " << level;

    std::vector<uint8_t> decompressed = pythonGzipDecompress(compressed);
    ASSERT_EQ(decompressed.size(), original.size())
        << "Size mismatch at level " << level;
    ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
        << "Data mismatch at level " << level;

    if (isVerbose()) {
      std::cout << "Level " << level << ": " << original.size() << " -> "
                << compressed.size() << " bytes" << std::endl;
    }
  }
}

//
// Tests: Python encoder, Our decoder
//

TEST_F(GzipOracleTest, PythonEncoder_OurDecoder_TextData) {
  if (!has_python_gzip_) {
    GTEST_SKIP() << "Python gzip not available";
  }

  std::vector<uint8_t> original = generateTextData(10 * 1024);
  std::vector<uint8_t> compressed = pythonGzipCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "Python compression failed";

  std::vector<uint8_t> decompressed =
      gcompDecompress(compressed, original.size());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(GzipOracleTest, PythonEncoder_OurDecoder_RandomData) {
  if (!has_python_gzip_) {
    GTEST_SKIP() << "Python gzip not available";
  }

  std::vector<uint8_t> original = generateRandomData(10 * 1024);
  std::vector<uint8_t> compressed = pythonGzipCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "Python compression failed";

  std::vector<uint8_t> decompressed =
      gcompDecompress(compressed, original.size());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(GzipOracleTest, PythonEncoder_OurDecoder_AllLevels) {
  if (!has_python_gzip_) {
    GTEST_SKIP() << "Python gzip not available";
  }

  std::vector<uint8_t> original = generateTextData(4 * 1024);

  for (int level = 0; level <= 9; level++) {
    std::vector<uint8_t> compressed = pythonGzipCompress(original, level);
    ASSERT_FALSE(compressed.empty())
        << "Python compression failed at level " << level;

    std::vector<uint8_t> decompressed =
        gcompDecompress(compressed, original.size());
    ASSERT_EQ(decompressed.size(), original.size())
        << "Size mismatch at level " << level;
    ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
        << "Data mismatch at level " << level;
  }
}

//
// Tests: gzip CLI interop
//

TEST_F(GzipOracleTest, OurEncoder_GunzipCli_TextData) {
  if (!has_gunzip_cli_) {
    GTEST_SKIP() << "gunzip CLI not available";
  }

  std::vector<uint8_t> original = generateTextData(10 * 1024);
  std::vector<uint8_t> compressed = gcompCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = gunzipCliDecompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(GzipOracleTest, GzipCli_OurDecoder_TextData) {
  if (!has_gzip_cli_) {
    GTEST_SKIP() << "gzip CLI not available";
  }

  std::vector<uint8_t> original = generateTextData(10 * 1024);
  std::vector<uint8_t> compressed = gzipCliCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "gzip CLI compression failed";

  std::vector<uint8_t> decompressed =
      gcompDecompress(compressed, original.size());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

//
// Tests: Empty and edge cases
//

TEST_F(GzipOracleTest, OurEncoder_PythonDecoder_Empty) {
  if (!has_python_gzip_) {
    GTEST_SKIP() << "Python gzip not available";
  }

  std::vector<uint8_t> original;
  std::vector<uint8_t> compressed = gcompCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonGzipDecompress(compressed);
  ASSERT_EQ(decompressed.size(), 0) << "Expected empty output";
}

TEST_F(GzipOracleTest, PythonEncoder_OurDecoder_Empty) {
  if (!has_python_gzip_) {
    GTEST_SKIP() << "Python gzip not available";
  }

  std::vector<uint8_t> original;
  std::vector<uint8_t> compressed = pythonGzipCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "Python compression failed";

  std::vector<uint8_t> decompressed = gcompDecompress(compressed, 0);
  ASSERT_EQ(decompressed.size(), 0) << "Expected empty output";
}

TEST_F(GzipOracleTest, OurEncoder_PythonDecoder_SingleByte) {
  if (!has_python_gzip_) {
    GTEST_SKIP() << "Python gzip not available";
  }

  std::vector<uint8_t> original = {0x42};
  std::vector<uint8_t> compressed = gcompCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonGzipDecompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(decompressed[0], original[0]) << "Data mismatch";
}

//
// Tests: Various sizes
//

TEST_F(GzipOracleTest, OurEncoder_PythonDecoder_VariousSizes) {
  if (!has_python_gzip_) {
    GTEST_SKIP() << "Python gzip not available";
  }

  std::vector<size_t> sizes = {1, 10, 100, 1000, 10000, 65535, 65536, 100000};

  for (size_t size : sizes) {
    std::vector<uint8_t> original = generateTextData(size);
    std::vector<uint8_t> compressed = gcompCompress(original, 6);
    ASSERT_FALSE(compressed.empty()) << "Compression failed for size " << size;

    std::vector<uint8_t> decompressed = pythonGzipDecompress(compressed);
    ASSERT_EQ(decompressed.size(), original.size())
        << "Size mismatch for size " << size;
    ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
        << "Data mismatch for size " << size;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
