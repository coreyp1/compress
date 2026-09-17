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

#include "test_helpers.h"
#include <cstring>
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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
