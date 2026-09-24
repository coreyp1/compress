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
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <temp_file.h>
#include <unistd.h>
#include <vector>

namespace {

bool skip_oracle() {
  const char * e = std::getenv("GCOMP_SKIP_ORACLE_TESTS");
  return e && std::string(e) == "1";
}

bool has_pyzstd_seekable() {
  return std::system(
             // Double quotes: cmd.exe passes single quotes through, and
             // python then evaluates a string literal and succeeds whether
             // pyzstd is there or not.
             "python3 -c \"import pyzstd; pyzstd.SeekableZstdFile\" "
             ">" GCOMP_TEST_NULL_DEVICE " 2>&1") == 0;
}

std::string temp_path(const char * tag) {
  // The name this replaces was "/tmp/...%d_%p" of a stack address: the
  // same value on every call from the same frame, in a directory named
  // outright rather than asked for.  cutil creates the file as it names
  // it, under gcu_path_temp_dir().
  //
  // Forward slashes, because these names are pasted into Python string
  // literals, where a Windows path's backslashes are escape sequences.
  return gcomp_test::toPosixPath(
      gcomp_test::uniqueTempPath("gcomp_seek", std::string("_") + tag));
}

std::vector<uint8_t> read_file(const std::string & p) {
  return gcomp_test::readWholeFile(p);
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

/**
 * @brief A gcomp_seek_cb over a std::vector, with instrumentation.
 *
 * Counts calls and bytes, so a test can assert what a read actually fetched
 * rather than only that it returned the right answer. `chunk` caps what one
 * call will hand over, which is how the short-read path gets walked: a source
 * is allowed to answer in pieces the way read(2) is.
 */
struct MemorySource {
  const std::vector<uint8_t> * file = nullptr;
  size_t chunk = 0; ///< 0 means "as much as asked for".
  size_t calls = 0;
  uint64_t bytes = 0;
  gcomp_status_t fail_with = GCOMP_OK; ///< Returned once `fail_after` is hit.
  size_t fail_after = SIZE_MAX;        ///< Calls before failing.

  static gcomp_status_t read(
      void * ctx, uint64_t offset, void * dst, size_t len, size_t * read_out) {
    MemorySource * self = static_cast<MemorySource *>(ctx);
    *read_out = 0;
    if (self->calls >= self->fail_after) {
      return self->fail_with;
    }
    self->calls++;
    if (offset >= self->file->size()) {
      return GCOMP_OK; // End of file: a legitimate zero-length answer.
    }
    size_t avail = self->file->size() - (size_t)offset;
    if (avail > len) {
      avail = len;
    }
    if (self->chunk && avail > self->chunk) {
      avail = self->chunk;
    }
    std::memcpy(dst, self->file->data() + (size_t)offset, avail);
    *read_out = avail;
    self->bytes += avail;
    return GCOMP_OK;
  }
};

/// A gcomp_seek_cb over a real file on disk, which is the point of the API.
struct FileSource {
  std::FILE * fp = nullptr;
  uint64_t bytes = 0;

  static gcomp_status_t read(
      void * ctx, uint64_t offset, void * dst, size_t len, size_t * read_out) {
    FileSource * self = static_cast<FileSource *>(ctx);
    *read_out = 0;
    if (std::fseek(self->fp, (long)offset, SEEK_SET) != 0) {
      return GCOMP_ERR_IO;
    }
    const size_t got = std::fread(dst, 1, len, self->fp);
    if (got == 0 && std::ferror(self->fp)) {
      return GCOMP_ERR_IO;
    }
    *read_out = got;
    self->bytes += got;
    return GCOMP_OK;
  }
};

/**
 * @brief Data that does not compress, so the file is about as big as the input.
 *
 * make_data() is a cheap arithmetic pattern and compresses roughly 40:1, which
 * makes a 4 MB input into a 100 KB file - fine for correctness, useless for
 * measuring whether a read fetched a frame or the file, because at that size
 * the two are not far apart.
 */
std::vector<uint8_t> make_incompressible(size_t len) {
  std::vector<uint8_t> v(len);
  uint64_t x = 0x9E3779B97F4A7C15ull;
  for (size_t i = 0; i < len; i++) {
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    v[i] = (uint8_t)((x * 0x2545F4914F6CDD1Dull) >> 56);
  }
  return v;
}

/// Write `in` as a seekable file and return the bytes.
std::vector<uint8_t> write_seekable(
    const std::vector<uint8_t> & in, uint64_t frame_size, int checksum) {
  gcomp_options_t * o = nullptr;
  EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_uint64(o, "zstd.seekable_frame_size", frame_size),
      GCOMP_OK);
  EXPECT_EQ(
      gcomp_options_set_bool(o, "zstd.seekable_checksum", checksum), GCOMP_OK);
  size_t bound = 0;
  EXPECT_EQ(gcomp_seekable_write_bound(nullptr, "zstd", o, in.size(), &bound),
      GCOMP_OK);
  std::vector<uint8_t> file(bound ? bound : 1);
  size_t written = 0;
  EXPECT_EQ(gcomp_seekable_write_buffer(nullptr, "zstd", o, in.data(),
                in.size(), file.data(), file.size(), &written),
      GCOMP_OK);
  gcomp_options_destroy(o);
  file.resize(written);
  return file;
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
        "max_frame_content_size=%zu); f.write(d); f.close()\" "
        "2>" GCOMP_TEST_NULL_DEVICE,
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
      "max_frame_content_size=32768); f.write(d); f.close()\" "
      "2>" GCOMP_TEST_NULL_DEVICE,
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

/**
 * @brief What we write, we read.
 *
 * The cheap half of the round trip, and the one that would pass even if our
 * idea of the format were wrong - which is why the reference test below
 * exists. Worth having anyway: it covers the option combinations quickly.
 */
TEST(Seekable, WritesFilesItCanReadBack) {
  const std::vector<uint8_t> in = make_data(500000);

  for (uint64_t frame_size : {(uint64_t)1024, (uint64_t)65536,
           (uint64_t)200000, (uint64_t)1000000}) {
    for (int checksum : {0, 1}) {
      gcomp_options_t * o = nullptr;
      ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
      ASSERT_EQ(
          gcomp_options_set_uint64(o, "zstd.seekable_frame_size", frame_size),
          GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_bool(o, "zstd.seekable_checksum", checksum),
          GCOMP_OK);

      size_t bound = 0;
      ASSERT_EQ(
          gcomp_seekable_write_bound(nullptr, "zstd", o, in.size(), &bound),
          GCOMP_OK)
          << "frame " << frame_size << " checksum " << checksum;
      std::vector<uint8_t> file(bound);
      size_t written = 0;
      ASSERT_EQ(gcomp_seekable_write_buffer(nullptr, "zstd", o, in.data(),
                    in.size(), file.data(), file.size(), &written),
          GCOMP_OK)
          << "frame " << frame_size << " checksum " << checksum;
      ASSERT_LE(written, bound)
          << "frame " << frame_size << ": the file is larger than the bound";
      file.resize(written);
      gcomp_options_destroy(o);

      gcomp_seekable_t * s = nullptr;
      ASSERT_EQ(gcomp_seekable_open_buffer(
                    nullptr, "zstd", nullptr, file.data(), file.size(), &s),
          GCOMP_OK)
          << "frame " << frame_size << " checksum " << checksum;
      EXPECT_TRUE(gcomp_seekable_has_table(s));
      const uint64_t want_frames =
          (in.size() + frame_size - 1u) / frame_size;
      EXPECT_EQ((uint64_t)gcomp_seekable_frame_count(s), want_frames)
          << "frame " << frame_size;
      check_reads_against(s, in);
      gcomp_seekable_close(s);
    }
  }
}

/**
 * @brief What we write, the reference reads - both ways round.
 *
 * Two separate claims, and a file can satisfy one without the other:
 *
 * - libzstd's own seekable reader seeks in it and gets the right bytes, which
 *   says the table means what the format says it means.
 * - libzstd decompresses it as an ordinary stream, which says the table is a
 *   skippable frame and not something a plain decoder would choke on. That is
 *   the property the whole format choice exists to keep, and it is easy to
 *   lose by writing a table that is correct but framed wrongly.
 */
TEST(Seekable, TheReferenceReadsWhatWeWrite) {
  if (skip_oracle()) {
    GTEST_SKIP() << "Oracle tests disabled via GCOMP_SKIP_ORACLE_TESTS";
  }
  if (!has_pyzstd_seekable()) {
    GTEST_SKIP() << "pyzstd with SeekableZstdFile not available";
  }

  const std::vector<uint8_t> in = make_data(500000);
  size_t ran = 0;

  for (uint64_t frame_size : {(uint64_t)16384, (uint64_t)65536,
           (uint64_t)250000}) {
    for (int checksum : {0, 1}) {
      gcomp_options_t * o = nullptr;
      ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
      ASSERT_EQ(
          gcomp_options_set_uint64(o, "zstd.seekable_frame_size", frame_size),
          GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_bool(o, "zstd.seekable_checksum", checksum),
          GCOMP_OK);
      size_t bound = 0;
      ASSERT_EQ(
          gcomp_seekable_write_bound(nullptr, "zstd", o, in.size(), &bound),
          GCOMP_OK);
      std::vector<uint8_t> file(bound);
      size_t written = 0;
      ASSERT_EQ(gcomp_seekable_write_buffer(nullptr, "zstd", o, in.data(),
                    in.size(), file.data(), file.size(), &written),
          GCOMP_OK);
      file.resize(written);
      gcomp_options_destroy(o);

      const std::string zst_path = temp_path("ours");
      {
        FILE * f = std::fopen(zst_path.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(std::fwrite(file.data(), 1, file.size(), f), file.size());
        std::fclose(f);
      }

      // The reference seeks in it, reads it whole, and decompresses it as a
      // plain stream. Any of the three failing is a different defect.
      char cmd[1400];
      std::snprintf(cmd, sizeof(cmd),
          "python3 -c \"import pyzstd; "
          "d=bytes(((i*7 + (i//997)) %% 251) for i in range(500000)); "
          "f=pyzstd.SeekableZstdFile('%s','r'); f.seek(123456); "
          "assert f.read(1000)==d[123456:124456], 'seek'; f.seek(0); "
          "assert f.read()==d, 'whole'; f.close(); "
          "assert pyzstd.decompress(open('%s','rb').read())==d, 'plain'\" "
          "2>" GCOMP_TEST_NULL_DEVICE,
          zst_path.c_str(), zst_path.c_str());
      EXPECT_EQ(std::system(cmd), 0)
          << "frame " << frame_size << " checksum " << checksum
          << ": the reference could not read a file we wrote";
      std::remove(zst_path.c_str());
      ran++;
    }
  }
  EXPECT_EQ(ran, 6u);
}

/// A buffer too small to hold the file is refused rather than half-filled.
TEST(Seekable, ASmallOutputBufferIsRefused) {
  const std::vector<uint8_t> in = make_data(200000);
  gcomp_options_t * o = nullptr;
  ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_uint64(o, "zstd.seekable_frame_size", 16384),
      GCOMP_OK);

  size_t bound = 0;
  ASSERT_EQ(gcomp_seekable_write_bound(nullptr, "zstd", o, in.size(), &bound),
      GCOMP_OK);
  std::vector<uint8_t> full(bound);
  size_t written = 0;
  ASSERT_EQ(gcomp_seekable_write_buffer(nullptr, "zstd", o, in.data(),
                in.size(), full.data(), full.size(), &written),
      GCOMP_OK);
  ASSERT_GT(written, 64u);

  // Just short of what it needs, and far short.
  for (size_t cap : {written - 1, written / 2, (size_t)16, (size_t)1}) {
    std::vector<uint8_t> out(cap);
    size_t w = 0;
    EXPECT_NE(gcomp_seekable_write_buffer(nullptr, "zstd", o, in.data(),
                  in.size(), out.data(), out.size(), &w),
        GCOMP_OK)
        << "capacity " << cap << " of " << written << " was accepted";
    EXPECT_EQ(w, 0u) << "capacity " << cap
                     << ": a failed write reported bytes written";
  }
  gcomp_options_destroy(o);
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

/**
 * @brief A callback source and a buffer source must agree exactly.
 *
 * The format and the index do not change with the source - only where the
 * bytes come from - so every answer has to be identical, not merely
 * plausible. Checked across frame sizes, with and without a seek table, and
 * with a source that answers in one-byte pieces so the read loop that
 * tolerates a short answer is actually walked.
 */
TEST(Seekable, ACallbackSourceAnswersLikeABuffer) {
  const std::vector<uint8_t> in = make_data(500000);

  for (uint64_t frame_size : {(uint64_t)1024, (uint64_t)65536,
           (uint64_t)200000, (uint64_t)1000000}) {
    const std::vector<uint8_t> file = write_seekable(in, frame_size, 1);

    // The buffer source is the reference.
    gcomp_seekable_t * b = nullptr;
    ASSERT_EQ(gcomp_seekable_open_buffer(
                  nullptr, "zstd", nullptr, file.data(), file.size(), &b),
        GCOMP_OK)
        << "frame " << frame_size;

    // Whole answers, and one-byte answers, which are the same source with a
    // different granularity.
    for (size_t chunk : {(size_t)0, (size_t)1, (size_t)7}) {
      MemorySource src;
      src.file = &file;
      src.chunk = chunk;

      gcomp_seekable_t * c = nullptr;
      ASSERT_EQ(gcomp_seekable_open_cb(nullptr, "zstd", nullptr,
                    &MemorySource::read, &src, (uint64_t)file.size(), &c),
          GCOMP_OK)
          << "frame " << frame_size << " chunk " << chunk;

      EXPECT_EQ(gcomp_seekable_size(c), gcomp_seekable_size(b))
          << "frame " << frame_size << " chunk " << chunk;
      EXPECT_EQ(gcomp_seekable_frame_count(c), gcomp_seekable_frame_count(b))
          << "frame " << frame_size << " chunk " << chunk;
      EXPECT_EQ(gcomp_seekable_has_table(c), gcomp_seekable_has_table(b))
          << "frame " << frame_size << " chunk " << chunk;

      // The same battery of windows the buffer path is held to.
      check_reads_against(c, in);
      gcomp_seekable_close(c);
      if (::testing::Test::HasFatalFailure()) {
        gcomp_seekable_close(b);
        return;
      }
    }
    gcomp_seekable_close(b);
  }
}

/**
 * @brief A file with no seek table is indexed through a callback too.
 *
 * This is the path that walks every byte of the file through a fixed window
 * rather than reading a table, so it is the one where the window's retention
 * of bytes the walker declined matters. Concatenated frames of deliberately
 * uneven sizes, so frame and block headers land at assorted offsets relative
 * to the window rather than all at multiples of it.
 */
TEST(Seekable, IndexesATablelessFileThroughACallback) {
  std::vector<uint8_t> file;
  std::vector<uint8_t> expect;
  for (size_t len : {size_t{1}, size_t{5}, size_t{999}, size_t{65536},
           size_t{100000}, size_t{7}, size_t{250000}}) {
    const std::vector<uint8_t> part = make_data(len);
    const std::vector<uint8_t> frame = encode_frame(part, 3);
    file.insert(file.end(), frame.begin(), frame.end());
    expect.insert(expect.end(), part.begin(), part.end());
  }

  gcomp_seekable_t * b = nullptr;
  ASSERT_EQ(gcomp_seekable_open_buffer(
                nullptr, "zstd", nullptr, file.data(), file.size(), &b),
      GCOMP_OK);
  ASSERT_FALSE(gcomp_seekable_has_table(b));

  for (size_t chunk : {(size_t)0, (size_t)1, (size_t)13}) {
    MemorySource src;
    src.file = &file;
    src.chunk = chunk;
    gcomp_seekable_t * c = nullptr;
    ASSERT_EQ(gcomp_seekable_open_cb(nullptr, "zstd", nullptr,
                  &MemorySource::read, &src, (uint64_t)file.size(), &c),
        GCOMP_OK)
        << "chunk " << chunk;
    EXPECT_EQ(gcomp_seekable_has_table(c), 0) << "chunk " << chunk;
    EXPECT_EQ(gcomp_seekable_frame_count(c), gcomp_seekable_frame_count(b))
        << "chunk " << chunk;
    EXPECT_EQ(gcomp_seekable_size(c), gcomp_seekable_size(b))
        << "chunk " << chunk;
    check_reads_against(c, expect);
    gcomp_seekable_close(c);
    if (::testing::Test::HasFatalFailure()) {
      gcomp_seekable_close(b);
      return;
    }
  }
  gcomp_seekable_close(b);
}

/**
 * @brief The whole point: a read fetches a frame, not the file.
 *
 * Every other test here would pass an implementation that read the entire file
 * into memory and then answered out of it - correct, and useless for the case
 * this API exists for. So this one measures what the source was actually asked
 * for.
 *
 * Opening a file that carries a table must not touch the frames at all, and
 * reading one byte must fetch about one frame rather than the file.
 */
TEST(Seekable, ACallbackSourceFetchesOnlyWhatItNeeds) {
  // Incompressible on purpose. make_data() compresses about 40:1, which would
  // make the file small enough that "a frame" and "the file" are the same
  // order of magnitude and the measurement below would prove nothing.
  const std::vector<uint8_t> in = make_incompressible(4u * 1024u * 1024u);
  const uint64_t frame_size = 64u * 1024u;
  const std::vector<uint8_t> file = write_seekable(in, frame_size, 1);
  ASSERT_GT(file.size(), size_t{1024u * 1024u})
      << "the file needs to be big enough for 'a frame' and 'the file' to "
         "differ by a lot; it is " << file.size();

  MemorySource src;
  src.file = &file;

  gcomp_seekable_t * s = nullptr;
  ASSERT_EQ(gcomp_seekable_open_cb(nullptr, "zstd", nullptr,
                &MemorySource::read, &src, (uint64_t)file.size(), &s),
      GCOMP_OK);
  ASSERT_TRUE(gcomp_seekable_has_table(s));

  // Opening read the footer, the skippable frame's header, and the entries.
  // Nothing else: the frames were not touched.
  const uint64_t open_bytes = src.bytes;
  const uint64_t frames = gcomp_seekable_frame_count(s);
  const uint64_t table_max = 9u + 8u + frames * 12u;
  EXPECT_LE(open_bytes, table_max)
      << "opening fetched " << open_bytes << " bytes; the footer, the "
      << "skippable header and " << frames << " entries are at most "
      << table_max;
  EXPECT_LT(open_bytes, file.size() / 4u)
      << "opening fetched " << open_bytes << " of " << file.size()
      << " bytes, which is not an index - it is reading the file";

  // One byte from the middle.
  const uint64_t before = src.bytes;
  uint8_t one = 0;
  size_t got = 0;
  ASSERT_EQ(gcomp_seekable_read(s, in.size() / 2u, &one, 1u, &got), GCOMP_OK);
  ASSERT_EQ(got, size_t{1});
  EXPECT_EQ(one, in[in.size() / 2u]);
  const uint64_t one_byte_cost = src.bytes - before;

  // A frame's compressed size, generously: the content is compressible, so a
  // frame's worth of bytes is well under frame_size. Being under a quarter of
  // the file is the claim that matters - it did not read the file to answer.
  EXPECT_LE(one_byte_cost, frame_size + 1024u)
      << "one byte cost " << one_byte_cost << " fetched bytes; a frame holds "
      << frame_size << " decompressed";
  EXPECT_LT(one_byte_cost, file.size() / 4u)
      << "one byte cost " << one_byte_cost << " of " << file.size();

  // A second read inside the same frame must fetch nothing at all: the decoded
  // frame is cached.
  const uint64_t cached_before = src.bytes;
  ASSERT_EQ(gcomp_seekable_read(s, in.size() / 2u + 10u, &one, 1u, &got),
      GCOMP_OK);
  ASSERT_EQ(got, size_t{1});
  EXPECT_EQ(src.bytes, cached_before)
      << "a second read in the same frame fetched "
      << (src.bytes - cached_before) << " bytes; the frame was already decoded";

  gcomp_seekable_close(s);
}

/**
 * @brief A real file on disk, which is what the API is for.
 *
 * MemorySource proves the logic; this proves the thing a caller will actually
 * write compiles and works, through fseek and fread, with the file never held
 * in memory.
 */
TEST(Seekable, ReadsFromARealFileThroughACallback) {
  const std::vector<uint8_t> in = make_data(300000);
  const std::vector<uint8_t> file = write_seekable(in, 32768, 1);

  const std::string path = temp_path("cbfile");
  {
    std::FILE * w = std::fopen(path.c_str(), "wb");
    ASSERT_NE(w, nullptr);
    ASSERT_EQ(std::fwrite(file.data(), 1, file.size(), w), file.size());
    ASSERT_EQ(std::fclose(w), 0);
  }

  FileSource src;
  src.fp = std::fopen(path.c_str(), "rb");
  ASSERT_NE(src.fp, nullptr);

  gcomp_seekable_t * s = nullptr;
  ASSERT_EQ(gcomp_seekable_open_cb(nullptr, "zstd", nullptr, &FileSource::read,
                &src, (uint64_t)file.size(), &s),
      GCOMP_OK);
  check_reads_against(s, in);

  // Closing the stream must not have touched the caller's source, which the
  // header promises: it is still usable here.
  gcomp_seekable_close(s);
  EXPECT_EQ(std::fseek(src.fp, 0, SEEK_SET), 0)
      << "gcomp_seekable_close() should not have closed the caller's file";
  std::fclose(src.fp);
  std::remove(path.c_str());
}

/**
 * @brief What a failing or lying source gets told.
 *
 * A source's own error comes back unchanged rather than being flattened into
 * something generic, because the caller is the only one who knows what
 * GCOMP_ERR_IO meant. A wrong total_size is a corrupt file, not a read past
 * the end of anything.
 */
TEST(Seekable, RefusesABadCallbackSource) {
  const std::vector<uint8_t> in = make_data(200000);
  const std::vector<uint8_t> file = write_seekable(in, 16384, 1);

  // NULL callback, and a zero length.
  gcomp_seekable_t * s = nullptr;
  EXPECT_EQ(gcomp_seekable_open_cb(nullptr, "zstd", nullptr, nullptr, nullptr,
                (uint64_t)file.size(), &s),
      GCOMP_ERR_INVALID_ARG);
  MemorySource ok;
  ok.file = &file;
  EXPECT_EQ(gcomp_seekable_open_cb(
                nullptr, "zstd", nullptr, &MemorySource::read, &ok, 0u, &s),
      GCOMP_ERR_INVALID_ARG);

  // A source that fails on its first call. The error is the source's own.
  {
    MemorySource bad;
    bad.file = &file;
    bad.fail_after = 0;
    bad.fail_with = GCOMP_ERR_IO;
    gcomp_seekable_t * c = nullptr;
    EXPECT_EQ(gcomp_seekable_open_cb(nullptr, "zstd", nullptr,
                  &MemorySource::read, &bad, (uint64_t)file.size(), &c),
        GCOMP_ERR_IO);
    EXPECT_EQ(c, nullptr);
  }

  // A source that opens but fails when a frame is fetched: the failure has to
  // surface from the read rather than be reported as a decode problem.
  {
    MemorySource late;
    late.file = &file;
    late.fail_with = GCOMP_ERR_IO;
    gcomp_seekable_t * c = nullptr;
    ASSERT_EQ(gcomp_seekable_open_cb(nullptr, "zstd", nullptr,
                  &MemorySource::read, &late, (uint64_t)file.size(), &c),
        GCOMP_OK);
    late.fail_after = late.calls; // Fail from the next call on.
    uint8_t byte = 0;
    size_t got = 0;
    EXPECT_EQ(gcomp_seekable_read(c, 0, &byte, 1u, &got), GCOMP_ERR_IO);
    gcomp_seekable_close(c);
  }

  // A total_size larger than the file. The footer is then read from bytes that
  // are not there, and the source reports a short read, which is corrupt.
  {
    MemorySource lying;
    lying.file = &file;
    gcomp_seekable_t * c = nullptr;
    EXPECT_EQ(gcomp_seekable_open_cb(nullptr, "zstd", nullptr,
                  &MemorySource::read, &lying, (uint64_t)file.size() + 4096u,
                  &c),
        GCOMP_ERR_CORRUPT);
    EXPECT_EQ(c, nullptr);
  }

  // A total_size *smaller* than the file is not an error, and it took a
  // measurement to establish that rather than a guess. Cutting the end off
  // removes the footer, so no table is found and the file is indexed by
  // walking instead; what the cut destroyed is part of the skippable frame the
  // table lives in, which carries no data, so the index that comes back is
  // complete and correct. A cut deep enough to reach a real frame drops that
  // frame and reports the shorter length, which is the documented behaviour
  // for a tableless file - it indexes the frames that are wholly there.
  //
  // What must hold is that the two sources say the same thing, and that is
  // asserted exhaustively in TruncationsAgreeBetweenSources below rather than
  // at one arbitrary offset here.
  {
    MemorySource cut;
    cut.file = &file;
    gcomp_seekable_t * c = nullptr;
    ASSERT_EQ(gcomp_seekable_open_cb(nullptr, "zstd", nullptr,
                  &MemorySource::read, &cut, (uint64_t)file.size() - 32u, &c),
        GCOMP_OK);
    EXPECT_EQ(gcomp_seekable_has_table(c), 0)
        << "the footer was cut off, so there is no table to have found";
    EXPECT_EQ(gcomp_seekable_size(c), (uint64_t)in.size())
        << "the cut only reached the skippable table frame, which holds no "
           "data, so every byte is still indexed";
    gcomp_seekable_close(c);
  }
}

/**
 * @brief Both sources agree on every possible truncation of one file.
 *
 * The property the callback path has to have is not "it works on a good file"
 * but "it is the same reader". A truncated file is where two implementations of
 * the same format drift: one runs off the end, the other stops early, and both
 * look fine on input that is not damaged.
 *
 * So this opens every prefix of one seekable file through both sources and
 * requires the status, the frame count, the decompressed size and the
 * has-table answer to match. Exhaustive rather than sampled, because which
 * offsets matter is decided by where the frame and block headers happen to
 * land, and that is not something to guess at.
 */
TEST(Seekable, TruncationsAgreeBetweenSources) {
  const std::vector<uint8_t> in = make_data(200000);
  const std::vector<uint8_t> file = write_seekable(in, 16384, 1);
  ASSERT_GT(file.size(), size_t{500});

  size_t opened = 0;
  for (size_t len = 1; len < file.size(); len++) {
    MemorySource src;
    src.file = &file;

    gcomp_seekable_t * c = nullptr;
    const gcomp_status_t sc = gcomp_seekable_open_cb(nullptr, "zstd", nullptr,
        &MemorySource::read, &src, (uint64_t)len, &c);

    gcomp_seekable_t * b = nullptr;
    const gcomp_status_t sb = gcomp_seekable_open_buffer(
        nullptr, "zstd", nullptr, file.data(), len, &b);

    ASSERT_EQ(sc, sb) << "at length " << len << ": callback said "
                      << gcomp_status_to_string(sc) << ", buffer said "
                      << gcomp_status_to_string(sb);
    if (sc == GCOMP_OK) {
      opened++;
      ASSERT_NE(c, nullptr);
      ASSERT_NE(b, nullptr);
      EXPECT_EQ(gcomp_seekable_frame_count(c), gcomp_seekable_frame_count(b))
          << "at length " << len;
      EXPECT_EQ(gcomp_seekable_size(c), gcomp_seekable_size(b))
          << "at length " << len;
      EXPECT_EQ(gcomp_seekable_has_table(c), gcomp_seekable_has_table(b))
          << "at length " << len;
    }
    gcomp_seekable_close(c);
    gcomp_seekable_close(b);
    if (::testing::Test::HasFatalFailure()) {
      return;
    }
  }

  // Both arms have to be reachable for the comparison to mean anything: a
  // sweep where every length was refused would agree perfectly and check
  // nothing.
  EXPECT_GT(opened, size_t{0})
      << "no truncation opened successfully, so the agreement is vacuous";
  EXPECT_LT(opened, file.size() - 1u)
      << "every truncation opened, so no rejection path was compared";
}

/**
 * @brief The streaming encoder writes a seekable file the reader accepts.
 *
 * gcomp_seekable_write_buffer() needs the whole input at once. `zstd.seekable`
 * on the encoder produces the same file from a stream, which is the case that
 * matters for anything that does not have its input in memory - a log being
 * appended to, a pipe, an upload.
 *
 * Driven through deliberately awkward chunk sizes, because the whole risk in a
 * streaming writer is the seams: a frame boundary that falls inside one call's
 * input, or an output buffer too small to take a staged frame in one go.
 */
TEST(Seekable, TheStreamingEncoderWritesSeekableFiles) {
  const std::vector<uint8_t> in = make_data(300000);

  for (uint64_t frame_size : {(uint64_t)1024, (uint64_t)65536,
           (uint64_t)100000, (uint64_t)1000000}) {
    for (int checksum : {0, 1}) {
      for (size_t in_chunk : {(size_t)1, (size_t)7, (size_t)4096,
               (size_t)300000}) {
        for (size_t out_chunk : {(size_t)1, (size_t)64, (size_t)1u << 20}) {
          gcomp_options_t * o = nullptr;
          ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
          ASSERT_EQ(gcomp_options_set_bool(o, "zstd.seekable", 1), GCOMP_OK);
          ASSERT_EQ(gcomp_options_set_uint64(
                        o, "zstd.seekable_frame_size", frame_size),
              GCOMP_OK);
          ASSERT_EQ(
              gcomp_options_set_bool(o, "zstd.seekable_checksum", checksum),
              GCOMP_OK);

          gcomp_encoder_t * enc = nullptr;
          ASSERT_EQ(gcomp_encoder_create(gcomp_registry_default(), "zstd", o, &enc), GCOMP_OK)
              << "frame " << frame_size << " checksum " << checksum;
          gcomp_options_destroy(o);

          std::vector<uint8_t> file;
          std::vector<uint8_t> scratch(out_chunk);

          size_t offset = 0;
          while (offset < in.size()) {
            size_t take = in_chunk;
            if (take > in.size() - offset) {
              take = in.size() - offset;
            }
            gcomp_buffer_t ib = {
                (void *)(in.data() + offset), take, 0};
            // Keep going until this chunk is consumed; a small output buffer
            // means several passes per chunk.
            while (ib.used < ib.size) {
              gcomp_buffer_t ob = {scratch.data(), scratch.size(), 0};
              const gcomp_status_t st =
                  gcomp_encoder_update(enc, &ib, &ob);
              ASSERT_EQ(st, GCOMP_OK)
                  << "frame " << frame_size << " in_chunk " << in_chunk
                  << " out_chunk " << out_chunk << ": "
                  << gcomp_status_to_string(st);
              file.insert(file.end(), scratch.begin(),
                  scratch.begin() + (long)ob.used);
              if (ob.used == 0 && ib.used == 0) {
                FAIL() << "update made no progress: frame " << frame_size
                       << " in_chunk " << in_chunk << " out_chunk "
                       << out_chunk;
              }
            }
            offset += ib.used;
          }

          // finish() returns GCOMP_ERR_LIMIT until it has handed everything
          // over, which with a one-byte buffer is a great many calls.
          for (;;) {
            gcomp_buffer_t ob = {scratch.data(), scratch.size(), 0};
            const gcomp_status_t st = gcomp_encoder_finish(enc, &ob);
            file.insert(file.end(), scratch.begin(),
                scratch.begin() + (long)ob.used);
            if (st == GCOMP_OK) {
              break;
            }
            ASSERT_EQ(st, GCOMP_ERR_LIMIT)
                << "frame " << frame_size << ": "
                << gcomp_status_to_string(st);
            ASSERT_GT(ob.used, size_t{0}) << "finish made no progress";
          }
          gcomp_encoder_destroy(enc);

          // It has a table, the right number of frames, and every read agrees
          // with the input.
          gcomp_seekable_t * sk = nullptr;
          ASSERT_EQ(gcomp_seekable_open_buffer(nullptr, "zstd", nullptr,
                        file.data(), file.size(), &sk),
              GCOMP_OK)
              << "frame " << frame_size << " checksum " << checksum
              << " in_chunk " << in_chunk << " out_chunk " << out_chunk;
          EXPECT_TRUE(gcomp_seekable_has_table(sk));
          const uint64_t want_frames =
              ((uint64_t)in.size() + frame_size - 1u) / frame_size;
          EXPECT_EQ((uint64_t)gcomp_seekable_frame_count(sk), want_frames)
              << "frame " << frame_size << " in_chunk " << in_chunk;
          check_reads_against(sk, in);
          gcomp_seekable_close(sk);

          // And it is an ordinary zstd stream: a plain decode of the whole
          // file returns the input, because the seek table is a skippable
          // frame that any decoder steps over.
          {
            std::vector<uint8_t> plain(in.size() + 64u);
            size_t got = 0;
            gcomp_options_t * d = nullptr;
            ASSERT_EQ(gcomp_options_create(&d), GCOMP_OK);
            ASSERT_EQ(gcomp_options_set_bool(d, "zstd.concat", 1), GCOMP_OK);
            ASSERT_EQ(gcomp_decode_buffer(nullptr, "zstd", d, file.data(),
                          file.size(), plain.data(), plain.size(), &got),
                GCOMP_OK)
                << "frame " << frame_size;
            gcomp_options_destroy(d);
            ASSERT_EQ(got, in.size());
            EXPECT_EQ(std::memcmp(plain.data(), in.data(), in.size()), 0);
          }

          if (::testing::Test::HasFatalFailure()) {
            return;
          }
        }
      }
    }
  }
}

/**
 * @brief The streaming writer and the buffer writer produce the same file.
 *
 * Not merely "both readable" - byte-identical. They are two writers of one
 * format, which is the arrangement that drifts, and the shared seek-table code
 * in src/core/seek_table.c is only worth having if this holds. If a field ever
 * moves in one and not the other, this is what says so.
 *
 * It is exact because both make frames of the same size out of the same bytes
 * with the same options, and a zstd frame is a deterministic function of those.
 */
TEST(Seekable, StreamingAndBufferWritersAgreeExactly) {
  for (size_t n : {size_t{0}, size_t{1}, size_t{1023}, size_t{1024},
           size_t{1025}, size_t{65536}, size_t{300000}}) {
    const std::vector<uint8_t> in = make_data(n);
    for (uint64_t frame_size : {(uint64_t)1024, (uint64_t)65536}) {
      for (int checksum : {0, 1}) {
        const std::vector<uint8_t> by_buffer =
            write_seekable(in, frame_size, checksum);

        gcomp_options_t * o = nullptr;
        ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
        ASSERT_EQ(gcomp_options_set_bool(o, "zstd.seekable", 1), GCOMP_OK);
        ASSERT_EQ(
            gcomp_options_set_uint64(o, "zstd.seekable_frame_size", frame_size),
            GCOMP_OK);
        ASSERT_EQ(gcomp_options_set_bool(o, "zstd.seekable_checksum", checksum),
            GCOMP_OK);
        gcomp_encoder_t * enc = nullptr;
        ASSERT_EQ(gcomp_encoder_create(gcomp_registry_default(), "zstd", o, &enc), GCOMP_OK);
        gcomp_options_destroy(o);

        std::vector<uint8_t> by_stream;
        std::vector<uint8_t> scratch(1u << 16);
        if (n > 0) {
          gcomp_buffer_t ib = {(void *)in.data(), in.size(), 0};
          while (ib.used < ib.size) {
            gcomp_buffer_t ob = {scratch.data(), scratch.size(), 0};
            ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
            by_stream.insert(by_stream.end(), scratch.begin(),
                scratch.begin() + (long)ob.used);
          }
        }
        for (;;) {
          gcomp_buffer_t ob = {scratch.data(), scratch.size(), 0};
          const gcomp_status_t st = gcomp_encoder_finish(enc, &ob);
          by_stream.insert(by_stream.end(), scratch.begin(),
              scratch.begin() + (long)ob.used);
          if (st == GCOMP_OK) {
            break;
          }
          ASSERT_EQ(st, GCOMP_ERR_LIMIT);
        }
        gcomp_encoder_destroy(enc);

        ASSERT_EQ(by_stream.size(), by_buffer.size())
            << "n=" << n << " frame=" << frame_size << " checksum=" << checksum
            << ": the streaming writer produced " << by_stream.size()
            << " bytes and the buffer writer " << by_buffer.size();
        EXPECT_EQ(std::memcmp(by_stream.data(), by_buffer.data(),
                      by_buffer.size()),
            0)
            << "n=" << n << " frame=" << frame_size << " checksum=" << checksum;
        if (::testing::Test::HasFatalFailure()) {
          return;
        }
      }
    }
  }
}

/**
 * @brief A flush ends the frame, so the seams follow the flushes.
 *
 * This is what makes the mode usable for something being appended to: flush
 * after each record and a reader can seek to it, rather than waiting for a
 * frame's worth of data to accumulate.
 *
 * Both flush modes do the same thing here and that is asserted rather than
 * assumed: in a file of independent frames there is no history for a sync flush
 * to keep, so there is nothing for the two modes to differ about.
 */
TEST(Seekable, AFlushEndsASeekableFrame) {
  for (gcomp_flush_t mode : {GCOMP_FLUSH_SYNC, GCOMP_FLUSH_FULL}) {
    // Three records, each far smaller than the frame size, flushed after each.
    const std::vector<size_t> records = {1000, 5, 20000};
    std::vector<uint8_t> all;
    std::vector<uint8_t> file;

    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_bool(o, "zstd.seekable", 1), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(
                  o, "zstd.seekable_frame_size", (uint64_t)1u << 20),
        GCOMP_OK);
    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(gcomp_encoder_create(gcomp_registry_default(), "zstd", o, &enc), GCOMP_OK);
    gcomp_options_destroy(o);

    std::vector<uint8_t> scratch(1u << 16);
    for (size_t len : records) {
      const std::vector<uint8_t> rec = make_data(len);
      all.insert(all.end(), rec.begin(), rec.end());

      gcomp_buffer_t ib = {(void *)rec.data(), rec.size(), 0};
      while (ib.used < ib.size) {
        gcomp_buffer_t ob = {scratch.data(), scratch.size(), 0};
        ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
        file.insert(
            file.end(), scratch.begin(), scratch.begin() + (long)ob.used);
      }
      for (;;) {
        gcomp_buffer_t ob = {scratch.data(), scratch.size(), 0};
        const gcomp_status_t st = gcomp_encoder_flush(enc, &ob, mode);
        file.insert(
            file.end(), scratch.begin(), scratch.begin() + (long)ob.used);
        if (st == GCOMP_OK) {
          break;
        }
        ASSERT_EQ(st, GCOMP_ERR_LIMIT) << gcomp_status_to_string(st);
      }
    }
    for (;;) {
      gcomp_buffer_t ob = {scratch.data(), scratch.size(), 0};
      const gcomp_status_t st = gcomp_encoder_finish(enc, &ob);
      file.insert(file.end(), scratch.begin(), scratch.begin() + (long)ob.used);
      if (st == GCOMP_OK) {
        break;
      }
      ASSERT_EQ(st, GCOMP_ERR_LIMIT);
    }
    gcomp_encoder_destroy(enc);

    gcomp_seekable_t * sk = nullptr;
    ASSERT_EQ(gcomp_seekable_open_buffer(
                  nullptr, "zstd", nullptr, file.data(), file.size(), &sk),
        GCOMP_OK);
    // One frame per flush, and no empty frame from the flush-then-finish at the
    // end: a flush with nothing buffered must not write a zero-byte frame.
    EXPECT_EQ(gcomp_seekable_frame_count(sk), records.size())
        << "mode " << (int)mode
        << ": one frame per flush, and none for the empty finish";
    EXPECT_EQ(gcomp_seekable_size(sk), (uint64_t)all.size());
    check_reads_against(sk, all);
    gcomp_seekable_close(sk);
  }
}

/**
 * @brief A redundant flush writes nothing, and reset starts a new file.
 */
TEST(Seekable, StreamingEncoderEdgeCases) {
  std::vector<uint8_t> scratch(1u << 16);

  // An encoder that is finished with no input at all: a valid file with a table
  // describing no frames.
  {
    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_bool(o, "zstd.seekable", 1), GCOMP_OK);
    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(gcomp_encoder_create(gcomp_registry_default(), "zstd", o, &enc), GCOMP_OK);
    gcomp_options_destroy(o);

    // Flushing with nothing buffered must produce no frame.
    gcomp_buffer_t fb = {scratch.data(), scratch.size(), 0};
    ASSERT_EQ(gcomp_encoder_flush(enc, &fb, GCOMP_FLUSH_SYNC), GCOMP_OK);
    EXPECT_EQ(fb.used, size_t{0})
        << "a flush with nothing buffered wrote " << fb.used << " bytes";

    gcomp_buffer_t ob = {scratch.data(), scratch.size(), 0};
    ASSERT_EQ(gcomp_encoder_finish(enc, &ob), GCOMP_OK);
    gcomp_encoder_destroy(enc);

    std::vector<uint8_t> file(scratch.begin(), scratch.begin() + (long)ob.used);
    gcomp_seekable_t * sk = nullptr;
    ASSERT_EQ(gcomp_seekable_open_buffer(
                  nullptr, "zstd", nullptr, file.data(), file.size(), &sk),
        GCOMP_OK);
    EXPECT_TRUE(gcomp_seekable_has_table(sk));
    EXPECT_EQ(gcomp_seekable_frame_count(sk), size_t{0});
    EXPECT_EQ(gcomp_seekable_size(sk), (uint64_t)0);
    gcomp_seekable_close(sk);
  }

  // reset() must drop the frames of the stream that just ended. Keeping them
  // would append the next stream's frames to the previous table and describe a
  // file that does not exist.
  {
    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_bool(o, "zstd.seekable", 1), GCOMP_OK);
    ASSERT_EQ(
        gcomp_options_set_uint64(o, "zstd.seekable_frame_size", 1024u),
        GCOMP_OK);
    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(gcomp_encoder_create(gcomp_registry_default(), "zstd", o, &enc), GCOMP_OK);
    gcomp_options_destroy(o);

    std::vector<std::vector<uint8_t>> streams;
    for (int pass = 0; pass < 2; pass++) {
      const std::vector<uint8_t> in = make_data(pass == 0 ? 5000 : 3000);
      std::vector<uint8_t> file;
      gcomp_buffer_t ib = {(void *)in.data(), in.size(), 0};
      while (ib.used < ib.size) {
        gcomp_buffer_t ob = {scratch.data(), scratch.size(), 0};
        ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
        file.insert(
            file.end(), scratch.begin(), scratch.begin() + (long)ob.used);
      }
      for (;;) {
        gcomp_buffer_t ob = {scratch.data(), scratch.size(), 0};
        const gcomp_status_t st = gcomp_encoder_finish(enc, &ob);
        file.insert(
            file.end(), scratch.begin(), scratch.begin() + (long)ob.used);
        if (st == GCOMP_OK) {
          break;
        }
        ASSERT_EQ(st, GCOMP_ERR_LIMIT);
      }

      gcomp_seekable_t * sk = nullptr;
      ASSERT_EQ(gcomp_seekable_open_buffer(
                    nullptr, "zstd", nullptr, file.data(), file.size(), &sk),
          GCOMP_OK)
          << "pass " << pass;
      EXPECT_EQ(gcomp_seekable_size(sk), (uint64_t)in.size()) << "pass " << pass;
      const size_t want = (in.size() + 1023u) / 1024u;
      EXPECT_EQ(gcomp_seekable_frame_count(sk), want)
          << "pass " << pass
          << ": the table should describe only this stream's frames";
      check_reads_against(sk, in);
      gcomp_seekable_close(sk);
      streams.push_back(file);

      if (pass == 0) {
        ASSERT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);
      }
    }
    gcomp_encoder_destroy(enc);
  }
}

/**
 * @brief What zstd.seekable refuses, and why it is refused rather than ignored.
 */
TEST(Seekable, StreamingSeekableRefusesWhatItCannotDo) {
  // threads.count > 1: parallel mode decides frame boundaries by job_size, so
  // the file would not have the frames that were asked for.
  {
    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_bool(o, "zstd.seekable", 1), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(o, "threads.count", 4u), GCOMP_OK);
    gcomp_encoder_t * enc = nullptr;
    EXPECT_EQ(gcomp_encoder_create(gcomp_registry_default(), "zstd", o, &enc),
        GCOMP_ERR_UNSUPPORTED);
    EXPECT_EQ(enc, nullptr);
    gcomp_options_destroy(o);
  }

  // zstd.content_size describes one frame's content, and this writes many.
  {
    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_bool(o, "zstd.seekable", 1), GCOMP_OK);
    ASSERT_EQ(gcomp_options_set_uint64(o, "zstd.content_size", 1000u),
        GCOMP_OK);
    gcomp_encoder_t * enc = nullptr;
    EXPECT_EQ(gcomp_encoder_create(gcomp_registry_default(), "zstd", o, &enc),
        GCOMP_ERR_INVALID_ARG);
    EXPECT_EQ(enc, nullptr);
    gcomp_options_destroy(o);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
