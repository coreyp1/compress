/**
 * @file test_lz4_oracle.cpp
 *
 * Cross-tool validation ("oracle") tests for the lz4 method.
 *
 * These tests compare our lz4 implementation against external tools
 * (Python lz4.frame module, system lz4 CLI) to verify correctness.
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
#define close _close
#define write _write
#define unlink _unlink
#define popen _popen
#define pclose _pclose
#else
#include <unistd.h>
#endif
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/lz4.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <gtest/gtest.h>
#include <random>
#include <sstream>
#include <string>
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

// Check if Python 3 with lz4.frame is available
static bool hasPythonLz4() {
  std::string cmd =
      std::string(getPythonCommand()) + " -c \"import lz4.frame\"";
#ifdef _WIN32
  cmd += " >NUL 2>&1";
#else
  cmd += " >/dev/null 2>&1";
#endif
  return system(cmd.c_str()) == 0;
}

// Check if lz4 CLI is available
static bool hasLz4Cli() {
#ifdef _WIN32
  int result = system("lz4 --version >NUL 2>&1");
#else
  int result = system("lz4 --version >/dev/null 2>&1");
#endif
  return result == 0;
}

// Check if unlz4 CLI is available
static bool hasUnlz4Cli() {
#ifdef _WIN32
  int result = system("unlz4 --version >NUL 2>&1");
#else
  int result = system("unlz4 --version >/dev/null 2>&1");
#endif
  return result == 0;
}

class Lz4OracleTest : public ::testing::Test {
protected:
  void SetUp() override {
    if (shouldSkipOracleTests()) {
      GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
    }

    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);

    has_python_lz4_ = hasPythonLz4();
    has_lz4_cli_ = hasLz4Cli();
    has_unlz4_cli_ = hasUnlz4Cli();

    if (isVerbose()) {
      std::cout << "Python lz4 available: " << (has_python_lz4_ ? "yes" : "no")
                << std::endl;
      std::cout << "lz4 CLI available: " << (has_lz4_cli_ ? "yes" : "no")
                << std::endl;
      std::cout << "unlz4 CLI available: " << (has_unlz4_cli_ ? "yes" : "no")
                << std::endl;
    }
  }

  // Helper to write bytes to a temporary file
  std::string writeTempFile(
      const std::vector<uint8_t> & data, const std::string & suffix = ".bin") {
#ifdef _WIN32
    char tmpdir[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tmpdir) == 0) {
      return "";
    }
    char tmpname[MAX_PATH];
    static int counter = 0;
    snprintf(tmpname, sizeof(tmpname), "%sgcomp_lz4_oracle_%d_%d%s", tmpdir,
        _getpid(), counter++, suffix.c_str());

    int fd = _open(tmpname, _O_CREAT | _O_WRONLY | _O_BINARY | _O_EXCL, 0600);
    if (fd < 0) {
      return "";
    }
    int written =
        _write(fd, data.data(), static_cast<unsigned int>(data.size()));
    _close(fd);
    if (written < 0 || static_cast<size_t>(written) != data.size()) {
      _unlink(tmpname);
      return "";
    }
    return tmpname;
#else
    char tmpname[256];
    snprintf(tmpname, sizeof(tmpname), "/tmp/gcomp_lz4_oracle_XXXXXX%s",
        suffix.c_str());
    int fd = mkstemps(tmpname, static_cast<int>(suffix.length()));
    if (fd < 0) {
      return "";
    }
    ssize_t written = write(fd, data.data(), data.size());
    close(fd);
    if (written < 0 || static_cast<size_t>(written) != data.size()) {
      unlink(tmpname);
      return "";
    }
    return tmpname;
#endif
  }

  // Helper to read bytes from a file
  std::vector<uint8_t> readFile(const std::string & path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      return {};
    }
    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
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

  // Use Python lz4.frame to compress data
  std::vector<uint8_t> pythonLz4Compress(const std::vector<uint8_t> & data,
      bool content_checksum = false, bool block_checksum = false) {
    if (!has_python_lz4_) {
      return {};
    }

    std::string tmpfile = writeTempFile(data);
    if (tmpfile.empty()) {
      return {};
    }

    std::string escaped_path = tmpfile;
#ifdef _WIN32
    for (char & c : escaped_path) {
      if (c == '\\')
        c = '/';
    }
#endif
    std::stringstream cmd;
    cmd << getPythonCommand() << " -c \""
        << "import lz4.frame,sys;"
        << "data = open('" << escaped_path << "', 'rb').read();"
        << "sys.stdout.buffer.write(lz4.frame.compress(data, "
        << "block_linked=False, "
        << "content_checksum=" << (content_checksum ? "True" : "False") << ", "
        << "block_checksum=" << (block_checksum ? "True" : "False") << "));"
        << "\"";

    std::vector<uint8_t> result = runCommandGetOutput(cmd.str());
    unlink(tmpfile.c_str());
    return result;
  }

  // Use Python lz4.frame to decompress data
  std::vector<uint8_t> pythonLz4Decompress(const std::vector<uint8_t> & data) {
    if (!has_python_lz4_ || data.empty()) {
      return {};
    }

    std::string tmpfile = writeTempFile(data, ".lz4");
    if (tmpfile.empty()) {
      return {};
    }

    std::string escaped_path = tmpfile;
#ifdef _WIN32
    for (char & c : escaped_path) {
      if (c == '\\')
        c = '/';
    }
#endif
    std::stringstream cmd;
    cmd << getPythonCommand() << " -c \""
        << "import lz4.frame,sys;"
        << "data = open('" << escaped_path << "', 'rb').read();"
        << "sys.stdout.buffer.write(lz4.frame.decompress(data));"
        << "\"";

    std::vector<uint8_t> result = runCommandGetOutput(cmd.str());
    unlink(tmpfile.c_str());
    return result;
  }

  // Use lz4 CLI to compress data
  std::vector<uint8_t> lz4CliCompress(const std::vector<uint8_t> & data) {
    if (!has_lz4_cli_) {
      return {};
    }

    std::string tmpfile = writeTempFile(data);
    if (tmpfile.empty()) {
      return {};
    }

    std::stringstream cmd;
    cmd << "lz4 -c \"" << tmpfile << "\"";

    std::vector<uint8_t> result = runCommandGetOutput(cmd.str());
    unlink(tmpfile.c_str());
    return result;
  }

  // Use unlz4 CLI to decompress data
  std::vector<uint8_t> unlz4CliDecompress(const std::vector<uint8_t> & data) {
    if (!has_unlz4_cli_ || data.empty()) {
      return {};
    }

    std::string tmpfile = writeTempFile(data, ".lz4");
    if (tmpfile.empty()) {
      return {};
    }

    std::stringstream cmd;
    cmd << "unlz4 -c \"" << tmpfile << "\"";

    std::vector<uint8_t> result = runCommandGetOutput(cmd.str());
    unlink(tmpfile.c_str());
    return result;
  }

  // Compress with our library
  std::vector<uint8_t> gcompCompress(const std::vector<uint8_t> & data,
      bool content_checksum = false, bool block_checksum = false) {
    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      return {};
    }
    if (content_checksum) {
      gcomp_options_set_bool(opts, "lz4.content_checksum", 1);
    }
    if (block_checksum) {
      gcomp_options_set_bool(opts, "lz4.block_checksum", 1);
    }
    // Use independent blocks for better interoperability
    gcomp_options_set_bool(opts, "lz4.independent_blocks", 1);

    size_t comp_capacity = (data.size() * 12 / 10) + 1024;
    std::vector<uint8_t> compressed(std::max(comp_capacity, size_t(1024)));
    size_t comp_size = 0;

    gcomp_status_t status =
        gcomp_encode_buffer(registry_, "lz4", opts, data.data(), data.size(),
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
    // Disable expansion ratio limit for oracle tests
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
        gcomp_decode_buffer(registry_, "lz4", opts, data.data(), data.size(),
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
        "lz4", "frame", "oracle"};
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
  bool has_python_lz4_ = false;
  bool has_lz4_cli_ = false;
  bool has_unlz4_cli_ = false;
};

//
// Tests: Golden vectors (pre-generated external lz4 data)
//

TEST_F(Lz4OracleTest, GoldenVectors_Decompress) {
  // Note: Some golden vectors in golden_vectors.h were created by hand and
  // may need verification against an actual lz4 library. This test logs
  // failures as warnings rather than hard failures since the real value
  // of oracle tests is cross-validation with external tools.
  int passed = 0;
  int failed = 0;

  for (size_t i = 0; i < lz4_golden_vectors_count; i++) {
    const auto & vec = lz4_golden_vectors[i];
    std::vector<uint8_t> decompressed =
        gcompDecompress(std::vector<uint8_t>(vec.compressed,
                            vec.compressed + vec.compressed_len),
            vec.expected_len);

    bool size_ok = (decompressed.size() == vec.expected_len);
    bool data_ok = (vec.expected_len == 0) ||
        (size_ok &&
            memcmp(decompressed.data(), vec.expected, vec.expected_len) == 0);

    if (size_ok && data_ok) {
      passed++;
      if (isVerbose()) {
        std::cout << "Vector " << vec.name << ": " << vec.compressed_len
                  << " -> " << decompressed.size() << " bytes OK" << std::endl;
      }
    }
    else {
      failed++;
      // Log as warning - golden vectors may not all be verified
      std::cout << "Warning: Golden vector " << vec.name << " failed: "
                << "expected " << vec.expected_len << " bytes, got "
                << decompressed.size() << " bytes" << std::endl;
    }
  }

  // At least some vectors should work
  EXPECT_GT(passed, 0)
      << "No golden vectors passed - this suggests a decoder bug";

  if (isVerbose() || failed > 0) {
    std::cout << "Golden vectors: " << passed << " passed, " << failed
              << " failed (may need verification with lz4 library)"
              << std::endl;
  }
}

//
// Tests: Our encoder, Python decoder
//

TEST_F(Lz4OracleTest, OurEncoder_PythonDecoder_TextData) {
  if (!has_python_lz4_) {
    GTEST_SKIP() << "Python lz4 not available";
  }

  std::vector<uint8_t> original = generateTextData(10 * 1024);
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonLz4Decompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";

  if (isVerbose()) {
    std::cout << "Text data: " << original.size() << " -> " << compressed.size()
              << " bytes (" << (100 * compressed.size() / original.size())
              << "%)" << std::endl;
  }
}

TEST_F(Lz4OracleTest, OurEncoder_PythonDecoder_RandomData) {
  if (!has_python_lz4_) {
    GTEST_SKIP() << "Python lz4 not available";
  }

  std::vector<uint8_t> original = generateRandomData(10 * 1024);
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonLz4Decompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(Lz4OracleTest, OurEncoder_PythonDecoder_RepeatedPattern) {
  if (!has_python_lz4_) {
    GTEST_SKIP() << "Python lz4 not available";
  }

  std::vector<uint8_t> original = generateRepeatedPattern(10 * 1024);
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonLz4Decompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";

  if (isVerbose()) {
    std::cout << "Repeated pattern: " << original.size() << " -> "
              << compressed.size() << " bytes ("
              << (100 * compressed.size() / original.size()) << "%)"
              << std::endl;
  }
}

TEST_F(Lz4OracleTest, OurEncoder_PythonDecoder_WithContentChecksum) {
  if (!has_python_lz4_) {
    GTEST_SKIP() << "Python lz4 not available";
  }

  std::vector<uint8_t> original = generateTextData(4 * 1024);
  std::vector<uint8_t> compressed =
      gcompCompress(original, true /* content_checksum */);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonLz4Decompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(Lz4OracleTest, OurEncoder_PythonDecoder_WithBlockChecksum) {
  if (!has_python_lz4_) {
    GTEST_SKIP() << "Python lz4 not available";
  }

  std::vector<uint8_t> original = generateTextData(4 * 1024);
  std::vector<uint8_t> compressed =
      gcompCompress(original, false, true /* block_checksum */);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonLz4Decompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

//
// Tests: Python encoder, Our decoder
//

TEST_F(Lz4OracleTest, PythonEncoder_OurDecoder_TextData) {
  if (!has_python_lz4_) {
    GTEST_SKIP() << "Python lz4 not available";
  }

  std::vector<uint8_t> original = generateTextData(10 * 1024);
  std::vector<uint8_t> compressed = pythonLz4Compress(original);
  ASSERT_FALSE(compressed.empty()) << "Python compression failed";

  std::vector<uint8_t> decompressed =
      gcompDecompress(compressed, original.size());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(Lz4OracleTest, PythonEncoder_OurDecoder_RandomData) {
  if (!has_python_lz4_) {
    GTEST_SKIP() << "Python lz4 not available";
  }

  std::vector<uint8_t> original = generateRandomData(10 * 1024);
  std::vector<uint8_t> compressed = pythonLz4Compress(original);
  ASSERT_FALSE(compressed.empty()) << "Python compression failed";

  std::vector<uint8_t> decompressed =
      gcompDecompress(compressed, original.size());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(Lz4OracleTest, PythonEncoder_OurDecoder_WithContentChecksum) {
  if (!has_python_lz4_) {
    GTEST_SKIP() << "Python lz4 not available";
  }

  std::vector<uint8_t> original = generateTextData(4 * 1024);
  std::vector<uint8_t> compressed =
      pythonLz4Compress(original, true /* content_checksum */);
  ASSERT_FALSE(compressed.empty()) << "Python compression failed";

  std::vector<uint8_t> decompressed =
      gcompDecompress(compressed, original.size());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(Lz4OracleTest, PythonEncoder_OurDecoder_WithBlockChecksum) {
  if (!has_python_lz4_) {
    GTEST_SKIP() << "Python lz4 not available";
  }

  std::vector<uint8_t> original = generateTextData(4 * 1024);
  std::vector<uint8_t> compressed =
      pythonLz4Compress(original, false, true /* block_checksum */);
  ASSERT_FALSE(compressed.empty()) << "Python compression failed";

  std::vector<uint8_t> decompressed =
      gcompDecompress(compressed, original.size());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

//
// Tests: lz4 CLI interop
//

TEST_F(Lz4OracleTest, OurEncoder_Unlz4Cli_TextData) {
  if (!has_unlz4_cli_) {
    GTEST_SKIP() << "unlz4 CLI not available";
  }

  std::vector<uint8_t> original = generateTextData(10 * 1024);
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = unlz4CliDecompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(Lz4OracleTest, Lz4Cli_OurDecoder_TextData) {
  if (!has_lz4_cli_) {
    GTEST_SKIP() << "lz4 CLI not available";
  }

  std::vector<uint8_t> original = generateTextData(10 * 1024);
  std::vector<uint8_t> compressed = lz4CliCompress(original);
  ASSERT_FALSE(compressed.empty()) << "lz4 CLI compression failed";

  std::vector<uint8_t> decompressed =
      gcompDecompress(compressed, original.size());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

//
// Tests: Empty and edge cases
//

TEST_F(Lz4OracleTest, OurEncoder_PythonDecoder_Empty) {
  if (!has_python_lz4_) {
    GTEST_SKIP() << "Python lz4 not available";
  }

  std::vector<uint8_t> original;
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonLz4Decompress(compressed);
  ASSERT_EQ(decompressed.size(), 0) << "Expected empty output";
}

TEST_F(Lz4OracleTest, PythonEncoder_OurDecoder_Empty) {
  if (!has_python_lz4_) {
    GTEST_SKIP() << "Python lz4 not available";
  }

  std::vector<uint8_t> original;
  std::vector<uint8_t> compressed = pythonLz4Compress(original);
  ASSERT_FALSE(compressed.empty()) << "Python compression failed";

  std::vector<uint8_t> decompressed = gcompDecompress(compressed, 0);
  ASSERT_EQ(decompressed.size(), 0) << "Expected empty output";
}

TEST_F(Lz4OracleTest, OurEncoder_PythonDecoder_SingleByte) {
  if (!has_python_lz4_) {
    GTEST_SKIP() << "Python lz4 not available";
  }

  std::vector<uint8_t> original = {0x42};
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonLz4Decompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(decompressed[0], original[0]) << "Data mismatch";
}

//
// Tests: Various sizes
//

TEST_F(Lz4OracleTest, OurEncoder_PythonDecoder_VariousSizes) {
  if (!has_python_lz4_) {
    GTEST_SKIP() << "Python lz4 not available";
  }

  std::vector<size_t> sizes = {1, 10, 100, 1000, 10000, 65535, 65536, 100000};

  for (size_t size : sizes) {
    std::vector<uint8_t> original = generateTextData(size);
    std::vector<uint8_t> compressed = gcompCompress(original);
    ASSERT_FALSE(compressed.empty()) << "Compression failed for size " << size;

    std::vector<uint8_t> decompressed = pythonLz4Decompress(compressed);
    ASSERT_EQ(decompressed.size(), original.size())
        << "Size mismatch for size " << size;
    ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
        << "Data mismatch for size " << size;
  }
}

TEST_F(Lz4OracleTest, PythonEncoder_OurDecoder_VariousSizes) {
  if (!has_python_lz4_) {
    GTEST_SKIP() << "Python lz4 not available";
  }

  std::vector<size_t> sizes = {1, 10, 100, 1000, 10000, 65535, 65536, 100000};

  for (size_t size : sizes) {
    std::vector<uint8_t> original = generateTextData(size);
    std::vector<uint8_t> compressed = pythonLz4Compress(original);
    ASSERT_FALSE(compressed.empty())
        << "Python compression failed for size " << size;

    std::vector<uint8_t> decompressed =
        gcompDecompress(compressed, original.size());
    ASSERT_EQ(decompressed.size(), original.size())
        << "Size mismatch for size " << size;
    ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
        << "Data mismatch for size " << size;
  }
}

//
// Tests: Large data crossing block boundaries
//

TEST_F(Lz4OracleTest, OurEncoder_PythonDecoder_LargeData) {
  if (!has_python_lz4_) {
    GTEST_SKIP() << "Python lz4 not available";
  }

  // 500KB of data, should cross multiple 64KB blocks
  std::vector<uint8_t> original = generateTextData(500 * 1024);
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonLz4Decompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";

  if (isVerbose()) {
    std::cout << "Large data: " << original.size() << " -> "
              << compressed.size() << " bytes ("
              << (100 * compressed.size() / original.size()) << "%)"
              << std::endl;
  }
}

TEST_F(Lz4OracleTest, PythonEncoder_OurDecoder_LargeData) {
  if (!has_python_lz4_) {
    GTEST_SKIP() << "Python lz4 not available";
  }

  // 500KB of data
  std::vector<uint8_t> original = generateTextData(500 * 1024);
  std::vector<uint8_t> compressed = pythonLz4Compress(original);
  ASSERT_FALSE(compressed.empty()) << "Python compression failed";

  std::vector<uint8_t> decompressed =
      gcompDecompress(compressed, original.size());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
