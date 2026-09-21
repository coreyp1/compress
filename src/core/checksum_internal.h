/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Compress.
 *
 * Ghoti.io Compress is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Compress is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file checksum_internal.h
 *
 * The several implementations of CRC-32 and Adler-32, and the runtime
 * choice between them.
 *
 * WHY THE VARIANTS ARE VISIBLE HERE
 * =================================
 *
 * `gcomp_crc32_update()` and `gcomp_adler32_update()` each pick an
 * implementation at run time.  A test that only calls the public entry point
 * therefore tests one implementation -- whichever the machine running the
 * test happens to select -- and silently skips the others.  That is the
 * failure mode `crc32.c` already had: its comment said "the tests exercise
 * both" of the slice-by-8 and byte-at-a-time paths, and nothing did, because
 * the choice between them was a compile-time `#ifdef` with no second build.
 *
 * So every variant is declared here under its own name.  The oracle test
 * calls each one directly and compares them against each other on the same
 * input, in one binary, whatever the host CPU turns out to support.  A
 * variant the host cannot execute is skipped by its `_available()` predicate
 * and reported as skipped, never silently passed over.
 *
 * Nothing in this header is public.  The library builds with
 * `-fvisibility=hidden`, so none of these names reach the shared object and
 * none of them need a `namespace.h` entry (CONVENTIONS.md section 4 is about
 * what a second copy of the library could collide with; hidden symbols
 * cannot).
 *
 * WHY THE VECTOR CODE IS NOT COMPILED FOR THE WHOLE LIBRARY
 * =========================================================
 *
 * PCLMULQDQ and SSSE3 are not in the x86-64 baseline.  A library built with
 * `-mpclmul` throughout would run faster on this machine and die with SIGILL
 * on a 2008 one, which is a far worse trade than being slow.  The two
 * implementations therefore live in their own translation units, and each
 * function in them carries `__attribute__((target(...)))`: the instruction
 * set is widened for exactly those functions and nothing else.
 *
 * That is a deliberate deviation from COMPRESS-PLAN.md phase F2, which said
 * to pass the flags on the command line for those two files.  The attribute
 * reaches the same place by a shorter road: the command-line spelling has to
 * be repeated for the release, ASan, TSan, AFL and coverage builds, and a
 * build that forgot one would compile scalar code with no diagnostic
 * whatsoever.  The attribute travels with the source, so no build can lose
 * it, and it is narrower -- a command-line flag also licenses the compiler
 * to use those instructions in that unit's *other* code.
 */

#ifndef GHOTI_IO_GCOMP_SRC_CORE_CHECKSUM_INTERNAL_H
#define GHOTI_IO_GCOMP_SRC_CORE_CHECKSUM_INTERNAL_H

#include <ghoti.io/compress/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Whether this build may contain x86 vector checksum code at all.
 *
 * Both the architecture and the compiler have to cooperate: the intrinsics
 * headers, `__attribute__((target))` and `__builtin_cpu_supports()` are GCC
 * and Clang spellings.  Where this is undefined the vector translation units
 * still compile -- they just contain the stubs at the bottom of each file,
 * whose `_available()` predicates answer false.
 *
 * Define GCOMP_NO_CHECKSUM_SIMD to force the scalar paths on a target that
 * would otherwise qualify.  GCOMP_CRC32_NO_PCLMUL and GCOMP_ADLER32_NO_SSSE3
 * do the same for one checksum each, the way GCOMP_CRC32_NO_SLICE_BY_8 does
 * for the scalar CRC.  documentation/building.md lists all four.
 */
#if !defined(GCOMP_NO_CHECKSUM_SIMD) &&                                        \
    (defined(__x86_64__) || defined(__i386__)) &&                              \
    (defined(__GNUC__) || defined(__clang__))
#define GCOMP_CHECKSUM_SIMD_X86 1
#endif

#if defined(GCOMP_CHECKSUM_SIMD_X86) && !defined(GCOMP_CRC32_NO_PCLMUL)
#define GCOMP_CRC32_PCLMUL 1
#endif

#if defined(GCOMP_CHECKSUM_SIMD_X86) && !defined(GCOMP_ADLER32_NO_SSSE3)
#define GCOMP_ADLER32_SSSE3 1
#endif

/**
 * @brief Whether the scalar CRC-32 may fold eight bytes at a time.
 *
 * The word loads that feed the slicing tables are little-endian by
 * construction: byte k of the input has to land in table row 7 - k.  A
 * big-endian target keeps the byte-at-a-time loop, which is what the fast
 * path is checked against.
 *
 * Define GCOMP_CRC32_NO_SLICE_BY_8 to force the byte loop on a target that
 * would otherwise qualify.  This lives here rather than in crc32.c so that
 * the test can ask the same question the implementation asked.
 */
#if !defined(GCOMP_CRC32_NO_SLICE_BY_8) && defined(__BYTE_ORDER__) &&          \
    defined(__ORDER_LITTLE_ENDIAN__) &&                                        \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define GCOMP_CRC32_SLICE_BY_8 1
#endif

/**
 * @brief Shortest input the folding CRC is used for.
 *
 * Structural, not tuned: the fold starts by loading four 16-byte registers,
 * so it has nothing to do below 64 bytes.  Measurement said no threshold
 * above that was wanted either -- pinned to a performance core on 64 bytes
 * the fold is already 1.23x the slice-by-8 loop, and the gap only widens
 * (2.1x at 96, 6.3x at 256, 11.9x at 4096).  There is no size at which
 * dispatching costs more than it saves.
 */
#define GCOMP_CRC32_PCLMUL_MIN 64u

/**
 * @brief Shortest input the vector Adler-32 is used for.
 *
 * Also structural: one pass of the loop consumes 32 bytes.  Measured 2.2x at
 * exactly 32 bytes, 4.6x at 128, 8.2x at 4096.
 */
#define GCOMP_ADLER32_SSSE3_MIN 32u

/**
 * @brief The folding constants, as 64-bit lanes.
 *
 * Each is x^n mod P for the exponent named, reflected into bits 32..63 of
 * its lane -- see the bit-order note above for why that is where a degree-31
 * polynomial lives, and why n is one less than the algebra asks for.
 *
 * Crc32PclmulConstants in the test suite recomputes all four from the
 * polynomial by repeated squaring and compares, exactly as Crc32SliceTable
 * does for the 2048 table entries.  Neither set of magic numbers can drift
 * from what it claims to be.
 *
 * - by-four, high lane: x^(4*128 + 63) = x^575
 * - by-four, low lane:  x^(4*128 - 1)  = x^511
 * - by-one,  high lane: x^(128 + 63)   = x^191
 * - by-one,  low lane:  x^(128 - 1)    = x^127
 */
#define GCOMP_CRC32_K_BY4_HI 0x653d982200000000ull
#define GCOMP_CRC32_K_BY4_LO 0xcad38e8f00000000ull
#define GCOMP_CRC32_K_BY1_HI 0x65673b4600000000ull
#define GCOMP_CRC32_K_BY1_LO 0x9ba54c6f00000000ull

/**
 * @brief CRC-32, one byte at a time through row 0 of the slicing table.
 *
 * The reference implementation: the loop RFC 1952 section 8 describes, and
 * what every other variant here is checked against.
 */
uint32_t gcomp_crc32_update_bytewise(
    uint32_t crc, const uint8_t * data, size_t len);

/**
 * @brief CRC-32, eight bytes at a time (Kounavis and Berry, 2008).
 *
 * Only declared where the byte order lets the word loads feed the tables
 * directly; see the note on GCOMP_CRC32_SLICE_BY_8 in crc32.c.
 */
#ifdef GCOMP_CRC32_SLICE_BY_8
uint32_t gcomp_crc32_update_slice8(
    uint32_t crc, const uint8_t * data, size_t len);
#endif

/**
 * @brief The fastest CRC-32 that needs no instruction set support.
 *
 * Slice-by-8 where the byte order allows it, the byte loop otherwise.  This
 * is what the vector path falls back to and what it reduces through.
 */
uint32_t gcomp_crc32_update_scalar(
    uint32_t crc, const uint8_t * data, size_t len);

/**
 * @brief Whether gcomp_crc32_update_pclmul() may be called on this CPU.
 *
 * False on every build without GCOMP_CRC32_PCLMUL, and on any x86 whose
 * CPUID says it has no carry-less multiply.  Decided on the first call and
 * remembered; the answer cannot change while the process runs.
 */
bool gcomp_crc32_pclmul_available(void);

/**
 * @brief CRC-32 by carry-less multiply folding.
 *
 * @pre gcomp_crc32_pclmul_available() answered true, and
 *      `len >= GCOMP_CRC32_PCLMUL_MIN`.
 */
uint32_t gcomp_crc32_update_pclmul(
    uint32_t crc, const uint8_t * data, size_t len);

/**
 * @brief Adler-32 with the unrolled scalar loop, RFC 1950 section 9.
 */
uint32_t gcomp_adler32_update_scalar(
    uint32_t adler, const uint8_t * data, size_t len);

/**
 * @brief Whether gcomp_adler32_update_ssse3() may be called on this CPU.
 */
bool gcomp_adler32_ssse3_available(void);

/**
 * @brief Adler-32 over 32-byte groups with `_mm_maddubs_epi16`.
 *
 * @pre gcomp_adler32_ssse3_available() answered true.  Any length is
 *      accepted; below GCOMP_ADLER32_SSSE3_MIN it is simply the scalar tail.
 */
uint32_t gcomp_adler32_update_ssse3(
    uint32_t adler, const uint8_t * data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_CORE_CHECKSUM_INTERNAL_H
