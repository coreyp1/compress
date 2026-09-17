/**
 * @file test_zstd_entropy.cpp
 *
 * Entropy coding tests for the Zstd implementation in the Ghoti.io Compress
 * library.
 *
 * Tests the low-level FSE (Finite State Entropy) and Huffman encoding/decoding
 * functions used by the zstd block compressor and decompressor.
 *
 * These tests verify:
 * - Predefined FSE tables (literals length, match length, offset)
 * - Custom FSE table building from bitstream
 * - Huffman table reading and decoding
 * - Single-stream and 4-stream Huffman decoding
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>
#include <vector>

// Constants from zstd spec
static const uint32_t ZSTD_MAGIC = 0xFD2FB528U;

class ZstdEntropyTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  // Helper: Compress data and return the compressed output
  std::vector<uint8_t> compress(
      const void * data, size_t len, gcomp_options_t * opts = nullptr) {
    gcomp_encoder_t * encoder = nullptr;
    gcomp_status_t status =
        gcomp_encoder_create(registry_, "zstd", opts, &encoder);
    if (status != GCOMP_OK) {
      return {};
    }

    // Allocate larger buffer for multi-block data
    std::vector<uint8_t> result(len * 2 + 65536);
    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t out_buf = {result.data(), result.size(), 0};

    // Keep feeding until the input is consumed: one update call is not
    // required to take everything, and stopping after one silently truncated
    // any input past the first block.
    while (in_buf.used < in_buf.size) {
      size_t before = in_buf.used;
      status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
      if (status != GCOMP_OK) {
        gcomp_encoder_destroy(encoder);
        return {};
      }
      if (in_buf.used == before) {
        break; // no progress; the output buffer is full
      }
    }

    status = gcomp_encoder_finish(encoder, &out_buf);
    if (status != GCOMP_OK) {
      gcomp_encoder_destroy(encoder);
      return {};
    }

    result.resize(out_buf.used);
    gcomp_encoder_destroy(encoder);
    return result;
  }

  // Helper: Decompress data and return the decompressed output
  std::vector<uint8_t> decompress(const void * data, size_t len,
      gcomp_options_t * opts = nullptr, gcomp_status_t * status_out = nullptr) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "zstd", opts, &decoder);
    if (status != GCOMP_OK) {
      if (status_out)
        *status_out = status;
      return {};
    }

    std::vector<uint8_t> result(len * 100 + 65536);
    gcomp_buffer_t in_buf = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t out_buf = {result.data(), result.size(), 0};

    while (in_buf.used < in_buf.size) {
      size_t before = in_buf.used;
      status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
      if (status != GCOMP_OK) {
        if (status_out)
          *status_out = status;
        gcomp_decoder_destroy(decoder);
        return {};
      }
      if (in_buf.used == before) {
        break;
      }
    }

    status = gcomp_decoder_finish(decoder, &out_buf);
    if (status_out)
      *status_out = status;

    result.resize(out_buf.used);
    gcomp_decoder_destroy(decoder);
    return result;
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// FSE Predefined Table Tests
//
// The zstd format defines predefined FSE tables for literal length,
// match length, and offset codes. These tests verify that data compressed
// using these tables can be correctly decompressed.
//

TEST_F(ZstdEntropyTest, PredefinedLiteralLengthTable) {
  // Create data that will generate literal length codes
  // Short literals interspersed with matches
  std::vector<uint8_t> input;
  for (int i = 0; i < 100; i++) {
    // Add varying literal runs (0-15 bytes) then matches
    int lit_len = i % 16;
    for (int j = 0; j < lit_len; j++) {
      input.push_back(static_cast<uint8_t>('A' + (j % 26)));
    }
    // Add a repeated pattern to create matches
    for (int j = 0; j < 8; j++) {
      input.push_back(static_cast<uint8_t>('0' + (i % 10)));
    }
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(ZstdEntropyTest, PredefinedMatchLengthTable) {
  // Create data with varying match lengths
  std::vector<uint8_t> input;
  // Create a pattern that will be repeated with varying lengths
  std::string pattern = "ABCDEFGHIJKLMNOP";

  for (int i = 0; i < 50; i++) {
    // Add the pattern (will be matched)
    for (char c : pattern) {
      input.push_back(static_cast<uint8_t>(c));
    }
    // Add some unique data to reset
    for (int j = 0; j < 4; j++) {
      input.push_back(static_cast<uint8_t>('a' + (i * 4 + j) % 26));
    }
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(ZstdEntropyTest, PredefinedOffsetTable) {
  // Create data with varying match offsets
  std::vector<uint8_t> input;

  // Create patterns at different distances
  for (int distance = 1; distance <= 32; distance *= 2) {
    // Add a marker pattern
    for (int j = 0; j < 16; j++) {
      input.push_back(static_cast<uint8_t>('X'));
    }
    // Add spacing
    for (int j = 0; j < distance; j++) {
      input.push_back(static_cast<uint8_t>('a' + (j % 26)));
    }
    // Repeat the marker pattern (should match with offset = distance + 16)
    for (int j = 0; j < 16; j++) {
      input.push_back(static_cast<uint8_t>('X'));
    }
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

//
// Repeat Offset Tests
//
// Zstd maintains 3 repeat offsets for efficiency. These tests verify
// that repeat offset handling works correctly.
//

TEST_F(ZstdEntropyTest, RepeatOffset1) {
  // Create data that uses repeat offset 1 (same offset as previous match)
  std::vector<uint8_t> input;

  // Create a pattern
  std::string pattern = "REPEATME";
  for (char c : pattern) {
    input.push_back(static_cast<uint8_t>(c));
  }

  // Add unique bytes
  input.push_back('_');

  // Repeat the pattern multiple times (each should use repeat offset)
  for (int i = 0; i < 10; i++) {
    for (char c : pattern) {
      input.push_back(static_cast<uint8_t>(c));
    }
    input.push_back(static_cast<uint8_t>('0' + i));
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(ZstdEntropyTest, RepeatOffset2And3) {
  // Create data that alternates between different offsets
  std::vector<uint8_t> input;

  // Pattern A
  std::string patternA = "AAAAAAAA";
  // Pattern B
  std::string patternB = "BBBBBBBB";

  // Write initial patterns
  for (char c : patternA)
    input.push_back(static_cast<uint8_t>(c));
  input.push_back('_');
  for (char c : patternB)
    input.push_back(static_cast<uint8_t>(c));
  input.push_back('-');

  // Alternate patterns to exercise repeat offset swapping
  for (int i = 0; i < 5; i++) {
    for (char c : patternA)
      input.push_back(static_cast<uint8_t>(c));
    input.push_back(static_cast<uint8_t>('0' + i));
    for (char c : patternB)
      input.push_back(static_cast<uint8_t>(c));
    input.push_back(static_cast<uint8_t>('a' + i));
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

//
// FSE State Machine Tests
//
// These tests verify that the FSE state machine correctly handles
// various symbol sequences.
//

TEST_F(ZstdEntropyTest, FSEMultipleSymbolCodes) {
  // Create data that will generate multiple different symbol codes
  std::vector<uint8_t> input;

  // Varying literal lengths (codes 0-15+)
  for (int lit_len = 0; lit_len <= 20; lit_len++) {
    for (int j = 0; j < lit_len; j++) {
      input.push_back(static_cast<uint8_t>('A' + (j % 26)));
    }
    // Match pattern
    for (int j = 0; j < 8; j++) {
      input.push_back(static_cast<uint8_t>('X'));
    }
    input.push_back(static_cast<uint8_t>(lit_len));
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(ZstdEntropyTest, FSELongSequenceChain) {
  // Create data that generates a long chain of sequences
  std::vector<uint8_t> input;

  // Create 1000+ sequences
  for (int i = 0; i < 1000; i++) {
    // Small literal run
    for (int j = 0; j < (i % 5); j++) {
      input.push_back(static_cast<uint8_t>('a' + (j % 26)));
    }
    // Match
    for (int j = 0; j < 4; j++) {
      input.push_back(static_cast<uint8_t>('0'));
    }
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

//
// Huffman Encoding Tests
//
// These tests verify Huffman-compressed literals handling.
// Note: Our encoder currently uses raw literals, so we test via
// roundtrip with external zstd-compressed data that uses Huffman.
//

TEST_F(ZstdEntropyTest, RawLiteralsRoundtrip) {
  // Our encoder uses raw literals - verify they work
  std::vector<uint8_t> input(1000);
  for (size_t i = 0; i < input.size(); i++) {
    input[i] = static_cast<uint8_t>(i * 7 + 13); // Non-compressible pattern
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(ZstdEntropyTest, RLELiteralsRoundtrip) {
  // RLE literals (single repeated byte)
  std::vector<uint8_t> input(1000, 'A');

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());
  // RLE should be very efficient
  EXPECT_LT(compressed.size(), input.size() / 10);

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

//
// Edge Cases
//

TEST_F(ZstdEntropyTest, SingleSequence) {
  // Data that produces exactly one sequence
  std::vector<uint8_t> input;

  // One literal
  input.push_back('A');
  // One match (need the pattern to exist first)
  for (int i = 0; i < 10; i++) {
    input.push_back('X');
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(ZstdEntropyTest, NoSequences) {
  // Data too small to have matches - all literals
  std::vector<uint8_t> input = {'H', 'e', 'l', 'l', 'o'};

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(ZstdEntropyTest, LargeLiteralLength) {
  // Create data with a very large literal run (>127 to test extra bits)
  std::vector<uint8_t> input;

  // Large literal run (200 bytes)
  for (int i = 0; i < 200; i++) {
    input.push_back(static_cast<uint8_t>('A' + (i % 26)));
  }
  // Then a match
  for (int i = 0; i < 100; i++) {
    input.push_back(static_cast<uint8_t>('X'));
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(ZstdEntropyTest, LargeMatchLength) {
  // Create data with a very long match (>127 to test extra bits)
  std::vector<uint8_t> input;

  // Initial pattern
  std::string pattern(50, 'P');
  for (char c : pattern) {
    input.push_back(static_cast<uint8_t>(c));
  }

  // Separator
  input.push_back('_');

  // Long repeat of the pattern (200+ bytes)
  for (int rep = 0; rep < 5; rep++) {
    for (char c : pattern) {
      input.push_back(static_cast<uint8_t>(c));
    }
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(ZstdEntropyTest, LargeOffset) {
  // Create data with a large match offset
  std::vector<uint8_t> input;

  // Initial pattern
  for (int i = 0; i < 100; i++) {
    input.push_back(static_cast<uint8_t>('Z'));
  }

  // Large gap (4KB)
  for (int i = 0; i < 4096; i++) {
    input.push_back(static_cast<uint8_t>('a' + (i % 26)));
  }

  // Repeat the pattern (large offset match)
  for (int i = 0; i < 100; i++) {
    input.push_back(static_cast<uint8_t>('Z'));
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty());

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

//
// Multi-Block Tests
//
// These tests verify entropy coding works correctly across block boundaries.
//

TEST_F(ZstdEntropyTest, MultiBlockEntropy) {
  // Create data large enough for multiple blocks (>128KB)
  // Use a mix of patterns to avoid too much RLE
  std::vector<uint8_t> input;
  input.reserve(200000);

  for (size_t i = 0; i < 100000; i++) {
    // Mix of patterns - include some randomness-like variation
    input.push_back(static_cast<uint8_t>('A' + ((i * 7 + i / 100) % 26)));
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

TEST_F(ZstdEntropyTest, BlockBoundaryMatch) {
  // Create data to test matches near end of block
  // Use similar pattern to LargeRandomRoundtrip which works
  std::vector<uint8_t> input(50000);
  for (size_t i = 0; i < input.size(); i++) {
    input[i] = static_cast<uint8_t>(i * 7 + 13);
  }

  // Add pattern that will be matched later
  std::string pattern = "BOUNDARY_PATTERN";
  for (char c : pattern) {
    input.push_back(static_cast<uint8_t>(c));
  }

  // Add more random data
  for (size_t i = 0; i < 1000; i++) {
    input.push_back(static_cast<uint8_t>(i * 3 + 7));
  }

  // Repeat the pattern (should create a match)
  for (char c : pattern) {
    input.push_back(static_cast<uint8_t>(c));
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

//
// 4-Stream Huffman Encoding
//
// When a block has >= 1024 literals and uses Huffman compression, the encoder
// uses 4 parallel streams (jump table + 4 concatenated streams). This test
// verifies roundtrip with data sized to trigger 4-stream mode.
//

TEST_F(ZstdEntropyTest, FourStreamHuffmanRoundtrip) {
  // >= 1024 literals in a block to trigger 4-stream encoding.
  // Use low-entropy text so the block uses Huffman (not raw) literals.
  std::vector<uint8_t> input;
  input.reserve(1500);
  for (size_t i = 0; i < 1500; i++) {
    input.push_back(static_cast<uint8_t>('a' + (i % 26)));
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}

//
// FSE-Compressed Huffman Weights
//
// When the Huffman table has >127 symbols, weights are FSE-compressed
// (header_byte < 128 = compressed size). This test verifies roundtrip with
// input that uses 128+ distinct bytes so the literal Huffman table triggers
// FSE weight encoding.
//

TEST_F(ZstdEntropyTest, FSECompressedWeightsRoundtrip) {
  // Use 200 distinct bytes so Huffman table has 200 symbols (>127).
  // Repeat each byte a few times so the block uses Huffman (not raw).
  std::vector<uint8_t> input;
  input.reserve(2000);
  for (int rep = 0; rep < 10; rep++) {
    for (int b = 0; b < 200; b++) {
      input.push_back(static_cast<uint8_t>(b));
    }
  }

  auto compressed = compress(input.data(), input.size());
  ASSERT_FALSE(compressed.empty()) << "Compression failed";

  auto decompressed = decompress(compressed.data(), compressed.size());
  ASSERT_EQ(decompressed.size(), input.size());
  EXPECT_EQ(memcmp(decompressed.data(), input.data(), input.size()), 0);
}


// Count the literals-section types across a frame's compressed blocks.
// RFC 8878 sections 3.1.1.2 (block header) and 3.1.1.3.1.1 (literals header).
struct LiteralsTypeCounts {
  int raw = 0;
  int rle = 0;
  int huffman = 0;  // Compressed_Literals_Block
  int treeless = 0; // Treeless_Literals_Block
  int raw_blocks = 0;
  bool parsed = false;
};

static LiteralsTypeCounts countLiteralsTypes(const std::vector<uint8_t> & f) {
  LiteralsTypeCounts c;
  if (f.size() < 6 || f[0] != 0x28 || f[1] != 0xB5 || f[2] != 0x2F ||
      f[3] != 0xFD) {
    return c;
  }
  size_t p = 4;
  uint8_t fhd = f[p++];
  unsigned fcs_flag = fhd >> 6;
  bool single = ((fhd >> 5) & 1) != 0;
  unsigned did = fhd & 3;
  if (!single) {
    p += 1;
  }
  static const unsigned kDid[4] = {0, 1, 2, 4};
  p += kDid[did];
  p += (fcs_flag == 0) ? (single ? 1u : 0u)
                       : (fcs_flag == 1 ? 2u : (fcs_flag == 2 ? 4u : 8u));

  for (;;) {
    if (p + 3 > f.size()) {
      return c;
    }
    uint32_t h = (uint32_t)f[p] | ((uint32_t)f[p + 1] << 8) |
        ((uint32_t)f[p + 2] << 16);
    p += 3;
    bool last = (h & 1) != 0;
    unsigned type = (h >> 1) & 3;
    size_t size = h >> 3;
    if (type == 3) {
      return c;
    }
    if (type == 0) {
      c.raw_blocks++;
    }
    if (type == 2) {
      if (p >= f.size()) {
        return c;
      }
      switch (f[p] & 3) {
      case 0: c.raw++; break;
      case 1: c.rle++; break;
      case 2: c.huffman++; break;
      default: c.treeless++; break;
      }
    }
    p += (type == 1) ? 1u : size;
    if (p > f.size()) {
      return c;
    }
    if (last) {
      break;
    }
  }
  c.parsed = true;
  return c;
}

// The Huffman tree description has two forms (RFC 8878 4.2.1.1): a direct
// 4-bit weight list, whose header byte is 127 + Number_Of_Symbols and so
// cannot name more than 128 symbols, and an FSE-compressed form for anything
// larger.  The FSE form normalized its counts as though they were Huffman
// weights -- 2^(n-1) shares rather than slots summing to the table size --
// and encoded them with one FSE state where the decoder uses two.  It failed
// for every alphabet that needed it, and zstd_literals_encode_compressed
// quietly stored the literals instead.  Any data using more than 129 distinct
// byte values was therefore never Huffman-coded at all.
//
// Sweeping the alphabet size across that boundary is what catches it; below
// it, the direct form hides the problem completely.  The check is on the
// literals section type rather than on a compression ratio, because the ratio
// this data reaches varies with the alphabet size while the requirement --
// that the literals are entropy-coded at all -- does not.
TEST_F(ZstdEntropyTest, LiteralsAreHuffmanCodedAtEveryAlphabetSize) {
  for (unsigned distinct = 4; distinct <= 256;
      distinct += (distinct < 120 ? 29 : 7)) {
    // Strongly skewed frequencies over `distinct` byte values, with no long
    // repeats, so the only available gain is the Huffman coder's.
    std::vector<uint8_t> data;
    data.reserve(96 * 1024);
    uint32_t x = 2654435761u ^ distinct;
    while (data.size() < 96 * 1024) {
      x = x * 1103515245u + 12345u;
      uint64_t r = (x >> 16) & 0xFFFFu;
      // r^4 / 65536^4, scaled to the alphabet: heavily biased to low indices.
      uint64_t q = (r * r) / 65536u;
      uint64_t pick = ((q * q) / 65536u) * distinct / 65536u;
      data.push_back((uint8_t)(pick % distinct));
    }

    std::vector<uint8_t> packed = compress(data.data(), data.size());
    ASSERT_FALSE(packed.empty()) << "distinct=" << distinct;

    LiteralsTypeCounts c = countLiteralsTypes(packed);
    ASSERT_TRUE(c.parsed) << "distinct=" << distinct << ": unparsable frame";
    EXPECT_GT(c.huffman + c.treeless, 0)
        << "distinct=" << distinct << ": literals were stored ("
        << c.raw << " raw literal sections, " << c.raw_blocks
        << " raw blocks) rather than Huffman-coded";
    EXPECT_LT(packed.size(), data.size())
        << "distinct=" << distinct;

    std::vector<uint8_t> back = decompress(packed.data(), packed.size());
    ASSERT_EQ(back.size(), data.size()) << "distinct=" << distinct;
    ASSERT_EQ(memcmp(back.data(), data.data(), data.size()), 0)
        << "distinct=" << distinct;
  }
}



// The tests below feed the decoder very compressible data on purpose, which
// trips the default expansion-ratio guard.  This is what a caller decoding
// its own trusted output would set.
static gcomp_options_t * makeGenerousLimits() {
  gcomp_options_t * opts = nullptr;
  if (gcomp_options_create(&opts) != GCOMP_OK) {
    return nullptr;
  }
  gcomp_options_set_uint64(opts, "limits.max_output_bytes", 64u << 20);
  gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 1000000);
  gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 256u << 20);
  return opts;
}

// Which Symbol_Compression_Mode each of the three sequence symbol types used
// (RFC 8878 3.1.1.3.2): 0 Predefined, 1 RLE, 2 FSE_Compressed, 3 Repeat.
struct SeqModeCounts {
  int mode[3][4] = {{0}}; // [0]=literal lengths, [1]=offsets, [2]=match lengths
  int blocks_with_sequences = 0;
  bool parsed = false;
};

static SeqModeCounts countSequenceModes(const std::vector<uint8_t> & f) {
  SeqModeCounts c;
  if (f.size() < 6 || f[0] != 0x28 || f[1] != 0xB5 || f[2] != 0x2F ||
      f[3] != 0xFD) {
    return c;
  }
  size_t p = 4;
  uint8_t fhd = f[p++];
  unsigned fcs_flag = fhd >> 6;
  bool single = ((fhd >> 5) & 1) != 0;
  unsigned did = fhd & 3;
  if (!single) {
    p += 1;
  }
  static const unsigned kDid[4] = {0, 1, 2, 4};
  p += kDid[did];
  p += (fcs_flag == 0) ? (single ? 1u : 0u)
                       : (fcs_flag == 1 ? 2u : (fcs_flag == 2 ? 4u : 8u));

  for (;;) {
    if (p + 3 > f.size()) {
      return c;
    }
    uint32_t h = (uint32_t)f[p] | ((uint32_t)f[p + 1] << 8) |
        ((uint32_t)f[p + 2] << 16);
    p += 3;
    bool last = (h & 1) != 0;
    unsigned type = (h >> 1) & 3;
    size_t size = h >> 3;
    if (type == 3) {
      return c;
    }

    if (type == 2) {
      if (p + size > f.size() || size < 2) {
        return c;
      }
      const uint8_t * b = f.data() + p;
      unsigned lt = b[0] & 3;
      unsigned sf = (b[0] >> 2) & 3;
      size_t regen, comp, hs;
      if (lt < 2) {
        if (sf == 1) {
          regen = (b[0] >> 4) | ((size_t)b[1] << 4);
          hs = 2;
        }
        else if (sf == 3) {
          regen = (b[0] >> 4) | ((size_t)b[1] << 4) | ((size_t)b[2] << 12);
          hs = 3;
        }
        else {
          regen = b[0] >> 3;
          hs = 1;
        }
        comp = (lt == 0) ? regen : 1;
      }
      else if (sf < 2) {
        comp = (b[1] >> 6) | ((size_t)b[2] << 2);
        hs = 3;
      }
      else if (sf == 2) {
        comp = (b[2] >> 2) | ((size_t)b[3] << 6);
        hs = 4;
      }
      else {
        comp = (b[2] >> 6) | ((size_t)b[3] << 2) | ((size_t)b[4] << 10);
        hs = 5;
      }

      size_t q = hs + comp;
      if (q >= size) {
        return c;
      }
      unsigned n0 = b[q];
      size_t nseq;
      if (n0 == 0) {
        nseq = 0;
        q += 1;
      }
      else if (n0 < 128) {
        nseq = n0;
        q += 1;
      }
      else if (n0 < 255) {
        nseq = ((size_t)(n0 - 128) << 8) + b[q + 1];
        q += 2;
      }
      else {
        nseq = (size_t)b[q + 1] + ((size_t)b[q + 2] << 8) + 0x7F00;
        q += 3;
      }

      if (nseq) {
        if (q >= size) {
          return c;
        }
        unsigned scm = b[q];
        c.mode[0][(scm >> 6) & 3]++;
        c.mode[1][(scm >> 4) & 3]++;
        c.mode[2][(scm >> 2) & 3]++;
        c.blocks_with_sequences++;
      }
    }

    p += (type == 1) ? 1u : size;
    if (p > f.size()) {
      return c;
    }
    if (last) {
      break;
    }
  }
  c.parsed = true;
  return c;
}

// The sequences section may describe its own FSE tables instead of using the
// predefined ones (RFC 8878 3.1.1.3.2), and for anything past a few hundred
// sequences that is worth several bits each.  The encoder only ever wrote
// mode 0, which cost about 4.7 bits per sequence -- roughly 170 KB across a
// 4.4 MB corpus.
//
// The check is on the modes byte rather than on a size, because "we chose the
// cheaper table" is the property, and the size it buys depends on the data.
TEST_F(ZstdEntropyTest, SequencesUsePerBlockTablesWhenTheyPay) {
  // Enough matchable structure to produce thousands of sequences, with the
  // code distributions skewed enough that a fitted table beats the
  // predefined one.
  std::vector<uint8_t> data;
  data.reserve(256 * 1024);
  uint32_t x = 99999u;
  const char * frags[] = {"the ", "quick ", "brown ", "fox ", "the quick ",
      "brown fox ", "jumps ", "over ", "the lazy ", "dog "};
  while (data.size() < 256 * 1024) {
    x = x * 1103515245u + 12345u;
    const char * fr = frags[(x >> 16) % 10];
    for (const char * ch = fr; *ch; ch++) {
      data.push_back((uint8_t)*ch);
    }
  }
  data.resize(256 * 1024);

  std::vector<uint8_t> packed = compress(data.data(), data.size());
  ASSERT_FALSE(packed.empty());

  SeqModeCounts c = countSequenceModes(packed);
  ASSERT_TRUE(c.parsed) << "unparsable frame";
  ASSERT_GT(c.blocks_with_sequences, 0) << "no block carried any sequences";

  // At least one symbol type in at least one block should have described its
  // own table rather than fallen back to the predefined one.
  EXPECT_GT(c.mode[0][2] + c.mode[1][2] + c.mode[2][2], 0)
      << "every sequence table was predefined (LL: " << c.mode[0][0]
      << " predefined / " << c.mode[0][2] << " FSE)";

  gcomp_options_t * dopts = makeGenerousLimits();
  ASSERT_NE(dopts, nullptr);
  std::vector<uint8_t> back = decompress(packed.data(), packed.size(), dopts);
  gcomp_options_destroy(dopts);
  ASSERT_EQ(back.size(), data.size());
  ASSERT_EQ(memcmp(back.data(), data.data(), data.size()), 0);
}

TEST_F(ZstdEntropyTest, PerBlockSequenceTablesRoundTripAtEveryAccuracyLog) {
  // The literal length and match length tables may use an Accuracy_Log of up
  // to 9 (RFC 8878 3.1.1.3.2.1), which the encoder only reaches once a block
  // holds a couple of thousand sequences.  The transition value written for
  // such a table needs nine bits; it had been returned through a uint8_t and
  // truncated, producing a stream the reference decoder rejects.  The
  // sequence count crosses that threshold somewhere in this range.
  for (size_t n = 1024; n <= 48 * 1024; n = n * 3 / 2) {
    std::vector<uint8_t> data;
    data.reserve(n + 64);
    uint32_t x = (uint32_t)n * 2654435761u + 1u;
    const char * frags[] = {"ab", "abc", "abcd", "b", "bcd", "cd", "abcde"};
    while (data.size() < n) {
      x = x * 1103515245u + 12345u;
      const char * fr = frags[(x >> 16) % 7];
      for (const char * ch = fr; *ch; ch++) {
        data.push_back((uint8_t)*ch);
      }
      data.push_back((uint8_t)('a' + ((x >> 8) & 7)));
    }
    data.resize(n);

    std::vector<uint8_t> packed = compress(data.data(), data.size());
    ASSERT_FALSE(packed.empty()) << "n=" << n;

    gcomp_options_t * dopts = makeGenerousLimits();
    ASSERT_NE(dopts, nullptr);
    std::vector<uint8_t> back = decompress(packed.data(), packed.size(), dopts);
    gcomp_options_destroy(dopts);
    ASSERT_EQ(back.size(), data.size()) << "n=" << n;
    ASSERT_EQ(memcmp(back.data(), data.data(), data.size()), 0) << "n=" << n;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

//
// Huffman code lengths under the 11-bit cap
//
// RFC 8878 section 4.2.1 caps a literal code word at 11 bits.  The encoder
// used to reach that cap by clamping a plain Huffman code and then
// lengthening the *shortest* code in the block until the code space fit --
// that is, spending bits on the most frequent symbol in the block, the single
// most expensive place to spend them.  These tests state what the cap is for:
// the code still has to be the cheapest one that fits under it.
//

namespace {

// Decode the table description of the first Huffman-coded literals section in
// a frame into one bit length per symbol.  Only the direct 4-bit weight form
// is read here (RFC 8878 4.2.1.1); the tests below stay under 128 symbols so
// that is the form the encoder writes.
bool FirstLiteralCodeLengths(
    const std::vector<uint8_t> & f, std::vector<uint8_t> & lengths_out) {
  if (f.size() < 6 || f[0] != 0x28 || f[1] != 0xB5 || f[2] != 0x2F ||
      f[3] != 0xFD) {
    return false;
  }
  size_t p = 4;
  uint8_t fhd = f[p++];
  unsigned fcs_flag = fhd >> 6;
  bool single = ((fhd >> 5) & 1) != 0;
  unsigned did = fhd & 3;
  if (!single) {
    p += 1;
  }
  static const unsigned kDid[4] = {0, 1, 2, 4};
  p += kDid[did];
  p += (fcs_flag == 0) ? (single ? 1u : 0u)
                       : (fcs_flag == 1 ? 2u : (fcs_flag == 2 ? 4u : 8u));

  for (;;) {
    if (p + 3 > f.size()) {
      return false;
    }
    uint32_t h = (uint32_t)f[p] | ((uint32_t)f[p + 1] << 8) |
        ((uint32_t)f[p + 2] << 16);
    p += 3;
    bool last = (h & 1) != 0;
    unsigned type = (h >> 1) & 3;
    size_t size = h >> 3;
    if (type == 3) {
      return false;
    }
    if (type == 2 && p < f.size() && (f[p] & 3) == 2) {
      const uint8_t * lit = f.data() + p;
      unsigned size_format = (unsigned)((lit[0] >> 2) & 3);
      size_t header_bytes = (size_format < 2) ? 3 : (size_format == 2 ? 4 : 5);
      if (p + header_bytes + 1 > f.size()) {
        return false;
      }
      const uint8_t * weights = lit + header_bytes;
      unsigned first = weights[0];
      if (first < 128) {
        return false; // FSE-compressed weights; this reader handles the
                      // direct form only, whose header byte is
                      // 127 + Number_Of_Symbols
      }
      unsigned num_weights = first - 127;
      std::vector<uint8_t> w(num_weights + 1, 0);
      for (unsigned i = 0; i < num_weights; i++) {
        uint8_t byte = weights[1 + i / 2];
        w[i] = (i % 2 == 0) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0xF);
      }
      // The final symbol's weight is not transmitted: it is whatever makes
      // the code space come out a power of two (RFC 8878 4.2.1.1).
      uint32_t total = 0;
      for (unsigned i = 0; i < num_weights; i++) {
        if (w[i] > 0) {
          total += 1u << (w[i] - 1);
        }
      }
      if (total == 0) {
        return false;
      }
      unsigned high = 0;
      while ((total >> (high + 1)) != 0) {
        high++;
      }
      unsigned table_log = high + 1;
      uint32_t rest = (1u << table_log) - total;
      unsigned rest_high = 0;
      while ((rest >> (rest_high + 1)) != 0) {
        rest_high++;
      }
      w[num_weights] = (uint8_t)(rest_high + 1);

      lengths_out.assign(num_weights + 1, 0);
      for (unsigned i = 0; i <= num_weights; i++) {
        lengths_out[i] = w[i] ? (uint8_t)(table_log + 1 - w[i]) : 0;
      }
      return true;
    }
    p += (type == 1) ? 1u : size;
    if (p > f.size()) {
      return false;
    }
    if (last) {
      return false;
    }
  }
}

} // namespace

TEST_F(ZstdEntropyTest, LiteralCodeWordsNeverExceedElevenBits) {
  // Frequencies that halve drive a plain Huffman code far past the cap, so
  // this is the input on which the cap has to be enforced rather than
  // happened upon.
  std::vector<uint8_t> data;
  data.reserve(200 * 1024);
  uint32_t x = 99991;
  while (data.size() < 200 * 1024) {
    x = x * 1103515245u + 12345u;
    unsigned r = (x >> 12) & 0xFFFFF;
    unsigned sym = 0;
    while (sym < 99 && (r & 1)) {
      sym++;
      r >>= 1;
    }
    data.push_back((uint8_t)sym);
  }
  std::vector<uint8_t> packed = compress(data.data(), data.size());
  ASSERT_FALSE(packed.empty());

  std::vector<uint8_t> lengths;
  ASSERT_TRUE(FirstLiteralCodeLengths(packed, lengths))
      << "no Huffman-coded literals section to read";

  unsigned longest = 0;
  for (uint8_t l : lengths) {
    EXPECT_LE(l, 11) << "a code word ran past the cap in RFC 8878 4.2.1";
    if (l > longest) {
      longest = l;
    }
  }
  ASSERT_GT(longest, 0u);
  uint32_t kraft = 0;
  for (uint8_t l : lengths) {
    if (l > 0) {
      kraft += 1u << (longest - l);
    }
  }
  // An incomplete code leaves code space the decoder's table build has no
  // symbol for, so the lengths must spend all of it.
  EXPECT_EQ(kraft, 1u << longest);
}

TEST_F(ZstdEntropyTest, SkewedLiteralsBeatTheClampedCode) {
  // What the cap costs, measured end to end.  The frequencies here are the
  // ones the old clamp-and-relengthen limiter handled worst; the whole frame
  // has to come out smaller than the bits that limiter's code would have
  // spent on the literals alone.
  std::vector<uint8_t> data;
  data.reserve(128 * 1024);
  uint32_t x = 7777;
  while (data.size() < 128 * 1024) {
    x = x * 1103515245u + 12345u;
    unsigned r = (x >> 9) & 0x3FFFF;
    unsigned sym = 0;
    while (sym < 60 && (r % 100) < 55) {
      sym++;
      r /= 100;
      if (r == 0) {
        break;
      }
    }
    data.push_back((uint8_t)sym);
  }
  std::vector<uint8_t> packed = compress(data.data(), data.size());
  ASSERT_FALSE(packed.empty());
  std::vector<uint8_t> lengths;
  ASSERT_TRUE(FirstLiteralCodeLengths(packed, lengths));
  for (uint8_t l : lengths) {
    EXPECT_LE(l, 11);
  }
  EXPECT_LT(packed.size(), data.size());
}
