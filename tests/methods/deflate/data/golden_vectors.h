/**
 * @file golden_vectors.h
 *
 * Golden test vectors for DEFLATE decoder validation.
 *
 * These vectors were generated using Python's zlib module with known inputs.
 * They serve as cross-validation to ensure our decoder produces correct output.
 *
 * Generation script example (Python 3):
 *   import zlib
 *   # For raw deflate (no zlib/gzip wrapper):
 *   c = zlib.compressobj(level, zlib.DEFLATED, -15)  # -15 = raw deflate
 *   compressed = c.compress(data) + c.flush()
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_GOLDEN_VECTORS_H
#define GHOTI_IO_GCOMP_GOLDEN_VECTORS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A golden test vector with compressed and expected decompressed data.
 */
typedef struct {
  const char * name;
  const char * description;
  const uint8_t * compressed;
  size_t compressed_len;
  const uint8_t * expected;
  size_t expected_len;
} gcomp_golden_vector_t;

//
// Vector 1: Empty input (level 0 stored block)
// Input: (empty)
// Compressed with: zlib.compressobj(0, zlib.DEFLATED, -15)
//
static const uint8_t golden_v1_compressed[] = {0x01, 0x00, 0x00, 0xFF, 0xFF};
// Note: expected is empty - use nullptr and length 0

//
// Vector 2: Single byte 'A' (level 6 fixed Huffman)
// Input: "A"
// Compressed with: zlib.compressobj(6, zlib.DEFLATED, -15)
//
static const uint8_t golden_v2_compressed[] = {0x73, 0x04, 0x00};
static const uint8_t golden_v2_expected[] = {'A'};

//
// Vector 3: "Hello" (level 0 stored block)
// Input: "Hello"
// Compressed with: zlib.compressobj(0, zlib.DEFLATED, -15)
//
static const uint8_t golden_v3_compressed[] = {
    0x01, 0x05, 0x00, 0xFA, 0xFF, 'H', 'e', 'l', 'l', 'o'};
static const uint8_t golden_v3_expected[] = {'H', 'e', 'l', 'l', 'o'};

//
// Vector 4: "Hello, world!" (level 6 fixed Huffman with matches)
// Input: "Hello, world!"
// Compressed with: zlib.compressobj(6, zlib.DEFLATED, -15)
//
static const uint8_t golden_v4_compressed[] = {0xF3, 0x48, 0xCD, 0xC9, 0xC9,
    0xD7, 0x51, 0x28, 0xCF, 0x2F, 0xCA, 0x49, 0x51, 0x04, 0x00};
static const uint8_t golden_v4_expected[] = {
    'H', 'e', 'l', 'l', 'o', ',', ' ', 'w', 'o', 'r', 'l', 'd', '!'};

//
// Vector 5: Repeated pattern "ABCABCABC" (level 6, should use back-references)
// Input: "ABCABCABCABCABC" (15 chars)
// Compressed with: zlib.compressobj(6, zlib.DEFLATED, -15)
//
static const uint8_t golden_v5_compressed[] = {
    0x73, 0x74, 0x72, 0x76, 0x44, 0x42, 0x00};
static const uint8_t golden_v5_expected[] = {
    'A', 'B', 'C', 'A', 'B', 'C', 'A', 'B', 'C', 'A', 'B', 'C', 'A', 'B', 'C'};

//
// Vector 6: All zeros (100 bytes) - highly compressible
// Input: 100 zero bytes
// Compressed with: zlib.compressobj(6, zlib.DEFLATED, -15)
//
static const uint8_t golden_v6_compressed[] = {
    0x63, 0x60, 0xA0, 0x3D, 0x00, 0x00};
static const uint8_t golden_v6_expected[100] = {0};

//
// Vector 7: Binary sequence 0x00-0xFF (256 bytes)
// Input: bytes 0x00 through 0xFF
// Compressed with: zlib.compressobj(6, zlib.DEFLATED, -15)
// Note: High-entropy data doesn't compress well, so uses stored block
//
static const uint8_t golden_v7_compressed[] = {0x01, 0x00, 0x01, 0xFF, 0xFE,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23,
    0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B,
    0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53,
    0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F,
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B,
    0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
    0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B,
    0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,
    0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB,
    0xCC, 0xCD, 0xCE, 0xCF, 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
    0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE0, 0xE1, 0xE2, 0xE3,
    0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF,
    0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB,
    0xFC, 0xFD, 0xFE, 0xFF};
// Expected: 0x00, 0x01, 0x02, ..., 0xFF (256 bytes)
// Generated at runtime in test

//
// Vector 8: Dynamic Huffman block - "Hello world! Hello world! " repeated 10x
// Input: "Hello world! Hello world! " * 10 = 260 bytes
// Compressed with: zlib.compressobj(6, zlib.DEFLATED, -15)
//
static const uint8_t golden_v8_compressed[] = {0x05, 0xC1, 0xC1, 0x09, 0x00,
    0x00, 0x08, 0x03, 0xB1, 0x55, 0xEA, 0x36, 0x0E, 0xA2, 0xBF, 0x83, 0x42,
    0x3F, 0xAE, 0x6F, 0xD2, 0x0B, 0xD6, 0x39, 0x4C, 0xA9, 0x17, 0xAC, 0x73,
    0x98, 0x52, 0x2F, 0x58, 0xE7, 0x30, 0xA5, 0x5E, 0xB0, 0xCE, 0x61, 0x4A,
    0xBD, 0x60, 0x9D, 0xC3, 0x94, 0x7A, 0xC1, 0x3A, 0x87, 0x29, 0xF5, 0x82,
    0x75, 0x0E, 0x53, 0xEA, 0x05, 0xEB, 0x1C, 0xA6, 0xD4, 0x0B, 0xD6, 0x39,
    0x4C, 0xA9, 0x17, 0xAC, 0x73, 0x98, 0x52, 0x2F, 0x58, 0xE7, 0x30, 0xA5,
    0x5E, 0xB0, 0xCE, 0x61, 0x4A, 0xBD, 0x60, 0x9D, 0xC3, 0x94, 0x7A, 0xC1,
    0x3A, 0x87, 0x29, 0xF5, 0x82, 0x75, 0x0E, 0x53, 0xEA, 0x05, 0xEB, 0x1C,
    0xA6, 0xD4, 0x0B, 0xD6, 0x39, 0x4C, 0xA9, 0x17, 0xAC, 0x73, 0x98, 0x52,
    0x2F, 0x58, 0xE7, 0x30, 0xA5, 0x5E, 0xB0, 0xCE, 0x61, 0x4A, 0x0F};
// Expected: "Hello world! Hello world! " repeated 10 times (260 bytes)
// Generated at runtime in test

//
// Vector 9: The quick brown fox... (classic pangram)
// Input: "The quick brown fox jumps over the lazy dog"
// Compressed with: zlib.compressobj(6, zlib.DEFLATED, -15)
//
static const uint8_t golden_v9_compressed[] = {0x0B, 0xC9, 0x48, 0x55, 0x28,
    0x2C, 0xCD, 0x4C, 0xCE, 0x56, 0x48, 0x2A, 0xCA, 0x2F, 0xCF, 0x53, 0x48,
    0xCB, 0xAF, 0x50, 0xC8, 0x2A, 0xCD, 0x2D, 0x28, 0x56, 0xC8, 0x2F, 0x4B,
    0x2D, 0x52, 0x28, 0x01, 0x4A, 0xE7, 0x24, 0x56, 0x55, 0x2A, 0xA4, 0xE4,
    0xA7, 0x03, 0x00};
static const uint8_t golden_v9_expected[] = {'T', 'h', 'e', ' ', 'q', 'u', 'i',
    'c', 'k', ' ', 'b', 'r', 'o', 'w', 'n', ' ', 'f', 'o', 'x', ' ', 'j', 'u',
    'm', 'p', 's', ' ', 'o', 'v', 'e', 'r', ' ', 't', 'h', 'e', ' ', 'l', 'a',
    'z', 'y', ' ', 'd', 'o', 'g'};

//
// Vector array for easy iteration in tests
//
static const gcomp_golden_vector_t g_golden_vectors[] = {
    {.name = "empty_stored",
        .description = "Empty input with stored block (level 0)",
        .compressed = golden_v1_compressed,
        .compressed_len = sizeof(golden_v1_compressed),
        .expected = nullptr,
        .expected_len = 0},
    {.name = "single_byte_A",
        .description = "Single byte 'A' with fixed Huffman",
        .compressed = golden_v2_compressed,
        .compressed_len = sizeof(golden_v2_compressed),
        .expected = golden_v2_expected,
        .expected_len = sizeof(golden_v2_expected)},
    {.name = "hello_stored",
        .description = "\"Hello\" with stored block",
        .compressed = golden_v3_compressed,
        .compressed_len = sizeof(golden_v3_compressed),
        .expected = golden_v3_expected,
        .expected_len = sizeof(golden_v3_expected)},
    {.name = "hello_world_fixed",
        .description = "\"Hello, world!\" with fixed Huffman",
        .compressed = golden_v4_compressed,
        .compressed_len = sizeof(golden_v4_compressed),
        .expected = golden_v4_expected,
        .expected_len = sizeof(golden_v4_expected)},
    {.name = "repeated_abc",
        .description = "Repeated \"ABC\" pattern with back-references",
        .compressed = golden_v5_compressed,
        .compressed_len = sizeof(golden_v5_compressed),
        .expected = golden_v5_expected,
        .expected_len = sizeof(golden_v5_expected)},
    {.name = "all_zeros_100",
        .description = "100 zero bytes - highly compressible",
        .compressed = golden_v6_compressed,
        .compressed_len = sizeof(golden_v6_compressed),
        .expected = golden_v6_expected,
        .expected_len = sizeof(golden_v6_expected)},
    // Vector 7 and 8 have expected data generated at runtime
    {.name = "pangram_quick_fox",
        .description = "The quick brown fox pangram",
        .compressed = golden_v9_compressed,
        .compressed_len = sizeof(golden_v9_compressed),
        .expected = golden_v9_expected,
        .expected_len = sizeof(golden_v9_expected)},
};

static const size_t g_golden_vectors_count =
    sizeof(g_golden_vectors) / sizeof(g_golden_vectors[0]);

// Special vectors with runtime-generated expected data
// Vector 7: 261 bytes compressed (stored block for high-entropy data)
static const uint8_t * golden_v7_compressed_ptr = golden_v7_compressed;
static const size_t golden_v7_compressed_len = sizeof(golden_v7_compressed);
static const size_t golden_v7_expected_len = 256;

// Vector 8: Dynamic Huffman block
static const uint8_t * golden_v8_compressed_ptr = golden_v8_compressed;
static const size_t golden_v8_compressed_len = sizeof(golden_v8_compressed);
static const size_t golden_v8_expected_len = 260;

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_GOLDEN_VECTORS_H
