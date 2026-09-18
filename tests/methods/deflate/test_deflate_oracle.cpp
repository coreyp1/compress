/**
 * @file test_deflate_oracle.cpp
 *
 * Cross-implementation ("oracle") tests for the deflate method, against
 * Python's `zlib` module.
 *
 * The oracle is deliberately Python's standard library rather than a system
 * tool or an optional C library.  Every other oracle suite in this tree
 * depends on something that may not be installed, and when it is not, the
 * tests do not fail -- they skip, and the summary line reports a green run
 * that compared nothing against a reference.  That is how a zstd decoder that
 * could not read an ordinary zstd file passed its own oracle suite.  `zlib`
 * ships with Python, so if python3 runs at all, these tests run.
 *
 * Two habits are worth keeping when adding to this file:
 *
 *   - Assert that the stream under test actually contains the construct the
 *     test is named for, before asserting that it decodes.  A test whose
 *     input stops reaching the interesting path silently stops testing.
 *   - Sweep, rather than picking a representative case.  The defects found in
 *     this library have almost all been at boundaries -- a size, a level, a
 *     symbol count -- that a single representative input does not touch.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#define popen _popen
#define pclose _pclose
#define unlink _unlink
#else
#include <unistd.h>
#endif

namespace {

const char * pythonCommand() {
#ifdef _WIN32
  static const char * cmd = nullptr;
  if (!cmd) {
    cmd = (system("python3 --version >NUL 2>&1") == 0) ? "python3" : "python";
  }
  return cmd;
#else
  return "python3";
#endif
}

std::vector<uint8_t> runCapture(const std::string & cmd) {
#ifdef _WIN32
  FILE * pipe = popen(cmd.c_str(), "rb");
#else
  FILE * pipe = popen(cmd.c_str(), "r");
#endif
  if (!pipe) {
    return {};
  }
  std::vector<uint8_t> out;
  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) {
    out.insert(out.end(), buf, buf + n);
  }
  pclose(pipe);
  return out;
}

std::string tempPath(const std::string & suffix) {
  static int counter = 0;
#ifdef _WIN32
  char dir[MAX_PATH];
  if (GetTempPathA(MAX_PATH, dir) == 0) {
    return "";
  }
  char name[MAX_PATH];
  snprintf(name, sizeof(name), "%sgcomp_deflate_oracle_%d_%d%s", dir, _getpid(),
      counter++, suffix.c_str());
  return name;
#else
  const char * dir = getenv("TMPDIR");
  if (!dir || !*dir) {
    dir = "/tmp";
  }
  char name[1024];
  snprintf(name, sizeof(name), "%s/gcomp_deflate_oracle_%d_%d%s", dir,
      (int)getpid(), counter++, suffix.c_str());
  return name;
#endif
}

bool writeFile(const std::string & path, const std::vector<uint8_t> & data) {
  FILE * f = fopen(path.c_str(), "wb");
  if (!f) {
    return false;
  }
  bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
  fclose(f);
  return ok;
}

bool readFile(const std::string & path, std::vector<uint8_t> * out) {
  FILE * f = fopen(path.c_str(), "rb");
  if (!f) {
    return false;
  }
  out->clear();
  uint8_t buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    out->insert(out->end(), buf, buf + n);
  }
  fclose(f);
  return true;
}

// Type of the first DEFLATE block in a raw stream: 0 stored, 1 fixed Huffman,
// 2 dynamic Huffman, -1 unreadable.  RFC 1951 section 3.2.3: BFINAL is one
// bit and BTYPE the two after it, read least-significant bit first.
//
// Only the first block is classified.  Finding where a compressed block ends
// requires decoding it, and a test should not lean on the decoder it is
// checking; every case here is shaped so the construct under test is in the
// first block.
int firstBlockType(const std::vector<uint8_t> & raw) {
  if (raw.empty()) {
    return -1;
  }
  return (raw[0] >> 1) & 3;
}

class DeflateOracleTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);
  }

  // Run a short Python program with `in_path` and `out_path` bound.
  bool runPython(const std::string & body, const std::string & in_path,
      const std::string & out_path) {
    std::string cmd = std::string(pythonCommand()) + " -c \"" + body + "\" \"" +
        in_path + "\" \"" + out_path + "\"";
#ifdef _WIN32
    cmd += " >NUL 2>&1";
#else
    cmd += " >/dev/null 2>&1";
#endif
    return system(cmd.c_str()) == 0;
  }

  // One Python process for a whole sweep.
  //
  // Spawning an interpreter per case caps how wide a sweep can afford to be,
  // and a narrow sweep is how defects at particular sizes and levels survive.
  // The manifest carries every case; the report carries one line of outcome
  // per case, in order.
  struct BatchCase {
    std::string in_path;
    std::string out_path;
    int level = 6;
    std::string strategy = "DEFAULT_STRATEGY";
  };

  bool runBatch(const std::string & body, const std::vector<BatchCase> & cases,
      std::vector<std::string> * report_out) {
    std::string manifest = tempPath(".manifest");
    std::string report = tempPath(".report");
    if (manifest.empty() || report.empty()) {
      return false;
    }

    FILE * mf = fopen(manifest.c_str(), "wb");
    if (!mf) {
      return false;
    }
    for (const BatchCase & c : cases) {
      fprintf(mf, "%d\t%s\t%s\t%s\n", c.level, c.strategy.c_str(),
          c.in_path.c_str(), c.out_path.c_str());
    }
    fclose(mf);

    bool ok = runPython(body, manifest, report);

    report_out->clear();
    if (ok) {
      FILE * rf = fopen(report.c_str(), "rb");
      if (rf) {
        char line[512];
        while (fgets(line, sizeof(line), rf)) {
          std::string l(line);
          while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) {
            l.pop_back();
          }
          report_out->push_back(l);
        }
        fclose(rf);
      }
    }

    unlink(manifest.c_str());
    unlink(report.c_str());
    return ok && report_out->size() == cases.size();
  }

  // Compress every case with zlib at its own level and strategy.
  bool zlibDeflateBatch(
      const std::vector<BatchCase> & cases, std::vector<std::string> * report) {
    static const char * body =
        "import sys,zlib\n"
        "rep=open(sys.argv[2],'w')\n"
        "for line in open(sys.argv[1]):\n"
        " line=line.rstrip('\\n')\n"
        " if not line: continue\n"
        " lv,st,ip,op=line.split('\\t')\n"
        " try:\n"
        "  d=open(ip,'rb').read()\n"
        "  c=zlib.compressobj(int(lv),zlib.DEFLATED,-15,9,getattr(zlib,'Z_'+st))\n"
        "  open(op,'wb').write(c.compress(d)+c.flush())\n"
        "  rep.write('ok\\n')\n"
        " except Exception as e:\n"
        "  rep.write('fail %s\\n'%type(e).__name__)\n";
    return runBatch(body, cases, report);
  }

  // Inflate every case; a stream zlib refuses is reported as a failure rather
  // than aborting the batch, so one bad case names itself.
  bool zlibInflateBatch(
      const std::vector<BatchCase> & cases, std::vector<std::string> * report) {
    static const char * body =
        "import sys,zlib\n"
        "rep=open(sys.argv[2],'w')\n"
        "for line in open(sys.argv[1]):\n"
        " line=line.rstrip('\\n')\n"
        " if not line: continue\n"
        " lv,st,ip,op=line.split('\\t')\n"
        " try:\n"
        "  d=open(ip,'rb').read()\n"
        "  open(op,'wb').write(zlib.decompress(d,-15))\n"
        "  rep.write('ok\\n')\n"
        " except Exception as e:\n"
        "  rep.write('fail %s\\n'%type(e).__name__)\n";
    return runBatch(body, cases, report);
  }

  // Raw DEFLATE (wbits -15) produced by zlib at the given level and strategy.
  std::vector<uint8_t> zlibDeflate(
      const std::vector<uint8_t> & data, int level, const char * strategy) {
    std::string in = tempPath(".raw");
    std::string out = tempPath(".def");
    if (in.empty() || out.empty() || !writeFile(in, data)) {
      return {};
    }

    std::string body =
        "import sys,zlib;"
        "d=open(sys.argv[1],'rb').read();"
        "c=zlib.compressobj(" + std::to_string(level) +
        ",zlib.DEFLATED,-15,9,zlib.Z_" + strategy + ");"
        "open(sys.argv[2],'wb').write(c.compress(d)+c.flush())";

    std::vector<uint8_t> result;
    if (runPython(body, in, out)) {
      readFile(out, &result);
    }
    unlink(in.c_str());
    unlink(out.c_str());
    return result;
  }

  std::vector<uint8_t> zlibInflate(const std::vector<uint8_t> & data) {
    std::string in = tempPath(".def");
    std::string out = tempPath(".raw");
    if (in.empty() || out.empty() || !writeFile(in, data)) {
      return {};
    }

    std::string body =
        "import sys,zlib;"
        "d=open(sys.argv[1],'rb').read();"
        "open(sys.argv[2],'wb').write(zlib.decompress(d,-15))";

    std::vector<uint8_t> result;
    bool ok = runPython(body, in, out);
    if (ok) {
      readFile(out, &result);
    }
    unlink(in.c_str());
    unlink(out.c_str());
    if (!ok) {
      return {};
    }
    // An empty input legitimately inflates to nothing; distinguish that from
    // failure with the flag above rather than by result.empty().
    return result;
  }

  std::vector<uint8_t> gcompDeflate(const std::vector<uint8_t> & data,
      int level = -1, const char * strategy = nullptr) {
    gcomp_options_t * opts = nullptr;
    if (level >= 0 || strategy) {
      if (gcomp_options_create(&opts) != GCOMP_OK) {
        return {};
      }
      if (level >= 0) {
        gcomp_options_set_int64(opts, "deflate.level", level);
      }
      if (strategy) {
        gcomp_options_set_string(opts, "deflate.strategy", strategy);
      }
    }

    std::vector<uint8_t> out(data.size() * 2 + 4096);
    size_t used = out.size();
    gcomp_status_t s = gcomp_encode_buffer(registry_, "deflate", opts,
        data.data(), data.size(), out.data(), out.size(), &used);
    if (opts) {
      gcomp_options_destroy(opts);
    }
    if (s != GCOMP_OK) {
      return {};
    }
    out.resize(used);
    return out;
  }

  std::vector<uint8_t> gcompInflate(
      const std::vector<uint8_t> & data, size_t expected, bool * ok_out) {
    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      *ok_out = false;
      return {};
    }
    gcomp_options_set_uint64(opts, "limits.max_output_bytes", 64u << 20);
    gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 1000000);
    gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 256u << 20);

    std::vector<uint8_t> out(expected + 4096);
    size_t used = out.size();
    gcomp_status_t s = gcomp_decode_buffer(registry_, "deflate", opts,
        data.data(), data.size(), out.data(), out.size(), &used);
    gcomp_options_destroy(opts);
    *ok_out = (s == GCOMP_OK);
    if (s != GCOMP_OK) {
      return {};
    }
    out.resize(used);
    return out;
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Test data shapes.  Each one is here because it drives the encoder down a
// different path, not for variety's sake.
//

std::vector<uint8_t> proseLike(size_t n, uint32_t seed) {
  static const char * words[] = {"the", "quick", "brown", "fox", "jumps",
      "over", "lazy", "dog", "and", "then", "returns", "home", "with",
      "several", "unusual", "souvenirs", "from", "distant", "places"};
  std::vector<uint8_t> out;
  out.reserve(n + 16);
  uint32_t x = seed;
  while (out.size() < n) {
    x = x * 1103515245u + 12345u;
    const char * w = words[(x >> 16) % 19];
    for (const char * c = w; *c; c++) {
      out.push_back((uint8_t)*c);
    }
    out.push_back((uint8_t)(((x >> 8) & 15) == 0 ? '\n' : ' '));
  }
  out.resize(n);
  return out;
}

// No repeats and a flat byte distribution: zlib gives up and emits a stored
// block (RFC 1951 section 3.2.4).
std::vector<uint8_t> incompressible(size_t n, uint32_t seed) {
  std::vector<uint8_t> out(n);
  uint32_t x = seed | 1u;
  for (size_t i = 0; i < n; i++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    out[i] = (uint8_t)(x >> 24);
  }
  return out;
}

// One long run: exercises distance-1 matches and the RLE strategy.
std::vector<uint8_t> singleRun(size_t n) {
  return std::vector<uint8_t>(n, 'Z');
}

// PNG-shaped: small deltas around zero, which is what a filtered image row
// looks like and what deflate.strategy "lazy" exists for.
std::vector<uint8_t> filterShaped(size_t n, uint32_t seed) {
  std::vector<uint8_t> out(n);
  uint32_t x = seed | 3u;
  for (size_t i = 0; i < n; i++) {
    x = x * 1664525u + 1013904223u;
    unsigned r = (x >> 16) & 0xFF;
    out[i] = (uint8_t)(r < 160 ? 0 : (r < 210 ? 1 : (r < 240 ? 255 : r)));
  }
  return out;
}

struct Shape {
  const char * name;
  std::vector<uint8_t> data;
};

std::vector<Shape> shapes(size_t n) {
  std::vector<Shape> v;
  v.push_back({"prose", proseLike(n, 20260917u)});
  v.push_back({"incompressible", incompressible(n, 7u)});
  v.push_back({"single run", singleRun(n)});
  v.push_back({"filter shaped", filterShaped(n, 11u)});
  return v;
}

//
// Tests
//

TEST_F(DeflateOracleTest, OracleIsActuallyAvailable) {
  // A skipped oracle test and an absent one are indistinguishable in the
  // summary.  zlib is part of Python's standard library, so this failing
  // means python3 itself is missing, which is worth reporting rather than
  // quietly passing a suite that checked nothing.
  std::vector<uint8_t> probe = {'h', 'e', 'l', 'l', 'o'};
  std::vector<uint8_t> packed = zlibDeflate(probe, 6, "DEFAULT_STRATEGY");
  ASSERT_FALSE(packed.empty())
      << "Could not run the reference deflate implementation. This suite "
         "needs python3 (the zlib module it uses is part of the standard "
         "library). Nothing in this file compares our output against a "
         "reference without it.";

  std::vector<uint8_t> back = zlibInflate(packed);
  ASSERT_EQ(back.size(), probe.size());
  ASSERT_EQ(memcmp(back.data(), probe.data(), probe.size()), 0);
}

// Sizes chosen to land on and around the places an encoder changes behaviour:
// empty, single bytes, the first block flush, and well past it.
std::vector<size_t> sweepSizes() {
  return {0, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 1597, 4181,
      10946, 28657};
}

TEST_F(DeflateOracleTest, OurEncoder_ZlibDecoder_Sweep) {
  // Every level and strategy, over four data shapes, at nineteen sizes.  The
  // defects this library has actually had did not show at a representative
  // size and level -- an incomplete code-length alphabet, for one, appears
  // only when the length limiter is pushed by a particular distribution, and
  // only a reference decoder objects to it.
  static const int kLevels[] = {0, 1, 2, 4, 6, 9};
  static const char * kStrategies[] = {
      "default", "lazy", "huffman_only", "rle", "fixed"};

  struct Case {
    std::string label;
    std::vector<uint8_t> original;
    std::string packed_path;
    std::string out_path;
  };

  std::vector<Case> cases;
  std::vector<BatchCase> batch;
  bool saw_stored = false, saw_fixed = false, saw_dynamic = false;

  for (size_t n : sweepSizes()) {
    for (const Shape & sh : shapes(n)) {
      for (int level : kLevels) {
        for (const char * strat : kStrategies) {
          std::vector<uint8_t> packed = gcompDeflate(sh.data, level, strat);
          ASSERT_FALSE(packed.empty() && n > 0)
              << sh.name << " n=" << n << " level=" << level << " " << strat;

          switch (firstBlockType(packed)) {
          case 0: saw_stored = true; break;
          case 1: saw_fixed = true; break;
          case 2: saw_dynamic = true; break;
          default: break;
          }

          Case c;
          c.label = std::string(sh.name) + " n=" + std::to_string(n) +
              " level=" + std::to_string(level) + " " + strat;
          c.original = sh.data;
          c.packed_path = tempPath(".def");
          c.out_path = c.packed_path + ".out";
          ASSERT_TRUE(writeFile(c.packed_path, packed)) << c.label;

          BatchCase bc;
          bc.in_path = c.packed_path;
          bc.out_path = c.out_path;
          batch.push_back(bc);
          cases.push_back(std::move(c));
        }
      }
    }
  }

  std::vector<std::string> report;
  bool ran = zlibInflateBatch(batch, &report);

  int rejected = 0, mismatched = 0;
  std::string first_bad;
  for (size_t i = 0; i < cases.size() && ran; i++) {
    if (report[i] != "ok") {
      rejected++;
      if (first_bad.empty()) {
        first_bad = cases[i].label + " (" + report[i] + ")";
      }
    }
    else {
      std::vector<uint8_t> back;
      readFile(cases[i].out_path, &back);
      if (back != cases[i].original) {
        mismatched++;
        if (first_bad.empty()) {
          first_bad = cases[i].label + " (content differs)";
        }
      }
    }
    unlink(cases[i].out_path.c_str());
  }
  for (const Case & c : cases) {
    unlink(c.packed_path.c_str());
  }

  ASSERT_TRUE(ran) << "the reference decoder did not run over the sweep";
  EXPECT_EQ(rejected, 0) << rejected << " of " << cases.size()
                         << " streams zlib refused; first: " << first_bad;
  EXPECT_EQ(mismatched, 0) << mismatched << " of " << cases.size()
                           << " decoded to different bytes; first: " << first_bad;

  EXPECT_TRUE(saw_stored) << "we never emitted a stored block";
  EXPECT_TRUE(saw_fixed) << "we never emitted a fixed-Huffman block";
  EXPECT_TRUE(saw_dynamic) << "we never emitted a dynamic-Huffman block";
}

TEST_F(DeflateOracleTest, OurEncoder_ZlibDecoder_DenseSmallSizeSweep) {
  // The code-length limiter (RFC 1951 section 3.2.2 caps a literal/length code
  // at 15 bits) only engages for particular symbol distributions, and when its
  // adjustment leaves the alphabet under-subscribed the result is a Huffman
  // code that is not complete -- a stream zlib rejects outright while our own
  // decoder, being equally forgiving, reads back fine.
  //
  // Instrumenting the limiter shows it engaging for roughly one size in 660 of
  // flat random data, clustered in this range.  A sweep of a dozen sizes has
  // about a three percent chance of touching it, which is why this one is
  // dense rather than representative.  Flat random data is the shape that gets
  // there: many symbols, near-uniform frequencies, so the code depths run long.
  static const int kLevels[] = {6, 9};
  static const char * kStrategies[] = {"default", "lazy"};

  struct Case {
    std::string label;
    std::vector<uint8_t> original;
    std::string packed_path;
    std::string out_path;
  };

  std::vector<Case> cases;
  std::vector<BatchCase> batch;

  for (size_t n = 1300; n <= 1800; n++) {
    std::vector<uint8_t> data = incompressible(n, 12345u + (uint32_t)n);
    for (int level : kLevels) {
      for (const char * strat : kStrategies) {
        std::vector<uint8_t> packed = gcompDeflate(data, level, strat);
        ASSERT_FALSE(packed.empty())
            << "n=" << n << " level=" << level << " " << strat;

        Case c;
        c.label = "n=" + std::to_string(n) + " level=" +
            std::to_string(level) + " " + strat;
        c.original = data;
        c.packed_path = tempPath(".def");
        c.out_path = c.packed_path + ".out";
        ASSERT_TRUE(writeFile(c.packed_path, packed)) << c.label;

        BatchCase bc;
        bc.in_path = c.packed_path;
        bc.out_path = c.out_path;
        batch.push_back(bc);
        cases.push_back(std::move(c));
      }
    }
  }

  std::vector<std::string> report;
  bool ran = zlibInflateBatch(batch, &report);

  int rejected = 0, mismatched = 0;
  std::string first_bad;
  for (size_t i = 0; i < cases.size() && ran; i++) {
    if (report[i] != "ok") {
      rejected++;
      if (first_bad.empty()) {
        first_bad = cases[i].label + " (" + report[i] + ")";
      }
    }
    else {
      std::vector<uint8_t> back;
      readFile(cases[i].out_path, &back);
      if (back != cases[i].original) {
        mismatched++;
        if (first_bad.empty()) {
          first_bad = cases[i].label + " (content differs)";
        }
      }
    }
    unlink(cases[i].out_path.c_str());
  }
  for (const Case & c : cases) {
    unlink(c.packed_path.c_str());
  }

  ASSERT_TRUE(ran) << "the reference decoder did not run over the sweep";
  EXPECT_EQ(rejected, 0) << rejected << " of " << cases.size()
                         << " streams zlib refused; first: " << first_bad;
  EXPECT_EQ(mismatched, 0) << mismatched << " of " << cases.size()
                           << " decoded to different bytes; first: "
                           << first_bad;
}

TEST_F(DeflateOracleTest, ZlibEncoder_OurDecoder_Sweep) {
  static const int kLevels[] = {0, 1, 2, 4, 6, 9};
  static const char * kStrategies[] = {"DEFAULT_STRATEGY", "FILTERED",
      "HUFFMAN_ONLY", "RLE", "FIXED"};

  struct Case {
    std::string label;
    std::vector<uint8_t> original;
    std::string in_path;
    std::string packed_path;
  };

  std::vector<Case> cases;
  std::vector<BatchCase> batch;

  for (size_t n : sweepSizes()) {
    for (const Shape & sh : shapes(n)) {
      for (int level : kLevels) {
        for (const char * strat : kStrategies) {
          Case c;
          c.label = std::string(sh.name) + " n=" + std::to_string(n) +
              " level=" + std::to_string(level) + " " + strat;
          c.original = sh.data;
          c.in_path = tempPath(".raw");
          c.packed_path = c.in_path + ".def";
          ASSERT_TRUE(writeFile(c.in_path, sh.data)) << c.label;

          BatchCase bc;
          bc.in_path = c.in_path;
          bc.out_path = c.packed_path;
          bc.level = level;
          bc.strategy = strat;
          batch.push_back(bc);
          cases.push_back(std::move(c));
        }
      }
    }
  }

  std::vector<std::string> report;
  bool ran = zlibDeflateBatch(batch, &report);

  int rejected = 0, mismatched = 0;
  bool saw_stored = false, saw_fixed = false, saw_dynamic = false;
  std::string first_bad;

  for (size_t i = 0; i < cases.size() && ran; i++) {
    if (report[i] != "ok") {
      continue; // zlib declining a combination is its business, not a defect
    }
    std::vector<uint8_t> packed;
    if (!readFile(cases[i].packed_path, &packed)) {
      continue;
    }

    switch (firstBlockType(packed)) {
    case 0: saw_stored = true; break;
    case 1: saw_fixed = true; break;
    case 2: saw_dynamic = true; break;
    default: break;
    }

    bool ok = false;
    std::vector<uint8_t> back =
        gcompInflate(packed, cases[i].original.size(), &ok);
    if (!ok) {
      rejected++;
      if (first_bad.empty()) {
        first_bad = cases[i].label;
      }
    }
    else if (back != cases[i].original) {
      mismatched++;
      if (first_bad.empty()) {
        first_bad = cases[i].label + " (content differs)";
      }
    }
  }

  for (const Case & c : cases) {
    unlink(c.in_path.c_str());
    unlink(c.packed_path.c_str());
  }

  ASSERT_TRUE(ran) << "the reference encoder did not run over the sweep";
  EXPECT_EQ(rejected, 0) << rejected << " of " << cases.size()
                         << " zlib streams our decoder refused; first: "
                         << first_bad;
  EXPECT_EQ(mismatched, 0) << mismatched << " decoded to different bytes; "
                           << "first: " << first_bad;

  EXPECT_TRUE(saw_stored) << "no case produced a stored block";
  EXPECT_TRUE(saw_fixed) << "no case produced a fixed-Huffman block";
  EXPECT_TRUE(saw_dynamic) << "no case produced a dynamic-Huffman block";
}

TEST_F(DeflateOracleTest, OurEncoder_ZlibDecoder_EveryShortLength) {
  // Every length from empty to 80, plus the sizes either side of the internal
  // buffer boundaries.  An encoder can be right for a megabyte and wrong for
  // seventeen bytes; several defects in this library were exactly that shape.
  std::vector<size_t> lengths;
  for (size_t n = 0; n <= 80; n++) {
    lengths.push_back(n);
  }
  for (size_t n : {size_t(127), size_t(128), size_t(129), size_t(255),
           size_t(256), size_t(257), size_t(511), size_t(512), size_t(513),
           size_t(4095), size_t(4096), size_t(4097), size_t(32767),
           size_t(32768), size_t(32769)}) {
    lengths.push_back(n);
  }

  std::vector<std::vector<uint8_t>> originals;
  std::vector<std::string> labels;
  std::vector<BatchCase> batch;
  std::vector<std::string> paths;

  for (size_t n : lengths) {
    std::vector<uint8_t> data = proseLike(n, (uint32_t)(n * 2654435761u + 1u));
    ASSERT_EQ(data.size(), n);

    std::vector<uint8_t> packed = gcompDeflate(data);
    ASSERT_FALSE(packed.empty() && n > 0) << "n=" << n;

    std::string path = tempPath(".def");
    ASSERT_TRUE(writeFile(path, packed)) << "n=" << n;

    BatchCase bc;
    bc.in_path = path;
    bc.out_path = path + ".out";
    batch.push_back(bc);
    paths.push_back(path);
    originals.push_back(std::move(data));
    labels.push_back("n=" + std::to_string(n));
  }

  std::vector<std::string> report;
  bool ran = zlibInflateBatch(batch, &report);

  int bad = 0;
  std::string first_bad;
  for (size_t i = 0; i < batch.size() && ran; i++) {
    std::vector<uint8_t> back;
    bool ok = (report[i] == "ok") && readFile(batch[i].out_path, &back) &&
        back == originals[i];
    if (!ok) {
      bad++;
      if (first_bad.empty()) {
        first_bad = labels[i] + " (" + report[i] + ")";
      }
    }
    unlink(batch[i].out_path.c_str());
  }
  for (const std::string & path : paths) {
    unlink(path.c_str());
  }

  ASSERT_TRUE(ran) << "the reference decoder did not run";
  EXPECT_EQ(bad, 0) << bad << " of " << batch.size()
                    << " lengths failed; first: " << first_bad;
}

TEST_F(DeflateOracleTest, ZlibEncoder_OurDecoder_EveryShortLength) {
  std::vector<std::vector<uint8_t>> originals;
  std::vector<BatchCase> batch;
  std::vector<std::string> in_paths;

  for (size_t n = 0; n <= 80; n++) {
    std::vector<uint8_t> data = proseLike(n, (uint32_t)(n * 40503u + 7u));
    std::string in = tempPath(".raw");
    ASSERT_TRUE(writeFile(in, data)) << "n=" << n;

    BatchCase bc;
    bc.in_path = in;
    bc.out_path = in + ".def";
    batch.push_back(bc);
    in_paths.push_back(in);
    originals.push_back(std::move(data));
  }

  std::vector<std::string> report;
  bool ran = zlibDeflateBatch(batch, &report);

  int bad = 0;
  std::string first_bad;
  for (size_t i = 0; i < batch.size() && ran; i++) {
    if (report[i] != "ok") {
      continue;
    }
    std::vector<uint8_t> packed;
    if (!readFile(batch[i].out_path, &packed)) {
      continue;
    }
    bool ok = false;
    std::vector<uint8_t> back =
        gcompInflate(packed, originals[i].size(), &ok);
    if (!ok || back != originals[i]) {
      bad++;
      if (first_bad.empty()) {
        first_bad = "n=" + std::to_string(i);
      }
    }
    unlink(batch[i].out_path.c_str());
  }
  for (const std::string & path : in_paths) {
    unlink(path.c_str());
  }

  ASSERT_TRUE(ran) << "the reference encoder did not run";
  EXPECT_EQ(bad, 0) << bad << " of " << batch.size()
                    << " lengths failed; first: " << first_bad;
}

TEST_F(DeflateOracleTest, ZlibEncoder_OurDecoder_StoredBlocks) {
  std::vector<uint8_t> data = incompressible(64 * 1024, 4242u);
  std::vector<uint8_t> packed = zlibDeflate(data, 1, "DEFAULT_STRATEGY");
  ASSERT_FALSE(packed.empty());
  ASSERT_EQ(firstBlockType(packed), 0)
      << "this input no longer produces a stored block, so the test is not "
         "exercising the path it was written for";

  bool ok = false;
  std::vector<uint8_t> back = gcompInflate(packed, data.size(), &ok);
  ASSERT_TRUE(ok);
  ASSERT_EQ(back.size(), data.size());
  ASSERT_EQ(memcmp(back.data(), data.data(), data.size()), 0);
}

TEST_F(DeflateOracleTest, ZlibEncoder_OurDecoder_FixedHuffmanBlocks) {
  std::vector<uint8_t> data = proseLike(48, 3u);
  std::vector<uint8_t> packed = zlibDeflate(data, 1, "FIXED");
  ASSERT_FALSE(packed.empty());
  ASSERT_EQ(firstBlockType(packed), 1)
      << "this input no longer produces a fixed-Huffman block, so the test is "
         "not exercising the path it was written for";

  bool ok = false;
  std::vector<uint8_t> back = gcompInflate(packed, data.size(), &ok);
  ASSERT_TRUE(ok);
  ASSERT_EQ(back.size(), data.size());
  ASSERT_EQ(memcmp(back.data(), data.data(), data.size()), 0);
}

TEST_F(DeflateOracleTest, RoundTripThroughBothImplementations) {
  // ours -> zlib -> ours, and zlib -> ours -> zlib, so a matched pair of
  // mistakes on one side cannot pass.
  for (const Shape & sh : shapes(40 * 1024)) {
    std::vector<uint8_t> ours = gcompDeflate(sh.data);
    ASSERT_FALSE(ours.empty()) << sh.name;

    std::vector<uint8_t> viaZlib = zlibInflate(ours);
    ASSERT_EQ(viaZlib.size(), sh.data.size()) << sh.name;

    std::vector<uint8_t> reencoded = zlibDeflate(viaZlib, 6, "DEFAULT_STRATEGY");
    ASSERT_FALSE(reencoded.empty()) << sh.name;

    bool ok = false;
    std::vector<uint8_t> back = gcompInflate(reencoded, sh.data.size(), &ok);
    ASSERT_TRUE(ok) << sh.name;
    ASSERT_EQ(back.size(), sh.data.size()) << sh.name;
    ASSERT_EQ(memcmp(back.data(), sh.data.data(), sh.data.size()), 0) << sh.name;
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
