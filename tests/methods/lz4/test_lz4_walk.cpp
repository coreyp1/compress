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
#include <dlfcn.h>
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
void check_blocks_decode_alone(const std::vector<uint8_t> & s,
    const std::vector<uint8_t> & in, size_t max_block = 65536) {
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
    std::vector<uint8_t> out(max_block + 1024);
    size_t produced = 0;
    // No history: an independent block must not need any, which is the
    // property being checked.
    // No slack: this buffer is the test's, and nothing may be written past
    // the capacity it names.
    ASSERT_EQ(lz4_block_decompress(s.data() + off, payload, out.data(),
                  out.size(), 0, &produced, 0),
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
 * The frames below are ours. What keeps that from being circular is that each
 * block is decoded standalone in EachIndependentBlockDecodesOnItsOwn - the
 * walker's offsets have to be right for a separate decoder to accept them -
 * and that the same walk is checked against frames liblz4 wrote, at the end of
 * this file.
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
                    out.size(), 0, &produced, 0),
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

//
// Frames a different implementation wrote
//
// Everything above walks frames our own encoder produced. That checks the
// walker against the encoder, and both were written from the same reading of
// the same specification by the same person - a round trip with one more step
// in it. A walker that misread the frame descriptor in exactly the way the
// encoder writes it would pass every test above.
//
// liblz4 closes that. The runtime library is present on most systems without
// its development header, so these bind to it at runtime rather than at build
// time - the same approach test_lz4_spec_oracle.cpp takes - and no new build
// dependency comes with them.
//
// Note for anyone who reads the LZ4 sources looking for a block-level API
// here: LZ4F_* *is* the frame API. LZ4_compress_default and friends are the
// block API. An earlier version of this file claimed the installed library
// offered only the latter and skipped the cross-check on that basis, which
// was simply wrong.
//

namespace {

/**
 * @brief Mirrors LZ4F_preferences_t, whose header is not installed here.
 *
 * Declaring another project's struct by hand is worth being nervous about: a
 * layout that has drifted would be written to silently and read back as
 * rubbish. Two things catch that rather than one. LZ4F_getFrameInfo reads the
 * frame back and has to report the settings that were asked for; and
 * LZ4F_headerSize, which takes no struct at all, has to agree about how long
 * the resulting header is - a content size adds eight bytes to it and a
 * dictionary ID four, so a preferences block that did not land where liblz4
 * expected would disagree there too.
 */
struct RealFrameInfo {
  int block_size_id;        ///< 0 default, 4 = 64 KB ... 7 = 4 MB.
  int block_mode;           ///< 0 linked, 1 independent.
  int content_checksum;     ///< 0 off, 1 on.
  int frame_type;           ///< 0 frame, 1 skippable.
  unsigned long long content_size;
  unsigned dict_id;
  int block_checksum;       ///< 0 off, 1 on.
};

struct RealPreferences {
  RealFrameInfo frame_info;
  int compression_level;
  unsigned auto_flush;
  unsigned favor_dec_speed;
  unsigned reserved[3];
};

struct RealLz4F {
  void * handle = nullptr;
  unsigned (*isError)(size_t) = nullptr;
  const char * (*errorName)(size_t) = nullptr;
  size_t (*compressFrameBound)(size_t, const RealPreferences *) = nullptr;
  size_t (*compressFrame)(void *, size_t, const void *, size_t,
      const RealPreferences *) = nullptr;
  size_t (*headerSize)(const void *, size_t) = nullptr;
  size_t (*createDctx)(void **, unsigned) = nullptr;
  size_t (*freeDctx)(void *) = nullptr;
  size_t (*getFrameInfo)(void *, RealFrameInfo *, const void *,
      size_t *) = nullptr;

  bool ok() const {
    return handle && compressFrame && compressFrameBound && headerSize &&
        createDctx && freeDctx && getFrameInfo && isError;
  }
};

const RealLz4F & realLz4f() {
  static RealLz4F r = [] {
    RealLz4F out;
    static const char * kNames[] = {"liblz4.so.1", "liblz4.so",
        "liblz4.1.dylib", "liblz4.dylib"};
    for (const char * name : kNames) {
      out.handle = dlopen(name, RTLD_NOW);
      if (out.handle) {
        break;
      }
    }
    if (!out.handle) {
      return out;
    }
    auto sym = [&](const char * n) { return dlsym(out.handle, n); };
    out.isError = (unsigned (*)(size_t))sym("LZ4F_isError");
    out.errorName = (const char * (*)(size_t))sym("LZ4F_getErrorName");
    out.compressFrameBound = (size_t (*)(size_t,
        const RealPreferences *))sym("LZ4F_compressFrameBound");
    out.compressFrame = (size_t (*)(void *, size_t, const void *, size_t,
        const RealPreferences *))sym("LZ4F_compressFrame");
    out.headerSize = (size_t (*)(const void *, size_t))sym("LZ4F_headerSize");
    out.createDctx = (size_t (*)(void **, unsigned))sym(
        "LZ4F_createDecompressionContext");
    out.freeDctx = (size_t (*)(void *))sym("LZ4F_freeDecompressionContext");
    out.getFrameInfo = (size_t (*)(void *, RealFrameInfo *, const void *,
        size_t *))sym("LZ4F_getFrameInfo");
    return out;
  }();
  return r;
}

/// What a foreign frame is being asked to look like.
struct ForeignShape {
  const char * name;
  int block_size_id;    ///< 4 = 64 KB, 5 = 256 KB, 6 = 1 MB.
  int independent;
  int block_checksum;
  int content_checksum;
  int declare_size;     ///< Put Content_Size in the descriptor.
  unsigned dict_id;     ///< Non-zero puts Dictionary_ID in the descriptor.
};

const ForeignShape k_foreign[] = {
    {"64K linked plain", 4, 0, 0, 0, 0, 0},
    {"64K independent", 4, 1, 0, 0, 0, 0},
    {"64K independent, block checksums", 4, 1, 1, 0, 0, 0},
    {"64K independent, both checksums", 4, 1, 1, 1, 0, 0},
    {"64K independent, content size", 4, 1, 0, 0, 1, 0},
    {"64K independent, size and dict id", 4, 1, 1, 1, 1, 0xABCDEF01u},
    {"256K independent", 5, 1, 0, 1, 0, 0},
    {"1M linked, both checksums", 6, 0, 1, 1, 1, 0},
};

size_t foreign_block_bytes(int block_size_id) {
  switch (block_size_id) {
  case 4: return 64u * 1024u;
  case 5: return 256u * 1024u;
  case 6: return 1024u * 1024u;
  default: return 4u * 1024u * 1024u;
  }
}

/**
 * @brief Compress with liblz4, and report the frame it actually made.
 *
 * ## Preferences are a request, not an instruction
 *
 * LZ4F_compressFrame normalises what it is given, and two of those
 * normalisations showed up the first time this ran:
 *
 * - A frame whose whole content fits in one block is marked B.Indep even when
 *   blockLinked was asked for. Nothing precedes the only block, so linking it
 *   to history would describe history that does not exist.
 * - The block size is reduced to the smallest one that still holds the input,
 *   so 150 KB asked for in 1 MB blocks comes back as 256 KB.
 *
 * So the effective frame info is handed back and the walker is checked against
 * *that*, not against the request. What stays an exact check is everything
 * liblz4 does not normalise - both checksum flags, the dictionary ID, the
 * declared content size - together with the header length, which crosses no
 * struct at all. Those are what would break if the mirrored
 * LZ4F_preferences_t above stopped matching its header, and they would break
 * loudly.
 *
 * Returns false only when the library is absent.
 */
bool foreign_encode(const ForeignShape & shape, const std::vector<uint8_t> & in,
    std::vector<uint8_t> * out, size_t * header_size_out,
    RealFrameInfo * effective_out) {
  const RealLz4F & lib = realLz4f();
  if (!lib.ok()) {
    return false;
  }

  // Oversized and zeroed: if the real preferences block is longer than the
  // mirror above, liblz4 reads defaults rather than stack rubbish.
  alignas(16) unsigned char prefs_storage[256];
  std::memset(prefs_storage, 0, sizeof(prefs_storage));
  RealPreferences * prefs = (RealPreferences *)prefs_storage;
  prefs->frame_info.block_size_id = shape.block_size_id;
  prefs->frame_info.block_mode = shape.independent;
  prefs->frame_info.content_checksum = shape.content_checksum;
  prefs->frame_info.block_checksum = shape.block_checksum;
  prefs->frame_info.dict_id = shape.dict_id;
  prefs->frame_info.content_size = shape.declare_size ? in.size() : 0;

  const size_t bound = lib.compressFrameBound(in.size(), prefs);
  EXPECT_FALSE(lib.isError(bound)) << shape.name;
  out->assign(bound ? bound : 1, 0);
  const size_t written = lib.compressFrame(out->data(), out->size(),
      in.data(), in.size(), prefs);
  EXPECT_FALSE(lib.isError(written))
      << shape.name << ": "
      << (lib.errorName ? lib.errorName(written) : "compressFrame failed");
  if (lib.isError(written)) {
    return false;
  }
  out->resize(written);

  // Read the descriptor back through liblz4's own parser. This is the check
  // that the mirrored struct landed where the library expected it to.
  void * dctx = nullptr;
  const size_t cr = lib.createDctx(&dctx, 100 /* LZ4F_VERSION */);
  EXPECT_FALSE(lib.isError(cr)) << shape.name;
  RealFrameInfo got;
  std::memset(&got, 0, sizeof(got));
  size_t consumed = out->size();
  const size_t gr = lib.getFrameInfo(dctx, &got, out->data(), &consumed);
  EXPECT_FALSE(lib.isError(gr)) << shape.name;
  lib.freeDctx(dctx);

  // Never normalised: these are the layout check.
  EXPECT_EQ(got.block_checksum, shape.block_checksum)
      << shape.name << ": liblz4 did not make the frame that was asked for, "
      << "which means the mirrored LZ4F_preferences_t no longer matches its "
      << "header";
  EXPECT_EQ(got.content_checksum, shape.content_checksum) << shape.name;
  EXPECT_EQ(got.dict_id, shape.dict_id) << shape.name;
  if (shape.declare_size) {
    EXPECT_EQ(got.content_size, (unsigned long long)in.size()) << shape.name;
  }

  // Normalised, but only ever downwards, and only in the two ways above.
  EXPECT_LE(got.block_size_id, shape.block_size_id)
      << shape.name << ": liblz4 enlarged the block size, which it has no "
      << "reason to do";
  if (in.size() > foreign_block_bytes(shape.block_size_id)) {
    EXPECT_EQ(got.block_size_id, shape.block_size_id)
        << shape.name << ": the input does not fit in one block, so there was "
        << "nothing to shrink to";
  }
  if (in.size() > foreign_block_bytes(got.block_size_id)) {
    EXPECT_EQ(got.block_mode, shape.independent)
        << shape.name << ": a multi-block frame kept the block mode asked for";
  }
  else {
    EXPECT_EQ(got.block_mode, 1)
        << shape.name << ": a single-block frame is independent whatever was "
        << "asked for";
  }

  // Layout-independent corroboration: no struct crosses this call.
  const size_t hs = lib.headerSize(out->data(), out->size());
  EXPECT_FALSE(lib.isError(hs)) << shape.name;
  // Magic (4) + FLG + BD (2) + HC (1), plus the optional fields.
  const size_t expect_hs =
      7u + (shape.declare_size ? 8u : 0u) + (shape.dict_id ? 4u : 0u);
  EXPECT_EQ(hs, expect_hs)
      << shape.name << ": the descriptor liblz4 wrote is not the length the "
      << "requested options call for";
  *header_size_out = hs;
  *effective_out = got;
  return true;
}

} // namespace

/**
 * @brief The walk of a frame written by liblz4.
 *
 * Tiling, the position of the first block, and a round trip through our own
 * decoder - on bytes nothing in this project produced.
 */
TEST(Lz4Walk, AFrameLiblz4WroteWalksToItsParts) {
  if (!realLz4f().ok()) {
    GTEST_SKIP() << "liblz4 is not present";
  }

  for (const ForeignShape & shape : k_foreign) {
    SCOPED_TRACE(shape.name);
    const std::vector<uint8_t> in = make_data(300000, 9001, 2);
    std::vector<uint8_t> s;
    size_t header = 0;
    RealFrameInfo eff;
    if (!foreign_encode(shape, in, &s, &header, &eff)) {
      continue;
    }

    std::vector<gcomp_walk_event_t> ev(8192);
    size_t n = 0, used = 0;
    ASSERT_EQ(lz4_walk_all(s.data(), s.size(), ev.data(), ev.size(), &n, &used),
        GCOMP_OK);
    EXPECT_EQ(used, s.size());

    // Top-level units tile the stream.
    uint64_t at = 0;
    for (size_t i = 0; i < n; i++) {
      if (ev[i].kind == GCOMP_WALK_BLOCK) {
        continue;
      }
      EXPECT_EQ(ev[i].offset, at) << "unit " << i;
      at = ev[i].offset + ev[i].size;
    }
    EXPECT_EQ(at, s.size());

    // The first block begins where liblz4 says the header ends. This is the
    // assertion the descriptor variations above could not make: there, both
    // sides of the comparison were ours.
    bool saw_block = false;
    uint64_t blocks = 0, last_end = 0;
    for (size_t i = 0; i < n; i++) {
      if (ev[i].kind != GCOMP_WALK_BLOCK) {
        continue;
      }
      if (!saw_block) {
        EXPECT_EQ(ev[i].offset, (uint64_t)header)
            << "the walker put the first block somewhere other than the end "
            << "of the descriptor liblz4 wrote";
        saw_block = true;
      }
      else {
        EXPECT_EQ(ev[i].offset, last_end) << "block " << blocks << " does not "
            << "begin where the one before it ended";
      }
      // The payload has to sit inside the unit, and the difference between
      // them is the block header and any B.Checksum.
      EXPECT_GE(ev[i].payload_offset, ev[i].offset);
      EXPECT_LE(ev[i].payload_offset + ev[i].payload_size,
          ev[i].offset + ev[i].size);
      EXPECT_EQ(ev[i].offset + ev[i].size - ev[i].payload_offset -
              ev[i].payload_size,
          (uint64_t)(eff.block_checksum ? 4u : 0u))
          << "block " << blocks << ": wrong number of trailing bytes for a "
          << "frame that " << (eff.block_checksum ? "sets" : "clears")
          << " B.Checksum";
      last_end = ev[i].offset + ev[i].size;
      blocks++;
    }
    EXPECT_TRUE(saw_block);
    if (in.size() > foreign_block_bytes(eff.block_size_id)) {
      EXPECT_GT(blocks, 1u) << in.size() << " bytes should be more than one "
                            << foreign_block_bytes(eff.block_size_id)
                            << "-byte block";
    }
    else {
      EXPECT_EQ(blocks, 1u) << "the input fits in one block";
    }

    // And it decodes, which is the coarse check the fine ones are built on.
    const std::vector<uint8_t> back = decode_all(s, in.size());
    ASSERT_EQ(back.size(), in.size());
    EXPECT_EQ(std::memcmp(back.data(), in.data(), in.size()), 0);
  }
}

/**
 * @brief Blocks of a foreign independent-block frame decode on their own.
 *
 * This is the claim parallel decode rests on, made against bytes our encoder
 * did not write: if liblz4's idea of B.Indep and ours differed, a job handed
 * to a worker would decode to something other than its slice of the output.
 */
TEST(Lz4Walk, BlocksOfAForeignIndependentFrameDecodeAlone) {
  if (!realLz4f().ok()) {
    GTEST_SKIP() << "liblz4 is not present";
  }

  for (const ForeignShape & shape : k_foreign) {
    if (!shape.independent) {
      continue;
    }
    SCOPED_TRACE(shape.name);
    // Two shapes, because an incompressible one makes liblz4 store its blocks
    // verbatim and a compressible one does not.
    for (int shape_of_data : {1, 2}) {
      const std::vector<uint8_t> in = make_data(300000, 4242, shape_of_data);
      std::vector<uint8_t> s;
      size_t header = 0;
      RealFrameInfo eff;
      if (!foreign_encode(shape, in, &s, &header, &eff)) {
        continue;
      }
      ASSERT_GT(in.size(), foreign_block_bytes(eff.block_size_id))
          << "a single-block frame would make this test vacuous";
      check_blocks_decode_alone(s, in, foreign_block_bytes(eff.block_size_id));
    }
  }
}

/// A foreign frame fed in pieces walks to the same events as a whole one.
TEST(Lz4Walk, ChunkingDoesNotChangeTheWalkOfAForeignFrame) {
  if (!realLz4f().ok()) {
    GTEST_SKIP() << "liblz4 is not present";
  }

  for (const ForeignShape & shape : k_foreign) {
    SCOPED_TRACE(shape.name);
    const std::vector<uint8_t> in = make_data(150000, 77, 2);
    std::vector<uint8_t> s;
    size_t header = 0;
    RealFrameInfo eff;
    if (!foreign_encode(shape, in, &s, &header, &eff)) {
      continue;
    }

    size_t whole_used = 0;
    const std::vector<gcomp_walk_event_t> whole =
        walk_chunked(s, s.size(), &whole_used);

    for (size_t chunk : {(size_t)1, (size_t)5, (size_t)4096}) {
      size_t used = 0;
      const std::vector<gcomp_walk_event_t> got = walk_chunked(s, chunk, &used);
      ASSERT_EQ(got.size(), whole.size()) << "chunk " << chunk;
      EXPECT_EQ(used, whole_used) << "chunk " << chunk;
      for (size_t i = 0; i < got.size(); i++) {
        EXPECT_EQ((int)got[i].kind, (int)whole[i].kind)
            << "chunk " << chunk << " event " << i;
        EXPECT_EQ(got[i].offset, whole[i].offset)
            << "chunk " << chunk << " event " << i;
        EXPECT_EQ(got[i].size, whole[i].size)
            << "chunk " << chunk << " event " << i;
        EXPECT_EQ(got[i].payload_offset, whole[i].payload_offset)
            << "chunk " << chunk << " event " << i;
        EXPECT_EQ(got[i].payload_size, whole[i].payload_size)
            << "chunk " << chunk << " event " << i;
      }
    }
  }
}
