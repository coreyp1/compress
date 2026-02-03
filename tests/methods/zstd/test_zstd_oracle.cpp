/**
 * @file test_zstd_oracle.cpp
 *
 * Cross-tool validation ("oracle") tests for the zstd method.
 *
 * These tests compare our zstd implementation against external tools
 * (Python zstandard module, system zstd CLI) to verify correctness.
 * Tests are skipped gracefully when external tools are not available.
 *
 * Environment variables:
 *   GCOMP_SKIP_ORACLE_TESTS - Set to "1" to skip all oracle tests
 *   GCOMP_ORACLE_VERBOSE - Set to "1" for verbose output
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../common/test_helpers.h"
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
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/zstd.h>
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

// Check if Python 3 with zstandard is available
static bool hasPythonZstd() {
  std::string cmd =
      std::string(getPythonCommand()) + " -c \"import zstandard\"";
#ifdef _WIN32
  cmd += " >NUL 2>&1";
#else
  cmd += " >/dev/null 2>&1";
#endif
  return system(cmd.c_str()) == 0;
}

// Check if zstd CLI is available
static bool hasZstdCli() {
#ifdef _WIN32
  int result = system("zstd --version >NUL 2>&1");
#else
  int result = system("zstd --version >/dev/null 2>&1");
#endif
  return result == 0;
}

class ZstdOracleTest : public ::testing::Test {
protected:
  void SetUp() override {
    if (shouldSkipOracleTests()) {
      GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
    }

    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);

    has_python_zstd_ = hasPythonZstd();
    has_zstd_cli_ = hasZstdCli();

    if (isVerbose()) {
      std::cout << "Python zstandard available: "
                << (has_python_zstd_ ? "yes" : "no") << std::endl;
      std::cout << "zstd CLI available: " << (has_zstd_cli_ ? "yes" : "no")
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
    snprintf(tmpname, sizeof(tmpname), "%sgcomp_zstd_oracle_%d_%d%s", tmpdir,
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
    snprintf(tmpname, sizeof(tmpname), "/tmp/gcomp_zstd_oracle_XXXXXX%s",
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

  // Run a command (same environment as popen). Returns true if exit code is 0.
  bool runCommand(const std::string & cmd) {
    FILE * pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
      return false;
    }
    char buffer[4096];
    while (fread(buffer, 1, sizeof(buffer), pipe) > 0) {
      (void)0;
    }
    int st = pclose(pipe);
#ifdef _WIN32
    return (st == 0);
#else
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0);
#endif
  }

  // Use Python zstandard to compress data
  std::vector<uint8_t> pythonZstdCompress(
      const std::vector<uint8_t> & data, bool content_checksum = false) {
    if (!has_python_zstd_) {
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
        << "import zstandard,sys;"
        << "cctx = zstandard.ZstdCompressor(level=3, write_checksum="
        << (content_checksum ? "True" : "False") << ");"
        << "data = open('" << escaped_path << "', 'rb').read();"
        << "sys.stdout.buffer.write(cctx.compress(data));"
        << "\"";

    std::vector<uint8_t> result = runCommandGetOutput(cmd.str());
    unlink(tmpfile.c_str());
    return result;
  }

  // Use Python zstandard to decompress data
  // Uses stream_reader for frames without content size in header
  std::vector<uint8_t> pythonZstdDecompress(const std::vector<uint8_t> & data) {
    if (!has_python_zstd_ || data.empty()) {
      return {};
    }

    std::string tmpfile = writeTempFile(data, ".zst");
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
    // Use stream_reader to handle frames without content size
    std::stringstream cmd;
    cmd << getPythonCommand() << " -c \""
        << "import zstandard,sys,io;"
        << "dctx = zstandard.ZstdDecompressor();"
        << "data = open('" << escaped_path << "', 'rb').read();"
        << "reader = dctx.stream_reader(io.BytesIO(data));"
        << "sys.stdout.buffer.write(reader.read());"
        << "\"";

    std::vector<uint8_t> result = runCommandGetOutput(cmd.str());
    unlink(tmpfile.c_str());
    return result;
  }

  // Use zstd CLI to compress data
  std::vector<uint8_t> zstdCliCompress(const std::vector<uint8_t> & data) {
    if (!has_zstd_cli_) {
      return {};
    }

    std::string tmpfile = writeTempFile(data);
    if (tmpfile.empty()) {
      return {};
    }

    std::stringstream cmd;
    cmd << "zstd -c \"" << tmpfile << "\"";

    std::vector<uint8_t> result = runCommandGetOutput(cmd.str());
    unlink(tmpfile.c_str());
    return result;
  }

  // Use zstd CLI to decompress data
  std::vector<uint8_t> zstdCliDecompress(const std::vector<uint8_t> & data) {
    if (!has_zstd_cli_ || data.empty()) {
      return {};
    }

    std::string tmpfile = writeTempFile(data, ".zst");
    if (tmpfile.empty()) {
      return {};
    }

    std::stringstream cmd;
    cmd << "zstd -dc \"" << tmpfile << "\"";

    std::vector<uint8_t> result = runCommandGetOutput(cmd.str());
    unlink(tmpfile.c_str());
    return result;
  }

  // Create a raw content dictionary (>= 8 bytes). Same bytes used by our
  // encoder/decoder and written to a temp file for zstd CLI / Python.
  // Returns (dict_bytes, dict_path). Caller should unlink dict_path when done.
  std::pair<std::vector<uint8_t>, std::string> createDictionaryFromSamples() {
    const size_t raw_dict_size = 8 * 1024;
    std::vector<uint8_t> dict_content(raw_dict_size);
    test_helpers_generate_random(
        dict_content.data(), dict_content.size(), 54321u);
    std::string dictpath = writeTempFile(dict_content, ".dict");
    if (dictpath.empty()) {
      return {{}, ""};
    }
    return {dict_content, dictpath};
  }

  // Create a formatted dictionary using zstd --train so that external encoders
  // (zstd CLI, Python) write Dictionary_ID and our decoder can match.
  // Returns (dict_bytes, dict_path). Path empty on failure. Caller unlinks.
  // Requires zstd CLI. Uses --maxdict and -B so small samples suffice.
  std::pair<std::vector<uint8_t>, std::string> createFormattedDictionary() {
    if (!has_zstd_cli_) {
      return {{}, ""};
    }
    const size_t sample_size = 4096;
    const int num_samples = 6;
    std::vector<std::string> paths;
    paths.reserve(static_cast<size_t>(num_samples));
    for (int i = 0; i < num_samples; i++) {
      std::vector<uint8_t> sample(sample_size + i * 128);
      test_helpers_generate_random(
          sample.data(), sample.size(), static_cast<uint32_t>(12345 + i * 111));
      std::string p = writeTempFile(sample, ".sample");
      if (p.empty()) {
        for (const auto & x : paths)
          unlink(x.c_str());
        return {{}, ""};
      }
      paths.push_back(p);
    }
    std::string dictpath = writeTempFile(std::vector<uint8_t>(), ".zstd");
    if (dictpath.empty()) {
      for (const auto & x : paths)
        unlink(x.c_str());
      return {{}, ""};
    }
    std::stringstream cmd;
    cmd << "zstd --train --maxdict=512 -B2048";
    for (const auto & x : paths) {
      cmd << " \"" << x << "\"";
    }
    cmd << " -o \"" << dictpath << "\"";
#ifdef _WIN32
    cmd << " >NUL 2>&1";
#else
    cmd << " 2>/dev/null";
#endif
    bool ok = runCommand(cmd.str());
    for (const auto & x : paths) {
      unlink(x.c_str());
    }
    if (!ok) {
      unlink(dictpath.c_str());
      return {{}, ""};
    }
    std::vector<uint8_t> dict_bytes = readFile(dictpath);
    if (dict_bytes.empty() || dict_bytes.size() < 8) {
      unlink(dictpath.c_str());
      return {{}, ""};
    }
    return {dict_bytes, dictpath};
  }

  // Compress with our library (optional dictionary)
  std::vector<uint8_t> gcompCompressWithDict(const std::vector<uint8_t> & data,
      const std::vector<uint8_t> * dict_data = nullptr) {
    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      return {};
    }
    if (dict_data && !dict_data->empty()) {
      if (gcomp_options_set_bytes(opts, "zstd.dictionary", dict_data->data(),
              dict_data->size()) != GCOMP_OK) {
        gcomp_options_destroy(opts);
        return {};
      }
    }
    size_t comp_capacity = (data.size() * 12 / 10) + 1024;
    std::vector<uint8_t> compressed(std::max(comp_capacity, size_t(1024)));
    size_t comp_size = 0;
    gcomp_status_t status =
        gcomp_encode_buffer(registry_, "zstd", opts, data.data(), data.size(),
            compressed.data(), compressed.size(), &comp_size);
    gcomp_options_destroy(opts);
    if (status != GCOMP_OK) {
      return {};
    }
    compressed.resize(comp_size);
    return compressed;
  }

  // Decompress with our library (optional dictionary)
  std::vector<uint8_t> gcompDecompressWithDict(
      const std::vector<uint8_t> & data,
      const std::vector<uint8_t> * dict_data = nullptr,
      size_t expected_size = 0) {
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
    if (dict_data && !dict_data->empty()) {
      if (gcomp_options_set_bytes(opts, "zstd.dictionary", dict_data->data(),
              dict_data->size()) != GCOMP_OK) {
        gcomp_options_destroy(opts);
        return {};
      }
    }
    size_t decomp_capacity =
        expected_size > 0 ? expected_size + 1024 : data.size() * 100 + 1024;
    std::vector<uint8_t> decompressed(decomp_capacity);
    size_t decomp_size = 0;
    gcomp_status_t status =
        gcomp_decode_buffer(registry_, "zstd", opts, data.data(), data.size(),
            decompressed.data(), decompressed.size(), &decomp_size);
    gcomp_options_destroy(opts);
    if (status != GCOMP_OK) {
      return {};
    }
    decompressed.resize(decomp_size);
    return decompressed;
  }

  // Use zstd CLI to compress with dictionary
  std::vector<uint8_t> zstdCliCompressWithDict(
      const std::vector<uint8_t> & data, const std::string & dict_path) {
    if (!has_zstd_cli_ || dict_path.empty()) {
      return {};
    }
    std::string datafile = writeTempFile(data);
    if (datafile.empty()) {
      return {};
    }
    std::stringstream cmd;
    cmd << "zstd -D \"" << dict_path << "\" -c \"" << datafile << "\"";
    std::vector<uint8_t> result = runCommandGetOutput(cmd.str());
    unlink(datafile.c_str());
    return result;
  }

  // Use zstd CLI to decompress with dictionary
  std::vector<uint8_t> zstdCliDecompressWithDict(
      const std::vector<uint8_t> & data, const std::string & dict_path) {
    if (!has_zstd_cli_ || data.empty() || dict_path.empty()) {
      return {};
    }
    std::string tmpfile = writeTempFile(data, ".zst");
    if (tmpfile.empty()) {
      return {};
    }
    std::stringstream cmd;
    cmd << "zstd -D \"" << dict_path << "\" -dc \"" << tmpfile << "\"";
    std::vector<uint8_t> result = runCommandGetOutput(cmd.str());
    unlink(tmpfile.c_str());
    return result;
  }

  // Use Python zstandard to compress with dictionary
  std::vector<uint8_t> pythonZstdCompressWithDict(
      const std::vector<uint8_t> & data, const std::string & dict_path) {
    if (!has_python_zstd_ || dict_path.empty()) {
      return {};
    }
    std::string datafile = writeTempFile(data);
    if (datafile.empty()) {
      return {};
    }
    std::string escaped_data = datafile;
    std::string escaped_dict = dict_path;
#ifdef _WIN32
    for (char & c : escaped_data) {
      if (c == '\\')
        c = '/';
    }
    for (char & c : escaped_dict) {
      if (c == '\\')
        c = '/';
    }
#endif
    std::stringstream cmd;
    cmd << getPythonCommand() << " -c \""
        << "import zstandard,sys;"
        << "dict_data=zstandard.ZstdCompressionDict(open('" << escaped_dict
        << "','rb').read());"
        << "cctx=zstandard.ZstdCompressor(level=3,dict_data=dict_data);"
        << "data=open('" << escaped_data << "','rb').read();"
        << "sys.stdout.buffer.write(cctx.compress(data));"
        << "\"";
    std::vector<uint8_t> result = runCommandGetOutput(cmd.str());
    unlink(datafile.c_str());
    return result;
  }

  // Use Python zstandard to decompress with dictionary
  std::vector<uint8_t> pythonZstdDecompressWithDict(
      const std::vector<uint8_t> & data, const std::string & dict_path) {
    if (!has_python_zstd_ || data.empty() || dict_path.empty()) {
      return {};
    }
    std::string tmpfile = writeTempFile(data, ".zst");
    if (tmpfile.empty()) {
      return {};
    }
    std::string escaped_tmp = tmpfile;
    std::string escaped_dict = dict_path;
#ifdef _WIN32
    for (char & c : escaped_tmp) {
      if (c == '\\')
        c = '/';
    }
    for (char & c : escaped_dict) {
      if (c == '\\')
        c = '/';
    }
#endif
    std::stringstream cmd;
    cmd << getPythonCommand() << " -c \""
        << "import zstandard,sys,io;"
        << "dict_data=zstandard.ZstdCompressionDict(open('" << escaped_dict
        << "','rb').read());"
        << "dctx=zstandard.ZstdDecompressor(dict_data=dict_data);"
        << "data=open('" << escaped_tmp << "','rb').read();"
        << "reader=dctx.stream_reader(io.BytesIO(data));"
        << "sys.stdout.buffer.write(reader.read());"
        << "\"";
    std::vector<uint8_t> result = runCommandGetOutput(cmd.str());
    unlink(tmpfile.c_str());
    return result;
  }

  // Compress with our library
  std::vector<uint8_t> gcompCompress(
      const std::vector<uint8_t> & data, bool content_checksum = false) {
    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      return {};
    }
    if (content_checksum) {
      gcomp_options_set_bool(opts, "zstd.checksum", 1);
    }

    size_t comp_capacity = (data.size() * 12 / 10) + 1024;
    std::vector<uint8_t> compressed(std::max(comp_capacity, size_t(1024)));
    size_t comp_size = 0;

    gcomp_status_t status =
        gcomp_encode_buffer(registry_, "zstd", opts, data.data(), data.size(),
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
        gcomp_decode_buffer(registry_, "zstd", opts, data.data(), data.size(),
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
        "zstd", "frame", "oracle"};
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
  bool has_python_zstd_ = false;
  bool has_zstd_cli_ = false;
};

//
// Tests: Our encoder, Python decoder
//

TEST_F(ZstdOracleTest, OurEncoder_PythonDecoder_TextData) {
  if (!has_python_zstd_) {
    GTEST_SKIP() << "Python zstandard not available";
  }

  std::vector<uint8_t> original = generateTextData(10 * 1024);
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonZstdDecompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";

  if (isVerbose()) {
    std::cout << "Text data: " << original.size() << " -> " << compressed.size()
              << " bytes (" << (100 * compressed.size() / original.size())
              << "%)" << std::endl;
  }
}

TEST_F(ZstdOracleTest, OurEncoder_PythonDecoder_RandomData) {
  if (!has_python_zstd_) {
    GTEST_SKIP() << "Python zstandard not available";
  }

  std::vector<uint8_t> original = generateRandomData(10 * 1024);
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonZstdDecompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(ZstdOracleTest, OurEncoder_PythonDecoder_RepeatedPattern) {
  if (!has_python_zstd_) {
    GTEST_SKIP() << "Python zstandard not available";
  }

  std::vector<uint8_t> original = generateRepeatedPattern(10 * 1024);
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonZstdDecompress(compressed);
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

TEST_F(ZstdOracleTest, OurEncoder_PythonDecoder_WithChecksum) {
  if (!has_python_zstd_) {
    GTEST_SKIP() << "Python zstandard not available";
  }

  std::vector<uint8_t> original = generateTextData(4 * 1024);
  std::vector<uint8_t> compressed =
      gcompCompress(original, true /* content_checksum */);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonZstdDecompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

//
// Tests: Python encoder, Our decoder
//

TEST_F(ZstdOracleTest, PythonEncoder_OurDecoder_TextData) {
  if (!has_python_zstd_) {
    GTEST_SKIP() << "Python zstandard not available";
  }

  std::vector<uint8_t> original = generateTextData(10 * 1024);
  std::vector<uint8_t> compressed = pythonZstdCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Python compression failed";

  std::vector<uint8_t> decompressed =
      gcompDecompress(compressed, original.size());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(ZstdOracleTest, PythonEncoder_OurDecoder_RandomData) {
  if (!has_python_zstd_) {
    GTEST_SKIP() << "Python zstandard not available";
  }

  std::vector<uint8_t> original = generateRandomData(10 * 1024);
  std::vector<uint8_t> compressed = pythonZstdCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Python compression failed";

  std::vector<uint8_t> decompressed =
      gcompDecompress(compressed, original.size());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(ZstdOracleTest, PythonEncoder_OurDecoder_WithChecksum) {
  if (!has_python_zstd_) {
    GTEST_SKIP() << "Python zstandard not available";
  }

  std::vector<uint8_t> original = generateTextData(4 * 1024);
  std::vector<uint8_t> compressed =
      pythonZstdCompress(original, true /* content_checksum */);
  ASSERT_FALSE(compressed.empty()) << "Python compression failed";

  std::vector<uint8_t> decompressed =
      gcompDecompress(compressed, original.size());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

//
// Tests: zstd CLI interop
//

TEST_F(ZstdOracleTest, OurEncoder_ZstdCli_TextData) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  std::vector<uint8_t> original = generateTextData(10 * 1024);
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = zstdCliDecompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_TextData) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  std::vector<uint8_t> original = generateTextData(10 * 1024);
  std::vector<uint8_t> compressed = zstdCliCompress(original);
  ASSERT_FALSE(compressed.empty()) << "zstd CLI compression failed";

  std::vector<uint8_t> decompressed =
      gcompDecompress(compressed, original.size());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

//
// Tests: Empty and edge cases
//

TEST_F(ZstdOracleTest, OurEncoder_PythonDecoder_Empty) {
  if (!has_python_zstd_) {
    GTEST_SKIP() << "Python zstandard not available";
  }

  std::vector<uint8_t> original;
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonZstdDecompress(compressed);
  ASSERT_EQ(decompressed.size(), 0) << "Expected empty output";
}

TEST_F(ZstdOracleTest, PythonEncoder_OurDecoder_Empty) {
  if (!has_python_zstd_) {
    GTEST_SKIP() << "Python zstandard not available";
  }

  std::vector<uint8_t> original;
  std::vector<uint8_t> compressed = pythonZstdCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Python compression failed";

  std::vector<uint8_t> decompressed = gcompDecompress(compressed, 0);
  ASSERT_EQ(decompressed.size(), 0) << "Expected empty output";
}

TEST_F(ZstdOracleTest, OurEncoder_PythonDecoder_SingleByte) {
  if (!has_python_zstd_) {
    GTEST_SKIP() << "Python zstandard not available";
  }

  std::vector<uint8_t> original = {0x42};
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonZstdDecompress(compressed);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(decompressed[0], original[0]) << "Data mismatch";
}

//
// Tests: Various sizes
//

TEST_F(ZstdOracleTest, OurEncoder_PythonDecoder_VariousSizes) {
  if (!has_python_zstd_) {
    GTEST_SKIP() << "Python zstandard not available";
  }

  std::vector<size_t> sizes = {1, 10, 100, 1000, 10000, 65535, 65536, 100000};

  for (size_t size : sizes) {
    std::vector<uint8_t> original = generateTextData(size);
    std::vector<uint8_t> compressed = gcompCompress(original);
    ASSERT_FALSE(compressed.empty()) << "Compression failed for size " << size;

    std::vector<uint8_t> decompressed = pythonZstdDecompress(compressed);
    ASSERT_EQ(decompressed.size(), original.size())
        << "Size mismatch for size " << size;
    ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
        << "Data mismatch for size " << size;
  }
}

TEST_F(ZstdOracleTest, PythonEncoder_OurDecoder_VariousSizes) {
  if (!has_python_zstd_) {
    GTEST_SKIP() << "Python zstandard not available";
  }

  std::vector<size_t> sizes = {1, 10, 100, 1000, 10000, 65535, 65536, 100000};

  for (size_t size : sizes) {
    std::vector<uint8_t> original = generateTextData(size);
    std::vector<uint8_t> compressed = pythonZstdCompress(original);
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
// Tests: Large data
//

TEST_F(ZstdOracleTest, OurEncoder_PythonDecoder_LargeData) {
  if (!has_python_zstd_) {
    GTEST_SKIP() << "Python zstandard not available";
  }

  // 500KB of data
  std::vector<uint8_t> original = generateTextData(500 * 1024);
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  std::vector<uint8_t> decompressed = pythonZstdDecompress(compressed);
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

TEST_F(ZstdOracleTest, PythonEncoder_OurDecoder_LargeData) {
  if (!has_python_zstd_) {
    GTEST_SKIP() << "Python zstandard not available";
  }

  // 500KB of data
  std::vector<uint8_t> original = generateTextData(500 * 1024);
  std::vector<uint8_t> compressed = pythonZstdCompress(original);
  ASSERT_FALSE(compressed.empty()) << "Python compression failed";

  std::vector<uint8_t> decompressed =
      gcompDecompress(compressed, original.size());
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";
}

//
// Tests: Dictionary compression (oracle)
//

TEST_F(ZstdOracleTest, OurEncoder_ZstdCli_WithDictionary) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  auto [dict_bytes, dict_path] = createDictionaryFromSamples();
  if (dict_bytes.empty() || dict_path.empty()) {
    GTEST_SKIP() << "Could not create dictionary";
  }

  std::vector<uint8_t> original = generateTextData(4 * 1024);
  std::vector<uint8_t> compressed =
      gcompCompressWithDict(original, &dict_bytes);
  ASSERT_FALSE(compressed.empty()) << "Our compression with dict failed";

  std::vector<uint8_t> decompressed =
      zstdCliDecompressWithDict(compressed, dict_path);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";

  unlink(dict_path.c_str());
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_WithDictionary) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  auto [dict_bytes, dict_path] = createDictionaryFromSamples();
  if (dict_bytes.empty() || dict_path.empty()) {
    GTEST_SKIP() << "Could not create dictionary";
  }

  std::vector<uint8_t> original = generateTextData(4 * 1024);
  std::vector<uint8_t> compressed =
      zstdCliCompressWithDict(original, dict_path);
  ASSERT_FALSE(compressed.empty()) << "zstd CLI compression with dict failed";

  std::vector<uint8_t> decompressed =
      gcompDecompressWithDict(compressed, &dict_bytes, original.size());
  ASSERT_FALSE(decompressed.empty()) << "Our decoder failed to decode";
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";

  unlink(dict_path.c_str());
}

TEST_F(ZstdOracleTest, OurEncoder_PythonDecoder_WithDictionary) {
  if (!has_python_zstd_) {
    GTEST_SKIP() << "Python zstandard not available";
  }

  auto [dict_bytes, dict_path] = createDictionaryFromSamples();
  if (dict_bytes.empty() || dict_path.empty()) {
    GTEST_SKIP() << "Could not create dictionary";
  }

  std::vector<uint8_t> original = generateTextData(4 * 1024);
  std::vector<uint8_t> compressed =
      gcompCompressWithDict(original, &dict_bytes);
  ASSERT_FALSE(compressed.empty()) << "Our compression with dict failed";

  std::vector<uint8_t> decompressed =
      pythonZstdDecompressWithDict(compressed, dict_path);
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";

  unlink(dict_path.c_str());
}

TEST_F(ZstdOracleTest, PythonEncoder_OurDecoder_WithDictionary) {
  if (!has_python_zstd_) {
    GTEST_SKIP() << "Python zstandard not available";
  }

  auto [dict_bytes, dict_path] = createDictionaryFromSamples();
  if (dict_bytes.empty() || dict_path.empty()) {
    GTEST_SKIP() << "Could not create dictionary";
  }

  std::vector<uint8_t> original = generateTextData(4 * 1024);
  std::vector<uint8_t> compressed =
      pythonZstdCompressWithDict(original, dict_path);
  ASSERT_FALSE(compressed.empty()) << "Python compression with dict failed";

  std::vector<uint8_t> decompressed =
      gcompDecompressWithDict(compressed, &dict_bytes, original.size());
  ASSERT_FALSE(decompressed.empty()) << "Our decoder failed to decode";
  ASSERT_EQ(decompressed.size(), original.size()) << "Size mismatch";
  ASSERT_EQ(memcmp(decompressed.data(), original.data(), original.size()), 0)
      << "Data mismatch";

  unlink(dict_path.c_str());
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
