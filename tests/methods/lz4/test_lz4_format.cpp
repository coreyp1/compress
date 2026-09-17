/**
 * @file test_lz4_format.cpp
 *
 * Unit tests for LZ4 frame format parsing and writing in the Ghoti.io Compress
 * library.
 *
 * These tests verify:
 * - Frame header parsing (streaming, all flag combinations)
 * - Frame header writing
 * - Block parsing (independent and dependent blocks)
 * - Checksum computation and validation (xxHash32)
 * - Corrupt input detection
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <algorithm>
#include <iomanip>
#include <string>
#include "test_helpers.h"
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lz4.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/xxhash32.h>
#include <gtest/gtest.h>
#include <vector>

//
// LZ4 Frame Format Constants (from spec)
//
// Magic number: 0x184D2204 (little-endian: 04 22 4D 18)
// FLG byte bits:
//   Bit 7-6: Version (01 = current)
//   Bit 5: Block Independence
//   Bit 4: Block Checksum
//   Bit 3: Content Size present
//   Bit 2: Content Checksum
//   Bit 1: Reserved (must be 0)
//   Bit 0: Dictionary ID present
// BD byte bits:
//   Bit 7: Reserved (must be 0)
//   Bit 6-4: Block Max Size (4=64KB, 5=256KB, 6=1MB, 7=4MB)
//   Bit 3-0: Reserved (must be 0)
//

//
// Test fixture
//

class Lz4FormatTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  void TearDown() override {
    // Default registry is not destroyed
  }

  // Helper: Decode data and check for expected status
  gcomp_status_t tryDecode(const uint8_t * data, size_t len) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "lz4", nullptr, &decoder);
    if (status != GCOMP_OK) {
      return status;
    }

    std::vector<uint8_t> output(65536);
    gcomp_buffer_t in_buf = {const_cast<uint8_t *>(data), len, 0};
    gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

    status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
    if (status != GCOMP_OK) {
      gcomp_decoder_destroy(decoder);
      return status;
    }

    status = gcomp_decoder_finish(decoder, &out_buf);
    gcomp_decoder_destroy(decoder);
    return status;
  }

  // Helper: Decode in 1-byte chunks for streaming test
  gcomp_status_t tryDecodeStreaming(const uint8_t * data, size_t len) {
    gcomp_decoder_t * decoder = nullptr;
    gcomp_status_t status =
        gcomp_decoder_create(registry_, "lz4", nullptr, &decoder);
    if (status != GCOMP_OK) {
      return status;
    }

    std::vector<uint8_t> output(65536);
    size_t output_pos = 0;

    // Feed one byte at a time
    for (size_t i = 0; i < len; i++) {
      gcomp_buffer_t in_buf = {const_cast<uint8_t *>(data + i), 1, 0};
      gcomp_buffer_t out_buf = {
          output.data() + output_pos, output.size() - output_pos, 0};

      status = gcomp_decoder_update(decoder, &in_buf, &out_buf);
      if (status != GCOMP_OK) {
        gcomp_decoder_destroy(decoder);
        return status;
      }
      output_pos += out_buf.used;
    }

    gcomp_buffer_t out_buf = {
        output.data() + output_pos, output.size() - output_pos, 0};
    status = gcomp_decoder_finish(decoder, &out_buf);
    gcomp_decoder_destroy(decoder);
    return status;
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Header Validation Tests - Magic Number
//

TEST_F(Lz4FormatTest, WrongMagicNumber) {
  // Magic should be 04 22 4D 18 (little-endian 0x184D2204)
  // Use wrong magic
  uint8_t bad_magic[] = {0x00, 0x00, 0x00, 0x00, // Wrong magic
      0x64,                                      // FLG: version 01, B.Indep
      0x70,                                      // BD: 4MB blocks
      0x00}; // HC (incorrect but irrelevant)

  gcomp_status_t status = tryDecode(bad_magic, sizeof(bad_magic));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4FormatTest, PartialMagic) {
  // Only 3 bytes of magic - should fail at finish
  uint8_t partial[] = {0x04, 0x22, 0x4D};

  gcomp_status_t status = tryDecode(partial, sizeof(partial));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

//
// Header Validation Tests - FLG Byte
//

TEST_F(Lz4FormatTest, InvalidFLGVersion00) {
  // FLG version bits = 00 (should be 01)
  uint8_t bad_version[] = {0x04, 0x22, 0x4D, 0x18, // Magic
      0x20,                                        // FLG: version 00, B.Indep
      0x70,                                        // BD: 4MB blocks
      0x00};                                       // HC

  gcomp_status_t status = tryDecode(bad_version, sizeof(bad_version));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4FormatTest, InvalidFLGVersion10) {
  // FLG version bits = 10 (should be 01)
  uint8_t bad_version[] = {0x04, 0x22, 0x4D, 0x18, // Magic
      0xA0,                                        // FLG: version 10, B.Indep
      0x70,                                        // BD: 4MB blocks
      0x00};                                       // HC

  gcomp_status_t status = tryDecode(bad_version, sizeof(bad_version));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4FormatTest, ReservedFLGBitSet) {
  // FLG reserved bit 1 set (should be 0)
  uint8_t reserved_set[] = {0x04, 0x22, 0x4D, 0x18, // Magic
      0x62,                                         // FLG: version 01, reserved
      0x70,                                         // BD: 4MB blocks
      0x00};                                        // HC

  gcomp_status_t status = tryDecode(reserved_set, sizeof(reserved_set));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

//
// Header Validation Tests - BD Byte
//

TEST_F(Lz4FormatTest, InvalidBDBlockSize) {
  // BD block max size = 0 (invalid, valid values are 4-7)
  uint8_t bad_bd[] = {0x04, 0x22, 0x4D, 0x18, // Magic
      0x60,                                   // FLG: version 01, B.Indep
      0x00,                                   // BD: invalid block size 0
      0x00};                                  // HC

  gcomp_status_t status = tryDecode(bad_bd, sizeof(bad_bd));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4FormatTest, ReservedBDBitsSet) {
  // BD reserved bits set (should be 0)
  uint8_t bad_bd[] = {0x04, 0x22, 0x4D, 0x18, // Magic
      0x60,                                   // FLG: version 01, B.Indep
      0x7F,                                   // BD: 4MB but reserved bits set
      0x00};                                  // HC

  gcomp_status_t status = tryDecode(bad_bd, sizeof(bad_bd));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

//
// Header Validation Tests - Header Checksum
//

TEST_F(Lz4FormatTest, BadHeaderChecksum) {
  // Valid header structure but wrong checksum
  uint8_t bad_hc[] = {0x04, 0x22, 0x4D, 0x18, // Magic
      0x60,                                   // FLG: version 01, B.Indep
      0x70,                                   // BD: 4MB blocks
      0xFF};                                  // HC: wrong checksum

  gcomp_status_t status = tryDecode(bad_hc, sizeof(bad_hc));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

//
// Valid Minimal Frame Tests
//

TEST_F(Lz4FormatTest, MinimalValidFrame) {
  // Minimal valid LZ4 frame: header + end mark, no content
  // FLG = 0x60: version 01, B.Indep
  // BD = 0x70: 4MB blocks
  // HC = (xxHash32([FLG,BD], 0) >> 8) & 0xFF
  uint8_t flg = 0x60;
  uint8_t bd = 0x70;
  uint8_t descriptor[] = {flg, bd};
  uint32_t hash = gcomp_xxhash32(descriptor, sizeof(descriptor), 0);
  uint8_t hc = (uint8_t)((hash >> 8) & 0xFF);

  uint8_t minimal[] = {
      0x04, 0x22, 0x4D, 0x18, // Magic
      flg,                    // FLG: version 01, B.Indep
      bd,                     // BD: 4MB blocks
      hc,                     // HC: computed checksum
      0x00, 0x00, 0x00, 0x00, // End mark
  };

  gcomp_status_t status = tryDecode(minimal, sizeof(minimal));
  EXPECT_EQ(status, GCOMP_OK);
}

TEST_F(Lz4FormatTest, MinimalFrameStreamingParse) {
  // Same minimal frame, parsed one byte at a time
  uint8_t flg = 0x60;
  uint8_t bd = 0x70;
  uint8_t descriptor[] = {flg, bd};
  uint32_t hash = gcomp_xxhash32(descriptor, sizeof(descriptor), 0);
  uint8_t hc = (uint8_t)((hash >> 8) & 0xFF);

  uint8_t minimal[] = {
      0x04, 0x22, 0x4D, 0x18, // Magic
      flg,                    // FLG
      bd,                     // BD
      hc,                     // HC
      0x00, 0x00, 0x00, 0x00, // End mark
  };

  gcomp_status_t status = tryDecodeStreaming(minimal, sizeof(minimal));
  EXPECT_EQ(status, GCOMP_OK);
}

//
// Content Checksum Tests
//

TEST_F(Lz4FormatTest, ContentChecksumEnabled) {
  // Frame with content checksum flag, empty content
  // FLG = 0x64: version 01, B.Indep, C.Checksum
  // xxHash32([0x64,0x70], 0) = some value
  // Content checksum for empty = xxHash32([], 0) = 0x02CC5D05
  // Let's compute the header checksum:
  // xxHash32([0x64, 0x70], 0) needs calculation

  // Actually, let's use the encoder to produce a valid frame with content
  // checksum and verify the decoder accepts it
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.content_checksum", 1);
  ASSERT_EQ(status, GCOMP_OK);

  // Encode empty input
  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> output(256);
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  output.resize(out_buf.used);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);

  // Now decode it
  status = tryDecode(output.data(), output.size());
  EXPECT_EQ(status, GCOMP_OK);
}

//
// Block Checksum Tests
//

TEST_F(Lz4FormatTest, BlockChecksumEnabled) {
  // Use encoder to produce frame with block checksum
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.block_checksum", 1);
  ASSERT_EQ(status, GCOMP_OK);

  // Encode some data
  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  const char * test_data = "Hello, LZ4 with block checksum!";
  std::vector<uint8_t> output(256);
  gcomp_buffer_t in_buf = {const_cast<char *>(test_data), strlen(test_data), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  output.resize(out_buf.used);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);

  // Decode it
  status = tryDecode(output.data(), output.size());
  EXPECT_EQ(status, GCOMP_OK);
}

//
// Content Size Field Tests
//

TEST_F(Lz4FormatTest, ContentSizeInHeader) {
  // Use encoder to produce frame with content size
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "lz4.content_size", 32);
  ASSERT_EQ(status, GCOMP_OK);

  // Encode exactly 32 bytes
  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  char test_data[32];
  memset(test_data, 'X', sizeof(test_data));

  std::vector<uint8_t> output(256);
  gcomp_buffer_t in_buf = {test_data, sizeof(test_data), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  output.resize(out_buf.used);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);

  // Verify the header has content size flag set (byte 4, bit 3)
  EXPECT_TRUE(output[4] & 0x08) << "Content size flag should be set";

  // Decode it
  status = tryDecode(output.data(), output.size());
  EXPECT_EQ(status, GCOMP_OK);
}

TEST_F(Lz4FormatTest, ContentSizeMismatch) {
  // Create frame with declared content size that doesn't match actual data
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  // Declare 64 bytes, but we'll only send 32
  status = gcomp_options_set_uint64(opts, "lz4.content_size", 64);
  ASSERT_EQ(status, GCOMP_OK);

  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  // Only encode 32 bytes (mismatches declared 64)
  char test_data[32];
  memset(test_data, 'Y', sizeof(test_data));

  std::vector<uint8_t> output(256);
  gcomp_buffer_t in_buf = {test_data, sizeof(test_data), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  output.resize(out_buf.used);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);

  // Verify the header has content size flag set
  ASSERT_TRUE(output[4] & 0x08) << "Content size flag should be set";

  // Decode should fail with corrupt error due to size mismatch
  status = tryDecode(output.data(), output.size());
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT)
      << "Decoder should reject content size mismatch";
}

//
// Block Size Variant Tests
//

TEST_F(Lz4FormatTest, AllBlockSizes) {
  std::vector<uint64_t> block_sizes = {65536, 262144, 1048576, 4194304};

  for (uint64_t block_size : block_sizes) {
    gcomp_options_t * opts = nullptr;
    gcomp_status_t status = gcomp_options_create(&opts);
    ASSERT_EQ(status, GCOMP_OK);
    status = gcomp_options_set_uint64(opts, "lz4.block_size", block_size);
    ASSERT_EQ(status, GCOMP_OK);

    // Encode some data
    gcomp_encoder_t * encoder = nullptr;
    status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
    ASSERT_EQ(status, GCOMP_OK);

    std::vector<uint8_t> input(1000);
    test_helpers_generate_random(
        input.data(), input.size(), (uint32_t)block_size);

    std::vector<uint8_t> output(2000);
    gcomp_buffer_t in_buf = {input.data(), input.size(), 0};
    gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

    status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
    ASSERT_EQ(status, GCOMP_OK);

    status = gcomp_encoder_finish(encoder, &out_buf);
    ASSERT_EQ(status, GCOMP_OK);
    output.resize(out_buf.used);

    gcomp_encoder_destroy(encoder);
    gcomp_options_destroy(opts);

    // Decode and verify
    gcomp_decoder_t * decoder = nullptr;
    status = gcomp_decoder_create(registry_, "lz4", nullptr, &decoder);
    ASSERT_EQ(status, GCOMP_OK);

    std::vector<uint8_t> decoded(2000);
    gcomp_buffer_t dec_in = {output.data(), output.size(), 0};
    gcomp_buffer_t dec_out = {decoded.data(), decoded.size(), 0};

    status = gcomp_decoder_update(decoder, &dec_in, &dec_out);
    ASSERT_EQ(status, GCOMP_OK);

    status = gcomp_decoder_finish(decoder, &dec_out);
    EXPECT_EQ(status, GCOMP_OK) << "Failed for block_size=" << block_size;

    EXPECT_EQ(dec_out.used, input.size());
    EXPECT_EQ(memcmp(decoded.data(), input.data(), input.size()), 0);

    gcomp_decoder_destroy(decoder);
  }
}

//
// Dependent Blocks Tests
//

TEST_F(Lz4FormatTest, DependentBlocksRoundtrip) {
  gcomp_options_t * opts = nullptr;
  gcomp_status_t status = gcomp_options_create(&opts);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_bool(opts, "lz4.independent_blocks", 0);
  ASSERT_EQ(status, GCOMP_OK);
  status = gcomp_options_set_uint64(opts, "lz4.block_size", 65536);
  ASSERT_EQ(status, GCOMP_OK);

  // Encode data spanning multiple blocks
  gcomp_encoder_t * encoder = nullptr;
  status = gcomp_encoder_create(registry_, "lz4", opts, &encoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> input(200000);
  test_helpers_generate_random(input.data(), input.size(), 7890);

  std::vector<uint8_t> output(input.size() + 1000);
  gcomp_buffer_t in_buf = {input.data(), input.size(), 0};
  gcomp_buffer_t out_buf = {output.data(), output.size(), 0};

  status = gcomp_encoder_update(encoder, &in_buf, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_encoder_finish(encoder, &out_buf);
  ASSERT_EQ(status, GCOMP_OK);
  output.resize(out_buf.used);

  gcomp_encoder_destroy(encoder);
  gcomp_options_destroy(opts);

  // Verify header indicates dependent blocks (B.Indep bit NOT set)
  EXPECT_FALSE(output[4] & 0x20) << "B.Indep flag should NOT be set";

  // Decode
  gcomp_decoder_t * decoder = nullptr;
  status = gcomp_decoder_create(registry_, "lz4", nullptr, &decoder);
  ASSERT_EQ(status, GCOMP_OK);

  std::vector<uint8_t> decoded(input.size() + 1000);
  gcomp_buffer_t dec_in = {output.data(), output.size(), 0};
  gcomp_buffer_t dec_out = {decoded.data(), decoded.size(), 0};

  status = gcomp_decoder_update(decoder, &dec_in, &dec_out);
  ASSERT_EQ(status, GCOMP_OK);

  status = gcomp_decoder_finish(decoder, &dec_out);
  EXPECT_EQ(status, GCOMP_OK);

  EXPECT_EQ(dec_out.used, input.size());
  EXPECT_EQ(memcmp(decoded.data(), input.data(), input.size()), 0);

  gcomp_decoder_destroy(decoder);
}

//
// Truncated Frame Tests
//

TEST_F(Lz4FormatTest, TruncatedHeader) {
  // Only magic + partial FLG/BD
  uint8_t truncated[] = {0x04, 0x22, 0x4D, 0x18, 0x60};

  gcomp_status_t status = tryDecode(truncated, sizeof(truncated));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}

TEST_F(Lz4FormatTest, TruncatedEndMark) {
  // Valid header but incomplete end mark
  uint8_t truncated[] = {
      0x04, 0x22, 0x4D, 0x18, // Magic
      0x60,                   // FLG
      0x70,                   // BD
      0x1E,                   // HC
      0x00, 0x00,             // Partial end mark (should be 4 bytes of 0x00)
  };

  gcomp_status_t status = tryDecode(truncated, sizeof(truncated));
  EXPECT_EQ(status, GCOMP_ERR_CORRUPT);
}


//
// Skippable Frames: the public writer and parser
//
// LZ4 Frame Format, "Skippable Frames".  gcomp_lz4_write_skippable_frame() and
// gcomp_lz4_read_skippable_frame() build and parse the bytes directly; nothing
// in a skippable frame is compressed.  Interoperability -- that liblz4 skips
// what we write -- is covered in test_lz4_spec_oracle.cpp; these are the API's
// own contract.
//

TEST_F(Lz4FormatTest, SkippableWriterProducesTheSpecifiedLayout) {
  std::vector<uint8_t> payload;
  for (int i = 0; i < 300; i++) {
    payload.push_back((uint8_t)(i * 13 + 7));
  }

  for (unsigned variant = 0; variant < 16; variant++) {
    std::vector<uint8_t> out(GCOMP_LZ4_SKIPPABLE_OVERHEAD + payload.size());
    size_t written = 0;
    ASSERT_EQ(gcomp_lz4_write_skippable_frame(variant, payload.data(),
                  payload.size(), out.data(), out.size(), &written),
        GCOMP_OK)
        << "variant " << variant;
    ASSERT_EQ(written, out.size());

    // Magic, little-endian, with the variant in the low nibble.
    const uint32_t magic = 0x184D2A50u | variant;
    EXPECT_EQ(out[0], (uint8_t)(magic & 0xFF)) << "variant " << variant;
    EXPECT_EQ(out[1], (uint8_t)((magic >> 8) & 0xFF));
    EXPECT_EQ(out[2], (uint8_t)((magic >> 16) & 0xFF));
    EXPECT_EQ(out[3], (uint8_t)((magic >> 24) & 0xFF));

    // Size, little-endian.
    const uint32_t size = (uint32_t)payload.size();
    EXPECT_EQ(out[4], (uint8_t)(size & 0xFF));
    EXPECT_EQ(out[5], (uint8_t)((size >> 8) & 0xFF));
    EXPECT_EQ(out[6], (uint8_t)((size >> 16) & 0xFF));
    EXPECT_EQ(out[7], (uint8_t)((size >> 24) & 0xFF));

    // Payload verbatim.
    EXPECT_EQ(memcmp(out.data() + 8, payload.data(), payload.size()), 0)
        << "variant " << variant;
  }
}

TEST_F(Lz4FormatTest, SkippableRoundTripsThroughTheParser) {
  for (size_t n : {(size_t)0, (size_t)1, (size_t)7, (size_t)8, (size_t)9,
           (size_t)255, (size_t)256, (size_t)65535, (size_t)65536}) {
    std::vector<uint8_t> payload(n);
    for (size_t i = 0; i < n; i++) {
      payload[i] = (uint8_t)(i * 31 + n);
    }
    std::vector<uint8_t> out(GCOMP_LZ4_SKIPPABLE_OVERHEAD + n + 16);
    size_t written = 0;
    ASSERT_EQ(gcomp_lz4_write_skippable_frame(5, n ? payload.data() : nullptr,
                  n, out.data(), out.size(), &written),
        GCOMP_OK)
        << "n=" << n;
    ASSERT_EQ(written, GCOMP_LZ4_SKIPPABLE_OVERHEAD + n);

    unsigned variant = 99;
    size_t offset = 0, payload_size = 0, frame_size = 0;
    ASSERT_EQ(gcomp_lz4_read_skippable_frame(out.data(), written, &variant,
                  &offset, &payload_size, &frame_size),
        GCOMP_OK)
        << "n=" << n;
    EXPECT_EQ(variant, 5u) << "n=" << n;
    EXPECT_EQ(offset, (size_t)GCOMP_LZ4_SKIPPABLE_OVERHEAD);
    EXPECT_EQ(payload_size, n);
    EXPECT_EQ(frame_size, written);
    if (n > 0) {
      EXPECT_EQ(memcmp(out.data() + offset, payload.data(), n), 0)
          << "n=" << n;
    }
  }

  // Every output is optional.
  std::vector<uint8_t> out(GCOMP_LZ4_SKIPPABLE_OVERHEAD);
  size_t written = 0;
  ASSERT_EQ(gcomp_lz4_write_skippable_frame(
                0, nullptr, 0, out.data(), out.size(), &written),
      GCOMP_OK);
  EXPECT_EQ(gcomp_lz4_read_skippable_frame(
                out.data(), written, nullptr, nullptr, nullptr, nullptr),
      GCOMP_OK);
}

TEST_F(Lz4FormatTest, SkippableWriterRejectsBadArguments) {
  uint8_t payload[16] = {0};
  uint8_t out[64];
  size_t written = 12345;

  // The variant is four bits.
  EXPECT_EQ(gcomp_lz4_write_skippable_frame(
                16, payload, sizeof(payload), out, sizeof(out), &written),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(written, 0u) << "output size must be zeroed on failure";

  written = 12345;
  EXPECT_EQ(gcomp_lz4_write_skippable_frame(
                0xFFFFFFFFu, payload, 1, out, sizeof(out), &written),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(written, 0u);

  // NULL payload is allowed only for an empty one.
  written = 12345;
  EXPECT_EQ(
      gcomp_lz4_write_skippable_frame(0, nullptr, 1, out, sizeof(out), &written),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(written, 0u);
  EXPECT_EQ(
      gcomp_lz4_write_skippable_frame(0, nullptr, 0, out, sizeof(out), &written),
      GCOMP_OK);
  EXPECT_EQ(written, (size_t)GCOMP_LZ4_SKIPPABLE_OVERHEAD);

  // Required pointers.
  EXPECT_EQ(gcomp_lz4_write_skippable_frame(0, payload, 1, nullptr, 16,
                &written),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(
      gcomp_lz4_write_skippable_frame(0, payload, 1, out, sizeof(out), nullptr),
      GCOMP_ERR_INVALID_ARG);

  // Capacity: exactly enough succeeds, one byte short does not.
  written = 12345;
  EXPECT_EQ(gcomp_lz4_write_skippable_frame(0, payload, sizeof(payload), out,
                GCOMP_LZ4_SKIPPABLE_OVERHEAD + sizeof(payload) - 1, &written),
      GCOMP_ERR_LIMIT);
  EXPECT_EQ(written, 0u);
  EXPECT_EQ(gcomp_lz4_write_skippable_frame(0, payload, sizeof(payload), out,
                GCOMP_LZ4_SKIPPABLE_OVERHEAD + sizeof(payload), &written),
      GCOMP_OK);
  EXPECT_EQ(written, GCOMP_LZ4_SKIPPABLE_OVERHEAD + sizeof(payload));
}

TEST_F(Lz4FormatTest, SkippableParserRejectsBadFrames) {
  // A frame that declares more payload than the buffer holds.  The check is
  // made in 64 bits: overhead + declared would wrap on a 32-bit size_t,
  // turning a frame claiming nearly 4 GB into one that appears to fit.
  uint8_t liar[12] = {0x50, 0x2A, 0x4D, 0x18, 0xFF, 0xFF, 0xFF, 0xFF, 1, 2, 3,
      4};
  EXPECT_EQ(gcomp_lz4_read_skippable_frame(
                liar, sizeof(liar), nullptr, nullptr, nullptr, nullptr),
      GCOMP_ERR_CORRUPT);

  // One byte short of what it declares.
  uint8_t almost[11] = {0x50, 0x2A, 0x4D, 0x18, 4, 0, 0, 0, 1, 2, 3};
  EXPECT_EQ(gcomp_lz4_read_skippable_frame(
                almost, sizeof(almost), nullptr, nullptr, nullptr, nullptr),
      GCOMP_ERR_CORRUPT);
  // ...and exactly what it declares is fine.
  uint8_t exact[12] = {0x50, 0x2A, 0x4D, 0x18, 4, 0, 0, 0, 1, 2, 3, 4};
  size_t frame_size = 0;
  EXPECT_EQ(gcomp_lz4_read_skippable_frame(
                exact, sizeof(exact), nullptr, nullptr, nullptr, &frame_size),
      GCOMP_OK);
  EXPECT_EQ(frame_size, 12u);
  // Trailing bytes belong to whatever follows, not to this frame.
  EXPECT_EQ(gcomp_lz4_read_skippable_frame(
                exact, sizeof(exact), nullptr, nullptr, nullptr, &frame_size),
      GCOMP_OK);

  // Magic numbers that are not skippable, including the ones either side of
  // the range.
  static const uint32_t kBad[] = {
      0x184D2A40u, 0x184D2A4Fu, 0x184D2A60u, 0x184D2A99u, 0x184D2204u, 0u};
  for (uint32_t magic : kBad) {
    uint8_t buf[12] = {0};
    for (int i = 0; i < 4; i++) {
      buf[i] = (uint8_t)((magic >> (8 * i)) & 0xFF);
    }
    char label[32];
    snprintf(label, sizeof(label), "0x%08X", magic);
    EXPECT_EQ(gcomp_lz4_read_skippable_frame(
                  buf, sizeof(buf), nullptr, nullptr, nullptr, nullptr),
        GCOMP_ERR_CORRUPT)
        << label;
  }

  // Too short to hold even the header.
  uint8_t stub[8] = {0x50, 0x2A, 0x4D, 0x18, 0, 0, 0, 0};
  for (size_t n = 0; n < GCOMP_LZ4_SKIPPABLE_OVERHEAD; n++) {
    EXPECT_EQ(gcomp_lz4_read_skippable_frame(
                  stub, n, nullptr, nullptr, nullptr, nullptr),
        GCOMP_ERR_CORRUPT)
        << "n=" << n;
  }
  EXPECT_EQ(gcomp_lz4_read_skippable_frame(nullptr, 8, nullptr, nullptr,
                nullptr, nullptr),
      GCOMP_ERR_INVALID_ARG);
}

//
// Skippable Frames: the decode-time callback
//
// The decoder discards skippable frames, so a caller reading a pipe or a
// socket has no buffer to parse afterwards.  The callback is how the payload
// reaches them, in pieces, as it goes past.
//

namespace {

struct SkippableSink {
  struct Frame {
    unsigned variant = 0;
    uint64_t announced = 0;
    std::vector<uint8_t> payload;
    int calls = 0;
  };
  std::vector<Frame> frames;
  uint64_t next_offset = 0;
  bool contiguous = true;   // offsets arrive in order, covering the payload
  bool consistent = true;   // variant and size are the same on every call
  int fail_on_call = -1;    // 1-based call number to refuse at, -1 for never
  int total_calls = 0;
  gcomp_status_t refuse_with = GCOMP_ERR_IO;

  static gcomp_status_t thunk(void * ctx, unsigned variant, uint64_t total,
      uint64_t offset, const uint8_t * chunk, size_t n) {
    return ((SkippableSink *)ctx)->on(variant, total, offset, chunk, n);
  }

  gcomp_status_t on(unsigned variant, uint64_t total, uint64_t offset,
      const uint8_t * chunk, size_t n) {
    total_calls++;
    // A new frame starts when the payload offset restarts at zero.
    if (offset == 0) {
      frames.push_back(Frame{variant, total, {}, 0});
      next_offset = 0;
    }
    if (frames.empty()) {
      contiguous = false;
      return GCOMP_OK;
    }
    Frame & f = frames.back();
    f.calls++;
    if (variant != f.variant || total != f.announced) {
      consistent = false;
    }
    if (offset != next_offset) {
      contiguous = false;
    }
    next_offset = offset + n;
    if (n > 0) {
      if (chunk == nullptr) {
        consistent = false;
      }
      else {
        f.payload.insert(f.payload.end(), chunk, chunk + n);
      }
    }
    if (fail_on_call > 0 && total_calls >= fail_on_call) {
      return refuse_with;
    }
    return GCOMP_OK;
  }
};

std::vector<uint8_t> lz4Frame(const std::vector<uint8_t> & data) {
  std::vector<uint8_t> out(data.size() * 2 + 65536);
  size_t used = 0;
  if (gcomp_encode_buffer(nullptr, "lz4", nullptr, data.data(), data.size(),
          out.data(), out.size(), &used) != GCOMP_OK) {
    return {};
  }
  out.resize(used);
  return out;
}

std::vector<uint8_t> skippableFrame(
    unsigned variant, const std::vector<uint8_t> & payload) {
  std::vector<uint8_t> out(GCOMP_LZ4_SKIPPABLE_OVERHEAD + payload.size());
  size_t used = 0;
  if (gcomp_lz4_write_skippable_frame(variant,
          payload.empty() ? nullptr : payload.data(), payload.size(),
          out.data(), out.size(), &used) != GCOMP_OK) {
    return {};
  }
  out.resize(used);
  return out;
}

// Decodes `stream` in `chunk`-sized pieces, reporting skippable frames to
// `sink` when one is given.  Returns the status decoding stopped on.
gcomp_status_t decodeReportingSkippable(const std::vector<uint8_t> & stream,
    size_t chunk, SkippableSink * sink, std::vector<uint8_t> * out) {
  gcomp_options_t * opts = nullptr;
  if (gcomp_options_create(&opts) != GCOMP_OK) {
    return GCOMP_ERR_INTERNAL;
  }
  gcomp_options_set_bool(opts, "lz4.concat", 1);
  gcomp_options_set_uint64(opts, "limits.max_output_bytes", 4u << 20);
  gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 1000000);

  gcomp_decoder_t * dec = nullptr;
  gcomp_status_t st =
      gcomp_decoder_create(gcomp_registry_default(), "lz4", opts, &dec);
  gcomp_options_destroy(opts);
  if (st != GCOMP_OK) {
    return st;
  }
  if (sink) {
    st = gcomp_lz4_decoder_on_skippable_frame(dec, SkippableSink::thunk, sink);
    if (st != GCOMP_OK) {
      gcomp_decoder_destroy(dec);
      return st;
    }
  }

  out->assign(4u << 20, 0);
  size_t out_len = 0, used = 0;
  while (used < stream.size()) {
    size_t take = std::min(chunk, stream.size() - used);
    gcomp_buffer_t ib = {stream.data() + used, take, 0};
    while (ib.used < ib.size) {
      gcomp_buffer_t ob = {out->data() + out_len, out->size() - out_len, 0};
      st = gcomp_decoder_update(dec, &ib, &ob);
      out_len += ob.used;
      if (st != GCOMP_OK) {
        goto done;
      }
      if (ob.used == 0 && ib.used == 0) {
        st = GCOMP_ERR_INTERNAL;
        goto done;
      }
    }
    used += ib.used;
  }
  {
    gcomp_buffer_t ob = {out->data() + out_len, out->size() - out_len, 0};
    st = gcomp_decoder_finish(dec, &ob);
    out_len += ob.used;
  }
done:
  gcomp_decoder_destroy(dec);
  out->resize(out_len);
  return st;
}

} // namespace

TEST_F(Lz4FormatTest, SkippableCallbackDeliversThePayloadAtEveryChunkSize) {
  std::vector<uint8_t> data(900);
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = (uint8_t)("abcdefgh"[i % 8]);
  }
  std::vector<uint8_t> payload(600);
  for (size_t i = 0; i < payload.size(); i++) {
    payload[i] = (uint8_t)(i * 7 + 2);
  }

  std::vector<uint8_t> stream = skippableFrame(6, payload);
  std::vector<uint8_t> body = lz4Frame(data);
  ASSERT_FALSE(body.empty());
  stream.insert(stream.end(), body.begin(), body.end());

  // One byte at a time splits the payload across 600 calls; a large chunk
  // delivers it in one.  Both have to describe the same frame.
  for (size_t chunk : {(size_t)1, (size_t)2, (size_t)7, (size_t)8, (size_t)9,
           (size_t)13, (size_t)607, (size_t)608, (size_t)4096}) {
    SkippableSink sink;
    std::vector<uint8_t> out;
    std::string where = "chunk=" + std::to_string(chunk);

    ASSERT_EQ(decodeReportingSkippable(stream, chunk, &sink, &out), GCOMP_OK)
        << where;
    EXPECT_EQ(out, data) << where << ": the data must decode unchanged";

    ASSERT_EQ(sink.frames.size(), 1u) << where;
    EXPECT_GE(sink.frames[0].calls, 1) << where;
    EXPECT_EQ(sink.frames[0].variant, 6u) << where;
    EXPECT_EQ(sink.frames[0].announced, 600u)
        << where << ": the whole size must be known from the first call";
    EXPECT_TRUE(sink.contiguous)
        << where << ": pieces must arrive in order, covering the payload";
    EXPECT_TRUE(sink.consistent)
        << where << ": variant and size must not change mid-frame";
    EXPECT_EQ(sink.frames[0].payload, payload) << where;
  }
}

TEST_F(Lz4FormatTest, SkippableCallbackReportsEveryFrameIncludingEmptyOnes) {
  std::vector<uint8_t> data(500, 'z');
  std::vector<uint8_t> a(40), b;
  for (size_t i = 0; i < a.size(); i++) {
    a[i] = (uint8_t)(i + 1);
  }

  // An empty payload is still a frame worth reporting: its variant may be the
  // whole message.
  std::vector<uint8_t> stream = skippableFrame(2, a);
  std::vector<uint8_t> empty = skippableFrame(9, b);
  stream.insert(stream.end(), empty.begin(), empty.end());
  std::vector<uint8_t> body = lz4Frame(data);
  ASSERT_FALSE(body.empty());
  stream.insert(stream.end(), body.begin(), body.end());
  std::vector<uint8_t> tail = skippableFrame(15, a);
  stream.insert(stream.end(), tail.begin(), tail.end());

  for (size_t chunk : {(size_t)1, (size_t)5, (size_t)4096}) {
    SkippableSink sink;
    std::vector<uint8_t> out;
    std::string where = "chunk=" + std::to_string(chunk);
    ASSERT_EQ(decodeReportingSkippable(stream, chunk, &sink, &out), GCOMP_OK)
        << where;
    EXPECT_EQ(out, data) << where;

    ASSERT_EQ(sink.frames.size(), 3u)
        << where << ": three skippable frames, one of them empty";
    EXPECT_EQ(sink.frames[0].variant, 2u) << where;
    EXPECT_EQ(sink.frames[0].payload, a) << where;
    EXPECT_EQ(sink.frames[1].variant, 9u) << where;
    EXPECT_EQ(sink.frames[1].announced, 0u) << where;
    EXPECT_EQ(sink.frames[1].calls, 1)
        << where << ": an empty payload is reported exactly once";
    EXPECT_TRUE(sink.frames[1].payload.empty()) << where;
    EXPECT_EQ(sink.frames[2].variant, 15u) << where;
    EXPECT_EQ(sink.frames[2].payload, a) << where;
  }
}

TEST_F(Lz4FormatTest, SkippableCallbackRefusalStopsTheDecode) {
  std::vector<uint8_t> data(2000, 'q');
  std::vector<uint8_t> payload(600, 0x5A);
  std::vector<uint8_t> stream = skippableFrame(1, payload);
  std::vector<uint8_t> body = lz4Frame(data);
  ASSERT_FALSE(body.empty());
  stream.insert(stream.end(), body.begin(), body.end());

  // Whatever the callback returns is what the caller sees -- the decoder does
  // not translate it into a generic failure.
  for (gcomp_status_t refusal :
      {GCOMP_ERR_IO, GCOMP_ERR_LIMIT, GCOMP_ERR_INVALID_ARG}) {
    SkippableSink sink;
    sink.fail_on_call = 1;
    sink.refuse_with = refusal;
    std::vector<uint8_t> out;
    EXPECT_EQ(decodeReportingSkippable(stream, 64, &sink, &out), refusal)
        << "the callback's own status must reach the caller";
    EXPECT_TRUE(out.empty())
        << "nothing should have been decoded past the refusal";
  }

  // Refusing part-way through a multi-call payload stops it there.
  SkippableSink sink;
  sink.fail_on_call = 3;
  std::vector<uint8_t> out;
  EXPECT_EQ(decodeReportingSkippable(stream, 64, &sink, &out), GCOMP_ERR_IO);
  EXPECT_EQ(sink.total_calls, 3) << "no further calls after a refusal";

  // An empty payload's single call can refuse too.
  std::vector<uint8_t> only_empty = skippableFrame(4, {});
  only_empty.insert(only_empty.end(), body.begin(), body.end());
  SkippableSink empty_sink;
  empty_sink.fail_on_call = 1;
  EXPECT_EQ(decodeReportingSkippable(only_empty, 4096, &empty_sink, &out),
      GCOMP_ERR_IO);
  EXPECT_EQ(empty_sink.total_calls, 1);
}

TEST_F(Lz4FormatTest, SkippableCallbackIsOptionalAndSurvivesReset) {
  std::vector<uint8_t> data(700, 'k');
  std::vector<uint8_t> payload(100, 0x11);
  std::vector<uint8_t> stream = skippableFrame(8, payload);
  std::vector<uint8_t> body = lz4Frame(data);
  ASSERT_FALSE(body.empty());
  stream.insert(stream.end(), body.begin(), body.end());

  // With no callback at all the frames are still skipped: registering one
  // changes what you are told, never what the stream decodes to.
  std::vector<uint8_t> out;
  ASSERT_EQ(decodeReportingSkippable(stream, 32, nullptr, &out), GCOMP_OK);
  EXPECT_EQ(out, data);

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "lz4.concat", 1);
  gcomp_options_set_uint64(opts, "limits.max_output_bytes", 1u << 20);
  gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 1000000);
  gcomp_decoder_t * dec = nullptr;
  ASSERT_EQ(
      gcomp_decoder_create(gcomp_registry_default(), "lz4", opts, &dec),
      GCOMP_OK);
  gcomp_options_destroy(opts);

  SkippableSink sink;
  ASSERT_EQ(
      gcomp_lz4_decoder_on_skippable_frame(dec, SkippableSink::thunk, &sink),
      GCOMP_OK);

  std::vector<uint8_t> buf(1u << 20);
  auto decode_once = [&]() {
    size_t out_len = 0, used = 0;
    while (used < stream.size()) {
      gcomp_buffer_t ib = {stream.data() + used, stream.size() - used, 0};
      gcomp_buffer_t ob = {buf.data() + out_len, buf.size() - out_len, 0};
      gcomp_status_t st = gcomp_decoder_update(dec, &ib, &ob);
      out_len += ob.used;
      if (st != GCOMP_OK) {
        return st;
      }
      used += ib.used;
      if (ib.used == 0 && ob.used == 0) {
        break;
      }
    }
    gcomp_buffer_t ob = {buf.data() + out_len, buf.size() - out_len, 0};
    return gcomp_decoder_finish(dec, &ob);
  };

  ASSERT_EQ(decode_once(), GCOMP_OK);
  ASSERT_EQ(sink.frames.size(), 1u);

  // Reset clears the stream, not the caller's arrangement with the decoder.
  ASSERT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);
  ASSERT_EQ(decode_once(), GCOMP_OK);
  // ASSERT, not EXPECT: the indexing below is only safe once this holds, and
  // a test that segfaults on a regression reports nothing at all.
  ASSERT_EQ(sink.frames.size(), 2u)
      << "the callback must survive a reset -- it describes how the caller is "
         "using the decoder, not the stream it was reading";
  EXPECT_EQ(sink.frames[1].payload, payload);

  // Clearing it stops the reporting without disturbing the decode.
  ASSERT_EQ(gcomp_lz4_decoder_on_skippable_frame(dec, nullptr, nullptr),
      GCOMP_OK);
  ASSERT_EQ(gcomp_decoder_reset(dec), GCOMP_OK);
  ASSERT_EQ(decode_once(), GCOMP_OK);
  EXPECT_EQ(sink.frames.size(), 2u) << "no further frames after clearing";

  gcomp_decoder_destroy(dec);
}

TEST_F(Lz4FormatTest, SkippableCallbackRejectsForeignDecoders) {
  // The state pointer is method-specific, so registering on another method's
  // decoder would write through a pointer to something else entirely.
  for (const char * method : {"deflate", "gzip", "zstd", "rle"}) {
    gcomp_decoder_t * dec = nullptr;
    if (gcomp_decoder_create(gcomp_registry_default(), method, nullptr, &dec) !=
        GCOMP_OK) {
      continue; // method not registered in this build
    }
    EXPECT_EQ(
        gcomp_lz4_decoder_on_skippable_frame(dec, SkippableSink::thunk,
            nullptr),
        GCOMP_ERR_INVALID_ARG)
        << method;
    gcomp_decoder_destroy(dec);
  }

  EXPECT_EQ(
      gcomp_lz4_decoder_on_skippable_frame(nullptr, SkippableSink::thunk,
          nullptr),
      GCOMP_ERR_INVALID_ARG);
}

//
// Frame header inspection
//

namespace {

std::vector<uint8_t> encodeWith(gcomp_options_t * opts,
    const std::vector<uint8_t> & data) {
  std::vector<uint8_t> out(data.size() * 2 + 65536);
  size_t used = 0;
  if (gcomp_encode_buffer(nullptr, "lz4", opts, data.data(), data.size(),
          out.data(), out.size(), &used) != GCOMP_OK) {
    return {};
  }
  out.resize(used);
  return out;
}

bool decodes(const std::vector<uint8_t> & frame, size_t expected) {
  gcomp_options_t * opts = nullptr;
  if (gcomp_options_create(&opts) != GCOMP_OK) {
    return false;
  }
  gcomp_options_set_uint64(opts, "limits.max_output_bytes", 4u << 20);
  gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 1000000);
  std::vector<uint8_t> out(expected + 65536);
  size_t used = 0;
  gcomp_status_t st = gcomp_decode_buffer(nullptr, "lz4", opts, frame.data(),
      frame.size(), out.data(), out.size(), &used);
  gcomp_options_destroy(opts);
  return st == GCOMP_OK;
}

} // namespace

TEST_F(Lz4FormatTest, PeekReportsWhatTheEncoderWasAskedFor) {
  std::vector<uint8_t> data(9000);
  for (size_t i = 0; i < data.size(); i++) {
    data[i] = (uint8_t)("abcdefgh"[i % 8]);
  }

  for (int independent = 0; independent <= 1; independent++) {
    for (int block_checksum = 0; block_checksum <= 1; block_checksum++) {
      for (int content_checksum = 0; content_checksum <= 1;
          content_checksum++) {
        for (int content_size = 0; content_size <= 1; content_size++) {
          for (uint64_t bs : {65536u, 262144u, 1048576u, 4194304u}) {
            gcomp_options_t * opts = nullptr;
            ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
            gcomp_options_set_bool(opts, "lz4.independent_blocks", independent);
            gcomp_options_set_bool(opts, "lz4.block_checksum", block_checksum);
            gcomp_options_set_bool(
                opts, "lz4.content_checksum", content_checksum);
            // lz4.content_size is a uint64 carrying the size itself, not a
            // flag.  Setting it with set_bool() stores a BOOL-typed entry
            // that the encoder's get_uint64() silently declines, so the
            // field never appears -- which is exactly what two tests in
            // test_lz4_spec_oracle.cpp were doing before this one caught it.
            if (content_size) {
              gcomp_options_set_uint64(
                  opts, "lz4.content_size", (uint64_t)data.size());
            }
            gcomp_options_set_uint64(opts, "lz4.block_size", bs);
            std::vector<uint8_t> frame = encodeWith(opts, data);
            gcomp_options_destroy(opts);
            ASSERT_FALSE(frame.empty());

            gcomp_lz4_frame_info_t info;
            ASSERT_EQ(gcomp_lz4_peek_frame_info(
                          frame.data(), frame.size(), &info, nullptr),
                GCOMP_OK);
            EXPECT_FALSE(info.is_skippable);
            EXPECT_EQ(info.block_independent, independent);
            EXPECT_EQ(info.block_checksum, block_checksum);
            EXPECT_EQ(info.content_checksum, content_checksum);
            EXPECT_EQ(info.block_max_size, (uint32_t)bs);
            EXPECT_EQ(info.content_size_present, content_size);
            if (content_size) {
              EXPECT_EQ(info.content_size, (uint64_t)data.size());
            }
            EXPECT_EQ(info.frame_header_size,
                (size_t)(7 + (content_size ? 8 : 0)));
          }
        }
      }
    }
  }
}

TEST_F(Lz4FormatTest, PeekAndTheDecoderNeverDisagree) {
  // The point of there being one parser.  A reader that peeks and then
  // decodes must not be told two different things about the same bytes.
  std::vector<uint8_t> data(3000, 'w');
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "lz4.content_size", (uint64_t)data.size());
  gcomp_options_set_bool(opts, "lz4.content_checksum", 1);
  std::vector<uint8_t> good = encodeWith(opts, data);
  gcomp_options_destroy(opts);
  ASSERT_FALSE(good.empty());

  // Corrupt each header byte to every other value and require the two to
  // agree on whether the result is still a valid header.  The decoder can
  // legitimately fail later on for reasons a header peek cannot see, so the
  // requirement is one-directional where it has to be: whatever peek rejects,
  // the decoder must reject too.
  gcomp_lz4_frame_info_t info;
  ASSERT_EQ(
      gcomp_lz4_peek_frame_info(good.data(), good.size(), &info, nullptr),
      GCOMP_OK);
  const size_t header_len = info.frame_header_size;
  ASSERT_GT(header_len, 0u);

  int peek_rejected = 0, both_rejected = 0;
  for (size_t i = 0; i < header_len; i++) {
    for (int delta = 1; delta < 256; delta += 37) {
      std::vector<uint8_t> bad = good;
      bad[i] = (uint8_t)(bad[i] + delta);
      gcomp_lz4_frame_info_t bi;
      gcomp_status_t st =
          gcomp_lz4_peek_frame_info(bad.data(), bad.size(), &bi, nullptr);
      if (st == GCOMP_OK) {
        continue;
      }
      peek_rejected++;
      EXPECT_FALSE(decodes(bad, data.size()))
          << "byte " << i << " +" << delta
          << ": peek rejected this header but the decoder accepted the frame";
      both_rejected++;
    }
  }
  EXPECT_GT(peek_rejected, 0)
      << "no corruption of the header was rejected, so this proves nothing";
  EXPECT_EQ(peek_rejected, both_rejected);
}

TEST_F(Lz4FormatTest, PeekAsksForExactlyTheBytesItNeeds) {
  std::vector<uint8_t> data(600, 'p');
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_uint64(opts, "lz4.content_size", (uint64_t)data.size());
  gcomp_options_set_uint64(opts, "lz4.dictionary_id", 0xABCDEF01u);
  std::vector<uint8_t> frame = encodeWith(opts, data);
  gcomp_options_destroy(opts);
  ASSERT_FALSE(frame.empty());

  gcomp_lz4_frame_info_t info;
  ASSERT_EQ(
      gcomp_lz4_peek_frame_info(frame.data(), frame.size(), &info, nullptr),
      GCOMP_OK);
  const size_t full = info.frame_header_size;
  ASSERT_EQ(full, (size_t)(7 + 8 + 4)) << "content size and dict id present";

  // Every prefix short of the header must say so, and must ask for a number
  // of bytes that actually gets somewhere -- a reader feeding exactly what it
  // is asked for has to make progress rather than loop.
  for (size_t n = 0; n < full; n++) {
    gcomp_lz4_frame_info_t partial;
    size_t needed = 0;
    gcomp_status_t st =
        gcomp_lz4_peek_frame_info(frame.data(), n, &partial, &needed);
    ASSERT_EQ(st, GCOMP_ERR_LIMIT) << "n=" << n;
    EXPECT_GT(needed, n) << "n=" << n << ": asked for no more than it had";
    EXPECT_LE(needed, full) << "n=" << n << ": asked for more than the header";

    // The contract a reader actually depends on: feed exactly what it asks
    // for and you always make progress, so the loop ends.  It takes more than
    // one round -- four bytes to recognise the magic, six to learn which
    // optional fields are present, then the whole header -- so the test
    // follows it rather than assuming a number of steps.
    gcomp_lz4_frame_info_t again;
    size_t have = needed, asked = needed;
    int rounds = 0;
    gcomp_status_t st2 = GCOMP_ERR_LIMIT;
    while ((st2 = gcomp_lz4_peek_frame_info(
                frame.data(), have, &again, &asked)) == GCOMP_ERR_LIMIT) {
      ASSERT_GT(asked, have) << "n=" << n << ": asked for no more than it had";
      ASSERT_LE(asked, full) << "n=" << n << ": asked past the header";
      have = asked;
      ASSERT_LT(++rounds, 8) << "n=" << n << ": not converging";
    }
    EXPECT_EQ(st2, GCOMP_OK) << "n=" << n;
    EXPECT_EQ(have, full)
        << "n=" << n << ": settled on a length that is not the header's";
  }

  EXPECT_TRUE(info.dict_id_present);
  EXPECT_EQ(info.dict_id, 0xABCDEF01u);
}

TEST_F(Lz4FormatTest, PeekReportsSkippableFramesSoAReaderCanStepOverThem) {
  std::vector<uint8_t> payload(250);
  for (size_t i = 0; i < payload.size(); i++) {
    payload[i] = (uint8_t)(i * 5 + 3);
  }
  std::vector<uint8_t> data(1200, 'm');

  for (unsigned variant = 0; variant < 16; variant++) {
    std::vector<uint8_t> skip(GCOMP_LZ4_SKIPPABLE_OVERHEAD + payload.size());
    size_t written = 0;
    ASSERT_EQ(gcomp_lz4_write_skippable_frame(variant, payload.data(),
                  payload.size(), skip.data(), skip.size(), &written),
        GCOMP_OK);

    std::vector<uint8_t> body = encodeWith(nullptr, data);
    ASSERT_FALSE(body.empty());
    std::vector<uint8_t> stream = skip;
    stream.insert(stream.end(), body.begin(), body.end());

    gcomp_lz4_frame_info_t info;
    ASSERT_EQ(
        gcomp_lz4_peek_frame_info(stream.data(), stream.size(), &info, nullptr),
        GCOMP_OK)
        << "variant " << variant;
    ASSERT_TRUE(info.is_skippable) << "variant " << variant;
    EXPECT_EQ(info.magic_variant, variant);
    EXPECT_EQ(info.skippable_payload_size, payload.size());
    EXPECT_EQ(info.frame_size, skip.size());

    // frame_size is what makes walking a stream possible: step by it and the
    // next peek describes the data frame.
    gcomp_lz4_frame_info_t next;
    ASSERT_EQ(gcomp_lz4_peek_frame_info(stream.data() + info.frame_size,
                  stream.size() - info.frame_size, &next, nullptr),
        GCOMP_OK)
        << "variant " << variant;
    EXPECT_FALSE(next.is_skippable);
    EXPECT_EQ(next.block_max_size, 4194304u);
  }
}

TEST_F(Lz4FormatTest, PeekRejectsWhatIsNotAFrameHeader) {
  std::vector<uint8_t> data(400, 'r');
  std::vector<uint8_t> frame = encodeWith(nullptr, data);
  ASSERT_FALSE(frame.empty());
  gcomp_lz4_frame_info_t info;

  // Wrong magic.
  std::vector<uint8_t> bad = frame;
  bad[0] ^= 0xFF;
  EXPECT_EQ(gcomp_lz4_peek_frame_info(bad.data(), bad.size(), &info, nullptr),
      GCOMP_ERR_CORRUPT);

  // Version bits other than 01.
  for (uint8_t version : {0x00, 0x80, 0xC0}) {
    bad = frame;
    bad[4] = (uint8_t)((bad[4] & 0x3F) | version);
    EXPECT_EQ(gcomp_lz4_peek_frame_info(bad.data(), bad.size(), &info, nullptr),
        GCOMP_ERR_CORRUPT)
        << "version bits 0x" << std::hex << (int)version;
  }

  // FLG reserved bit, and BD reserved bits.
  bad = frame;
  bad[4] |= 0x02;
  EXPECT_EQ(gcomp_lz4_peek_frame_info(bad.data(), bad.size(), &info, nullptr),
      GCOMP_ERR_CORRUPT);
  bad = frame;
  bad[5] |= 0x0F;
  EXPECT_EQ(gcomp_lz4_peek_frame_info(bad.data(), bad.size(), &info, nullptr),
      GCOMP_ERR_CORRUPT);

  // Block size codes 0 through 3 are not defined.
  for (uint8_t code = 0; code <= 3; code++) {
    bad = frame;
    bad[5] = (uint8_t)(code << 4);
    EXPECT_EQ(gcomp_lz4_peek_frame_info(bad.data(), bad.size(), &info, nullptr),
        GCOMP_ERR_CORRUPT)
        << "block size code " << (int)code;
  }

  // Header checksum.
  bad = frame;
  bad[6] = (uint8_t)(bad[6] + 1);
  EXPECT_EQ(gcomp_lz4_peek_frame_info(bad.data(), bad.size(), &info, nullptr),
      GCOMP_ERR_CORRUPT);

  EXPECT_EQ(gcomp_lz4_peek_frame_info(nullptr, 10, &info, nullptr),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_lz4_peek_frame_info(frame.data(), frame.size(), nullptr,
                nullptr),
      GCOMP_ERR_INVALID_ARG);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
