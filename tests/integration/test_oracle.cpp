/**
 * @file test_oracle.cpp
 *
 * Cross-tool validation ("oracle") tests for the Ghoti.io Compress library.
 *
 * These tests compare our deflate implementation against external tools
 * (Python zlib, system gzip) to verify correctness. Tests are skipped
 * gracefully when external tools are not available.
 *
 * Environment variables:
 *   GCOMP_SKIP_ORACLE_TESTS - Set to "1" to skip all oracle tests
 *   GCOMP_ORACLE_VERBOSE - Set to "1" for verbose output
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
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
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
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
  // On Windows, check if python3 exists, otherwise fall back to python
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

// Check if Python 3 with zlib is available
static bool hasPythonZlib() {
  std::string cmd = std::string(getPythonCommand()) + " -c \"import zlib\"";
#ifdef _WIN32
  cmd += " >NUL 2>&1";
#else
  cmd += " >/dev/null 2>&1";
#endif
  return system(cmd.c_str()) == 0;
}

// Check if gzip is available
static bool hasGzip() {
#ifdef _WIN32
  int result = system("gzip --version >NUL 2>&1");
#else
  int result = system("gzip --version >/dev/null 2>&1");
#endif
  return result == 0;
}

class OracleTest : public ::testing::Test {
protected:
  void SetUp() override {
    if (shouldSkipOracleTests()) {
      GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
    }

    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);

    has_python_zlib_ = hasPythonZlib();
    has_gzip_ = hasGzip();

    if (isVerbose()) {
      std::cout << "Python zlib available: "
                << (has_python_zlib_ ? "yes" : "no") << std::endl;
      std::cout << "gzip available: " << (has_gzip_ ? "yes" : "no")
                << std::endl;
    }
  }

  // Helper to write bytes to a temporary file
  std::string writeTempFile(
      const std::vector<uint8_t> & data, const std::string & suffix = ".bin") {
#ifdef _WIN32
    // Windows implementation
    char tmpdir[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tmpdir) == 0) {
      return "";
    }
    char tmpname[MAX_PATH];
    // Generate a unique filename using process ID and a counter
    static int counter = 0;
    snprintf(tmpname, sizeof(tmpname), "%sgcomp_oracle_%d_%d%s", tmpdir,
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
    // POSIX implementation
    char tmpname[256];
    snprintf(
        tmpname, sizeof(tmpname), "/tmp/gcomp_oracle_XXXXXX%s", suffix.c_str());
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
    // Windows needs binary mode for popen
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

  // Use Python zlib to compress data (returns raw deflate stream)
  std::vector<uint8_t> pythonZlibCompress(
      const std::vector<uint8_t> & data, int level = 6) {
    if (!has_python_zlib_) {
      return {};
    }

    std::string tmpfile = writeTempFile(data);
    if (tmpfile.empty()) {
      return {};
    }

    // Python script to compress and output raw deflate
    // Use escaped quotes and forward slashes for cross-platform compatibility
    std::string escaped_path = tmpfile;
#ifdef _WIN32
    // Convert backslashes to forward slashes for Python
    for (char & c : escaped_path) {
      if (c == '\\')
        c = '/';
    }
#endif
    std::stringstream cmd;
    cmd << getPythonCommand() << " -c \""
        << "import zlib,sys;"
        << "data = open('" << escaped_path << "', 'rb').read();"
        << "comp = zlib.compressobj(" << level << ", zlib.DEFLATED, -15);"
        << "sys.stdout.buffer.write(comp.compress(data) + comp.flush());"
        << "\"";

    std::vector<uint8_t> result = runCommandGetOutput(cmd.str());
    unlink(tmpfile.c_str());
    return result;
  }

  // Use Python zlib to decompress data (expects raw deflate stream)
  std::vector<uint8_t> pythonZlibDecompress(const std::vector<uint8_t> & data) {
    if (!has_python_zlib_ || data.empty()) {
      return {};
    }

    std::string tmpfile = writeTempFile(data);
    if (tmpfile.empty()) {
      return {};
    }

    // Python script to decompress raw deflate
    // Use escaped quotes and forward slashes for cross-platform compatibility
    std::string escaped_path = tmpfile;
#ifdef _WIN32
    // Convert backslashes to forward slashes for Python
    for (char & c : escaped_path) {
      if (c == '\\')
        c = '/';
    }
#endif
    std::stringstream cmd;
    cmd << getPythonCommand() << " -c \""
        << "import zlib,sys;"
        << "data = open('" << escaped_path << "', 'rb').read();"
        << "decomp = zlib.decompressobj(-15);"
        << "sys.stdout.buffer.write(decomp.decompress(data));"
        << "\"";

    std::vector<uint8_t> result = runCommandGetOutput(cmd.str());
    unlink(tmpfile.c_str());
    return result;
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
        gcomp_encode_buffer(registry_, "deflate", opts, data.data(),
            data.size(), compressed.data(), compressed.size(), &comp_size);
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

    // Disable expansion ratio limit for oracle tests
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

    gcomp_status_t status = gcomp_decode_buffer(registry_, "deflate", opts,
        data.data(), data.size(), decompressed.data(), decompressed.size(),
        &decomp_size);
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
        "deflate", "zlib", "oracle"};
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

  std::vector<uint8_t> generateHighEntropy(size_t size) {
    // Use a better random source for high entropy
    std::vector<uint8_t> data(size);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    for (size_t i = 0; i < size; i++) {
      data[i] = static_cast<uint8_t>(dis(gen));
    }
    return data;
  }

  gcomp_registry_t * registry_ = nullptr;
  bool has_python_zlib_ = false;
  bool has_gzip_ = false;
};

//
// Tests: Our encoder, Python decoder
// Verifies our compressed output can be decompressed by Python zlib
//

TEST_F(OracleTest, OurEncoder_PythonDecoder_TextData) {
  if (!has_python_zlib_) {
    GTEST_SKIP() << "Python zlib not available";
  }

  std::vector<uint8_t> original = generateTextData(10 * 1024);
  std::vector<uint8_t> compressed = gcompCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonZlibDecompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";

  if (isVerbose()) {
    std::cout << "Text data: " << original.size() << " -> " << compressed.size()
              << " bytes (" << (100 * compressed.size() / original.size())
              << "%)" << std::endl;
  }
}

TEST_F(OracleTest, OurEncoder_PythonDecoder_RandomData) {
  if (!has_python_zlib_) {
    GTEST_SKIP() << "Python zlib not available";
  }

  std::vector<uint8_t> original = generateRandomData(10 * 1024);
  std::vector<uint8_t> compressed = gcompCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonZlibDecompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(OracleTest, OurEncoder_PythonDecoder_RepeatedPattern) {
  if (!has_python_zlib_) {
    GTEST_SKIP() << "Python zlib not available";
  }

  std::vector<uint8_t> original = generateRepeatedPattern(10 * 1024);
  std::vector<uint8_t> compressed = gcompCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonZlibDecompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(OracleTest, OurEncoder_PythonDecoder_HighEntropy) {
  if (!has_python_zlib_) {
    GTEST_SKIP() << "Python zlib not available";
  }

  std::vector<uint8_t> original = generateHighEntropy(10 * 1024);
  std::vector<uint8_t> compressed =
      gcompCompress(original, 1); // Fast for high entropy
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonZlibDecompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(OracleTest, OurEncoder_PythonDecoder_AllLevels) {
  if (!has_python_zlib_) {
    GTEST_SKIP() << "Python zlib not available";
  }

  std::vector<uint8_t> original = generateTextData(4 * 1024);

  for (int level = 0; level <= 9; level++) {
    std::vector<uint8_t> compressed = gcompCompress(original, level);
    ASSERT_FALSE(compressed.empty()) << "Compression failed at level " << level;

    std::vector<uint8_t> decompressed = pythonZlibDecompress(compressed);
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
// Verifies we can decompress Python zlib's output
//

TEST_F(OracleTest, PythonEncoder_OurDecoder_TextData) {
  if (!has_python_zlib_) {
    GTEST_SKIP() << "Python zlib not available";
  }

  std::vector<uint8_t> original = generateTextData(10 * 1024);
  std::vector<uint8_t> compressed = pythonZlibCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "Python compression failed";

  std::vector<uint8_t> decompressed =
      gcompDecompress(compressed, original.size());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(OracleTest, PythonEncoder_OurDecoder_RandomData) {
  if (!has_python_zlib_) {
    GTEST_SKIP() << "Python zlib not available";
  }

  std::vector<uint8_t> original = generateRandomData(10 * 1024);
  std::vector<uint8_t> compressed = pythonZlibCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "Python compression failed";

  std::vector<uint8_t> decompressed =
      gcompDecompress(compressed, original.size());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(OracleTest, PythonEncoder_OurDecoder_RepeatedPattern) {
  if (!has_python_zlib_) {
    GTEST_SKIP() << "Python zlib not available";
  }

  std::vector<uint8_t> original = generateRepeatedPattern(10 * 1024);
  std::vector<uint8_t> compressed = pythonZlibCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "Python compression failed";

  std::vector<uint8_t> decompressed =
      gcompDecompress(compressed, original.size());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(OracleTest, PythonEncoder_OurDecoder_AllLevels) {
  if (!has_python_zlib_) {
    GTEST_SKIP() << "Python zlib not available";
  }

  std::vector<uint8_t> original = generateTextData(4 * 1024);

  for (int level = 0; level <= 9; level++) {
    std::vector<uint8_t> compressed = pythonZlibCompress(original, level);
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
// Tests: Empty and edge cases
//

TEST_F(OracleTest, OurEncoder_PythonDecoder_Empty) {
  if (!has_python_zlib_) {
    GTEST_SKIP() << "Python zlib not available";
  }

  std::vector<uint8_t> original;
  std::vector<uint8_t> compressed = gcompCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonZlibDecompress(compressed);
  ASSERT_EQ(decompressed.size(), 0) << "Expected empty output";
}

TEST_F(OracleTest, PythonEncoder_OurDecoder_Empty) {
  if (!has_python_zlib_) {
    GTEST_SKIP() << "Python zlib not available";
  }

  std::vector<uint8_t> original;
  std::vector<uint8_t> compressed = pythonZlibCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "Python compression failed";

  std::vector<uint8_t> decompressed = gcompDecompress(compressed, 0);
  ASSERT_EQ(decompressed.size(), 0) << "Expected empty output";
}

TEST_F(OracleTest, OurEncoder_PythonDecoder_SingleByte) {
  if (!has_python_zlib_) {
    GTEST_SKIP() << "Python zlib not available";
  }

  std::vector<uint8_t> original = {0x42};
  std::vector<uint8_t> compressed = gcompCompress(original, 6);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonZlibDecompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(decompressed[0], original[0]) << "Data mismatch";
}

//
// Tests: Various sizes
//

TEST_F(OracleTest, OurEncoder_PythonDecoder_VariousSizes) {
  if (!has_python_zlib_) {
    GTEST_SKIP() << "Python zlib not available";
  }

  std::vector<size_t> sizes = {1, 10, 100, 1000, 10000, 65535, 65536, 100000};

  for (size_t size : sizes) {
    std::vector<uint8_t> original = generateTextData(size);
    std::vector<uint8_t> compressed = gcompCompress(original, 6);
    ASSERT_FALSE(compressed.empty()) << "Compression failed for size " << size;

    std::vector<uint8_t> decompressed = pythonZlibDecompress(compressed);
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
