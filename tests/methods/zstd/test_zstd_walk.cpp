/**
 * @file test_zstd_walk.cpp
 *
 * The walker reports where a Zstandard stream's frames and blocks are without
 * decoding them. Three things have to be true of it, and only the third is
 * about compression at all:
 *
 * 1. **It tiles.** Every byte of the stream belongs to exactly one top-level
 *    unit, with no gap and no overlap. A walker that is off by one reports
 *    plausible-looking offsets from then on, so this is checked by
 *    reconstruction rather than by spot checks.
 *
 * 2. **Chunking does not change it.** The same bytes fed in pieces of 1, 7 and
 *    4096 produce the same events as the whole stream at once. A resumable
 *    state machine that is only ever given complete input is one whose
 *    resumption has never been tested, and every interesting bug in this file
 *    would hide there.
 *
 * 3. **A frame really is a decodable unit.** Each frame the walker reports,
 *    cut out and decoded on its own, gives the same bytes as its slice of the
 *    whole-stream decode. That is the claim parallel decode rests on, so it is
 *    checked directly instead of being inferred from the offsets being tidy.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>

extern "C" {
#include "methods/zstd/zstd_walk.h"
}

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <temp_file.h>
#include <string>
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
    case 0: v[i] = 0; break;                        // one long RLE block
    case 1: v[i] = (uint8_t)(s >> 24); break;       // incompressible: raw
    case 2: v[i] = (uint8_t)('a' + (s >> 28)); break; // compressible
    default: v[i] = (uint8_t)(i & 0xFF); break;
    }
  }
  return v;
}

std::vector<uint8_t> encode(const std::vector<uint8_t> & in, int64_t level,
    bool checksum) {
  gcomp_options_t * o = nullptr;
  EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_int64(o, "zstd.level", level), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_bool(o, "zstd.checksum", checksum ? 1 : 0),
      GCOMP_OK);
  size_t bound = 0;
  EXPECT_EQ(gcomp_encode_bound(nullptr, "zstd", o, in.size(), &bound),
      GCOMP_OK);
  std::vector<uint8_t> out(bound ? bound : 1);
  size_t w = 0;
  EXPECT_EQ(gcomp_encode_buffer(nullptr, "zstd", o, in.data(), in.size(),
                out.data(), out.size(), &w),
      GCOMP_OK);
  gcomp_options_destroy(o);
  out.resize(w);
  return out;
}

/// Walk in fixed-size pieces, which is what a caller reading a file does.
std::vector<gcomp_walk_event_t> walk_chunked(
    const std::vector<uint8_t> & s, size_t chunk, size_t * used_out) {
  zstd_walk_t w;
  std::memset(&w, 0, sizeof(w));
  std::vector<gcomp_walk_event_t> all;

  // `fed` is how much of the stream has been made visible to the walker and
  // only ever grows; `used_total` is how much of it the walker has accounted
  // for. Deriving the first from the second - which the first version of this
  // did - hangs the moment the walker needs more bytes than it has, because
  // the window it is offered then never widens.
  size_t fed = 0, used_total = 0;

  for (;;) {
    fed += chunk;
    if (fed > s.size()) {
      fed = s.size();
    }
    for (;;) {
      gcomp_walk_event_t ev[8];
      size_t used = 0, n = 0;
      const gcomp_status_t st = zstd_walk_update(&w, s.data() + used_total,
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
      // Ask again only while it is still making progress on what it has.
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

std::vector<uint8_t> decode_all(const std::vector<uint8_t> & s, size_t hint) {
  gcomp_options_t * o = nullptr;
  EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 0),
      GCOMP_OK);
  std::vector<uint8_t> out(hint + 1024);
  size_t produced = 0;
  const gcomp_status_t st = gcomp_decode_buffer(
      nullptr, "zstd", o, s.data(), s.size(), out.data(), out.size(),
      &produced);
  gcomp_options_destroy(o);
  EXPECT_EQ(st, GCOMP_OK);
  out.resize(st == GCOMP_OK ? produced : 0);
  return out;
}

struct Case {
  size_t len;
  uint32_t seed;
  int shape;
  int64_t level;
  bool checksum;
};

const Case k_cases[] = {
    {0, 1, 0, 3, false},
    {1, 2, 1, 3, false},
    {1, 2, 1, 3, true},
    {64, 3, 2, 1, true},
    {4096, 4, 1, 3, false},
    {4096, 4, 1, 3, true},
    {70000, 5, 2, 3, true},
    {70000, 5, 0, 9, true},
    {200000, 6, 1, 1, false},
    {200000, 7, 2, 6, true},
};

} // namespace

/// Every byte belongs to exactly one top-level unit.
TEST(ZstdWalk, TheUnitsTileTheStream) {
  for (const Case & c : k_cases) {
    const std::vector<uint8_t> in = make_data(c.len, c.seed, c.shape);
    const std::vector<uint8_t> s = encode(in, c.level, c.checksum);

    gcomp_walk_event_t ev[4096];
    size_t n = 0, used = 0;
    ASSERT_EQ(zstd_walk_all(s.data(), s.size(), ev, 4096, &n, &used), GCOMP_OK)
        << "len " << c.len << " level " << c.level;
    EXPECT_EQ(used, s.size())
        << "len " << c.len << ": walk stopped " << (s.size() - used)
        << " bytes short of the end";

    uint64_t at = 0;
    for (size_t i = 0; i < n; i++) {
      if (ev[i].kind == GCOMP_WALK_BLOCK) {
        continue; // Blocks tile a frame's interior, checked below.
      }
      EXPECT_EQ(ev[i].offset, at)
          << "len " << c.len << ": unit " << i << " starts at " << ev[i].offset
          << ", not where the previous one ended (" << at << ")";
      at = ev[i].offset + ev[i].size;
    }
    EXPECT_EQ(at, s.size())
        << "len " << c.len << ": the units cover " << at << " of " << s.size();
  }
}

/// The blocks of a frame tile it, between its header and its trailer.
TEST(ZstdWalk, TheBlocksTileTheirFrame) {
  for (const Case & c : k_cases) {
    const std::vector<uint8_t> in = make_data(c.len, c.seed, c.shape);
    const std::vector<uint8_t> s = encode(in, c.level, c.checksum);

    gcomp_walk_event_t ev[4096];
    size_t n = 0, used = 0;
    ASSERT_EQ(zstd_walk_all(s.data(), s.size(), ev, 4096, &n, &used), GCOMP_OK);

    uint64_t block_end = 0;
    int seen_last = 0;
    for (size_t i = 0; i < n; i++) {
      if (ev[i].kind == GCOMP_WALK_BLOCK) {
        if (block_end != 0) {
          EXPECT_EQ(ev[i].offset, block_end)
              << "len " << c.len << ": block " << i << " leaves a gap";
        }
        block_end = ev[i].offset + ev[i].size;
        if (ev[i].last) {
          seen_last++;
        }
      }
      else if (ev[i].kind == GCOMP_WALK_FRAME) {
        const uint64_t trailer = c.checksum ? 4u : 0u;
        EXPECT_EQ(block_end + trailer, ev[i].offset + ev[i].size)
            << "len " << c.len
            << ": the frame ends somewhere other than after its last block "
            << "and its checksum";
        block_end = 0;
      }
    }
    EXPECT_GT(seen_last, 0) << "len " << c.len << ": no block marked last";
  }
}

/// Same bytes, different chunk sizes, same events.
TEST(ZstdWalk, ChunkingDoesNotChangeTheWalk) {
  for (const Case & c : k_cases) {
    const std::vector<uint8_t> in = make_data(c.len, c.seed, c.shape);
    const std::vector<uint8_t> s = encode(in, c.level, c.checksum);

    size_t whole_used = 0;
    const std::vector<gcomp_walk_event_t> whole =
        walk_chunked(s, s.size() ? s.size() : 1, &whole_used);

    for (size_t chunk : {(size_t)1, (size_t)7, (size_t)4096}) {
      size_t used = 0;
      const std::vector<gcomp_walk_event_t> got = walk_chunked(s, chunk, &used);
      ASSERT_EQ(got.size(), whole.size())
          << "len " << c.len << " chunk " << chunk << ": " << got.size()
          << " events against " << whole.size() << " for the whole stream";
      EXPECT_EQ(used, whole_used) << "len " << c.len << " chunk " << chunk;
      for (size_t i = 0; i < got.size(); i++) {
        EXPECT_EQ((int)got[i].kind, (int)whole[i].kind)
            << "len " << c.len << " chunk " << chunk << " event " << i;
        EXPECT_EQ(got[i].offset, whole[i].offset)
            << "len " << c.len << " chunk " << chunk << " event " << i;
        EXPECT_EQ(got[i].size, whole[i].size)
            << "len " << c.len << " chunk " << chunk << " event " << i;
        EXPECT_EQ(got[i].last, whole[i].last)
            << "len " << c.len << " chunk " << chunk << " event " << i;
      }
    }
  }
}

/**
 * @brief A frame the walker reports decodes on its own.
 *
 * The claim parallel decode rests on. Checked by cutting each frame out and
 * decoding it alone, then comparing the concatenation against the whole-stream
 * decode - if a frame boundary were wrong by a byte the cut-out frame would
 * fail to decode rather than quietly differ, which is the failure mode worth
 * having.
 */
TEST(ZstdWalk, EachFrameDecodesOnItsOwn) {
  for (const Case & c : k_cases) {
    const std::vector<uint8_t> in = make_data(c.len, c.seed, c.shape);
    const std::vector<uint8_t> s = encode(in, c.level, c.checksum);
    const std::vector<uint8_t> whole = decode_all(s, c.len);
    ASSERT_EQ(whole.size(), in.size()) << "len " << c.len;

    gcomp_walk_event_t ev[4096];
    size_t n = 0, used = 0;
    ASSERT_EQ(zstd_walk_all(s.data(), s.size(), ev, 4096, &n, &used), GCOMP_OK);

    std::vector<uint8_t> rebuilt;
    for (size_t i = 0; i < n; i++) {
      if (ev[i].kind != GCOMP_WALK_FRAME) {
        continue;
      }
      const std::vector<uint8_t> frame(s.begin() + (size_t)ev[i].offset,
          s.begin() + (size_t)(ev[i].offset + ev[i].size));
      const std::vector<uint8_t> part = decode_all(frame, c.len + 1024);
      rebuilt.insert(rebuilt.end(), part.begin(), part.end());
    }
    ASSERT_EQ(rebuilt.size(), whole.size())
        << "len " << c.len << ": frames decoded separately gave "
        << rebuilt.size() << " bytes against " << whole.size();
    if (!rebuilt.empty()) {
      EXPECT_EQ(std::memcmp(rebuilt.data(), whole.data(), whole.size()), 0)
          << "len " << c.len;
    }
  }
}

namespace {

/// A skippable frame, built by hand: RFC 8878 section 3.1.2.
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

void append(std::vector<uint8_t> & dst, const std::vector<uint8_t> & src) {
  dst.insert(dst.end(), src.begin(), src.end());
}

} // namespace

/**
 * @brief Several frames in a row, with skippable frames between them.
 *
 * RFC 8878 section 3.1: "a stream is one or more frames". Everything above
 * uses a stream our encoder produced, which is always a single frame, so
 * nothing there would notice a walker that could not find the second one - and
 * multi-frame streams are exactly what parallel decode and a seek table are
 * for.
 */
TEST(ZstdWalk, AMultiFrameStreamWalksToItsParts) {
  const std::vector<uint8_t> a = make_data(5000, 11, 2);
  const std::vector<uint8_t> b = make_data(60000, 12, 1);
  const std::vector<uint8_t> c = make_data(300, 13, 0);

  std::vector<uint8_t> s;
  append(s, skippable(0, 17)); // leading, and an odd size
  append(s, encode(a, 3, true));
  append(s, encode(b, 1, false));
  append(s, skippable(15, 0)); // empty payload, highest magic
  append(s, encode(c, 9, true));
  append(s, skippable(7, 4096)); // trailing

  gcomp_walk_event_t ev[4096];
  size_t n = 0, used = 0;
  ASSERT_EQ(zstd_walk_all(s.data(), s.size(), ev, 4096, &n, &used), GCOMP_OK);
  EXPECT_EQ(used, s.size());

  size_t frames = 0, skippables = 0;
  uint64_t at = 0;
  for (size_t i = 0; i < n; i++) {
    if (ev[i].kind == GCOMP_WALK_BLOCK) {
      continue;
    }
    EXPECT_EQ(ev[i].offset, at) << "unit " << i << " leaves a gap";
    at = ev[i].offset + ev[i].size;
    if (ev[i].kind == GCOMP_WALK_FRAME) {
      frames++;
    }
    else {
      skippables++;
    }
  }
  EXPECT_EQ(frames, 3u);
  EXPECT_EQ(skippables, 3u);
  EXPECT_EQ(at, s.size());

  // And each data frame still decodes on its own, in order.
  std::vector<uint8_t> rebuilt;
  for (size_t i = 0; i < n; i++) {
    if (ev[i].kind != GCOMP_WALK_FRAME) {
      continue;
    }
    const std::vector<uint8_t> frame(s.begin() + (size_t)ev[i].offset,
        s.begin() + (size_t)(ev[i].offset + ev[i].size));
    const std::vector<uint8_t> part = decode_all(frame, 70000);
    rebuilt.insert(rebuilt.end(), part.begin(), part.end());
  }
  std::vector<uint8_t> expect;
  append(expect, a);
  append(expect, b);
  append(expect, c);
  ASSERT_EQ(rebuilt.size(), expect.size());
  EXPECT_EQ(std::memcmp(rebuilt.data(), expect.data(), expect.size()), 0);
}

/// Chunking must not change a multi-frame walk either.
TEST(ZstdWalk, ChunkingDoesNotChangeAMultiFrameWalk) {
  std::vector<uint8_t> s;
  append(s, skippable(0, 17));
  append(s, encode(make_data(5000, 11, 2), 3, true));
  append(s, encode(make_data(9000, 12, 1), 1, false));
  append(s, skippable(3, 9));

  size_t whole_used = 0;
  const std::vector<gcomp_walk_event_t> whole =
      walk_chunked(s, s.size(), &whole_used);
  ASSERT_GT(whole.size(), 0u);

  for (size_t chunk : {(size_t)1, (size_t)3, (size_t)64, (size_t)4096}) {
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
    }
  }
}

/**
 * @brief Truncation is reported as "not yet", not as an error.
 *
 * A walker is fed whatever has arrived, so a stream that stops part way
 * through a unit is the ordinary case, not a corrupt one. It must consume up
 * to the last complete unit and say so, leaving the caller to decide whether
 * more is coming.
 */
TEST(ZstdWalk, ATruncatedStreamStopsAtTheLastCompleteUnit) {
  const std::vector<uint8_t> full = encode(make_data(40000, 21, 2), 3, true);
  ASSERT_GT(full.size(), 32u);

  for (size_t cut : {full.size() - 1, full.size() / 2, (size_t)10,
           (size_t)5, (size_t)3, (size_t)1}) {
    const std::vector<uint8_t> s(full.begin(), full.begin() + cut);
    gcomp_walk_event_t ev[4096];
    size_t n = 0, used = 0;
    EXPECT_EQ(zstd_walk_all(s.data(), s.size(), ev, 4096, &n, &used), GCOMP_OK)
        << "cut at " << cut << ": truncation reported as an error";
    EXPECT_LE(used, s.size()) << "cut at " << cut;
    for (size_t i = 0; i < n; i++) {
      EXPECT_LE(ev[i].offset + ev[i].size, (uint64_t)cut)
          << "cut at " << cut << ": event " << i
          << " runs past the bytes it was given";
    }
  }
}

/// A stream that does not begin with a magic number is corrupt, and says so.
TEST(ZstdWalk, RubbishIsRefused) {
  const uint8_t junk[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
  gcomp_walk_event_t ev[8];
  size_t n = 0, used = 0;
  EXPECT_EQ(zstd_walk_all(junk, sizeof(junk), ev, 8, &n, &used),
      GCOMP_ERR_CORRUPT);
}

namespace {

bool skip_oracle() {
  const char * e = std::getenv("GCOMP_SKIP_ORACLE_TESTS");
  return e && std::string(e) == "1";
}

bool has_zstd_cli() {
  return std::system("zstd --version >/dev/null 2>&1") == 0;
}

std::string temp_path(const char * tag) {
  // The name this replaces was "/tmp/...%d_%p" of a stack address: the
  // same value on every call from the same frame, in a directory named
  // outright rather than asked for.  cutil creates the file as it names
  // it, under gcu_path_temp_dir().
  return gcomp_test::uniqueTempPath("gcomp_walk", std::string("_") + tag);
}

bool write_file(const std::string & p, const std::vector<uint8_t> & v) {
  return gcomp_test::writeWholeFile(p, v);
}

std::vector<uint8_t> read_file(const std::string & p) {
  return gcomp_test::readWholeFile(p);
}

} // namespace

/**
 * @brief The walker agrees with a stream libzstd produced.
 *
 * Everything above walks a stream this library encoded, so a walker that had
 * quietly learned our encoder's habits - one block shape, one header layout,
 * never a dictionary ID - would pass all of it. The reference emits frames we
 * do not: `--long`, `--no-check`, several levels, and `-T4`, whose output is
 * one frame from libzstd but several from older versions and other tools.
 *
 * The check is the strong one: each frame the walker finds is cut out and
 * decoded on its own, and the concatenation must equal the original file.
 */
TEST(ZstdWalkOracle, WalksStreamsTheReferenceProduced) {
  if (skip_oracle()) {
    GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
  }
  if (!has_zstd_cli()) {
    GTEST_SKIP() << "zstd CLI not available";
  }

  const std::vector<uint8_t> original = make_data(400000, 99, 2);
  const std::string in_path = temp_path("in");
  ASSERT_TRUE(write_file(in_path, original));

  const char * const flag_sets[] = {
      "-1",
      "-9",
      "-19 --long",
      "--no-check -5",
      "-T4 -3",
      "-T4 --no-check -1",
  };

  // Counted, because every one of these could be rejected by a CLI build that
  // does not take the flag - and a loop whose body never runs is a test that
  // passes without asking anything.
  size_t ran = 0;

  for (const char * flags : flag_sets) {
    const std::string out_path = temp_path("out");
    char cmd[512];
    std::snprintf(cmd, sizeof(cmd), "zstd -q -f %s -o %s %s 2>/dev/null",
        flags, out_path.c_str(), in_path.c_str());
    const int rc = std::system(cmd);
    if (rc != 0) {
      std::remove(out_path.c_str());
      continue; // This build of the CLI does not take those flags.
    }

    const std::vector<uint8_t> s = read_file(out_path);
    std::remove(out_path.c_str());
    ASSERT_GT(s.size(), 0u) << flags;
    ran++;

    std::vector<gcomp_walk_event_t> ev(65536);
    size_t n = 0, used = 0;
    ASSERT_EQ(zstd_walk_all(s.data(), s.size(), ev.data(), ev.size(), &n, &used),
        GCOMP_OK)
        << "zstd " << flags;
    EXPECT_EQ(used, s.size())
        << "zstd " << flags << ": walk stopped " << (s.size() - used)
        << " bytes short";

    uint64_t at = 0;
    size_t frames = 0;
    std::vector<uint8_t> rebuilt;
    for (size_t i = 0; i < n; i++) {
      if (ev[i].kind == GCOMP_WALK_BLOCK) {
        continue;
      }
      EXPECT_EQ(ev[i].offset, at) << "zstd " << flags << ": unit " << i;
      at = ev[i].offset + ev[i].size;
      if (ev[i].kind == GCOMP_WALK_FRAME) {
        frames++;
        const std::vector<uint8_t> frame(s.begin() + (size_t)ev[i].offset,
            s.begin() + (size_t)(ev[i].offset + ev[i].size));
        const std::vector<uint8_t> part =
            decode_all(frame, original.size() + 1024);
        rebuilt.insert(rebuilt.end(), part.begin(), part.end());
      }
    }
    EXPECT_EQ(at, s.size()) << "zstd " << flags;
    EXPECT_GT(frames, 0u) << "zstd " << flags;
    ASSERT_EQ(rebuilt.size(), original.size()) << "zstd " << flags;
    EXPECT_EQ(std::memcmp(rebuilt.data(), original.data(), original.size()), 0)
        << "zstd " << flags;
  }

  std::remove(in_path.c_str());

  EXPECT_GE(ran, 4u)
      << "only " << ran << " of " << (sizeof(flag_sets) / sizeof(flag_sets[0]))
      << " reference encodings ran; this test checked almost nothing";
}

/**
 * @brief Say so if no reference ran.
 *
 * A skipped oracle test and an absent one look identical in the summary line.
 */
TEST(ZstdWalkOracle, OracleIsActuallyAvailable) {
  if (skip_oracle()) {
    GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
  }
  ASSERT_TRUE(has_zstd_cli())
      << "the zstd CLI was not found, so nothing in this file walked a stream "
         "this library did not itself produce. Install it, or set "
         "GCOMP_SKIP_ORACLE_TESTS=1 to say the gap is intentional.";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
