/**
 * @file test_zstd_oracle.cpp
 *
 * Cross-tool validation ("oracle") tests for the zstd method.
 *
 * This file used to carry a second set of tests driven through the Python
 * `zstandard` module.  Sixteen of the twenty tests here needed it, and where
 * it is not installed -- which is usual -- they skipped, leaving a summary
 * line that read PASSED over four tests that between them never exercised
 * Huffman-coded literals, FSE-coded sequence tables, or treeless blocks.  All
 * three were broken.
 *
 * They were removed rather than fixed: the zstd CLI covers the same ground,
 * is far more commonly present, and is what the tests below use.  Short
 * inputs were the only thing the Python tests reached that nothing else did,
 * and that moved into *_EveryShortLength.
 *
 * These tests compare our zstd implementation against external tools
 * (the system zstd CLI) to verify correctness.
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

    has_zstd_cli_ = hasZstdCli();

    if (isVerbose()) {
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


  // Uses stream_reader for frames without content size in header

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
  // encoder/decoder and written to a temp file for the zstd CLI.
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
  // (the zstd CLI) writes Dictionary_ID and our decoder can match.
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
  // Same as zstdCliCompress(), but with explicit flags so a test can ask the
  // reference encoder for a specific level or frame shape.
  std::vector<uint8_t> zstdCliCompressWith(
      const std::vector<uint8_t> & data, const std::string & flags) {
    if (!has_zstd_cli_) {
      return {};
    }

    std::string tmpfile = writeTempFile(data);
    if (tmpfile.empty()) {
      return {};
    }

    std::stringstream cmd;
    cmd << "zstd " << flags << " -c \"" << tmpfile << "\"";

    std::vector<uint8_t> result = runCommandGetOutput(cmd.str());
    unlink(tmpfile.c_str());
    return result;
  }

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
  bool has_zstd_cli_ = false;
};

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

//
// Tests: Various sizes
//

//
// Tests: Large data
//

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

//
// Tests: paths that only a real zstd stream reaches
//
// The oracle tests above compress generateTextData(), an eight-word list
// repeated to fill the buffer.  That is compressible enough that the
// reference encoder spends almost all of it on sequences and leaves the
// literals STORED -- so the single test that fed real zstd output to our
// decoder never exercised Huffman-coded literals, the FSE-compressed
// Huffman tree description, treeless blocks, or FSE-coded sequence tables.
// Every one of those was broken, and the suite was green.
//
// The tests below assert first that the stream actually contains the
// construct under test, and only then that we decode it.  Without that
// assertion a test like this silently stops testing anything the day the
// reference encoder changes its mind about a heuristic.
//

// What a frame actually contains, by walking its block headers.
struct ZstdFrameShape {
  int blocks = 0;
  int raw_blocks = 0;
  int rle_blocks = 0;
  int compressed_blocks = 0;
  int literals_raw = 0;
  int literals_rle = 0;
  int literals_huffman = 0;  // Compressed_Literals_Block (new tree)
  int literals_treeless = 0; // Treeless_Literals_Block (reuses previous tree)
  bool parsed = false;
};

// Walk a zstd frame far enough to classify each block and, for compressed
// blocks, the literals section type.  RFC 8878 sections 3.1.1.1 (frame
// header), 3.1.1.2 (block header) and 3.1.1.3.1.1 (literals header).
static ZstdFrameShape inspectZstdFrame(const std::vector<uint8_t> & f) {
  ZstdFrameShape shape;
  size_t p = 0;

  if (f.size() < 6) {
    return shape;
  }
  if (!(f[0] == 0x28 && f[1] == 0xB5 && f[2] == 0x2F && f[3] == 0xFD)) {
    return shape;
  }
  p = 4;

  uint8_t fhd = f[p++];
  unsigned fcs_flag = fhd >> 6;
  bool single_segment = ((fhd >> 5) & 1) != 0;
  unsigned did_flag = fhd & 3;

  if (!single_segment) {
    p += 1; // Window_Descriptor
  }
  static const unsigned kDidSize[4] = {0, 1, 2, 4};
  p += kDidSize[did_flag];

  unsigned fcs_size;
  switch (fcs_flag) {
  case 0: fcs_size = single_segment ? 1u : 0u; break;
  case 1: fcs_size = 2; break;
  case 2: fcs_size = 4; break;
  default: fcs_size = 8; break;
  }
  p += fcs_size;

  for (;;) {
    if (p + 3 > f.size()) {
      return shape;
    }
    uint32_t h = (uint32_t)f[p] | ((uint32_t)f[p + 1] << 8) |
        ((uint32_t)f[p + 2] << 16);
    p += 3;

    bool last = (h & 1) != 0;
    unsigned type = (h >> 1) & 3;
    size_t size = h >> 3;

    shape.blocks++;
    switch (type) {
    case 0: shape.raw_blocks++; break;
    case 1: shape.rle_blocks++; break;
    case 2: shape.compressed_blocks++; break;
    default: return shape; // Reserved: not a frame we can classify
    }

    if (type == 2) {
      if (p >= f.size()) {
        return shape;
      }
      switch (f[p] & 3) {
      case 0: shape.literals_raw++; break;
      case 1: shape.literals_rle++; break;
      case 2: shape.literals_huffman++; break;
      default: shape.literals_treeless++; break;
      }
    }

    p += (type == 1) ? 1u : size;
    if (p > f.size()) {
      return shape;
    }
    if (last) {
      break;
    }
  }

  shape.parsed = true;
  return shape;
}

// Prose-shaped bytes: a skewed letter distribution with few long repeats, so
// the reference encoder Huffman-codes the literals instead of storing them.
static std::vector<uint8_t> generateProseLikeData(size_t size, unsigned seed) {
  static const char * kWords[] = {"the", "quick", "brown", "fox", "jumps",
      "over", "lazy", "dog", "and", "then", "returns", "home", "with",
      "several", "unusual", "souvenirs", "from", "distant", "places",
      "nobody", "remembers", "clearly", "anymore", "which", "is", "perhaps",
      "the", "point", "of", "travelling", "at", "all"};
  const size_t kNumWords = sizeof(kWords) / sizeof(kWords[0]);

  std::mt19937 gen(seed);
  std::vector<uint8_t> data;
  data.reserve(size + 16);

  while (data.size() < size) {
    const char * w = kWords[gen() % kNumWords];
    for (const char * c = w; *c; c++) {
      data.push_back((uint8_t)*c);
    }
    unsigned r = gen() % 16;
    data.push_back(r == 0 ? (uint8_t)'\n' : (r < 3 ? (uint8_t)',' : (uint8_t)' '));
  }

  data.resize(size);
  return data;
}

// A skewed alphabet with no repeated substrings worth matching: the entire
// gain has to come from entropy-coding the literals.
static std::vector<uint8_t> generateSkewedLiterals(size_t size, unsigned seed) {
  static const int kWeights[16] = {
      100, 50, 25, 12, 6, 3, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  std::mt19937 gen(seed);
  std::discrete_distribution<int> dist(kWeights, kWeights + 16);

  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; i++) {
    data[i] = (uint8_t)dist(gen);
  }
  return data;
}

TEST_F(ZstdOracleTest, OracleIsActuallyAvailable) {
  // A skipped oracle test and an absent one look identical in the summary
  // line.  If no oracle at all can run, say so as a failure rather than
  // reporting a green suite that checked nothing against a reference.
  ASSERT_TRUE(has_zstd_cli_)
      << "The zstd CLI was not found, so nothing in this file compares our "
         "output against a reference implementation. Install it, or set "
         "GCOMP_SKIP_ORACLE_TESTS=1 to say the gap is intentional.";
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_HuffmanCodedLiterals) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  std::vector<uint8_t> original = generateProseLikeData(64 * 1024, 20260917u);
  std::vector<uint8_t> compressed = zstdCliCompressWith(original, "-3");
  ASSERT_FALSE(compressed.empty());

  ZstdFrameShape shape = inspectZstdFrame(compressed);
  ASSERT_TRUE(shape.parsed) << "Could not walk the reference frame";
  ASSERT_GT(shape.literals_huffman, 0)
      << "This input no longer produces Huffman-coded literals, so the test "
         "is not exercising the path it was written for";

  std::vector<uint8_t> out = gcompDecompress(compressed, original.size());
  ASSERT_EQ(out.size(), original.size());
  ASSERT_EQ(memcmp(out.data(), original.data(), original.size()), 0);
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_TreelessLiterals) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  // Over a long enough input the encoder keeps the Huffman tree it built for
  // an earlier block and emits Treeless_Literals_Blocks.  Which shapes do
  // that is a heuristic, so sweep a few and require that at least one did.
  static const char * kFlags[] = {"-1", "-3", "-1 -B16384", "-6", "-9"};
  static const size_t kSizes[] = {256 * 1024, 512 * 1024, 1024 * 1024};

  int treeless_seen = 0;

  for (const char * flags : kFlags) {
    for (size_t n : kSizes) {
      std::vector<uint8_t> original = generateProseLikeData(n, 7u);
      std::vector<uint8_t> compressed = zstdCliCompressWith(original, flags);
      ASSERT_FALSE(compressed.empty()) << flags << " n=" << n;

      ZstdFrameShape shape = inspectZstdFrame(compressed);
      ASSERT_TRUE(shape.parsed) << "Could not walk the reference frame";
      treeless_seen += shape.literals_treeless;

      std::vector<uint8_t> out = gcompDecompress(compressed, original.size());
      ASSERT_EQ(out.size(), original.size()) << flags << " n=" << n;
      ASSERT_EQ(memcmp(out.data(), original.data(), original.size()), 0)
          << flags << " n=" << n;
    }
  }

  ASSERT_GT(treeless_seen, 0)
      << "None of these inputs produced a treeless literals block, so the "
         "test is not exercising the path it was written for";
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_FourStreamLiteralsBelowJumpThreshold) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  // Small inputs still get four Huffman streams, and the Jump_Table is
  // present at any size (RFC 8878 4.2.2).  Sweep the sizes where the
  // reference encoder switches between one and four streams.
  for (size_t n = 128; n <= 4096; n += 97) {
    std::vector<uint8_t> original = generateProseLikeData(n, (unsigned)n);
    std::vector<uint8_t> compressed = zstdCliCompressWith(original, "-3");
    ASSERT_FALSE(compressed.empty()) << "n=" << n;

    std::vector<uint8_t> out = gcompDecompress(compressed, original.size());
    ASSERT_EQ(out.size(), original.size()) << "n=" << n;
    ASSERT_EQ(memcmp(out.data(), original.data(), original.size()), 0)
        << "n=" << n;
  }
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_EveryLevel) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  std::vector<uint8_t> original = generateProseLikeData(256 * 1024, 31337u);

  static const char * kFlags[] = {"-1", "-3", "-6", "-9", "-12", "-19",
      "--ultra -22", "--long=27 -9", "-5 -B32768", "-3 --no-check"};

  for (const char * flags : kFlags) {
    std::vector<uint8_t> compressed = zstdCliCompressWith(original, flags);
    ASSERT_FALSE(compressed.empty()) << "flags: " << flags;

    std::vector<uint8_t> out = gcompDecompress(compressed, original.size());
    ASSERT_EQ(out.size(), original.size()) << "flags: " << flags;
    ASSERT_EQ(memcmp(out.data(), original.data(), original.size()), 0)
        << "flags: " << flags;
  }
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_LiteralsOnlyBlocks) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  std::vector<uint8_t> original = generateSkewedLiterals(256 * 1024, 99u);
  std::vector<uint8_t> compressed = zstdCliCompressWith(original, "-3");
  ASSERT_FALSE(compressed.empty());

  ZstdFrameShape shape = inspectZstdFrame(compressed);
  ASSERT_TRUE(shape.parsed) << "Could not walk the reference frame";
  ASSERT_GT(shape.literals_huffman + shape.literals_treeless, 0)
      << "This input no longer produces entropy-coded literals";

  std::vector<uint8_t> out = gcompDecompress(compressed, original.size());
  ASSERT_EQ(out.size(), original.size());
  ASSERT_EQ(memcmp(out.data(), original.data(), original.size()), 0);
}

TEST_F(ZstdOracleTest, OurEncoder_EmitsEntropyCodedLiterals) {
  // Not an interoperability test: it pins the encoder to actually using its
  // Huffman coder.  Every literals section came out stored for as long as
  // zstd_huf_build_enc_table returned zero-length codes, and nothing noticed
  // because the output was still valid, just large.
  std::vector<uint8_t> original = generateProseLikeData(256 * 1024, 5u);
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty());

  ZstdFrameShape shape = inspectZstdFrame(compressed);
  ASSERT_TRUE(shape.parsed) << "Could not walk our own frame";
  ASSERT_GT(shape.compressed_blocks, 0)
      << "Every block was stored rather than compressed";
  ASSERT_GT(shape.literals_huffman, 0)
      << "No literals section was Huffman-coded";
  ASSERT_LT(compressed.size(), original.size() / 2)
      << "Prose-shaped input should compress to well under half its size";
}

TEST_F(ZstdOracleTest, OurEncoder_CompressesBlocksWithoutMatches) {
  // A block with no usable matches still has entropy to remove: RFC 8878
  // 3.1.1.3 allows a Compressed_Block whose Sequences_Section is empty.
  std::vector<uint8_t> original = generateSkewedLiterals(256 * 1024, 4242u);
  std::vector<uint8_t> compressed = gcompCompress(original);
  ASSERT_FALSE(compressed.empty());
  ASSERT_LT(compressed.size(), original.size() * 3 / 4)
      << "Skewed literals were stored rather than entropy-coded";

  std::vector<uint8_t> out = gcompDecompress(compressed, original.size());
  ASSERT_EQ(out.size(), original.size());
  ASSERT_EQ(memcmp(out.data(), original.data(), original.size()), 0);
}

TEST_F(ZstdOracleTest, OurEncoder_ZstdCli_AcrossShapes) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  struct Case {
    const char * name;
    std::vector<uint8_t> data;
  };

  std::vector<Case> cases;
  cases.push_back({"prose", generateProseLikeData(128 * 1024, 1u)});
  cases.push_back({"skewed literals", generateSkewedLiterals(128 * 1024, 2u)});
  cases.push_back({"repetitive", generateTextData(128 * 1024)});
  cases.push_back({"incompressible", generateRandomData(64 * 1024, 3u)});
  cases.push_back({"single run", std::vector<uint8_t>(64 * 1024, 'A')});

  for (const Case & c : cases) {
    std::vector<uint8_t> compressed = gcompCompress(c.data);
    ASSERT_FALSE(compressed.empty()) << c.name;

    std::vector<uint8_t> out = zstdCliDecompress(compressed);
    ASSERT_EQ(out.size(), c.data.size())
        << c.name << ": the reference CLI could not read our frame";
    ASSERT_EQ(memcmp(out.data(), c.data.data(), c.data.size()), 0) << c.name;
  }
}


TEST_F(ZstdOracleTest, OurEncoder_ZstdCli_EveryShortLength) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  // Empty, single bytes, and the sizes around the frame header's content-size
  // field widths.  Short inputs were the only ground the Python-module tests
  // covered that nothing else did, so it moved here when those were removed.
  std::vector<size_t> lengths;
  for (size_t n = 0; n <= 40; n++) {
    lengths.push_back(n);
  }
  for (size_t n : {size_t(254), size_t(255), size_t(256), size_t(257),
           size_t(65534), size_t(65535), size_t(65536), size_t(65537)}) {
    lengths.push_back(n);
  }

  for (size_t n : lengths) {
    std::vector<uint8_t> original = generateProseLikeData(n, (unsigned)n + 1u);
    ASSERT_EQ(original.size(), n);

    std::vector<uint8_t> compressed = gcompCompress(original);
    ASSERT_FALSE(compressed.empty()) << "n=" << n;

    std::vector<uint8_t> out = zstdCliDecompress(compressed);
    ASSERT_EQ(out.size(), n)
        << "n=" << n << ": the zstd CLI could not read our frame";
    if (n) {
      ASSERT_EQ(memcmp(out.data(), original.data(), n), 0) << "n=" << n;
    }
  }
}

TEST_F(ZstdOracleTest, ZstdCli_OurDecoder_EveryShortLength) {
  if (!has_zstd_cli_) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  for (size_t n = 0; n <= 40; n++) {
    std::vector<uint8_t> original = generateProseLikeData(n, (unsigned)n + 7u);
    std::vector<uint8_t> compressed = zstdCliCompressWith(original, "-3");
    ASSERT_FALSE(compressed.empty()) << "n=" << n;

    std::vector<uint8_t> out = gcompDecompress(compressed, n);
    ASSERT_EQ(out.size(), n) << "n=" << n;
    if (n) {
      ASSERT_EQ(memcmp(out.data(), original.data(), n), 0) << "n=" << n;
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
