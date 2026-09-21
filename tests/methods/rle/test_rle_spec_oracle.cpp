/**
 * @file test_rle_spec_oracle.cpp
 *
 * Cross-implementation tests for the RLE method against references written
 * from the PackBits and TGA descriptions.
 *
 * This method had no oracle tests of any kind: its round-trip tests check
 * our encoder against our decoder, which agree with each other by
 * construction and would agree just as happily on a private format.  These
 * formats are small enough that a reference is forty lines, and both are
 * things other programs write -- a TIFF or a Targa file produced elsewhere
 * has to decode here.
 *
 * The reference encoder deliberately emits constructs our own encoder never
 * does, notably PackBits control byte 128 (a no-op that a decoder must skip)
 * and maximum-length literal and run packets.  A decoder is only known to
 * handle what something has actually handed it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/rle.h>
#include <gtest/gtest.h>
#include <string>
#include <temp_file.h>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <process.h>
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

std::string tempPath(const std::string & suffix) {
  // cutil creates the file as it names it, so nothing can occupy the
  // name in between, and it puts it where gcu_path_temp_dir() says.
  return gcomp_test::uniqueTempPath("gcomp_rle_oracle", suffix);
}

bool writeFile(const std::string & path, const std::vector<uint8_t> & data) {
  return gcomp_test::writeWholeFile(path, data);
}

bool readFile(const std::string & path, std::vector<uint8_t> * out) {
  return gcomp_test::readWholeFile(path, out);
}

const char * kReferenceProgram = R"PYSRC(
import sys

# ---- PackBits (TIFF / Apple) ------------------------------------------
# One-byte control: 0..127 means the next n+1 bytes are literal; 128 is a
# no-op; 129..255 means the next byte repeats 257-n times.

def packbits_decode(d):
    out = bytearray()
    i, n = 0, len(d)
    while i < n:
        c = d[i]; i += 1
        if c == 128:
            continue
        if c < 128:
            count = c + 1
            if i + count > n: raise ValueError('literal past end')
            out += d[i:i+count]; i += count
        else:
            count = 257 - c
            if i >= n: raise ValueError('run past end')
            out += bytes([d[i]]) * count; i += 1
    return bytes(out)

def packbits_encode(src, awkward):
    out = bytearray()
    i, n = 0, len(src)
    noop_budget = 3 if awkward else 0
    while i < n:
        # Longest run at i, capped at 128.
        run = 1
        while i + run < n and src[i + run] == src[i] and run < 128:
            run += 1
        if run >= 2:
            if noop_budget:
                out.append(128); noop_budget -= 1   # decoder must skip this
            out.append(257 - run); out.append(src[i])
            i += run
        else:
            start = i
            lit = 0
            while i < n and lit < 128:
                r = 1
                while i + r < n and src[i + r] == src[i] and r < 3:
                    r += 1
                if r >= 3: break
                i += 1; lit += 1
            out.append(lit - 1); out += src[start:start+lit]
    return bytes(out)

# ---- TGA (Truevision Targa) -------------------------------------------
# One-byte header: bit 7 clear means (h&0x7F)+1 raw bytes follow; bit 7 set
# means (h&0x7F)+1 copies of the single byte that follows.

def tga_decode(d):
    out = bytearray()
    i, n = 0, len(d)
    while i < n:
        h = d[i]; i += 1
        count = (h & 0x7F) + 1
        if h & 0x80:
            if i >= n: raise ValueError('run past end')
            out += bytes([d[i]]) * count; i += 1
        else:
            if i + count > n: raise ValueError('raw past end')
            out += d[i:i+count]; i += count
    return bytes(out)

def tga_encode(src, awkward):
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        run = 1
        while i + run < n and src[i + run] == src[i] and run < 128:
            run += 1
        if run >= 2:
            out.append(0x80 | (run - 1)); out.append(src[i]); i += run
        else:
            start = i
            lit = 0
            # When asked to be awkward, break literal packets at 1 byte so the
            # stream is a long chain of minimum-size packets.
            cap = 1 if awkward else 128
            while i < n and lit < cap:
                r = 1
                while i + r < n and src[i + r] == src[i] and r < 3:
                    r += 1
                if r >= 3: break
                i += 1; lit += 1
            out.append(lit - 1); out += src[start:start+lit]
    return bytes(out)

DEC = {'packbits': packbits_decode, 'tga': tga_decode}
ENC = {'packbits': packbits_encode, 'tga': tga_encode}

rep = open(sys.argv[2], 'w')
for line in open(sys.argv[1]):
    line = line.rstrip('\n')
    if not line: continue
    op, arg, ip, op_path = line.split('\t')
    try:
        data = open(ip, 'rb').read()
        fmt, mode = arg.split(':')
        if op == 'decode':
            open(op_path, 'wb').write(DEC[fmt](data))
        else:
            open(op_path, 'wb').write(ENC[fmt](data, mode == 'awkward'))
        rep.write('ok\n')
    except Exception as e:
        rep.write('fail %s: %s\n' % (type(e).__name__, str(e)[:60]))
rep.close()
)PYSRC";

struct BatchCase {
  std::string op;
  std::string arg;
  std::string in_path;
  std::string out_path;
};

class RleSpecOracleTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
    ASSERT_NE(registry_, nullptr);

    script_path_ = tempPath(".py");
    ASSERT_FALSE(script_path_.empty());
    FILE * f = fopen(script_path_.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fputs(kReferenceProgram, f);
    fclose(f);
  }

  void TearDown() override {
    if (!script_path_.empty()) {
      unlink(script_path_.c_str());
    }
  }

  bool runBatch(const std::vector<BatchCase> & cases,
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
      fprintf(mf, "%s\t%s\t%s\t%s\n", c.op.c_str(), c.arg.c_str(),
          c.in_path.c_str(), c.out_path.c_str());
    }
    fclose(mf);

    std::string cmd = std::string(pythonCommand()) + " \"" + script_path_ +
        "\" \"" + manifest + "\" \"" + report + "\"";
#ifdef _WIN32
    cmd += " >NUL 2>&1";
#else
    cmd += " >/dev/null 2>&1";
#endif
    bool ok = (system(cmd.c_str()) == 0);

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

  std::vector<uint8_t> gcompEncode(
      const std::vector<uint8_t> & data, const char * format) {
    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      return {};
    }
    gcomp_options_set_string(opts, "rle.format", format);

    std::vector<uint8_t> out(data.size() * 3 + 4096);
    size_t used = out.size();
    gcomp_status_t s = gcomp_encode_buffer(registry_, "rle", opts, data.data(),
        data.size(), out.data(), out.size(), &used);
    gcomp_options_destroy(opts);
    if (s != GCOMP_OK) {
      return {};
    }
    out.resize(used);
    return out;
  }

  std::vector<uint8_t> gcompDecode(const std::vector<uint8_t> & data,
      const char * format, size_t expected, bool * ok_out) {
    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      *ok_out = false;
      return {};
    }
    gcomp_options_set_string(opts, "rle.format", format);
    gcomp_options_set_uint64(opts, "limits.max_output_bytes", 64u << 20);
    gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 1000000);
    gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 256u << 20);

    std::vector<uint8_t> out(expected + 4096);
    size_t used = out.size();
    gcomp_status_t s = gcomp_decode_buffer(registry_, "rle", opts, data.data(),
        data.size(), out.data(), out.size(), &used);
    gcomp_options_destroy(opts);
    *ok_out = (s == GCOMP_OK);
    if (s != GCOMP_OK) {
      return {};
    }
    out.resize(used);
    return out;
  }

  gcomp_registry_t * registry_ = nullptr;
  std::string script_path_;
};

// Shapes that put the packet boundaries in different places: runs exactly at
// the 128-byte cap, alternating bytes that can never form a run, and mixtures.
std::vector<uint8_t> runsAndLiterals(size_t n, uint32_t seed) {
  std::vector<uint8_t> out;
  out.reserve(n + 300);
  uint32_t x = seed | 1u;
  while (out.size() < n) {
    x = x * 1103515245u + 12345u;
    unsigned kind = (x >> 16) % 4;
    unsigned len = 1 + ((x >> 8) % 200); // spans the 128 cap
    uint8_t b = (uint8_t)(x >> 24);
    if (kind == 0) {
      for (unsigned i = 0; i < len; i++) {
        out.push_back(b);
      }
    }
    else {
      for (unsigned i = 0; i < len; i++) {
        x = x * 1103515245u + 12345u;
        out.push_back((uint8_t)(x >> 24));
      }
    }
  }
  out.resize(n);
  return out;
}

std::vector<uint8_t> alternating(size_t n) {
  std::vector<uint8_t> out(n);
  for (size_t i = 0; i < n; i++) {
    out[i] = (uint8_t)(i & 1 ? 0xAA : 0x55);
  }
  return out;
}

std::vector<uint8_t> singleRun(size_t n) {
  return std::vector<uint8_t>(n, 0x7E);
}

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

struct Shape {
  const char * name;
  std::vector<uint8_t> data;
};

std::vector<Shape> shapes(size_t n) {
  std::vector<Shape> v;
  v.push_back({"runs and literals", runsAndLiterals(n, 20260917u)});
  v.push_back({"alternating", alternating(n)});
  v.push_back({"single run", singleRun(n)});
  v.push_back({"incompressible", incompressible(n, 5u)});
  return v;
}

// Sizes on and around the packet-length boundaries of both formats.
std::vector<size_t> sweepSizes() {
  return {0, 1, 2, 3, 4, 126, 127, 128, 129, 130, 254, 255, 256, 257, 258, 383,
      384, 385, 511, 512, 513, 1000, 4096, 50000};
}

const char * kFormats[] = {"packbits", "tga"};

TEST_F(RleSpecOracleTest, OracleIsActuallyAvailable) {
  std::vector<uint8_t> probe = singleRun(300);
  std::string in = tempPath(".raw");
  std::string out = tempPath(".rle");
  ASSERT_TRUE(writeFile(in, probe));

  std::vector<BatchCase> batch = {{"encode", "packbits:plain", in, out}};
  std::vector<std::string> report;
  ASSERT_TRUE(runBatch(batch, &report))
      << "Could not run the reference RLE implementation. This suite needs "
         "python3 and nothing else.";
  ASSERT_EQ(report[0], "ok") << report[0];

  unlink(in.c_str());
  unlink(out.c_str());
}

TEST_F(RleSpecOracleTest, OurEncoder_SpecDecoder_Sweep) {
  std::vector<std::vector<uint8_t>> originals;
  std::vector<std::string> labels;
  std::vector<BatchCase> batch;
  std::vector<std::string> paths;

  for (size_t n : sweepSizes()) {
    for (const Shape & sh : shapes(n)) {
      for (const char * fmt : kFormats) {
        std::vector<uint8_t> packed = gcompEncode(sh.data, fmt);
        ASSERT_FALSE(packed.empty() && n > 0)
            << sh.name << " n=" << n << " " << fmt;

        std::string path = tempPath(".rle");
        ASSERT_TRUE(writeFile(path, packed));
        batch.push_back(
            {"decode", std::string(fmt) + ":plain", path, path + ".out"});
        paths.push_back(path);
        originals.push_back(sh.data);
        labels.push_back(
            std::string(sh.name) + " n=" + std::to_string(n) + " " + fmt);
      }
    }
  }

  std::vector<std::string> report;
  bool ran = runBatch(batch, &report);

  int bad = 0;
  std::string detail;
  for (size_t i = 0; i < batch.size() && ran; i++) {
    std::vector<uint8_t> back;
    bool ok = (report[i] == "ok") && readFile(batch[i].out_path, &back) &&
        back == originals[i];
    if (!ok) {
      bad++;
      if (detail.size() < 300) {
        detail += (detail.empty() ? "" : "; ") + labels[i] + " (" + report[i] + ")";
      }
    }
    unlink(batch[i].out_path.c_str());
  }
  for (const std::string & p : paths) {
    unlink(p.c_str());
  }

  ASSERT_TRUE(ran) << "the reference decoder did not run over the sweep";
  EXPECT_EQ(bad, 0) << bad << " of " << batch.size()
                    << " of our streams the reference could not read; "
                    << detail;
}

TEST_F(RleSpecOracleTest, SpecEncoder_OurDecoder_Sweep) {
  // The "awkward" mode emits PackBits no-op control bytes and minimum-length
  // TGA packets: valid constructs that our own encoder never produces, and
  // therefore ones our decoder has never been handed.
  static const char * kModes[] = {"plain", "awkward"};

  std::vector<std::vector<uint8_t>> originals;
  std::vector<std::string> labels;
  std::vector<std::string> formats;
  std::vector<BatchCase> batch;
  std::vector<std::string> paths;

  for (size_t n : sweepSizes()) {
    for (const Shape & sh : shapes(n)) {
      for (const char * fmt : kFormats) {
        for (const char * mode : kModes) {
          std::string in = tempPath(".raw");
          ASSERT_TRUE(writeFile(in, sh.data));
          batch.push_back({"encode", std::string(fmt) + ":" + mode, in,
              in + ".rle"});
          paths.push_back(in);
          originals.push_back(sh.data);
          formats.push_back(fmt);
          labels.push_back(std::string(sh.name) + " n=" + std::to_string(n) +
              " " + fmt + " " + mode);
        }
      }
    }
  }

  std::vector<std::string> report;
  bool ran = runBatch(batch, &report);

  int rejected = 0, mismatched = 0;
  std::string detail;
  for (size_t i = 0; i < batch.size() && ran; i++) {
    if (report[i] != "ok") {
      continue;
    }
    std::vector<uint8_t> packed;
    if (!readFile(batch[i].out_path, &packed)) {
      continue;
    }
    bool ok = false;
    std::vector<uint8_t> back = gcompDecode(
        packed, formats[i].c_str(), originals[i].size(), &ok);
    if (!ok) {
      rejected++;
      if (detail.size() < 300) {
        detail += (detail.empty() ? "" : "; ") + labels[i];
      }
    }
    else if (back != originals[i]) {
      mismatched++;
      if (detail.size() < 300) {
        detail += (detail.empty() ? "" : "; ") + labels[i] + " (differs)";
      }
    }
    unlink(batch[i].out_path.c_str());
  }
  for (const std::string & p : paths) {
    unlink(p.c_str());
  }

  ASSERT_TRUE(ran) << "the reference encoder did not run over the sweep";
  EXPECT_EQ(rejected, 0) << rejected << " of " << batch.size()
                         << " reference streams our decoder refused; " << detail;
  EXPECT_EQ(mismatched, 0) << mismatched << " decoded to different bytes; "
                           << detail;
}

TEST_F(RleSpecOracleTest, OurDecoderSkipsThePackBitsNoOp) {
  // Control byte 128 is a no-op that a conformant decoder skips.  Our encoder
  // never writes one, so without this the path is unreachable from our own
  // tests, and a TIFF written elsewhere would be the first thing to find out.
  std::vector<uint8_t> stream = {
      128,          // no-op
      2, 'a', 'b', 'c', // three literals
      128, 128,     // two more no-ops in a row
      253, 'z',     // 257-253 = 4 copies of 'z'
      128,          // trailing no-op
  };
  std::vector<uint8_t> expected = {'a', 'b', 'c', 'z', 'z', 'z', 'z'};

  bool ok = false;
  std::vector<uint8_t> back =
      gcompDecode(stream, "packbits", expected.size(), &ok);
  ASSERT_TRUE(ok) << "our decoder rejected a stream containing no-ops";
  EXPECT_EQ(back, expected);

  // And the reference agrees about what that stream means.
  std::string in = tempPath(".rle");
  ASSERT_TRUE(writeFile(in, stream));
  std::vector<BatchCase> batch = {
      {"decode", "packbits:plain", in, in + ".out"}};
  std::vector<std::string> report;
  ASSERT_TRUE(runBatch(batch, &report));
  ASSERT_EQ(report[0], "ok") << report[0];
  std::vector<uint8_t> refOut;
  ASSERT_TRUE(readFile(in + ".out", &refOut));
  EXPECT_EQ(refOut, expected);

  unlink(in.c_str());
  unlink((in + ".out").c_str());
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
