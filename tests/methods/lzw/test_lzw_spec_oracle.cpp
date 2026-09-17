/**
 * @file test_lzw_spec_oracle.cpp
 *
 * Cross-implementation tests for the LZW method against a reference written
 * from the GIF and TIFF descriptions.
 *
 * LZW had no oracle tests: its round-trips checked our encoder against our
 * decoder, which agree by construction.  That is not a hypothetical concern
 * here -- the RLE method's round-trips passed for a PackBits run length that
 * was one byte short of what every other implementation writes.
 *
 * The reference below was itself checked before being trusted: its GIF
 * decoder reads real GIFs produced by Pillow's encoder, byte-exact, across
 * seven images of different sizes and structures.  A reference that only
 * agrees with the thing it is testing is no better than a round-trip.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lzw.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <gtest/gtest.h>
#include <string>
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
  static int counter = 0;
#ifdef _WIN32
  char dir[MAX_PATH];
  if (GetTempPathA(MAX_PATH, dir) == 0) {
    return "";
  }
  char name[MAX_PATH];
  snprintf(name, sizeof(name), "%sgcomp_lzw_spec_%d_%d%s", dir, _getpid(),
      counter++, suffix.c_str());
  return name;
#else
  const char * dir = getenv("TMPDIR");
  if (!dir || !*dir) {
    dir = "/tmp";
  }
  char name[1024];
  snprintf(name, sizeof(name), "%s/gcomp_lzw_spec_%d_%d%s", dir, (int)getpid(),
      counter++, suffix.c_str());
  return name;
#endif
}

bool writeFile(const std::string & path, const std::vector<uint8_t> & data) {
  FILE * f = fopen(path.c_str(), "wb");
  if (!f) {
    return false;
  }
  bool ok =
      data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
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

const char * kReferenceProgram = R"PYSRC(
import sys
# LZW as GIF and TIFF use it, written from their descriptions.
# clear = 256, end-of-information = 257, first sequence code 258, code width
# starts at 9 and grows to 12.  GIF packs bits least-significant-first and
# grows the width when the next free code reaches 2^width; TIFF packs
# most-significant-first and grows one code early, at 2^width - 1.

CLEAR, EOI, FIRST = 256, 257, 258

class BitReader:
    def __init__(self, data, msb):
        self.d, self.msb, self.pos = data, msb, 0
    def read(self, n):
        v = 0
        for i in range(n):
            byte_i, bit_i = self.pos >> 3, self.pos & 7
            if byte_i >= len(self.d):
                raise ValueError('out of bits')
            if self.msb:
                bit = (self.d[byte_i] >> (7 - bit_i)) & 1
                v = (v << 1) | bit
            else:
                bit = (self.d[byte_i] >> bit_i) & 1
                v |= bit << i
            self.pos += 1
        return v

class BitWriter:
    def __init__(self, msb):
        self.out, self.msb, self.acc, self.nbits = bytearray(), msb, 0, 0
    def write(self, v, n):
        for i in range(n):
            bit = (v >> (n - 1 - i)) & 1 if self.msb else (v >> i) & 1
            if self.msb:
                self.acc = (self.acc << 1) | bit
            else:
                self.acc |= bit << self.nbits
            self.nbits += 1
            if self.nbits == 8:
                self.out.append(self.acc & 0xFF); self.acc = 0; self.nbits = 0
    def finish(self):
        if self.nbits:
            if self.msb:
                self.acc <<= (8 - self.nbits)
            self.out.append(self.acc & 0xFF)
            self.acc = 0; self.nbits = 0
        return bytes(self.out)

def grows(profile, next_code, width, encoding=False):
    """When the code width steps up.

    GIF widens once the next free code reaches 2^width; TIFF widens one code
    earlier ("early change"), at 2^width - 1.

    The encoder widens one code LATER than the decoder does.  That is not an
    inconsistency: at the same point in the stream the decoder has added one
    fewer dictionary entry than the encoder, because it only learns an entry
    after it has seen the code that follows it.  The same threshold applied to
    each side's own next_code therefore lands one code apart.  Using the
    decoder's threshold in the encoder produces a stream that widens too early
    and that no decoder can read past the first boundary."""
    if width >= 12:
        return False
    base = (1 << width) - 1 if profile == 'tiff' else (1 << width)
    return next_code == (base + 1 if encoding else base)

def decode(data, profile):
    br = BitReader(data, profile == 'tiff')
    out = bytearray()
    table = {}
    next_code = FIRST
    width = 9
    prev = None
    while True:
        try:
            code = br.read(width)
        except ValueError:
            break
        if code == EOI:
            break
        if code == CLEAR:
            table = {}; next_code = FIRST; width = 9; prev = None
            continue
        if code < 256:
            entry = bytes([code])
        elif code in table:
            entry = table[code]
        elif code == next_code and prev is not None:
            entry = prev + prev[:1]           # KwKwK
        else:
            raise ValueError('bad code %d' % code)
        out += entry
        if prev is not None and next_code < 4096:
            table[next_code] = prev + entry[:1]
            next_code += 1
            if grows(profile, next_code, width):
                width += 1
        prev = entry
    return bytes(out)

def encode(src, profile, emit_clears=False):
    bw = BitWriter(profile == 'tiff')
    table = {}
    next_code = FIRST
    width = 9
    bw.write(CLEAR, width)
    w = b''
    for i, ch in enumerate(src):
        c = bytes([ch])
        wc = w + c
        if len(wc) == 1 or wc in table:
            w = wc
            continue
        bw.write(table[w] if len(w) > 1 else w[0], width)
        if next_code < 4096:
            table[wc] = next_code
            next_code += 1
            if grows(profile, next_code, width, encoding=True):
                width += 1
        else:
            bw.write(CLEAR, width)
            table = {}; next_code = FIRST; width = 9
        w = c
    if w:
        bw.write(table[w] if len(w) > 1 else w[0], width)
    bw.write(EOI, width)
    return bw.finish()

DEC = decode
ENC = encode

rep = open(sys.argv[2], 'w')
for line in open(sys.argv[1]):
    line = line.rstrip('\n')
    if not line: continue
    op, arg, ip, op_path = line.split('\t')
    try:
        data = open(ip, 'rb').read()
        if op == 'decode':
            open(op_path, 'wb').write(DEC(data, arg))
        else:
            open(op_path, 'wb').write(ENC(data, arg))
        rep.write('ok\n')
    except Exception as e:
        rep.write('fail %s: %s\n' % (type(e).__name__, str(e)[:60]))
rep.close()
)PYSRC";

struct BatchCase {
  std::string op;
  std::string arg; // "gif" or "tiff"
  std::string in_path;
  std::string out_path;
};

class LzwSpecOracleTest : public ::testing::Test {
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
    gcomp_options_set_string(opts, "lzw.format", format);

    std::vector<uint8_t> out(data.size() * 3 + 65536);
    size_t used = out.size();
    gcomp_status_t s = gcomp_encode_buffer(registry_, "lzw", opts, data.data(),
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
    gcomp_options_set_string(opts, "lzw.format", format);
    gcomp_options_set_uint64(opts, "limits.max_output_bytes", 64u << 20);
    gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 1000000);
    gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 256u << 20);

    std::vector<uint8_t> out(expected + 65536);
    size_t used = out.size();
    gcomp_status_t s = gcomp_decode_buffer(registry_, "lzw", opts, data.data(),
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

// Shapes chosen to drive the dictionary differently: one that fills it fast
// and forces the code width up through every step, one that never repeats, and
// one that repeats so hard the table fills and has to be cleared.
std::vector<uint8_t> structured(size_t n) {
  std::vector<uint8_t> out(n);
  for (size_t i = 0; i < n; i++) {
    out[i] = (uint8_t)((i / 7) % 256);
  }
  return out;
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

std::vector<uint8_t> fewSymbols(size_t n) {
  std::vector<uint8_t> out(n);
  for (size_t i = 0; i < n; i++) {
    out[i] = (uint8_t)('A' + (i % 3));
  }
  return out;
}

std::vector<uint8_t> singleRun(size_t n) {
  return std::vector<uint8_t>(n, 0x42);
}

struct Shape {
  const char * name;
  std::vector<uint8_t> data;
};

std::vector<Shape> shapes(size_t n) {
  std::vector<Shape> v;
  v.push_back({"structured", structured(n)});
  v.push_back({"incompressible", incompressible(n, 17u)});
  v.push_back({"few symbols", fewSymbols(n)});
  v.push_back({"single run", singleRun(n)});
  return v;
}

// Sizes small enough to stay in 9-bit codes, large enough to walk the width up
// to 12, and large enough to fill the dictionary and force a reset.
std::vector<size_t> sweepSizes() {
  return {0, 1, 2, 3, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 2047,
      2048, 4095, 4096, 4097, 20000, 120000};
}

const char * kFormats[] = {"gif", "tiff"};

TEST_F(LzwSpecOracleTest, OracleIsActuallyAvailable) {
  std::vector<uint8_t> probe = structured(600);
  std::string in = tempPath(".raw");
  std::string out = tempPath(".lzw");
  ASSERT_TRUE(writeFile(in, probe));

  std::vector<BatchCase> batch = {{"encode", "gif", in, out}};
  std::vector<std::string> report;
  ASSERT_TRUE(runBatch(batch, &report))
      << "Could not run the reference LZW implementation. This suite needs "
         "python3 and nothing else.";
  ASSERT_EQ(report[0], "ok") << report[0];

  unlink(in.c_str());
  unlink(out.c_str());
}

TEST_F(LzwSpecOracleTest, OurEncoder_SpecDecoder_Sweep) {
  std::vector<std::vector<uint8_t>> originals;
  std::vector<std::string> labels;
  std::vector<BatchCase> batch;
  std::vector<std::string> paths;

  for (size_t n : sweepSizes()) {
    for (const Shape & sh : shapes(n)) {
      for (const char * fmt : kFormats) {
        std::vector<uint8_t> packed = gcompEncode(sh.data, fmt);
        ASSERT_FALSE(packed.empty()) << sh.name << " n=" << n << " " << fmt;

        std::string path = tempPath(".lzw");
        ASSERT_TRUE(writeFile(path, packed));
        batch.push_back({"decode", fmt, path, path + ".out"});
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
        detail +=
            (detail.empty() ? "" : "; ") + labels[i] + " (" + report[i] + ")";
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

TEST_F(LzwSpecOracleTest, SpecEncoder_OurDecoder_Sweep) {
  std::vector<std::vector<uint8_t>> originals;
  std::vector<std::string> labels;
  std::vector<std::string> formats;
  std::vector<BatchCase> batch;
  std::vector<std::string> paths;

  for (size_t n : sweepSizes()) {
    for (const Shape & sh : shapes(n)) {
      for (const char * fmt : kFormats) {
        std::string in = tempPath(".raw");
        ASSERT_TRUE(writeFile(in, sh.data));
        batch.push_back({"encode", fmt, in, in + ".lzw"});
        paths.push_back(in);
        originals.push_back(sh.data);
        formats.push_back(fmt);
        labels.push_back(
            std::string(sh.name) + " n=" + std::to_string(n) + " " + fmt);
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
    std::vector<uint8_t> back =
        gcompDecode(packed, formats[i].c_str(), originals[i].size(), &ok);
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

TEST_F(LzwSpecOracleTest, CodeWidthGrowsAtTheRightPointForEachProfile) {
  // GIF widens when the next free code reaches 2^width; TIFF widens one code
  // earlier, at 2^width - 1.  That single-code difference is invisible to a
  // round-trip and fatal to interoperability, and it only shows on input long
  // enough to build 254 dictionary entries.  These sizes straddle each of the
  // three width steps.
  for (size_t n : {size_t(600), size_t(1200), size_t(2400), size_t(5000),
           size_t(9000), size_t(20000)}) {
    std::vector<uint8_t> data = structured(n);

    std::vector<uint8_t> gif = gcompEncode(data, "gif");
    std::vector<uint8_t> tiff = gcompEncode(data, "tiff");
    ASSERT_FALSE(gif.empty()) << "n=" << n;
    ASSERT_FALSE(tiff.empty()) << "n=" << n;

    // If the two profiles produced identical streams, one of the two rules is
    // not being applied and this test would be checking nothing.
    EXPECT_NE(gif, tiff)
        << "n=" << n
        << ": the GIF and TIFF profiles produced the same bytes, so their "
           "differing code-width rules are not both in effect";

    std::string gp = tempPath(".gif.lzw");
    std::string tp = tempPath(".tiff.lzw");
    ASSERT_TRUE(writeFile(gp, gif));
    ASSERT_TRUE(writeFile(tp, tiff));

    std::vector<BatchCase> batch = {{"decode", "gif", gp, gp + ".out"},
        {"decode", "tiff", tp, tp + ".out"}};
    std::vector<std::string> report;
    ASSERT_TRUE(runBatch(batch, &report)) << "n=" << n;

    for (int k = 0; k < 2; k++) {
      ASSERT_EQ(report[k], "ok")
          << "n=" << n << " " << (k ? "tiff" : "gif") << ": " << report[k];
      std::vector<uint8_t> back;
      ASSERT_TRUE(readFile(batch[k].out_path, &back));
      ASSERT_EQ(back, data) << "n=" << n << " " << (k ? "tiff" : "gif");
      unlink(batch[k].out_path.c_str());
    }
    unlink(gp.c_str());
    unlink(tp.c_str());
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
