/**
 * @file test_seekable.cpp
 *
 * Random access into a multi-frame Zstandard stream.
 *
 * Two things have to be true, and they are tested against different things on
 * purpose:
 *
 * 1. **The table we read is the table the format defines.** Checked against
 *    files written by libzstd through pyzstd's `SeekableZstdFile`, because a
 *    seek table we both wrote and read would agree with itself whatever it
 *    meant.
 * 2. **A read returns what a full decode returns.** Checked by decoding the
 *    whole stream once and comparing every windowed read against it -
 *    including reads that straddle frame boundaries, which is where an index
 *    that is off by one frame still looks plausible.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/seekable.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

bool skip_oracle() {
  const char * e = std::getenv("GCOMP_SKIP_ORACLE_TESTS");
  return e && std::string(e) == "1";
}

bool has_pyzstd_seekable() {
  return std::system(
             "python3 -c 'import pyzstd; pyzstd.SeekableZstdFile' "
             ">/dev/null 2>&1") == 0;
}

std::string temp_path(const char * tag) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), "/tmp/gcomp_seek_%s_%d_%p", tag,
      (int)getpid(), (void *)buf);
  return std::string(buf);
}

std::vector<uint8_t> read_file(const std::string & p) {
  std::vector<uint8_t> v;
  FILE * f = std::fopen(p.c_str(), "rb");
  if (!f) {
    return v;
  }
  uint8_t buf[65536];
  size_t got;
  while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    v.insert(v.end(), buf, buf + got);
  }
  std::fclose(f);
  return v;
}

std::vector<uint8_t> make_data(size_t len) {
  std::vector<uint8_t> v(len);
  for (size_t i = 0; i < len; i++) {
    v[i] = (uint8_t)((i * 7u + (i / 997u)) % 251u);
  }
  return v;
}

/// One frame with its content size declared, so it can be indexed.
std::vector<uint8_t> encode_frame(
    const std::vector<uint8_t> & in, int64_t level) {
  gcomp_options_t * o = nullptr;
  EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_int64(o, "zstd.level", level), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_uint64(o, "zstd.content_size", in.size()),
      GCOMP_OK);
  size_t bound = 0;
  EXPECT_EQ(gcomp_encode_bound(nullptr, "zstd", o, in.size(), &bound), GCOMP_OK);
  std::vector<uint8_t> out(bound ? bound : 1);
  size_t w = 0;
  EXPECT_EQ(gcomp_encode_buffer(nullptr, "zstd", o, in.data(), in.size(),
                out.data(), out.size(), &w),
      GCOMP_OK);
  gcomp_options_destroy(o);
  out.resize(w);
  return out;
}

/// Every read a seekable stream can answer must match the plain decode.
void check_reads_against(
    gcomp_seekable_t * s, const std::vector<uint8_t> & expect) {
  ASSERT_EQ(gcomp_seekable_size(s), (uint64_t)expect.size());

  struct Window {
    uint64_t offset;
    size_t len;
  };
  std::vector<Window> windows;
  const size_t n = expect.size();
  // Boundaries first: a frame edge is where an index that is off by one still
  // returns the right number of bytes.
  for (uint64_t at : {(uint64_t)0, (uint64_t)1, (uint64_t)65535,
           (uint64_t)65536, (uint64_t)65537, (uint64_t)131072}) {
    if (at < n) {
      windows.push_back({at, 1});
      windows.push_back({at, 1000});
      windows.push_back({at, 70000}); // spans frames
    }
  }
  windows.push_back({(uint64_t)(n - 1), 1});
  windows.push_back({(uint64_t)(n / 2), n / 2});
  windows.push_back({0, n});                 // the whole thing
  windows.push_back({(uint64_t)n, 100});     // exactly at the end
  windows.push_back({(uint64_t)(n + 10), 100}); // past it

  for (const Window & w : windows) {
    std::vector<uint8_t> got(w.len ? w.len : 1, 0);
    size_t read = 0;
    ASSERT_EQ(gcomp_seekable_read(s, w.offset, got.data(), w.len, &read),
        GCOMP_OK)
        << "offset " << w.offset << " len " << w.len;

    size_t want = 0;
    if (w.offset < n) {
      want = (size_t)((uint64_t)n - w.offset);
      if (want > w.len) {
        want = w.len;
      }
    }
    ASSERT_EQ(read, want) << "offset " << w.offset << " len " << w.len;
    if (want > 0) {
      EXPECT_EQ(std::memcmp(got.data(), expect.data() + w.offset, want), 0)
          << "offset " << w.offset << " len " << w.len;
    }
  }
}

} // namespace

/**
 * @brief The seek table libzstd writes is the one we read.
 *
 * pyzstd's SeekableZstdFile writes the format as the reference implements it.
 * Reading our own output back would say only that we are self-consistent.
 */
TEST(Seekable, ReadsATableTheReferenceWrote) {
  if (skip_oracle()) {
    GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
  }
  if (!has_pyzstd_seekable()) {
    GTEST_SKIP() << "pyzstd with SeekableZstdFile not available";
  }

  const std::vector<uint8_t> original = make_data(500000);
  const std::string raw_path = temp_path("raw");
  const std::string zst_path = temp_path("zst");
  {
    FILE * f = std::fopen(raw_path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(std::fwrite(original.data(), 1, original.size(), f),
        original.size());
    std::fclose(f);
  }

  // Several frame sizes, so the index is exercised at more than one shape.
  for (size_t frame_size : {(size_t)16384, (size_t)65536, (size_t)200000}) {
    char cmd[1024];
    std::snprintf(cmd, sizeof(cmd),
        "python3 -c \"import pyzstd; "
        "d=open('%s','rb').read(); "
        "f=pyzstd.SeekableZstdFile('%s','w',level_or_option=3,"
        "max_frame_content_size=%zu); f.write(d); f.close()\" 2>/dev/null",
        raw_path.c_str(), zst_path.c_str(), frame_size);
    ASSERT_EQ(std::system(cmd), 0) << "frame size " << frame_size;

    const std::vector<uint8_t> comp = read_file(zst_path);
    ASSERT_GT(comp.size(), 0u) << "frame size " << frame_size;

    gcomp_seekable_t * s = nullptr;
    ASSERT_EQ(gcomp_seekable_open_buffer(
                  nullptr, "zstd", nullptr, comp.data(), comp.size(), &s),
        GCOMP_OK)
        << "frame size " << frame_size;
    EXPECT_TRUE(gcomp_seekable_has_table(s))
        << "frame size " << frame_size
        << ": the reference wrote a seek table and it was not found";
    EXPECT_GT(gcomp_seekable_frame_count(s), 1u) << "frame size " << frame_size;
    check_reads_against(s, original);
    gcomp_seekable_close(s);

    // And the same file is still an ordinary stream to a decoder that knows
    // nothing about tables, which is the property the format exists to keep.
    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 0),
        GCOMP_OK);
    std::vector<uint8_t> plain(original.size() + 1024);
    size_t produced = 0;
    EXPECT_EQ(gcomp_decode_buffer(nullptr, "zstd", o, comp.data(), comp.size(),
                  plain.data(), plain.size(), &produced),
        GCOMP_OK)
        << "frame size " << frame_size
        << ": a seekable file did not decode as a plain stream";
    EXPECT_EQ(produced, original.size());
    gcomp_options_destroy(o);
    std::remove(zst_path.c_str());
  }
  std::remove(raw_path.c_str());
}

/**
 * @brief A file with no table is indexed by walking it.
 *
 * Concatenated frames, each declaring its size. Nothing wrote a table, and the
 * file is random-access anyway - which is what makes this useful for files
 * that were never produced with seeking in mind.
 */
TEST(Seekable, IndexesAFileThatHasNoTable) {
  std::vector<uint8_t> expect;
  std::vector<uint8_t> stream;
  const size_t pieces[] = {65536, 65536, 40000, 65536, 12345};
  size_t at = 0;
  const std::vector<uint8_t> all = make_data(300000);
  for (size_t p : pieces) {
    if (at >= all.size()) {
      break;
    }
    size_t len = p;
    if (at + len > all.size()) {
      len = all.size() - at;
    }
    const std::vector<uint8_t> piece(all.begin() + at, all.begin() + at + len);
    expect.insert(expect.end(), piece.begin(), piece.end());
    const std::vector<uint8_t> f = encode_frame(piece, 3);
    stream.insert(stream.end(), f.begin(), f.end());
    at += len;
  }

  gcomp_seekable_t * s = nullptr;
  ASSERT_EQ(gcomp_seekable_open_buffer(
                nullptr, "zstd", nullptr, stream.data(), stream.size(), &s),
      GCOMP_OK);
  EXPECT_FALSE(gcomp_seekable_has_table(s))
      << "no table was written, so none should have been found";
  EXPECT_EQ(gcomp_seekable_frame_count(s), 5u);
  check_reads_against(s, expect);
  gcomp_seekable_close(s);
}

/**
 * @brief What must be refused, and refused at open.
 *
 * A frame with no declared size cannot be placed without decoding everything
 * before it, so opening one would produce an object whose every read decoded
 * the whole file. And a table that does not describe its own file is a
 * truncated or edited file, which is worth saying at open rather than part way
 * through somebody's read.
 */
TEST(Seekable, RefusesWhatItCannotIndex) {
  const std::vector<uint8_t> in = make_data(100000);

  // A frame that does not declare its decompressed size.
  {
    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_int64(o, "zstd.level", 3), GCOMP_OK);
    size_t bound = 0;
    ASSERT_EQ(gcomp_encode_bound(nullptr, "zstd", o, in.size(), &bound),
        GCOMP_OK);
    std::vector<uint8_t> f(bound);
    size_t w = 0;
    ASSERT_EQ(gcomp_encode_buffer(nullptr, "zstd", o, in.data(), in.size(),
                  f.data(), f.size(), &w),
        GCOMP_OK);
    gcomp_options_destroy(o);
    f.resize(w);

    gcomp_seekable_t * s = nullptr;
    EXPECT_EQ(gcomp_seekable_open_buffer(
                  nullptr, "zstd", nullptr, f.data(), f.size(), &s),
        GCOMP_ERR_UNSUPPORTED)
        << "a frame with no Frame_Content_Size was accepted";
    EXPECT_EQ(s, nullptr);
  }

  // A method that has no seekable form.
  {
    const std::vector<uint8_t> f = encode_frame(in, 3);
    gcomp_seekable_t * s = nullptr;
    EXPECT_EQ(gcomp_seekable_open_buffer(
                  nullptr, "lz4", nullptr, f.data(), f.size(), &s),
        GCOMP_ERR_UNSUPPORTED);
  }

  // Rubbish.
  {
    const uint8_t junk[32] = {0};
    gcomp_seekable_t * s = nullptr;
    EXPECT_NE(gcomp_seekable_open_buffer(
                  nullptr, "zstd", nullptr, junk, sizeof(junk), &s),
        GCOMP_OK);
  }
}

/**
 * @brief A truncated seekable file is refused at open.
 *
 * The table says how long each frame is; if those do not add up to the bytes
 * in front of the table, the file is not the one the table describes.
 */
TEST(Seekable, RefusesATableThatDoesNotDescribeItsFile) {
  if (skip_oracle()) {
    GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
  }
  if (!has_pyzstd_seekable()) {
    GTEST_SKIP() << "pyzstd with SeekableZstdFile not available";
  }

  const std::vector<uint8_t> original = make_data(200000);
  const std::string raw_path = temp_path("raw2");
  const std::string zst_path = temp_path("zst2");
  {
    FILE * f = std::fopen(raw_path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(
        std::fwrite(original.data(), 1, original.size(), f), original.size());
    std::fclose(f);
  }
  char cmd[1024];
  std::snprintf(cmd, sizeof(cmd),
      "python3 -c \"import pyzstd; d=open('%s','rb').read(); "
      "f=pyzstd.SeekableZstdFile('%s','w',level_or_option=3,"
      "max_frame_content_size=32768); f.write(d); f.close()\" 2>/dev/null",
      raw_path.c_str(), zst_path.c_str());
  ASSERT_EQ(std::system(cmd), 0);
  const std::vector<uint8_t> comp = read_file(zst_path);
  std::remove(zst_path.c_str());
  std::remove(raw_path.c_str());
  ASSERT_GT(comp.size(), 64u);

  // Cut a frame's worth out of the middle, keeping the table intact.
  std::vector<uint8_t> cut;
  cut.insert(cut.end(), comp.begin(), comp.begin() + (comp.size() / 4));
  cut.insert(cut.end(), comp.begin() + (comp.size() / 2), comp.end());

  gcomp_seekable_t * s = nullptr;
  EXPECT_EQ(gcomp_seekable_open_buffer(
                nullptr, "zstd", nullptr, cut.data(), cut.size(), &s),
      GCOMP_ERR_CORRUPT)
      << "a table whose frames do not fill the file was accepted";
  EXPECT_EQ(s, nullptr);
}

/// Say so if the reference is missing.
TEST(Seekable, OracleIsActuallyAvailable) {
  if (skip_oracle()) {
    GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
  }
  ASSERT_TRUE(has_pyzstd_seekable())
      << "pyzstd with SeekableZstdFile was not found, so nothing here checked "
         "our seek-table reader against the format as the reference writes "
         "it. Install it (apt install python3-pyzstd), or set "
         "GCOMP_SKIP_ORACLE_TESTS=1 to say the gap is intentional.";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
