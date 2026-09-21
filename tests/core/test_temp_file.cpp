/**
 * @file test_temp_file.cpp
 *
 * Tests for the shared temporary-file helper.
 *
 * The helper is test infrastructure, which is exactly why it is tested:
 * twelve suites now depend on it, and a helper that silently stopped creating
 * files would turn all nine green for the wrong reason - the shape recorded
 * in `COMPRESS-TODO` as an absent oracle that skips instead of failing.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <temp_file.h>

using gcomp_test::TempFile;
using gcomp_test::readWholeFile;

namespace {

bool exists(const std::string & path) {
  struct stat st;
  return !path.empty() && stat(path.c_str(), &st) == 0;
}

} // namespace

TEST(TempFile, CreatesAFileThatExists) {
  TempFile t("gcomp_test", ".bin");
  ASSERT_TRUE(t.valid());
  EXPECT_TRUE(exists(t.path()));
}

TEST(TempFile, CarriesTheSuffixTheOracleDispatchesOn) {
  TempFile t("gcomp_test", ".gz");
  ASSERT_TRUE(t.valid());
  ASSERT_GT(t.path().size(), 3u);
  EXPECT_EQ(t.path().compare(t.path().size() - 3, 3, ".gz"), 0);
}

TEST(TempFile, TwoFilesDoNotCollide) {
  TempFile a("gcomp_test", ".bin");
  TempFile b("gcomp_test", ".bin");
  ASSERT_TRUE(a.valid());
  ASSERT_TRUE(b.valid());
  EXPECT_NE(a.path(), b.path());
}

TEST(TempFile, HonoursTmpdir) {
  // The behavioural point of moving to cutil: the mkstemp copies this
  // replaces named "/tmp" outright, so a machine whose TMPDIR points
  // elsewhere had its tests writing somewhere it had not been told to.
  const char * tmpdir = getenv("TMPDIR");
  if (!tmpdir || !*tmpdir) {
    GTEST_SKIP() << "TMPDIR is not set; nothing to compare against";
  }
  TempFile t("gcomp_test", ".bin");
  ASSERT_TRUE(t.valid());
  EXPECT_EQ(t.path().compare(0, strlen(tmpdir), tmpdir), 0)
      << "wrote to " << t.path() << " rather than under " << tmpdir;
}

TEST(TempFile, RoundTripsBytes) {
  TempFile t("gcomp_test", ".bin");
  ASSERT_TRUE(t.valid());
  const std::vector<uint8_t> payload = {0x1f, 0x8b, 0x00, 0xff, 0x00, 0x42};
  ASSERT_TRUE(t.write(payload));
  EXPECT_EQ(readWholeFile(t.path()), payload);
}

TEST(TempFile, RoundTripsAnEmptyFile) {
  TempFile t("gcomp_test", ".bin");
  ASSERT_TRUE(t.valid());
  ASSERT_TRUE(t.write(std::vector<uint8_t>{}));
  EXPECT_TRUE(readWholeFile(t.path()).empty());
}

TEST(TempFile, OverwritesRatherThanAppends) {
  TempFile t("gcomp_test", ".bin");
  ASSERT_TRUE(t.valid());
  ASSERT_TRUE(t.write(std::vector<uint8_t>(100, 0xaa)));
  ASSERT_TRUE(t.write(std::vector<uint8_t>(3, 0xbb)));
  EXPECT_EQ(readWholeFile(t.path()), std::vector<uint8_t>(3, 0xbb));
}

TEST(TempFile, RemovesItselfAndAnythingDerived) {
  std::string main_path;
  std::string derived_path;
  {
    TempFile t("gcomp_test", ".raw");
    ASSERT_TRUE(t.valid());
    derived_path = t.derived(".out");
    ASSERT_EQ(derived_path, t.path() + ".out");
    // Stand in for the oracle creating its output file.
    ASSERT_TRUE(t.write(std::vector<uint8_t>{1}));
    FILE * f = fopen(derived_path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fputc('x', f);
    fclose(f);
    ASSERT_TRUE(exists(derived_path));
    main_path = t.path();
  }
  EXPECT_FALSE(exists(main_path));
  EXPECT_FALSE(exists(derived_path)) << "the derived name was left behind";
}

TEST(TempFile, ReportsFailureRatherThanCrashing) {
  // A suffix carrying a separator names a directory that does not exist.
  TempFile bad("gcomp_test", "/nested/x.bin");
  EXPECT_FALSE(bad.valid());
  EXPECT_TRUE(bad.path().empty());
  EXPECT_FALSE(bad.write(std::vector<uint8_t>{1}));
}

TEST(TempFile, ReadingAMissingFileIsEmptyNotACrash) {
  EXPECT_TRUE(readWholeFile("/nonexistent/gcomp/temp_file/probe").empty());
}

TEST(TempFile, PosixPathLeavesAPosixPathAlone) {
  // The contract cutil states: under POSIX rules it copies unchanged, which
  // is why the #ifdef this replaces could simply be deleted.
  EXPECT_EQ(gcomp_test::toPosixPath("/tmp/gcomp_x.bin"), "/tmp/gcomp_x.bin");
  EXPECT_EQ(gcomp_test::toPosixPath(""), "");
}

TEST(TempFile, PosixPathOfATempFileIsUsableAsWritten) {
  TempFile t("gcomp_test", ".bin");
  ASSERT_TRUE(t.valid());
  ASSERT_TRUE(t.write(std::vector<uint8_t>{7, 8, 9}));
  // Whatever the host's separator is, the rewritten name still names the
  // same file.
  EXPECT_EQ(readWholeFile(gcomp_test::toPosixPath(t.path())),
      (std::vector<uint8_t>{7, 8, 9}));
}

TEST(TempFile, UniqueTempPathCreatesTheFile) {
  // The distinction that matters: the name comes back with the file already
  // on disk, so nothing can be put there between the naming and the opening.
  std::string a = gcomp_test::uniqueTempPath("gcomp_test", ".raw");
  ASSERT_FALSE(a.empty());
  EXPECT_TRUE(exists(a));
  std::string b = gcomp_test::uniqueTempPath("gcomp_test", ".raw");
  ASSERT_FALSE(b.empty());
  EXPECT_NE(a, b);
  EXPECT_EQ(std::remove(a.c_str()), 0);
  EXPECT_EQ(std::remove(b.c_str()), 0);
}

TEST(TempFile, WriteWholeFileRoundTrips) {
  std::string path = gcomp_test::uniqueTempPath("gcomp_test", ".raw");
  ASSERT_FALSE(path.empty());
  const std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
  EXPECT_TRUE(gcomp_test::writeWholeFile(path, payload));
  EXPECT_EQ(readWholeFile(path), payload);
  EXPECT_TRUE(gcomp_test::writeWholeFile(path, {}));
  EXPECT_TRUE(readWholeFile(path).empty());
  std::remove(path.c_str());
}

TEST(TempFile, WriteWholeFileReportsAnUnwritablePath) {
  EXPECT_FALSE(
      gcomp_test::writeWholeFile("/nonexistent/gcomp/x.raw", {1, 2, 3}));
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
