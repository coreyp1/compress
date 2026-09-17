/**
 * @file test_lz4_spec_oracle.cpp
 *
 * Cross-implementation tests for the LZ4 method against a reference written
 * from the LZ4 frame and block specifications.
 *
 * The existing test_lz4_oracle.cpp compares against the `lz4` CLI or the
 * Python `lz4.frame` module.  Where neither is installed -- which is the
 * common case, and the case here -- 18 of its 19 tests skip and the summary
 * line still reports a pass.  A skipped oracle test and an absent one are
 * indistinguishable, so in practice this method had one golden-vector test
 * standing between 3,000 lines of codec and nobody noticing.
 *
 * The reference below is written from the specification rather than adapted
 * from any implementation, including xxHash-32, which the frame format needs
 * for its header, block and content checksums.  It depends on nothing beyond
 * Python itself, so it cannot be skipped.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/lz4.h>
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
  snprintf(name, sizeof(name), "%sgcomp_lz4_spec_%d_%d%s", dir, _getpid(),
      counter++, suffix.c_str());
  return name;
#else
  const char * dir = getenv("TMPDIR");
  if (!dir || !*dir) {
    dir = "/tmp";
  }
  char name[1024];
  snprintf(name, sizeof(name), "%s/gcomp_lz4_spec_%d_%d%s", dir, (int)getpid(),
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

// The reference, written from the LZ4 frame format and block format
// descriptions.  Reads a manifest of "op<TAB>arg<TAB>in<TAB>out" lines and
// writes one outcome line per entry.
const char * kReferenceProgram = R"PYSRC(
import sys, struct

# ---- xxHash-32, from its specification; the LZ4 frame format uses it for
# ---- the header, block and content checksums, all with seed 0.
P1, P2, P3, P4, P5 = 2654435761, 2246822519, 3266489917, 668265263, 374761393
M = 0xFFFFFFFF

def rotl(x, r):
    return ((x << r) | (x >> (32 - r))) & M

def xxh32(data, seed=0):
    n = len(data)
    i = 0
    if n >= 16:
        v1 = (seed + P1 + P2) & M
        v2 = (seed + P2) & M
        v3 = seed & M
        v4 = (seed - P1) & M
        while i + 16 <= n:
            for vi in range(4):
                lane = struct.unpack_from('<I', data, i + vi * 4)[0]
                v = (v1, v2, v3, v4)[vi]
                v = (v + lane * P2) & M
                v = rotl(v, 13)
                v = (v * P1) & M
                if vi == 0: v1 = v
                elif vi == 1: v2 = v
                elif vi == 2: v3 = v
                else: v4 = v
            i += 16
        h = (rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18)) & M
    else:
        h = (seed + P5) & M
    h = (h + n) & M
    while i + 4 <= n:
        h = (h + struct.unpack_from('<I', data, i)[0] * P3) & M
        h = (rotl(h, 17) * P4) & M
        i += 4
    while i < n:
        h = (h + data[i] * P5) & M
        h = (rotl(h, 11) * P1) & M
        i += 1
    h ^= h >> 15
    h = (h * P2) & M
    h ^= h >> 13
    h = (h * P3) & M
    h ^= h >> 16
    return h & M

# ---- LZ4 block format -------------------------------------------------
# A block is a series of sequences: a token byte whose high nibble is the
# literal length and low nibble the match length (minus 4), optional extra
# length bytes of 255, the literals, a 2-byte little-endian offset, and
# optional extra match-length bytes.  The last sequence carries literals only.

def block_decode(d, expected=None):
    out = bytearray()
    i, n = 0, len(d)
    while i < n:
        token = d[i]; i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = d[i]; i += 1
                lit += b
                if b != 255: break
        if i + lit > n: raise ValueError('literal run past end')
        out += d[i:i+lit]; i += lit
        if i == n:
            break
        offset = d[i] | (d[i+1] << 8); i += 2
        if offset == 0: raise ValueError('offset 0')
        ml = token & 15
        if ml == 15:
            while True:
                b = d[i]; i += 1
                ml += b
                if b != 255: break
        ml += 4
        if offset > len(out): raise ValueError('offset past history')
        start = len(out) - offset
        for k in range(ml):
            out.append(out[start + k])
    return bytes(out)

def emit_length(buf, value):
    while value >= 255:
        buf.append(255); value -= 255
    buf.append(value)

def block_encode(src, style):
    """A deliberately awkward encoder: valid, and shaped to hit the cases a
    decoder is most likely to get wrong -- long literal runs that need extra
    length bytes, offset-1 matches, and long matches.

    It is not trying to compress well, only to be correct and varied.  The
    match search is a single-slot hash of the last position each 4-byte
    prefix was seen at, which keeps it linear; an exhaustive search made this
    suite take minutes instead of a second."""
    out = bytearray()
    n = len(src)
    i = 0
    pending = bytearray()
    table = {}
    limit = n - 5          # the last 5 bytes must be literals
    last_start = n - 12    # no match may start within 12 bytes of the end

    def flush_sequence(match_off, match_len):
        token_lit = len(pending)
        token = (min(token_lit, 15) << 4) | min(match_len - 4, 15)
        out.append(token)
        if token_lit >= 15: emit_length(out, token_lit - 15)
        out.extend(pending)
        out.append(match_off & 0xFF); out.append((match_off >> 8) & 0xFF)
        if match_len - 4 >= 15: emit_length(out, match_len - 4 - 15)
        pending.clear()

    while i < n:
        best_off = 0; best_len = 0
        if style != 'literals_only' and i >= 1 and i <= last_start:
            cands = []
            if style == 'offset1_only':
                cands = [i - 1]
            else:
                key = bytes(src[i:i+4])
                prev = table.get(key)
                if prev is not None and i - prev <= 65535:
                    cands = [prev]
            for p in cands:
                if p < 0: continue
                ln = 0
                while i + ln < limit and src[p + ln] == src[i + ln] and ln < 65535:
                    ln += 1
                if ln >= 4 and ln > best_len:
                    best_len = ln; best_off = i - p
        if style not in ('literals_only', 'offset1_only') and i + 4 <= n:
            table[bytes(src[i:i+4])] = i
        if best_len >= 4:
            flush_sequence(best_off, best_len)
            i += best_len
        else:
            pending.append(src[i]); i += 1

    # Final sequence: literals only, no offset.
    token = (min(len(pending), 15) << 4)
    out.append(token)
    if len(pending) >= 15: emit_length(out, len(pending) - 15)
    out.extend(pending)
    return bytes(out)

# ---- LZ4 frame format -------------------------------------------------

def frame_decode(d):
    if len(d) < 7: raise ValueError('too short')
    if struct.unpack_from('<I', d, 0)[0] != 0x184D2204: raise ValueError('bad magic')
    p = 4
    flg, bd = d[p], d[p+1]
    if (flg >> 6) != 1: raise ValueError('bad version')
    block_cksum = (flg >> 4) & 1
    content_size = (flg >> 3) & 1
    content_cksum = (flg >> 2) & 1
    dict_id = flg & 1
    q = p + 2
    if content_size: q += 8
    if dict_id: q += 4
    if ((xxh32(d[4:q]) >> 8) & 0xFF) != d[q]:
        raise ValueError('header checksum')
    q += 1
    out = bytearray()
    while True:
        bs = struct.unpack_from('<I', d, q)[0]; q += 4
        if bs == 0: break
        raw = bool(bs & 0x80000000); size = bs & 0x7FFFFFFF
        payload = d[q:q+size]; q += size
        if block_cksum:
            if xxh32(payload) != struct.unpack_from('<I', d, q)[0]:
                raise ValueError('block checksum')
            q += 4
        out += payload if raw else block_decode(payload)
    if content_cksum:
        if xxh32(bytes(out)) != struct.unpack_from('<I', d, q)[0]:
            raise ValueError('content checksum')
        q += 4
    return bytes(out)

def frame_encode(src, style, block_size=65536, with_content_size=False,
                 with_block_cksum=False, with_content_cksum=True):
    flg = 0x40 | 0x20  # version 1, independent blocks
    if with_block_cksum: flg |= 0x10
    if with_content_size: flg |= 0x08
    if with_content_cksum: flg |= 0x04
    bd = {65536: 0x40, 262144: 0x50, 1048576: 0x60, 4194304: 0x70}[block_size]
    hdr = bytearray([flg, bd])
    if with_content_size: hdr += struct.pack('<Q', len(src))
    hc = (xxh32(bytes(hdr)) >> 8) & 0xFF
    out = bytearray(struct.pack('<I', 0x184D2204)) + hdr + bytes([hc])

    # An empty input carries no blocks at all: a block size of zero is the
    # EndMark, so emitting a zero-length block would end the frame early.
    for off in range(0, len(src), block_size):
        chunk = src[off:off+block_size]
        if not chunk: break
        body = block_encode(chunk, style)
        if len(body) >= len(chunk):
            out += struct.pack('<I', len(chunk) | 0x80000000) + chunk
            stored = chunk
        else:
            out += struct.pack('<I', len(body)) + body
            stored = body
        if with_block_cksum: out += struct.pack('<I', xxh32(stored))
    out += struct.pack('<I', 0)
    if with_content_cksum: out += struct.pack('<I', xxh32(src))
    return bytes(out)

# ---- driver -----------------------------------------------------------

rep = open(sys.argv[2], 'w')
for line in open(sys.argv[1]):
    line = line.rstrip('\n')
    if not line: continue
    op, arg, ip, op_path = line.split('\t')
    try:
        data = open(ip, 'rb').read()
        if op == 'decode':
            open(op_path, 'wb').write(frame_decode(data))
        else:
            style, rest = arg.split(':', 1)
            bs, flags = rest.split(':', 1)
            open(op_path, 'wb').write(frame_encode(data, style, int(bs),
                'S' in flags, 'B' in flags, 'C' in flags))
        rep.write('ok\n')
    except Exception as e:
        rep.write('fail %s: %s\n' % (type(e).__name__, str(e)[:60]))
rep.close()
)PYSRC";

struct BatchCase {
  std::string op;   // "decode" or "encode"
  std::string arg;  // encode: "style:blocksize:flags"
  std::string in_path;
  std::string out_path;
};

class Lz4SpecOracleTest : public ::testing::Test {
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
      const std::vector<uint8_t> & data, gcomp_options_t * opts = nullptr) {
    std::vector<uint8_t> out(data.size() * 2 + 65536);
    size_t used = out.size();
    gcomp_status_t s = gcomp_encode_buffer(registry_, "lz4", opts, data.data(),
        data.size(), out.data(), out.size(), &used);
    if (s != GCOMP_OK) {
      return {};
    }
    out.resize(used);
    return out;
  }

  std::vector<uint8_t> gcompDecode(
      const std::vector<uint8_t> & data, size_t expected, bool * ok_out) {
    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      *ok_out = false;
      return {};
    }
    gcomp_options_set_uint64(opts, "limits.max_output_bytes", 64u << 20);
    gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 1000000);
    gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 256u << 20);

    std::vector<uint8_t> out(expected + 65536);
    size_t used = out.size();
    gcomp_status_t s = gcomp_decode_buffer(registry_, "lz4", opts, data.data(),
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

std::vector<uint8_t> proseLike(size_t n, uint32_t seed) {
  static const char * words[] = {"the", "quick", "brown", "fox", "jumps",
      "over", "lazy", "dog", "and", "then", "returns", "home"};
  std::vector<uint8_t> out;
  out.reserve(n + 16);
  uint32_t x = seed;
  while (out.size() < n) {
    x = x * 1103515245u + 12345u;
    const char * w = words[(x >> 16) % 12];
    for (const char * c = w; *c; c++) {
      out.push_back((uint8_t)*c);
    }
    out.push_back(' ');
  }
  out.resize(n);
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

std::vector<uint8_t> singleRun(size_t n) {
  return std::vector<uint8_t>(n, 'Q');
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
  return v;
}

std::vector<size_t> sweepSizes() {
  return {0, 1, 2, 3, 4, 5, 11, 12, 13, 15, 16, 17, 19, 20, 31, 32, 33, 63, 64,
      65, 127, 254, 255, 256, 269, 270, 271, 512, 1023, 1024, 4096, 20000,
      65535, 65536, 65537, 140000};
}

//
// Tests
//

TEST_F(Lz4SpecOracleTest, OracleIsActuallyAvailable) {
  // If this fails, python3 is missing and nothing in this file is comparing
  // our output against anything. Said out loud rather than skipped.
  std::vector<uint8_t> probe = proseLike(200, 1u);
  std::string in = tempPath(".raw");
  std::string out = tempPath(".lz4");
  ASSERT_TRUE(writeFile(in, probe));

  std::vector<BatchCase> batch = {{"encode", "first_match:65536:C", in, out}};
  std::vector<std::string> report;
  ASSERT_TRUE(runBatch(batch, &report))
      << "Could not run the reference LZ4 implementation. This suite needs "
         "python3 and nothing else; without it nothing here is checked "
         "against a reference.";
  ASSERT_EQ(report[0], "ok") << report[0];

  std::vector<uint8_t> framed;
  ASSERT_TRUE(readFile(out, &framed));
  ASSERT_GT(framed.size(), 7u);

  unlink(in.c_str());
  unlink(out.c_str());
}

TEST_F(Lz4SpecOracleTest, OurEncoder_SpecDecoder_Sweep) {
  std::vector<std::vector<uint8_t>> originals;
  std::vector<std::string> labels;
  std::vector<BatchCase> batch;
  std::vector<std::string> paths;

  for (size_t n : sweepSizes()) {
    for (const Shape & sh : shapes(n)) {
      for (int cksum = 0; cksum <= 1; cksum++) {
        gcomp_options_t * opts = nullptr;
        ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
        gcomp_options_set_bool(opts, "lz4.content_checksum", cksum);
        gcomp_options_set_bool(opts, "lz4.content_size", cksum);

        std::vector<uint8_t> packed = gcompEncode(sh.data, opts);
        gcomp_options_destroy(opts);
        ASSERT_FALSE(packed.empty()) << sh.name << " n=" << n;

        std::string path = tempPath(".lz4");
        ASSERT_TRUE(writeFile(path, packed));
        batch.push_back({"decode", "-", path, path + ".out"});
        paths.push_back(path);
        originals.push_back(sh.data);
        labels.push_back(std::string(sh.name) + " n=" + std::to_string(n) +
            (cksum ? " +checksum" : ""));
      }
    }
  }

  std::vector<std::string> report;
  bool ran = runBatch(batch, &report);

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
  for (const std::string & p : paths) {
    unlink(p.c_str());
  }

  ASSERT_TRUE(ran) << "the reference decoder did not run over the sweep";
  EXPECT_EQ(bad, 0) << bad << " of " << batch.size()
                    << " of our frames the reference could not read; first: "
                    << first_bad;
}

TEST_F(Lz4SpecOracleTest, SpecEncoder_OurDecoder_Sweep) {
  // Three encoding styles on purpose: one that emits nothing but literals
  // (so long literal runs and their extra length bytes are exercised), one
  // that only ever uses offset 1 (the overlapping-copy path), and one that
  // takes the first match it finds (ordinary mixed sequences).
  static const char * kStyles[] = {"literals_only", "offset1_only", "first_match"};
  static const char * kFlagSets[] = {"", "C", "SC", "BC", "SBC"};

  std::vector<std::vector<uint8_t>> originals;
  std::vector<std::string> labels;
  std::vector<BatchCase> batch;
  std::vector<std::string> paths;

  for (size_t n : sweepSizes()) {
    if (n > 70000) {
      continue; // the reference encoder is deliberately naive, not fast
    }
    for (const Shape & sh : shapes(n)) {
      for (const char * style : kStyles) {
        for (const char * flags : kFlagSets) {
          std::string in = tempPath(".raw");
          ASSERT_TRUE(writeFile(in, sh.data));
          std::string arg = std::string(style) + ":65536:" + flags;
          batch.push_back({"encode", arg, in, in + ".lz4"});
          paths.push_back(in);
          originals.push_back(sh.data);
          labels.push_back(std::string(sh.name) + " n=" + std::to_string(n) +
              " " + style + " flags=" + (flags[0] ? flags : "none"));
        }
      }
    }
  }

  std::vector<std::string> report;
  bool ran = runBatch(batch, &report);

  int rejected = 0, mismatched = 0;
  std::string first_bad;
  for (size_t i = 0; i < batch.size() && ran; i++) {
    if (report[i] != "ok") {
      continue; // the reference declining to build a frame is its business
    }
    std::vector<uint8_t> framed;
    if (!readFile(batch[i].out_path, &framed)) {
      continue;
    }
    bool ok = false;
    std::vector<uint8_t> back =
        gcompDecode(framed, originals[i].size(), &ok);
    if (!ok) {
      rejected++;
      if (first_bad.size() < 400) {
        first_bad += (first_bad.empty() ? "" : "; ") + labels[i];
      }
    }
    else if (back != originals[i]) {
      mismatched++;
      if (first_bad.size() < 400) {
        first_bad += (first_bad.empty() ? "" : "; ") + labels[i] +
            " (content differs)";
      }
    }
    unlink(batch[i].out_path.c_str());
  }
  for (const std::string & p : paths) {
    unlink(p.c_str());
  }

  ASSERT_TRUE(ran) << "the reference encoder did not run over the sweep";
  EXPECT_EQ(rejected, 0) << rejected << " of " << batch.size()
                         << " reference frames our decoder refused; first: "
                         << first_bad;
  EXPECT_EQ(mismatched, 0) << mismatched << " decoded to different bytes; "
                           << "first: " << first_bad;
}

TEST_F(Lz4SpecOracleTest, RoundTripThroughBothImplementations) {
  for (const Shape & sh : shapes(40000)) {
    std::vector<uint8_t> ours = gcompEncode(sh.data);
    ASSERT_FALSE(ours.empty()) << sh.name;

    std::string p1 = tempPath(".lz4");
    ASSERT_TRUE(writeFile(p1, ours));
    std::vector<BatchCase> batch = {{"decode", "-", p1, p1 + ".out"}};
    std::vector<std::string> report;
    ASSERT_TRUE(runBatch(batch, &report)) << sh.name;
    ASSERT_EQ(report[0], "ok") << sh.name << ": " << report[0];

    std::vector<uint8_t> viaRef;
    ASSERT_TRUE(readFile(p1 + ".out", &viaRef));
    ASSERT_EQ(viaRef, sh.data) << sh.name;

    // And back the other way, so a matched pair of mistakes cannot pass.
    std::string p2 = tempPath(".raw");
    ASSERT_TRUE(writeFile(p2, viaRef));
    batch = {{"encode", "first_match:65536:SC", p2, p2 + ".lz4"}};
    ASSERT_TRUE(runBatch(batch, &report)) << sh.name;
    ASSERT_EQ(report[0], "ok") << sh.name << ": " << report[0];

    std::vector<uint8_t> refFrame;
    ASSERT_TRUE(readFile(p2 + ".lz4", &refFrame));
    bool ok = false;
    std::vector<uint8_t> back = gcompDecode(refFrame, sh.data.size(), &ok);
    ASSERT_TRUE(ok) << sh.name;
    ASSERT_EQ(back, sh.data) << sh.name;

    unlink(p1.c_str());
    unlink((p1 + ".out").c_str());
    unlink(p2.c_str());
    unlink((p2 + ".lz4").c_str());
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
