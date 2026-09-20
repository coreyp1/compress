/**
 * @file test_lz4_walk.cpp
 *
 * The LZ4 walker, checked the same four ways as the Zstandard one: the units
 * tile the stream, chunking does not change the walk, a unit is reported only
 * once it is wholly present, and - the claim parallel decode rests on - a
 * block of a frame that sets B.Indep decodes on its own.
 *
 * That last one is the reason this walker matters more than the Zstandard
 * walker. A Zstandard stream our encoder produces is a single frame and offers
 * no parallelism; an LZ4 stream is many independent blocks by default, so the
 * block is the job.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lz4.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <gtest/gtest.h>

extern "C" {
#include "methods/lz4/lz4_internal.h"
#include "methods/lz4/lz4_walk.h"
}

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

std::vector<uint8_t> make_data(size_t len, uint32_t seed, int shape) {
  std::vector<uint8_t> v(len);
  uint32_t s = seed ? seed : 1u;
  for (size_t i = 0; i < len; i++) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    switch (shape) {
    case 0: v[i] = 0; break;
    case 1: v[i] = (uint8_t)(s >> 24); break;
    case 2: v[i] = (uint8_t)('a' + (s >> 28)); break;
    default: v[i] = (uint8_t)(i & 0xFF); break;
    }
  }
  return v;
}

struct EncodeOpts {
  bool block_checksum;
  bool content_checksum;
  bool independent;
  uint64_t block_size; ///< 0 to leave the default
};

std::vector<uint8_t> encode(
    const std::vector<uint8_t> & in, const EncodeOpts & e) {
  gcomp_options_t * o = nullptr;
  EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_bool(o, "lz4.block_checksum",
                e.block_checksum ? 1 : 0),
      GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_bool(o, "lz4.content_checksum",
                e.content_checksum ? 1 : 0),
      GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_bool(o, "lz4.independent_blocks",
                e.independent ? 1 : 0),
      GCOMP_OK);
  if (e.block_size) {
    EXPECT_EQ(gcomp_options_set_uint64(o, "lz4.block_size", e.block_size),
        GCOMP_OK);
  }
  size_t bound = 0;
  EXPECT_EQ(gcomp_encode_bound(nullptr, "lz4", o, in.size(), &bound), GCOMP_OK);
  std::vector<uint8_t> out(bound ? bound : 1);
  size_t w = 0;
  EXPECT_EQ(gcomp_encode_buffer(nullptr, "lz4", o, in.data(), in.size(),
                out.data(), out.size(), &w),
      GCOMP_OK);
  gcomp_options_destroy(o);
  out.resize(w);
  return out;
}

std::vector<uint8_t> decode_all(const std::vector<uint8_t> & s, size_t hint) {
  gcomp_options_t * o = nullptr;
  EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 0),
      GCOMP_OK);
  std::vector<uint8_t> out(hint + 1024);
  size_t produced = 0;
  const gcomp_status_t st = gcomp_decode_buffer(nullptr, "lz4", o, s.data(),
      s.size(), out.data(), out.size(), &produced);
  gcomp_options_destroy(o);
  EXPECT_EQ(st, GCOMP_OK);
  out.resize(st == GCOMP_OK ? produced : 0);
  return out;
}

std::vector<gcomp_walk_event_t> walk_chunked(
    const std::vector<uint8_t> & s, size_t chunk, size_t * used_out) {
  lz4_walk_t w;
  std::memset(&w, 0, sizeof(w));
  std::vector<gcomp_walk_event_t> all;
  size_t fed = 0, used_total = 0;

  for (;;) {
    fed += chunk;
    if (fed > s.size()) {
      fed = s.size();
    }
    for (;;) {
      gcomp_walk_event_t ev[8];
      size_t used = 0, n = 0;
      const gcomp_status_t st = lz4_walk_update(&w, s.data() + used_total,
          fed - used_total, &used, ev, 8, &n);
      EXPECT_EQ(st, GCOMP_OK);
      if (st != GCOMP_OK) {
        *used_out = used_total;
        return all;
      }
      used_total += used;
      for (size_t i = 0; i < n; i++) {
        all.push_back(ev[i]);
      }
      if (used == 0 && n == 0) {
        break;
      }
    }
    if (fed >= s.size()) {
      break;
    }
  }
  *used_out = used_total;
  return all;
}

std::vector<uint8_t> skippable(uint32_t variant, size_t payload) {
  std::vector<uint8_t> f(8 + payload);
  const uint32_t magic = 0x184D2A50u + (variant & 0x0Fu);
  f[0] = (uint8_t)(magic & 0xFF);
  f[1] = (uint8_t)((magic >> 8) & 0xFF);
  f[2] = (uint8_t)((magic >> 16) & 0xFF);
  f[3] = (uint8_t)((magic >> 24) & 0xFF);
  f[4] = (uint8_t)(payload & 0xFF);
  f[5] = (uint8_t)((payload >> 8) & 0xFF);
  f[6] = (uint8_t)((payload >> 16) & 0xFF);
  f[7] = (uint8_t)((payload >> 24) & 0xFF);
  for (size_t i = 0; i < payload; i++) {
    f[8 + i] = (uint8_t)(i * 31u);
  }
  return f;
}

/// Every block of an independent-block frame, decoded on its own.
void check_blocks_decode_alone(
    const std::vector<uint8_t> & s, const std::vector<uint8_t> & in) {
  std::vector<gcomp_walk_event_t> ev(8192);
  size_t n = 0, used = 0;
  ASSERT_EQ(lz4_walk_all(s.data(), s.size(), ev.data(), ev.size(), &n, &used),
      GCOMP_OK);
  ASSERT_EQ(used, s.size());

  size_t blocks = 0;
  std::vector<uint8_t> rebuilt;
  for (size_t i = 0; i < n; i++) {
    if (ev[i].kind != GCOMP_WALK_BLOCK) {
      continue;
    }
    blocks++;
    // Where the payload is and how long it is come from the event, not from
    // arithmetic on the unit's bounds: those differ by the block's own header
    // and by a B.Checksum when the frame asks for one.
    const size_t off = (size_t)ev[i].payload_offset;
    const size_t payload = (size_t)ev[i].payload_size;
    ASSERT_LE(off + payload, s.size()) << "block " << i;

    if (ev[i].stored) {
      rebuilt.insert(rebuilt.end(), s.begin() + off, s.begin() + off + payload);
      continue;
    }
    std::vector<uint8_t> out(65536 + 1024);
    size_t produced = 0;
    // No history: an independent block must not need any, which is the
    // property being checked.
    ASSERT_EQ(lz4_block_decompress(s.data() + off, payload, out.data(),
                  out.size(), &produced, nullptr, 0),
        GCOMP_OK)
        << "block " << i << " did not decode on its own";
    rebuilt.insert(rebuilt.end(), out.begin(), out.begin() + produced);
  }
  EXPECT_GT(blocks, 1u) << "the stream has only one block, so this checked "
                        << "nothing about independence";
  ASSERT_EQ(rebuilt.size(), in.size());
  EXPECT_EQ(std::memcmp(rebuilt.data(), in.data(), in.size()), 0);
}

struct Case {
  size_t len;
  uint32_t seed;
  int shape;
  EncodeOpts opts;
};

const Case k_cases[] = {
    {0, 1, 0, {false, false, true, 0}},
    {1, 2, 1, {false, true, true, 0}},
    {1000, 3, 2, {true, true, true, 0}},
    {70000, 4, 1, {false, false, true, 65536}},
    {70000, 4, 2, {true, false, true, 65536}},
    {300000, 5, 2, {true, true, true, 65536}},
    {300000, 6, 1, {false, true, false, 65536}}, // linked blocks
    {300000, 7, 0, {false, false, true, 65536}},
};

} // namespace

/// Every byte belongs to exactly one top-level unit.
TEST(Lz4Walk, TheUnitsTileTheStream) {
  for (const Case & c : k_cases) {
    const std::vector<uint8_t> in = make_data(c.len, c.seed, c.shape);
    const std::vector<uint8_t> s = encode(in, c.opts);

    std::vector<gcomp_walk_event_t> ev(4096);
    size_t n = 0, used = 0;
    ASSERT_EQ(lz4_walk_all(s.data(), s.size(), ev.data(), ev.size(), &n, &used),
        GCOMP_OK)
        << "len " << c.len;
    EXPECT_EQ(used, s.size()) << "len " << c.len;

    uint64_t at = 0;
    for (size_t i = 0; i < n; i++) {
      if (ev[i].kind == GCOMP_WALK_BLOCK) {
        continue;
      }
      EXPECT_EQ(ev[i].offset, at) << "len " << c.len << " unit " << i;
      at = ev[i].offset + ev[i].size;
    }
    EXPECT_EQ(at, s.size()) << "len " << c.len;
  }
}

/// Same bytes, different chunk sizes, same events.
TEST(Lz4Walk, ChunkingDoesNotChangeTheWalk) {
  for (const Case & c : k_cases) {
    const std::vector<uint8_t> in = make_data(c.len, c.seed, c.shape);
    const std::vector<uint8_t> s = encode(in, c.opts);

    size_t whole_used = 0;
    const std::vector<gcomp_walk_event_t> whole =
        walk_chunked(s, s.size() ? s.size() : 1, &whole_used);

    for (size_t chunk : {(size_t)1, (size_t)3, (size_t)7, (size_t)4096}) {
      size_t used = 0;
      const std::vector<gcomp_walk_event_t> got = walk_chunked(s, chunk, &used);
      ASSERT_EQ(got.size(), whole.size())
          << "len " << c.len << " chunk " << chunk;
      EXPECT_EQ(used, whole_used) << "len " << c.len << " chunk " << chunk;
      for (size_t i = 0; i < got.size(); i++) {
        EXPECT_EQ((int)got[i].kind, (int)whole[i].kind)
            << "len " << c.len << " chunk " << chunk << " event " << i;
        EXPECT_EQ(got[i].offset, whole[i].offset)
            << "len " << c.len << " chunk " << chunk << " event " << i;
        EXPECT_EQ(got[i].size, whole[i].size)
            << "len " << c.len << " chunk " << chunk << " event " << i;
      }
    }
  }
}

/// Several frames with skippable frames between them.
TEST(Lz4Walk, AMultiFrameStreamWalksToItsParts) {
  std::vector<uint8_t> s;
  const std::vector<uint8_t> a = make_data(5000, 11, 2);
  const std::vector<uint8_t> b = make_data(90000, 12, 1);

  const std::vector<uint8_t> sk0 = skippable(0, 17);
  const std::vector<uint8_t> fa = encode(a, {true, true, true, 0});
  const std::vector<uint8_t> fb = encode(b, {false, false, true, 65536});
  const std::vector<uint8_t> sk1 = skippable(15, 0);
  s.insert(s.end(), sk0.begin(), sk0.end());
  s.insert(s.end(), fa.begin(), fa.end());
  s.insert(s.end(), sk1.begin(), sk1.end());
  s.insert(s.end(), fb.begin(), fb.end());

  std::vector<gcomp_walk_event_t> ev(8192);
  size_t n = 0, used = 0;
  ASSERT_EQ(lz4_walk_all(s.data(), s.size(), ev.data(), ev.size(), &n, &used),
      GCOMP_OK);
  EXPECT_EQ(used, s.size());

  size_t frames = 0, skippables = 0;
  uint64_t at = 0;
  for (size_t i = 0; i < n; i++) {
    if (ev[i].kind == GCOMP_WALK_BLOCK) {
      continue;
    }
    EXPECT_EQ(ev[i].offset, at) << "unit " << i;
    at = ev[i].offset + ev[i].size;
    if (ev[i].kind == GCOMP_WALK_FRAME) {
      frames++;
    }
    else {
      skippables++;
    }
  }
  EXPECT_EQ(frames, 2u);
  EXPECT_EQ(skippables, 2u);
  EXPECT_EQ(at, s.size());
}

/**
 * @brief A block of an independent-block frame decodes on its own.
 *
 * This is what makes LZ4 the format parallel decode actually helps, so it is
 * checked the hard way: each block the walker reports is decompressed as a
 * standalone LZ4 block, and the pieces must concatenate to the original.
 *
 * Only for a frame that sets B.Indep. A frame with linked blocks is walked the
 * same way, but its blocks reference earlier ones and are not jobs - the
 * walker says which through lz4_walk_blocks_independent().
 */
TEST(Lz4Walk, EachIndependentBlockDecodesOnItsOwn) {
  // Both with and without B.Checksum, and on data that does not compress as
  // well as prose: a frame with block checksums puts four bytes after every
  // block, and one with incompressible content stores blocks verbatim. Deriving
  // the payload from the unit's bounds is right for neither, and the first
  // version of this test did exactly that - it passed because the one stream it
  // used had no checksums and no stored blocks.
  for (bool block_checksum : {false, true}) {
    for (int shape : {1, 2}) {
      SCOPED_TRACE(std::string("block_checksum=") +
          (block_checksum ? "1" : "0") + " shape=" + std::to_string(shape));
      const std::vector<uint8_t> in = make_data(300000, 42, shape);
      const std::vector<uint8_t> s =
          encode(in, {block_checksum, false, true, 65536});
      check_blocks_decode_alone(s, in);
    }
  }
}

/// Truncation is "not yet", not an error, and nothing is reported past the end.
TEST(Lz4Walk, ATruncatedStreamStopsAtTheLastCompleteUnit) {
  const std::vector<uint8_t> full =
      encode(make_data(80000, 21, 2), {true, true, true, 65536});
  ASSERT_GT(full.size(), 64u);

  for (size_t cut : {full.size() - 1, full.size() / 2, (size_t)20, (size_t)9,
           (size_t)5, (size_t)1}) {
    const std::vector<uint8_t> s(full.begin(), full.begin() + cut);
    std::vector<gcomp_walk_event_t> ev(4096);
    size_t n = 0, used = 0;
    EXPECT_EQ(lz4_walk_all(s.data(), s.size(), ev.data(), ev.size(), &n, &used),
        GCOMP_OK)
        << "cut at " << cut;
    for (size_t i = 0; i < n; i++) {
      EXPECT_LE(ev[i].offset + ev[i].size, (uint64_t)cut)
          << "cut at " << cut << ": event " << i << " runs past its bytes";
    }
  }
}

/// A stream that does not begin with a magic number is corrupt.
TEST(Lz4Walk, RubbishIsRefused) {
  const uint8_t junk[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
  gcomp_walk_event_t ev[8];
  size_t n = 0, used = 0;
  EXPECT_EQ(lz4_walk_all(junk, sizeof(junk), ev, 8, &n, &used),
      GCOMP_ERR_CORRUPT);
}

/**
 * @brief Descriptor shapes that move where the first block begins.
 *
 * The frame descriptor is variable length: a content size adds eight bytes and
 * a dictionary ID four (LZ4 frame format, section "Frame Descriptor"). A
 * walker that assumed a fixed header would be right for the default frame and
 * wrong by twelve bytes here - and being wrong about where the first block
 * starts means every offset after it is wrong too, without anything looking
 * obviously broken.
 *
 * ## The reference this file does not have
 *
 * The Zstandard walker is checked against streams the `zstd` CLI produced.
 * There is no equivalent here: no `lz4` binary and no Python `lz4` module is
 * installed on this machine, and the LZ4 suite's existing oracle
 * (test_lz4_spec_oracle.cpp) is a reference for the *block* format, not the
 * frame format. So the frames below are ours, and what keeps that from being
 * circular is that each block is decoded standalone in
 * EachIndependentBlockDecodesOnItsOwn - the walker's offsets have to be right
 * for a separate decoder to accept them.
 *
 * Recorded rather than papered over: an external frame-level LZ4 reference
 * would be worth installing.
 */
TEST(Lz4Walk, DescriptorVariationsMoveTheFirstBlock) {
  const std::vector<uint8_t> in = make_data(200000, 5150, 2);

  struct Variant {
    const char * name;
    bool content_size;
    uint64_t dict_id;
  };
  const Variant variants[] = {
      {"plain", false, 0},
      {"content size", true, 0},
      {"dictionary id", false, 0xABCDEF01u},
      {"both", true, 0xABCDEF01u},
  };

  for (const Variant & v : variants) {
    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_bool(o, "lz4.independent_blocks", 1), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(o, "lz4.block_size", 65536), GCOMP_OK);
    if (v.content_size) {
      ASSERT_EQ(gcomp_options_set_uint64(o, "lz4.content_size", in.size()),
          GCOMP_OK);
    }
    if (v.dict_id) {
      ASSERT_EQ(gcomp_options_set_uint64(o, "lz4.dictionary_id", v.dict_id),
          GCOMP_OK);
    }
    size_t bound = 0;
    ASSERT_EQ(gcomp_encode_bound(nullptr, "lz4", o, in.size(), &bound),
        GCOMP_OK);
    std::vector<uint8_t> s(bound ? bound : 1);
    size_t w = 0;
    const gcomp_status_t es = gcomp_encode_buffer(
        nullptr, "lz4", o, in.data(), in.size(), s.data(), s.size(), &w);
    gcomp_options_destroy(o);
    if (es != GCOMP_OK) {
      continue; // This build does not offer that combination.
    }
    s.resize(w);

    std::vector<gcomp_walk_event_t> ev(8192);
    size_t n = 0, used = 0;
    ASSERT_EQ(lz4_walk_all(s.data(), s.size(), ev.data(), ev.size(), &n, &used),
        GCOMP_OK)
        << v.name;
    EXPECT_EQ(used, s.size()) << v.name;

    // Each block still decodes alone, which is what says the walker found the
    // first one in the right place.
    size_t blocks = 0;
    std::vector<uint8_t> rebuilt;
    for (size_t i = 0; i < n; i++) {
      if (ev[i].kind != GCOMP_WALK_BLOCK) {
        continue;
      }
      blocks++;
      const size_t off = (size_t)ev[i].payload_offset;
      const size_t payload = (size_t)ev[i].payload_size;
      ASSERT_LE(off + payload, s.size()) << v.name << ": block " << i;
      if (ev[i].stored) {
        // An incompressible block is stored verbatim; there is nothing to
        // decode and handing it to the block decoder would be wrong.
        rebuilt.insert(rebuilt.end(), s.begin() + off, s.begin() + off + payload);
        continue;
      }
      std::vector<uint8_t> out(65536 + 1024);
      size_t produced = 0;
      ASSERT_EQ(lz4_block_decompress(s.data() + off, payload, out.data(),
                    out.size(), &produced, nullptr, 0),
          GCOMP_OK)
          << v.name << ": block " << i;
      rebuilt.insert(rebuilt.end(), out.begin(), out.begin() + produced);
    }
    EXPECT_GT(blocks, 1u) << v.name;
    ASSERT_EQ(rebuilt.size(), in.size()) << v.name;
    EXPECT_EQ(std::memcmp(rebuilt.data(), in.data(), in.size()), 0) << v.name;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
