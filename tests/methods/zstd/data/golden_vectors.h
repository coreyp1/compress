/**
 * @file golden_vectors.h
 *
 * Golden test vectors for Zstd decoder validation.
 *
 * Vectors are minimal valid Zstandard frames (RFC 8878 / zstd format spec).
 * They can be used to validate the decoder without depending on the encoder.
 *
 * Layout: Magic (4) | Frame Header Descriptor | [Window] | [DictID] |
 * [ContentSize] | Block header (3) | Block data | [Content checksum (4)]
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GCOMP_ZSTD_GOLDEN_VECTORS_H
#define GHOTI_IO_GCOMP_ZSTD_GOLDEN_VECTORS_H

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
} gcomp_zstd_golden_vector_t;

//
// Vector 1: Minimal frame, empty payload
// Single segment, FCS flag 00 => 1 byte content size (0). One raw last block of
// 0. Magic(4) + FHD(0x20 = single segment, FCS 00) + FCS(0) + Block(01 00 00)
//
static const uint8_t zstd_v1_empty_compressed[] = {
    0x28,
    0xB5,
    0x2F,
    0xFD,
    0x20,
    0x00,
    0x01,
    0x00,
    0x00,
};
#define zstd_v1_empty_expected_len 0
static const uint8_t * const zstd_v1_empty_expected = nullptr;

//
// Vector 2: Small payload "Hello"
// Single segment, FCS 00 => 1 byte content size (5). One raw last block of 5.
// Magic(4) + FHD(0x20) + FCS(5) + Block(last=1, raw, size=5) + "Hello"
//
static const uint8_t zstd_v2_hello_compressed[] = {
    0x28,
    0xB5,
    0x2F,
    0xFD,
    0x20,
    0x05,
    0x29,
    0x00,
    0x00,
    0x48,
    0x65,
    0x6C,
    0x6C,
    0x6F,
};
static const uint8_t zstd_v2_hello_expected[] = {
    0x48,
    0x65,
    0x6C,
    0x6C,
    0x6F,
};

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GCOMP_ZSTD_GOLDEN_VECTORS_H */
