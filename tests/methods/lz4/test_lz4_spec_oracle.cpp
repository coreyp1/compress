/**
 * @file test_lz4_spec_oracle.cpp
 *
 * Cross-implementation tests for the LZ4 method against a reference written
 * from the LZ4 frame and block specifications.
 *
 * This file replaced test_lz4_oracle.cpp, which compared against the `lz4`
 * CLI or the Python `lz4.frame` module.  Where neither was installed -- the
 * common case, and the case here -- 18 of its 19 tests skipped and the
 * summary line still reported a pass.  A skipped oracle test and an absent
 * one are indistinguishable, so in practice this method had one golden-vector
 * test standing between 3,000 lines of codec and nobody noticing.
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
#include <ghoti.io/compress/stream.h>
#include <gtest/gtest.h>
#include <ghoti.io/cutil/library.h>
#include <algorithm>
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
  return gcomp_test::uniqueTempPath("gcomp_lz4_oracle", suffix);
}

bool writeFile(const std::string & path, const std::vector<uint8_t> & data) {
  return gcomp_test::writeWholeFile(path, data);
}

bool readFile(const std::string & path, std::vector<uint8_t> * out) {
  return gcomp_test::readWholeFile(path, out);
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

  std::vector<uint8_t> gcompDecode(const std::vector<uint8_t> & data,
      size_t expected, bool * ok_out, int concat = -1) {
    gcomp_options_t * opts = nullptr;
    if (gcomp_options_create(&opts) != GCOMP_OK) {
      *ok_out = false;
      return {};
    }
    if (concat >= 0) {
      gcomp_options_set_bool(opts, "lz4.concat", concat);
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
// Frame walking
//
// A test that only checks "it decoded" cannot tell a multi-block frame from a
// single-block one, which is exactly how the block loop went uncovered: the
// default block size is 4 MB (LZ4_DEFAULT_BLOCK_SIZE) and the sweep above
// stops at 140,000 bytes, so every frame in every other test here holds one
// block.  These tests assert the construct is present before asserting it
// round-trips.
//
struct FrameShape {
  unsigned blocks = 0;
  unsigned stored_blocks = 0;
  unsigned block_size_id = 0; // BD bits 6-4: 4=64KB, 5=256KB, 6=1MB, 7=4MB
  bool block_independent = false;
  bool block_checksum = false;
  bool content_checksum = false;
  bool content_size = false;
  bool dict_id = false;
};

// Walks the frame header and block sequence per RFC-less LZ4 Frame Format
// v1.6.3 section "General Structure of LZ4 Frame format".  Returns false if
// the frame is malformed or truncated.
bool walkFrame(const std::vector<uint8_t> & f, FrameShape * out) {
  if (f.size() < 7) {
    return false;
  }
  if (f[0] != 0x04 || f[1] != 0x22 || f[2] != 0x4D || f[3] != 0x18) {
    return false; // not a frame magic number
  }
  const uint8_t flg = f[4];
  const uint8_t bd = f[5];
  if ((flg & 0xC0) != 0x40) {
    return false; // version bits must be 01
  }

  FrameShape sh;
  sh.block_independent = (flg >> 5) & 1;
  sh.block_checksum = (flg >> 4) & 1;
  sh.content_size = (flg >> 3) & 1;
  sh.content_checksum = (flg >> 2) & 1;
  sh.dict_id = flg & 1;
  sh.block_size_id = (bd >> 4) & 7;

  size_t p = 6;
  p += sh.content_size ? 8 : 0;
  p += sh.dict_id ? 4 : 0;
  p += 1; // header checksum
  if (p > f.size()) {
    return false;
  }

  for (;;) {
    if (p + 4 > f.size()) {
      return false; // no room for a block size or the EndMark
    }
    uint32_t raw = (uint32_t)f[p] | ((uint32_t)f[p + 1] << 8) |
        ((uint32_t)f[p + 2] << 16) | ((uint32_t)f[p + 3] << 24);
    p += 4;
    if (raw == 0) {
      break; // EndMark
    }
    if (raw & 0x80000000u) {
      sh.stored_blocks++;
    }
    size_t len = raw & 0x7FFFFFFFu;
    p += len;
    if (sh.block_checksum) {
      p += 4;
    }
    if (p > f.size()) {
      return false;
    }
    sh.blocks++;
  }

  if (sh.content_checksum) {
    p += 4;
  }
  if (p != f.size()) {
    return false; // trailing bytes, or a truncated trailer
  }
  *out = sh;
  return true;
}

// Counts LZ4 sequences whose match offset reaches back past the start of the
// block containing them -- that is, into a previous block of the same frame.
// Only a linked (non-independent) block may hold one.
//
// Walks the block's sequences per the LZ4 block format: token (literal length
// high nibble, match length low nibble), optional 255-extended lengths, the
// literals, a two-byte little-endian offset, then optional extended match
// length.  The final sequence of a block is literals with no match.
struct MatchStats {
  long matches = 0;
  long cross_block = 0;
};

bool countCrossBlockMatches(const std::vector<uint8_t> & f, MatchStats * out) {
  FrameShape sh;
  if (!walkFrame(f, &sh)) {
    return false;
  }
  MatchStats st;
  size_t p = 6 + (sh.content_size ? 8 : 0) + (sh.dict_id ? 4 : 0) + 1;
  for (;;) {
    if (p + 4 > f.size()) {
      return false;
    }
    uint32_t raw = (uint32_t)f[p] | ((uint32_t)f[p + 1] << 8) |
        ((uint32_t)f[p + 2] << 16) | ((uint32_t)f[p + 3] << 24);
    p += 4;
    if (raw == 0) {
      break;
    }
    const bool stored = (raw & 0x80000000u) != 0;
    const size_t len = raw & 0x7FFFFFFFu;
    if (p + len > f.size()) {
      return false;
    }
    if (!stored) {
      size_t b = p;
      const size_t end = p + len;
      size_t produced = 0; // decoded bytes so far within THIS block
      while (b < end) {
        const uint8_t token = f[b++];
        size_t ll = token >> 4;
        if (ll == 15) {
          uint8_t x;
          do {
            if (b >= end) {
              return false;
            }
            x = f[b++];
            ll += x;
          } while (x == 255);
        }
        if (b + ll > end) {
          return false;
        }
        b += ll;
        produced += ll;
        if (b == end) {
          break; // last sequence: literals only
        }
        if (b + 2 > end) {
          return false;
        }
        size_t offset = (size_t)f[b] | ((size_t)f[b + 1] << 8);
        b += 2;
        size_t ml = token & 0x0F;
        if (ml == 15) {
          uint8_t x;
          do {
            if (b >= end) {
              return false;
            }
            x = f[b++];
            ml += x;
          } while (x == 255);
        }
        ml += 4; // MINMATCH
        st.matches++;
        if (offset > produced) {
          st.cross_block++;
        }
        produced += ml;
      }
    }
    p += len;
    if (sh.block_checksum) {
      p += 4;
    }
  }
  *out = st;
  return true;
}

// Builds a skippable frame from the specification: LZ4 Frame Format,
// "Skippable Frames" -- a magic of 0x184D2A50 through 0x184D2A5F (the low
// nibble is the writer's to choose), a 4-byte little-endian size, and that
// many bytes of the writer's own data.  Written from the spec rather than
// taken from any implementation, and anchored by feeding the same bytes to
// liblz4.
std::vector<uint8_t> makeSkippableFrame(
    unsigned nibble, const std::vector<uint8_t> & payload) {
  const uint32_t magic = 0x184D2A50u | (nibble & 0x0Fu);
  std::vector<uint8_t> f;
  f.reserve(8 + payload.size());
  for (int i = 0; i < 4; i++) {
    f.push_back((uint8_t)((magic >> (8 * i)) & 0xFF));
  }
  const uint32_t size = (uint32_t)payload.size();
  for (int i = 0; i < 4; i++) {
    f.push_back((uint8_t)((size >> (8 * i)) & 0xFF));
  }
  f.insert(f.end(), payload.begin(), payload.end());
  return f;
}

std::vector<uint8_t> operator+(
    const std::vector<uint8_t> & a, const std::vector<uint8_t> & b) {
  std::vector<uint8_t> out(a);
  out.insert(out.end(), b.begin(), b.end());
  return out;
}

struct BlockSizeCase {
  const char * name;
  uint64_t bytes;
  unsigned bd_id;
};

const BlockSizeCase kBlockSizes[] = {
    {"64 KB", 65536u, 4},
    {"256 KB", 262144u, 5},
    {"1 MB", 1048576u, 6},
    {"4 MB", 4194304u, 7},
};

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
        // A uint64 carrying the size, not a flag: set_bool() stores the wrong
        // type and the encoder silently declines it, so this swept the
        // content-size field without ever setting it.
        if (cksum) {
          gcomp_options_set_uint64(
              opts, "lz4.content_size", (uint64_t)sh.data.size());
        }

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


//
// The real library, when it is here
//
// liblz4 is installed on many systems without its development header, so
// these bind to it at runtime instead of at build time: no new build
// dependency, and no reason to skip when the library is present.
//
// They matter more than they look.  Everything above compares our code
// against a reference written from the same specification by the same person
// who read it -- exactly the circularity that a round-trip has, one step
// removed.  Only these tests put a genuinely independent implementation on
// the other side, and one of them checks the reference itself.
//

struct RealLz4 {
  GCU_Library handle{};
  size_t (*createDctx)(void **, unsigned) = nullptr;
  size_t (*freeDctx)(void *) = nullptr;
  size_t (*decompress)(void *, void *, size_t *, const void *, size_t *,
      const void *) = nullptr;
  unsigned (*isError)(size_t) = nullptr;
  const char * (*errorName)(size_t) = nullptr;
  size_t (*compressBound)(size_t, const void *) = nullptr;
  size_t (*compressFrame)(void *, size_t, const void *, size_t,
      const void *) = nullptr;
  // Dictionary entry points, used by the dictionary tests only.
  void * (*createCDict)(const void *, size_t) = nullptr;
  void (*freeCDict)(void *) = nullptr;
  size_t (*createCctx)(void **, unsigned) = nullptr;
  size_t (*freeCctx)(void *) = nullptr;
  size_t (*beginUsingCDict)(void *, void *, size_t, const void *,
      const void *) = nullptr;
  size_t (*compressUpdate)(void *, void *, size_t, const void *, size_t,
      const void *) = nullptr;
  size_t (*compressEnd)(void *, void *, size_t, const void *) = nullptr;
  size_t (*decompressUsingDict)(void *, void *, size_t *, const void *,
      size_t *, const void *, size_t, const void *) = nullptr;

  bool ok() const { return handle && createDctx && decompress && compressFrame; }
  bool dictOk() const {
    return ok() && createCDict && createCctx && beginUsingCDict &&
        compressUpdate && compressEnd;
  }
};

const RealLz4 & realLz4() {
  static RealLz4 r = [] {
    RealLz4 out;
    // liblz4.dll is what MSYS2 and vcpkg install; a bare name is found on
    // PATH.
    static const char * kNames[] = {"liblz4.so.1", "liblz4.so", "liblz4.dll",
        "liblz4.1.dylib", "liblz4.dylib"};
    for (const char * name : kNames) {
      if (gcu_library_open(&out.handle, name) != 0) {
        out.handle = GCU_Library{};
      }
      if (out.handle) {
        break;
      }
    }
    if (!out.handle) {
      return out;
    }
    auto sym = [&](const char * n) {
      return gcu_library_symbol(out.handle, n);
    };
    out.createDctx = (size_t (*)(void **, unsigned))sym(
        "LZ4F_createDecompressionContext");
    out.freeDctx = (size_t (*)(void *))sym("LZ4F_freeDecompressionContext");
    out.decompress = (size_t (*)(void *, void *, size_t *, const void *,
        size_t *, const void *))sym("LZ4F_decompress");
    out.isError = (unsigned (*)(size_t))sym("LZ4F_isError");
    out.errorName = (const char * (*)(size_t))sym("LZ4F_getErrorName");
    out.compressBound =
        (size_t (*)(size_t, const void *))sym("LZ4F_compressFrameBound");
    out.compressFrame = (size_t (*)(void *, size_t, const void *, size_t,
        const void *))sym("LZ4F_compressFrame");
    out.createCDict = (void * (*)(const void *, size_t))sym("LZ4F_createCDict");
    out.freeCDict = (void (*)(void *))sym("LZ4F_freeCDict");
    out.createCctx =
        (size_t (*)(void **, unsigned))sym("LZ4F_createCompressionContext");
    out.freeCctx =
        (size_t (*)(void *))sym("LZ4F_freeCompressionContext");
    out.beginUsingCDict = (size_t (*)(void *, void *, size_t, const void *,
        const void *))sym("LZ4F_compressBegin_usingCDict");
    out.compressUpdate = (size_t (*)(void *, void *, size_t, const void *,
        size_t, const void *))sym("LZ4F_compressUpdate");
    out.compressEnd = (size_t (*)(void *, void *, size_t,
        const void *))sym("LZ4F_compressEnd");
    out.decompressUsingDict = (size_t (*)(void *, void *, size_t *,
        const void *, size_t *, const void *, size_t,
        const void *))sym("LZ4F_decompress_usingDict");
    return out;
  }();
  return r;
}

// Decode a frame with liblz4. Returns false and fills `err` if it refuses.
bool realLz4Decode(const std::vector<uint8_t> & frame,
    std::vector<uint8_t> * out, std::string * err) {
  const RealLz4 & lib = realLz4();
  void * ctx = nullptr;
  size_t r = lib.createDctx(&ctx, 100 /* LZ4F_VERSION */);
  if (lib.isError && lib.isError(r)) {
    *err = "context";
    return false;
  }

  out->assign(4096, 0);
  size_t produced = 0, consumed = 0;
  bool ok = true;
  while (consumed < frame.size()) {
    if (out->size() - produced < 65536) {
      out->resize(out->size() * 2 + 65536);
    }
    size_t osz = out->size() - produced;
    size_t isz = frame.size() - consumed;
    r = lib.decompress(ctx, out->data() + produced, &osz,
        frame.data() + consumed, &isz, nullptr);
    if (lib.isError && lib.isError(r)) {
      *err = lib.errorName ? lib.errorName(r) : "error";
      ok = false;
      break;
    }
    produced += osz;
    consumed += isz;
    if (r == 0 && consumed >= frame.size()) {
      break; // A frame ended and nothing follows it.
    }
    // r == 0 with input still to go means one frame of several ended -- a
    // skippable frame ahead of the data, say.  Keep feeding; LZ4F_decompress
    // starts the next frame on the following call.  Every single-frame caller
    // reaches r == 0 with the input exhausted, so this is the same for them.

    if (osz == 0 && isz == 0) {
      *err = "stalled";
      ok = false;
      break;
    }
  }
  lib.freeDctx(ctx);
  out->resize(ok ? produced : 0);
  return ok;
}

TEST_F(Lz4SpecOracleTest, RealLz4_ReadsOurFrames) {
  const RealLz4 & lib = realLz4();
  if (!lib.ok()) {
    GTEST_SKIP() << "liblz4 is not installed; the spec-based tests in this "
                    "file still cover this method unconditionally";
  }

  // Every frame option our encoder exposes, including the two that nothing
  // else here reaches: block checksums, and linked (non-independent) blocks,
  // which the reference encoder above cannot even produce.
  //
  // These are all SINGLE-block frames.  sweepSizes() stops at 140,000 bytes
  // and the default block size is 4 MB, so the "linked blocks" rows below say
  // only that the flag reaches the header -- there is no second block for a
  // linked block to link to.  Multi-block frames are
  // RealLz4_ReadsOurMultiBlockFrames, below.
  struct Opt {
    const char * name;
    int content_checksum, block_checksum, content_size, independent;
  };
  static const Opt kOpts[] = {
      {"defaults", -1, -1, -1, -1},
      {"content checksum", 1, -1, -1, -1},
      {"block checksum", -1, 1, -1, -1},
      {"content size", -1, -1, 1, -1},
      {"all three", 1, 1, 1, -1},
      {"linked blocks", -1, -1, -1, 0},
      {"linked + block checksum", -1, 1, -1, 0},
      {"independent blocks", -1, -1, -1, 1},
  };

  for (size_t n : sweepSizes()) {
    for (const Shape & sh : shapes(n)) {
      for (const Opt & o : kOpts) {
        gcomp_options_t * opts = nullptr;
        ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
        if (o.content_checksum >= 0) {
          gcomp_options_set_bool(opts, "lz4.content_checksum", o.content_checksum);
        }
        if (o.block_checksum >= 0) {
          gcomp_options_set_bool(opts, "lz4.block_checksum", o.block_checksum);
        }
        if (o.content_size >= 0) {
          gcomp_options_set_uint64(
              opts, "lz4.content_size", (uint64_t)sh.data.size());
        }
        if (o.independent >= 0) {
          gcomp_options_set_bool(opts, "lz4.independent_blocks", o.independent);
        }

        std::vector<uint8_t> framed = gcompEncode(sh.data, opts);
        gcomp_options_destroy(opts);
        ASSERT_FALSE(framed.empty())
            << sh.name << " n=" << n << " " << o.name;

        // Assert the options actually reached the frame.  Every row here
        // named a flag, and three of them were silently doing nothing.
        FrameShape fs;
        ASSERT_TRUE(walkFrame(framed, &fs)) << sh.name << " " << o.name;
        if (o.content_checksum >= 0) {
          EXPECT_EQ(fs.content_checksum, o.content_checksum != 0)
              << sh.name << " n=" << n << " " << o.name;
        }
        if (o.block_checksum >= 0) {
          EXPECT_EQ(fs.block_checksum, o.block_checksum != 0)
              << sh.name << " n=" << n << " " << o.name;
        }
        if (o.content_size >= 0 && !sh.data.empty()) {
          EXPECT_EQ(fs.content_size, o.content_size != 0)
              << sh.name << " n=" << n << " " << o.name
              << ": the content size option did not reach the frame";
        }
        if (o.independent >= 0) {
          EXPECT_EQ(fs.block_independent, o.independent != 0)
              << sh.name << " n=" << n << " " << o.name;
        }

        std::vector<uint8_t> back;
        std::string err;
        ASSERT_TRUE(realLz4Decode(framed, &back, &err))
            << sh.name << " n=" << n << " " << o.name
            << ": liblz4 refused our frame (" << err << ")";
        ASSERT_EQ(back, sh.data) << sh.name << " n=" << n << " " << o.name;
      }
    }
  }
}

TEST_F(Lz4SpecOracleTest, RealLz4_ReadsOurMultiBlockFrames) {
  const RealLz4 & lib = realLz4();
  if (!lib.ok()) {
    GTEST_SKIP() << "liblz4 is not installed";
  }

  // The cross-product runs at the two small block sizes, where it is cheap.
  // What is genuinely block-size-dependent -- the BD byte, and whether the
  // encoder splits at the size it was asked for -- is checked at all four.
  unsigned total_blocks = 0, stored_blocks = 0, checksummed_frames = 0;
  unsigned multiblock_frames = 0;

  for (const BlockSizeCase & bs : kBlockSizes) {
    const bool small = bs.bytes <= 262144u;

    std::vector<size_t> sizes;
    if (small) {
      // Straddle the boundary in both directions, then two whole blocks, then
      // a partial final block.
      sizes = {(size_t)bs.bytes - 1, (size_t)bs.bytes, (size_t)bs.bytes + 1,
          (size_t)(2 * bs.bytes), (size_t)(2 * bs.bytes + 37),
          (size_t)(3 * bs.bytes + bs.bytes / 2)};
    }
    else {
      sizes = {(size_t)bs.bytes + 1, (size_t)(2 * bs.bytes + 37)};
    }

    for (size_t n : sizes) {
      std::vector<Shape> shape_list =
          small ? shapes(n) : std::vector<Shape>{{"prose", proseLike(n, 4u)},
                    {"incompressible", incompressible(n, 11u)}};

      for (const Shape & sh : shape_list) {
        for (int independent = 0; independent <= 1; independent++) {
          for (int cksum = 0; cksum <= (small ? 1 : 0); cksum++) {
            gcomp_options_t * opts = nullptr;
            ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
            gcomp_options_set_uint64(opts, "lz4.block_size", bs.bytes);
            gcomp_options_set_bool(opts, "lz4.independent_blocks", independent);
            gcomp_options_set_bool(opts, "lz4.block_checksum", cksum || !small);

            std::vector<uint8_t> framed = gcompEncode(sh.data, opts);
            gcomp_options_destroy(opts);

            std::string where = std::string(bs.name) + " blocks, n=" +
                std::to_string(n) + ", " + sh.name + ", " +
                (independent ? "independent" : "linked") +
                (cksum || !small ? ", block checksums" : "");
            ASSERT_FALSE(framed.empty()) << where << ": encode failed";

            // The construct has to be there before its round-trip means
            // anything.
            FrameShape fs;
            ASSERT_TRUE(walkFrame(framed, &fs))
                << where << ": our own frame does not walk";
            EXPECT_EQ(fs.block_size_id, bs.bd_id)
                << where << ": BD byte does not name the block size we asked "
                            "for";
            EXPECT_EQ(fs.block_independent, independent != 0) << where;
            // A frame must hold exactly as many blocks as the block size
            // implies -- including the n = block_size - 1 case, which is one
            // block and is here to catch an off-by-one at the split.
            unsigned want = (unsigned)((n + bs.bytes - 1) / bs.bytes);
            ASSERT_EQ(fs.blocks, want)
                << where << ": expected " << want << " block(s), got "
                << fs.blocks << " -- the encoder did not split at "
                << bs.bytes;

            total_blocks += fs.blocks;
            stored_blocks += fs.stored_blocks;
            if (fs.blocks >= 2) {
              multiblock_frames++;
            }
            if (fs.block_checksum && fs.blocks >= 2) {
              checksummed_frames++;
            }

            std::vector<uint8_t> back;
            std::string err;
            ASSERT_TRUE(realLz4Decode(framed, &back, &err))
                << where << ": liblz4 refused our frame (" << err << ")";
            ASSERT_EQ(back, sh.data) << where;
          }
        }
      }
    }
  }

  // Falsifiable floors.  If a later change quietly stops splitting blocks, or
  // stops storing incompressible ones, these fail rather than the test
  // silently shrinking back to what it used to cover.
  EXPECT_GT(multiblock_frames, 50u);
  EXPECT_GT(total_blocks, 200u);
  EXPECT_GT(checksummed_frames, 20u)
      << "no multi-block frame carried per-block checksums";
  EXPECT_GT(stored_blocks, 0u)
      << "no incompressible block was stored uncompressed, so the stored-block "
         "path inside a multi-block frame went untested";
}

TEST_F(Lz4SpecOracleTest, LinkedBlocksActuallyLink) {
  const RealLz4 & lib = realLz4();
  if (!lib.ok()) {
    GTEST_SKIP() << "liblz4 is not installed";
  }

  // LZ4 Frame Format, "Blocks": with the Block Independence flag at 0 a block
  // may reference the blocks before it.  A frame is legal either way -- the
  // flag permits back-references, it does not require them -- so nothing that
  // merely round-trips can tell a linked encoder from one that sets the bit
  // and then compresses each block alone, which is what this encoder used to
  // do.  Counting the sequences is the only way to see the difference.
  static const uint64_t kSizes[] = {65536u, 262144u};
  const size_t n = 700000;

  long linked_cross_total = 0;
  bool saw_linked_gain = false;

  for (uint64_t bs : kSizes) {
    for (const Shape & sh : shapes(n)) {
      size_t sizes[2] = {0, 0};
      MatchStats stats[2];
      unsigned blocks_seen = 0;

      for (int independent = 0; independent <= 1; independent++) {
        gcomp_options_t * opts = nullptr;
        ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
        gcomp_options_set_uint64(opts, "lz4.block_size", bs);
        gcomp_options_set_bool(opts, "lz4.independent_blocks", independent);

        std::vector<uint8_t> framed = gcompEncode(sh.data, opts);
        gcomp_options_destroy(opts);

        std::string where = std::string(sh.name) + ", bs=" +
            std::to_string(bs) + ", " +
            (independent ? "independent" : "linked");
        ASSERT_FALSE(framed.empty()) << where << ": encode failed";

        FrameShape fs;
        ASSERT_TRUE(walkFrame(framed, &fs)) << where;
        ASSERT_GE(fs.blocks, 2u) << where << ": need a multi-block frame";
        ASSERT_TRUE(countCrossBlockMatches(framed, &stats[independent]))
            << where << ": could not walk the block sequences";
        if (!independent) {
          blocks_seen = fs.blocks;
        }

        // Whatever it emits still has to be a frame the real library reads.
        std::vector<uint8_t> back;
        std::string err;
        ASSERT_TRUE(realLz4Decode(framed, &back, &err))
            << where << ": liblz4 refused our frame (" << err << ")";
        ASSERT_EQ(back, sh.data) << where;

        sizes[independent] = framed.size();
      }

      std::string where =
          std::string(sh.name) + ", bs=" + std::to_string(bs);

      // An independent block that reaches outside itself is a decoder bug
      // waiting to happen, and a spec violation besides.
      EXPECT_EQ(stats[1].cross_block, 0)
          << where << ": an INDEPENDENT block referenced data before it ("
          << stats[1].cross_block << " of " << stats[1].matches
          << " matches)";

      linked_cross_total += stats[0].cross_block;

      // A floor, not just "more than none".  The window and the hash table
      // have to slide together; if the table is not rebased with the bytes,
      // its entries name the wrong positions and nearly every cross-block
      // candidate fails the 4-byte check.  That failure is invisible to a
      // round-trip -- the check means a surviving match is still correct, so
      // the frame decodes perfectly and only the ratio suffers.  Measured on
      // this data: ~34 cross-block matches per boundary when the rebase is
      // right, ~1 when it is missing.
      //
      // Only shapes that produce matches at all can be held to it; random
      // data has none to find, and a single long run needs none.
      if (stats[0].matches > 1000) {
        long boundaries = (long)blocks_seen - 1;
        EXPECT_GE(stats[0].cross_block, 5 * boundaries)
            << where << ": only " << stats[0].cross_block
            << " cross-block matches across " << boundaries
            << " block boundaries -- the hash table is probably not being "
               "rebased when the window slides";
      }

      // Linking may not pay on every shape, but it must never cost.
      EXPECT_LE(sizes[0], sizes[1])
          << where << ": linked blocks came out larger than independent ones";
      if (sizes[0] < sizes[1]) {
        saw_linked_gain = true;
      }
    }
  }

  EXPECT_GT(linked_cross_total, 0)
      << "no linked block referenced a previous block, so lz4.independent_"
         "blocks=false set the header bit and changed nothing else";
  EXPECT_TRUE(saw_linked_gain)
      << "linking never produced a smaller frame on any shape";
}

TEST_F(Lz4SpecOracleTest, LinkedBlocksSurviveAwkwardStreaming) {
  const RealLz4 & lib = realLz4();
  if (!lib.ok()) {
    GTEST_SKIP() << "liblz4 is not installed";
  }

  // The window slides as soon as a block is staged, which can be several
  // update() calls before that block finishes reaching the output.  These
  // chunk sizes force the slide to happen mid-flush.
  static const size_t kIn[] = {1, 3, 997, 65535, 65536, 65537};
  static const size_t kOut[] = {1, 7, 4096};
  const size_t n = 200000;
  std::vector<uint8_t> data = proseLike(n, 31u);

  for (size_t in_chunk : kIn) {
    for (size_t out_chunk : kOut) {
      gcomp_options_t * opts = nullptr;
      ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
      gcomp_options_set_uint64(opts, "lz4.block_size", 65536u);
      gcomp_options_set_bool(opts, "lz4.independent_blocks", 0);
      gcomp_options_set_bool(opts, "lz4.content_checksum", 1);

      gcomp_encoder_t * enc = nullptr;
      ASSERT_EQ(gcomp_encoder_create(registry_, "lz4", opts, &enc), GCOMP_OK);
      gcomp_options_destroy(opts);

      std::string where = "in=" + std::to_string(in_chunk) + " out=" +
          std::to_string(out_chunk);

      std::vector<uint8_t> out(n * 2 + 65536);
      size_t out_len = 0, in_used = 0;
      while (in_used < n) {
        size_t take = std::min(in_chunk, n - in_used);
        gcomp_buffer_t ib = {data.data() + in_used, take, 0};
        while (ib.used < ib.size) {
          size_t room = std::min(out_chunk, out.size() - out_len);
          ASSERT_GT(room, 0u) << where << ": output buffer exhausted";
          gcomp_buffer_t ob = {out.data() + out_len, room, 0};
          ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK) << where;
          out_len += ob.used;
          ASSERT_FALSE(ob.used == 0 && ib.used == 0) << where << ": stalled";
        }
        in_used += ib.used;
      }
      for (;;) {
        size_t room = std::min(out_chunk, out.size() - out_len);
        gcomp_buffer_t ob = {out.data() + out_len, room, 0};
        gcomp_status_t st = gcomp_encoder_finish(enc, &ob);
        out_len += ob.used;
        if (st == GCOMP_OK) {
          break;
        }
        ASSERT_EQ(st, GCOMP_ERR_LIMIT) << where;
        ASSERT_GT(ob.used, 0u) << where << ": finish stalled";
      }
      gcomp_encoder_destroy(enc);
      out.resize(out_len);

      FrameShape fs;
      ASSERT_TRUE(walkFrame(out, &fs)) << where;
      ASSERT_GE(fs.blocks, 2u) << where;
      EXPECT_FALSE(fs.block_independent) << where;

      std::vector<uint8_t> back;
      std::string err;
      ASSERT_TRUE(realLz4Decode(out, &back, &err))
          << where << ": liblz4 refused a frame built in " << in_chunk
          << "-byte pieces (" << err << ")";
      ASSERT_EQ(back, data) << where;
    }
  }
}

TEST_F(Lz4SpecOracleTest, SkippableFramesAreSkipped) {
  // LZ4 Frame Format, "Skippable Frames".  A decoder that rejects one cannot
  // read files other tools legitimately write, so this is an interoperability
  // requirement rather than a nicety.
  const RealLz4 & lib = realLz4();

  std::vector<uint8_t> data = proseLike(3000, 77u);
  std::vector<uint8_t> frame = gcompEncode(data);
  ASSERT_FALSE(frame.empty());

  std::vector<uint8_t> payload;
  for (int i = 0; i < 300; i++) {
    payload.push_back((uint8_t)(i * 7 + 1));
  }
  const std::vector<uint8_t> empty;

  struct Case {
    const char * name;
    bool before, after;
    unsigned nibble;
    bool empty_payload;
  };
  std::vector<Case> cases;
  // Every nibble, since the low four bits are the writer's to choose and a
  // decoder must skip the frame whatever they say.
  for (unsigned nibble = 0; nibble < 16; nibble++) {
    cases.push_back({"before", true, false, nibble, false});
    cases.push_back({"after", false, true, nibble, false});
    cases.push_back({"both", true, true, nibble, true});
  }

  for (const Case & c : cases) {
    const std::vector<uint8_t> & pl = c.empty_payload ? empty : payload;
    std::vector<uint8_t> skip = makeSkippableFrame(c.nibble, pl);

    std::vector<uint8_t> stream;
    if (c.before) {
      stream = stream + skip;
    }
    stream = stream + frame;
    if (c.after) {
      stream = stream + skip;
    }

    for (int concat = 0; concat <= 1; concat++) {
      std::string where = std::string(c.name) + " nibble=" +
          std::to_string(c.nibble) +
          (c.empty_payload ? " empty" : " 300-byte") +
          " concat=" + std::to_string(concat);
      bool ok = false;
      std::vector<uint8_t> back =
          gcompDecode(stream, data.size(), &ok, concat);
      ASSERT_TRUE(ok) << where << ": our decoder rejected a skippable frame";
      ASSERT_EQ(back, data) << where;
    }

    // Anchor the constructed frames: the real library has to agree that what
    // this test built is a skippable frame, or the test proves nothing about
    // the format.
    if (lib.ok()) {
      std::vector<uint8_t> real_back;
      std::string err;
      ASSERT_TRUE(realLz4Decode(stream, &real_back, &err))
          << c.name << " nibble=" << c.nibble
          << ": liblz4 refused the stream this test built (" << err << ")";
      ASSERT_EQ(real_back, data) << c.name << " nibble=" << c.nibble;
    }
  }

  // Several in a row, mixed sizes and nibbles, ahead of the data.
  std::vector<uint8_t> run = makeSkippableFrame(0, payload) +
      makeSkippableFrame(1, empty) + makeSkippableFrame(15, payload) + frame;
  bool ok = false;
  std::vector<uint8_t> back = gcompDecode(run, data.size(), &ok, 0);
  ASSERT_TRUE(ok) << "three skippable frames ahead of the data";
  ASSERT_EQ(back, data);

  // A stream that is nothing but a skippable frame decodes to nothing, and is
  // not an error.  It ends with the parser waiting on a magic number, which
  // is also what a truncated stream looks like -- the difference is whether a
  // frame finished.
  std::vector<uint8_t> alone = makeSkippableFrame(3, payload);
  ok = false;
  back = gcompDecode(alone, 0, &ok, 0);
  EXPECT_TRUE(ok) << "a lone skippable frame should decode to nothing";
  EXPECT_TRUE(back.empty());
}

TEST_F(Lz4SpecOracleTest, SkippableFramesSurviveByteAtATime) {
  // The magic, the size field and the payload can each be split across
  // update() calls; the zero-length payload is the awkward one, because the
  // frame ends on the same call that read its size.
  std::vector<uint8_t> data = proseLike(2000, 5u);
  std::vector<uint8_t> frame = gcompEncode(data);
  ASSERT_FALSE(frame.empty());

  std::vector<uint8_t> payload(200, 0xA5);
  std::vector<uint8_t> stream = makeSkippableFrame(2, payload) + frame +
      makeSkippableFrame(9, std::vector<uint8_t>());

  for (size_t chunk : {(size_t)1, (size_t)2, (size_t)3, (size_t)7, (size_t)8,
           (size_t)9, (size_t)207, (size_t)208}) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    gcomp_options_set_bool(opts, "lz4.concat", 1);
    gcomp_options_set_uint64(opts, "limits.max_output_bytes", 1u << 20);
    gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 1000000);

    gcomp_decoder_t * dec = nullptr;
    ASSERT_EQ(gcomp_decoder_create(registry_, "lz4", opts, &dec), GCOMP_OK);
    gcomp_options_destroy(opts);

    std::string where = "chunk=" + std::to_string(chunk);
    std::vector<uint8_t> out(data.size() + 65536);
    size_t out_len = 0, in_used = 0;
    while (in_used < stream.size()) {
      size_t take = std::min(chunk, stream.size() - in_used);
      gcomp_buffer_t ib = {stream.data() + in_used, take, 0};
      while (ib.used < ib.size) {
        gcomp_buffer_t ob = {out.data() + out_len, out.size() - out_len, 0};
        ASSERT_EQ(gcomp_decoder_update(dec, &ib, &ob), GCOMP_OK) << where;
        out_len += ob.used;
        ASSERT_FALSE(ob.used == 0 && ib.used == 0) << where << ": stalled";
      }
      in_used += ib.used;
    }
    gcomp_buffer_t ob = {out.data() + out_len, out.size() - out_len, 0};
    ASSERT_EQ(gcomp_decoder_finish(dec, &ob), GCOMP_OK)
        << where << ": a stream ending on a skippable frame was called "
                    "truncated";
    out_len += ob.used;
    gcomp_decoder_destroy(dec);

    out.resize(out_len);
    ASSERT_EQ(out, data) << where;
  }
}

TEST_F(Lz4SpecOracleTest, SkippableIsNotAWildcardForBadMagic) {
  // Only 0x184D2A50-0x184D2A5F are skippable.  Neighbouring values must stay
  // errors, or "skip what you don't recognise" quietly swallows corruption.
  std::vector<uint8_t> data = proseLike(500, 9u);
  std::vector<uint8_t> frame = gcompEncode(data);

  static const uint32_t kBad[] = {
      0x184D2A40u, 0x184D2A60u, 0x184D2A99u, 0x184D2A4Fu, 0x184D2B50u,
      0x174D2A50u, 0x184D2205u, 0x00000000u};
  for (uint32_t magic : kBad) {
    std::vector<uint8_t> stream;
    for (int i = 0; i < 4; i++) {
      stream.push_back((uint8_t)((magic >> (8 * i)) & 0xFF));
    }
    for (int i = 0; i < 4; i++) {
      stream.push_back(0); // a size field, if it were skippable
    }
    stream = stream + frame;

    bool ok = true;
    gcompDecode(stream, data.size(), &ok, 0);
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%08X", magic);
    EXPECT_FALSE(ok) << "magic " << buf << " is not skippable and must be "
                                           "rejected";
  }

  // A skippable frame whose payload is cut short is a truncated stream, not a
  // clean end -- the size field promised bytes that never arrived.
  std::vector<uint8_t> truncated =
      makeSkippableFrame(0, std::vector<uint8_t>(100, 'x'));
  truncated.resize(truncated.size() - 40);
  bool ok = true;
  gcompDecode(truncated, 0, &ok, 0);
  EXPECT_FALSE(ok) << "a truncated skippable payload must not read as a clean "
                      "end of stream";
}

TEST_F(Lz4SpecOracleTest, RealLz4_SkipsTheFramesWeWrite) {
  const RealLz4 & lib = realLz4();
  if (!lib.ok()) {
    GTEST_SKIP() << "liblz4 is not installed";
  }

  // What the writer emits has to be skippable by something that is not us.
  // liblz4 1.10 keeps LZ4F_writeSkippableFrame in its static-only header and
  // does not export it, so the comparison runs one way: its decoder over our
  // frames.  That is the direction interoperability actually needs.
  std::vector<uint8_t> data = proseLike(4000, 41u);
  std::vector<uint8_t> frame = gcompEncode(data);
  ASSERT_FALSE(frame.empty());

  std::vector<uint8_t> payload;
  for (int i = 0; i < 700; i++) {
    payload.push_back((uint8_t)(i * 17 + 5));
  }

  for (unsigned variant = 0; variant < 16; variant++) {
    for (size_t plen : {(size_t)0, (size_t)1, (size_t)700}) {
      std::vector<uint8_t> skip(GCOMP_LZ4_SKIPPABLE_OVERHEAD + plen);
      size_t written = 0;
      ASSERT_EQ(gcomp_lz4_write_skippable_frame(variant,
                    plen ? payload.data() : nullptr, plen, skip.data(),
                    skip.size(), &written),
          GCOMP_OK);
      ASSERT_EQ(written, skip.size());

      std::string where = "variant " + std::to_string(variant) + " payload " +
          std::to_string(plen);

      // Ahead of the data, behind it, and on both sides.
      std::vector<std::pair<std::string, std::vector<uint8_t>>> streams = {
          {"before", skip + frame},
          {"after", frame + skip},
          {"both", skip + frame + skip},
          {"doubled before", skip + skip + frame},
      };
      for (auto & [placement, stream] : streams) {
        std::vector<uint8_t> back;
        std::string err;
        ASSERT_TRUE(realLz4Decode(stream, &back, &err))
            << where << " " << placement
            << ": liblz4 refused a skippable frame we wrote (" << err << ")";
        ASSERT_EQ(back, data) << where << " " << placement;

        // And our own decoder agrees about the same bytes.
        bool ok = false;
        std::vector<uint8_t> ours = gcompDecode(stream, data.size(), &ok, 1);
        ASSERT_TRUE(ok) << where << " " << placement;
        ASSERT_EQ(ours, data) << where << " " << placement;
      }
    }
  }
}

TEST_F(Lz4SpecOracleTest, EmbeddedMetadataSurvivesARoundTrip) {
  // The point of the writer: carry an application's own bytes through an LZ4
  // stream that other tools still read as ordinary data.  This walks the
  // whole path -- write the metadata, compress the data, concatenate, then
  // recover both sides -- because the writer, the parser and the decoder are
  // only useful if they compose.
  const RealLz4 & lib = realLz4();

  const std::string note = "ghoti.io/compress: unit test metadata, v1";
  std::vector<uint8_t> meta(note.begin(), note.end());
  std::vector<uint8_t> data = proseLike(9000, 12u);

  std::vector<uint8_t> skip(GCOMP_LZ4_SKIPPABLE_OVERHEAD + meta.size());
  size_t written = 0;
  ASSERT_EQ(gcomp_lz4_write_skippable_frame(3, meta.data(), meta.size(),
                skip.data(), skip.size(), &written),
      GCOMP_OK);
  ASSERT_EQ(written, skip.size());

  std::vector<uint8_t> stream = skip + gcompEncode(data);

  // The data comes back, from us and from the real library.
  bool ok = false;
  std::vector<uint8_t> back = gcompDecode(stream, data.size(), &ok, 0);
  ASSERT_TRUE(ok);
  ASSERT_EQ(back, data);
  if (lib.ok()) {
    std::vector<uint8_t> real_back;
    std::string err;
    ASSERT_TRUE(realLz4Decode(stream, &real_back, &err)) << err;
    ASSERT_EQ(real_back, data);
  }

  // And the metadata comes back, which the decoder alone cannot give you --
  // it discards skippable frames, as the format requires.
  unsigned variant = 99;
  size_t offset = 0, payload_size = 0, frame_size = 0;
  ASSERT_EQ(gcomp_lz4_read_skippable_frame(stream.data(), stream.size(),
                &variant, &offset, &payload_size, &frame_size),
      GCOMP_OK);
  EXPECT_EQ(variant, 3u);
  EXPECT_EQ(payload_size, meta.size());
  EXPECT_EQ(frame_size, skip.size());
  EXPECT_EQ(
      std::string(stream.begin() + (long)offset,
          stream.begin() + (long)offset + (long)payload_size),
      note);

  // frame_size steps past it to the data frame, which is where a caller
  // walking a stream of mixed frames would carry on.
  EXPECT_EQ(gcomp_lz4_read_skippable_frame(stream.data() + frame_size,
                stream.size() - frame_size, nullptr, nullptr, nullptr,
                nullptr),
      GCOMP_ERR_CORRUPT)
      << "what follows the skippable frame is a data frame, not another "
         "skippable one";
}

//
// Dictionaries
//
// LZ4 Frame Format: a frame may be compressed against a dictionary, which the
// decoder must be given to read it.  liblz4 exports the compression side of
// this, so these build genuine dictionary-compressed frames with it and read
// them back -- there is no way to write one from the specification without
// also writing an LZ4 encoder, so the anchor is the real library throughout.
//

// LZ4F_preferences_t as of liblz4 1.8 through 1.10: a frameInfo of
// {blockSizeID, blockMode, contentChecksumFlag, frameType, contentSize,
// dictID, blockChecksumFlag}, then compressionLevel, autoFlush,
// favorDecSpeed, reserved[3].  Declared here because the layout lives in
// lz4frame.h, which is not installed alongside the shared library.  The tests
// below check the frame this produces against what was asked for, so a wrong
// layout shows up as a failed assertion rather than as silence.
struct RealLz4Prefs {
  int block_size_id;
  int block_mode; // 0 linked, 1 independent
  int content_checksum;
  int frame_type;
  unsigned long long content_size;
  unsigned dict_id;
  int block_checksum;
  int level;
  unsigned auto_flush;
  unsigned favor_dec_speed;
  unsigned reserved[3];
};

// Compresses `data` with liblz4 against `dict`.  Returns an empty vector if
// the library refuses.
std::vector<uint8_t> realLz4CompressWithDict(const std::vector<uint8_t> & data,
    const std::vector<uint8_t> & dict, int block_mode, int content_checksum,
    int block_size_id) {
  const RealLz4 & lib = realLz4();
  void * cdict = lib.createCDict(dict.data(), dict.size());
  if (!cdict) {
    return {};
  }
  void * cctx = nullptr;
  if (lib.isError && lib.isError(lib.createCctx(&cctx, 100))) {
    return {};
  }

  RealLz4Prefs prefs = {};
  prefs.block_size_id = block_size_id;
  prefs.block_mode = block_mode;
  prefs.content_checksum = content_checksum;
  prefs.auto_flush = 1;

  std::vector<uint8_t> out(data.size() + (data.size() / 2) + 65536);
  size_t p = lib.beginUsingCDict(cctx, out.data(), out.size(), cdict, &prefs);
  bool bad = lib.isError && lib.isError(p);
  if (!bad) {
    size_t r = lib.compressUpdate(cctx, out.data() + p, out.size() - p,
        data.data(), data.size(), nullptr);
    bad = lib.isError && lib.isError(r);
    if (!bad) {
      p += r;
      r = lib.compressEnd(cctx, out.data() + p, out.size() - p, nullptr);
      bad = lib.isError && lib.isError(r);
      p += bad ? 0 : r;
    }
  }
  if (lib.freeCctx) {
    lib.freeCctx(cctx);
  }
  if (lib.freeCDict) {
    lib.freeCDict(cdict);
  }
  if (bad) {
    return {};
  }
  out.resize(p);
  return out;
}

std::vector<uint8_t> decodeWithDictionary(const std::vector<uint8_t> & frame,
    const std::vector<uint8_t> * dict, size_t expected, gcomp_status_t * st_out,
    int concat = -1) {
  gcomp_options_t * opts = nullptr;
  if (gcomp_options_create(&opts) != GCOMP_OK) {
    *st_out = GCOMP_ERR_INTERNAL;
    return {};
  }
  if (dict && !dict->empty()) {
    gcomp_options_set_bytes(opts, "lz4.dictionary", dict->data(), dict->size());
  }
  if (concat >= 0) {
    gcomp_options_set_bool(opts, "lz4.concat", concat);
  }
  gcomp_options_set_uint64(opts, "limits.max_output_bytes", 64u << 20);
  gcomp_options_set_uint64(opts, "limits.max_expansion_ratio", 1000000);
  gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 256u << 20);

  std::vector<uint8_t> out(expected + 65536);
  size_t used = out.size();
  *st_out = gcomp_decode_buffer(nullptr, "lz4", opts, frame.data(),
      frame.size(), out.data(), out.size(), &used);
  gcomp_options_destroy(opts);
  if (*st_out != GCOMP_OK) {
    return {};
  }
  out.resize(used);
  return out;
}

std::vector<uint8_t> dictionaryText(size_t n, const char * phrase) {
  std::vector<uint8_t> d(n);
  size_t len = strlen(phrase);
  for (size_t i = 0; i < n; i++) {
    d[i] = (uint8_t)phrase[i % len];
  }
  return d;
}

TEST_F(Lz4SpecOracleTest, DictionaryCompressedFramesDecodeInBothBlockModes) {
  const RealLz4 & lib = realLz4();
  if (!lib.dictOk()) {
    GTEST_SKIP() << "liblz4 does not export the dictionary API here";
  }

  std::vector<uint8_t> dict =
      dictionaryText(70000, "the quick brown fox jumps over the lazy dog ");

  // A dictionary is only useful where it resembles the data, which is the
  // point of having one.
  for (size_t n : {(size_t)1, (size_t)100, (size_t)5000, (size_t)70000,
           (size_t)200000}) {
    std::vector<uint8_t> data =
        dictionaryText(n, "the quick brown fox jumps over the lazy dog ");

    for (int block_mode = 0; block_mode <= 1; block_mode++) {
      for (int bsid : {4, 5}) {
        std::vector<uint8_t> frame =
            realLz4CompressWithDict(data, dict, block_mode, 1, bsid);
        ASSERT_FALSE(frame.empty())
            << "liblz4 failed to compress with a dictionary; the "
               "LZ4F_preferences_t layout assumed by this test may be wrong";

        FrameShape fs;
        ASSERT_TRUE(walkFrame(frame, &fs)) << "n=" << n;
        // If the preferences layout were wrong these would not match what was
        // asked for, so they double as a check on the struct above.
        EXPECT_EQ(fs.block_independent, block_mode != 0)
            << "n=" << n << ": liblz4 did not honour the block mode asked for";
        EXPECT_EQ(fs.block_size_id, (unsigned)bsid) << "n=" << n;

        std::string where = "n=" + std::to_string(n) + " " +
            (block_mode ? "independent" : "linked") + " bsid=" +
            std::to_string(bsid);

        gcomp_status_t st = GCOMP_OK;
        std::vector<uint8_t> back =
            decodeWithDictionary(frame, &dict, data.size(), &st);
        ASSERT_EQ(st, GCOMP_OK)
            << where << ": our decoder refused a dictionary-compressed frame";
        ASSERT_EQ(back, data) << where;

        // Independent blocks are the interesting half: each one starts from
        // the dictionary again rather than from the block before it, which is
        // not something the specification's wording settles.
        if (block_mode == 1 && n > 65536) {
          MatchStats stats;
          ASSERT_TRUE(countCrossBlockMatches(frame, &stats)) << where;
          EXPECT_GT(stats.cross_block, 0)
              << where << ": an independent block should still be reaching "
                          "into the dictionary";
        }
      }
    }
  }
}

TEST_F(Lz4SpecOracleTest, DictionaryFramesNeedTheDictionary) {
  const RealLz4 & lib = realLz4();
  if (!lib.dictOk()) {
    GTEST_SKIP() << "liblz4 does not export the dictionary API here";
  }

  std::vector<uint8_t> dict =
      dictionaryText(70000, "the quick brown fox jumps over the lazy dog ");
  std::vector<uint8_t> data =
      dictionaryText(50000, "the quick brown fox jumps over the lazy dog ");

  // Without the dictionary the matches reach before anything the decoder
  // holds, so the frame must be refused -- not decoded to whatever happens to
  // be there.
  std::vector<uint8_t> frame = realLz4CompressWithDict(data, dict, 0, 0, 4);
  ASSERT_FALSE(frame.empty());
  gcomp_status_t st = GCOMP_OK;
  decodeWithDictionary(frame, nullptr, data.size(), &st);
  EXPECT_EQ(st, GCOMP_ERR_CORRUPT)
      << "a frame that needs a dictionary must be refused without one";

  // The tail is what matters: the match offset is two bytes, so anything
  // before the last 64 KB is unreachable and passing it changes nothing.
  std::vector<uint8_t> tail(dict.end() - 65536, dict.end());
  gcomp_status_t st_full = GCOMP_OK, st_tail = GCOMP_OK;
  std::vector<uint8_t> from_full =
      decodeWithDictionary(frame, &dict, data.size(), &st_full);
  std::vector<uint8_t> from_tail =
      decodeWithDictionary(frame, &tail, data.size(), &st_tail);
  ASSERT_EQ(st_full, GCOMP_OK);
  ASSERT_EQ(st_tail, GCOMP_OK);
  EXPECT_EQ(from_full, data);
  EXPECT_EQ(from_tail, data)
      << "the last 64 KB of the dictionary must be enough on its own";
}

TEST_F(Lz4SpecOracleTest, WrongDictionaryIsCaughtByTheContentChecksum) {
  const RealLz4 & lib = realLz4();
  if (!lib.dictOk()) {
    GTEST_SKIP() << "liblz4 does not export the dictionary API here";
  }

  std::vector<uint8_t> dict =
      dictionaryText(70000, "the quick brown fox jumps over the lazy dog ");
  std::vector<uint8_t> wrong =
      dictionaryText(70000, "pack my box with five dozen liquor jugs! ");
  std::vector<uint8_t> data =
      dictionaryText(50000, "the quick brown fox jumps over the lazy dog ");

  // This is how LZ4 works and is worth pinning down rather than discovering:
  // the wrong dictionary yields wrong bytes, and nothing in the block format
  // can tell.  The content checksum is the frame's own defence, and it is
  // why a frame meant to travel with a dictionary should carry one.
  std::vector<uint8_t> unchecked = realLz4CompressWithDict(data, dict, 0, 0, 4);
  ASSERT_FALSE(unchecked.empty());
  gcomp_status_t st = GCOMP_OK;
  std::vector<uint8_t> garbled =
      decodeWithDictionary(unchecked, &wrong, data.size(), &st);
  EXPECT_EQ(st, GCOMP_OK)
      << "without a content checksum there is nothing to detect this with";
  EXPECT_NE(garbled, data) << "the wrong dictionary must not yield the right "
                              "bytes, or this test proves nothing";

  std::vector<uint8_t> checked = realLz4CompressWithDict(data, dict, 0, 1, 4);
  ASSERT_FALSE(checked.empty());
  FrameShape fs;
  ASSERT_TRUE(walkFrame(checked, &fs));
  ASSERT_TRUE(fs.content_checksum) << "this frame was asked for a checksum";

  decodeWithDictionary(checked, &wrong, data.size(), &st);
  EXPECT_EQ(st, GCOMP_ERR_CORRUPT)
      << "the content checksum must catch a wrong dictionary";

  // And the right dictionary still passes that checksum.
  std::vector<uint8_t> good =
      decodeWithDictionary(checked, &dict, data.size(), &st);
  EXPECT_EQ(st, GCOMP_OK);
  EXPECT_EQ(good, data);
}

TEST_F(Lz4SpecOracleTest, DictionaryAppliesToEveryConcatenatedFrame) {
  const RealLz4 & lib = realLz4();
  if (!lib.dictOk()) {
    GTEST_SKIP() << "liblz4 does not export the dictionary API here";
  }

  std::vector<uint8_t> dict =
      dictionaryText(70000, "the quick brown fox jumps over the lazy dog ");

  // The first frame's content must NOT resemble the dictionary.  If it does,
  // history carried over from it holds the same bytes the dictionary would
  // have supplied, and the second frame decodes correctly whether or not the
  // history was reset -- which is exactly how an earlier version of this test
  // passed against a decoder that never reset it.
  std::vector<uint8_t> a =
      dictionaryText(9000, "PACK MY BOX WITH FIVE DOZEN LIQUOR JUGS -- ");
  std::vector<uint8_t> b =
      dictionaryText(4000, "the quick brown fox jumps over the lazy dog ");

  std::vector<uint8_t> fa = realLz4CompressWithDict(a, dict, 0, 1, 4);
  std::vector<uint8_t> fb = realLz4CompressWithDict(b, dict, 0, 1, 4);
  ASSERT_FALSE(fa.empty());
  ASSERT_FALSE(fb.empty());

  // The second frame has to actually consult the dictionary, or there is
  // nothing here to get wrong.
  MatchStats stats;
  ASSERT_TRUE(countCrossBlockMatches(fb, &stats));
  ASSERT_GT(stats.cross_block, 0)
      << "the second frame does not reach into the dictionary, so this test "
         "cannot tell whether the history was reset at the frame boundary";

  // Each frame was compressed against the dictionary, not against the frame
  // before it, so the history has to go back to the dictionary at every frame
  // boundary rather than carrying over.
  std::vector<uint8_t> stream = fa + fb;
  std::vector<uint8_t> want = a;
  want.insert(want.end(), b.begin(), b.end());

  gcomp_status_t st = GCOMP_OK;
  std::vector<uint8_t> back =
      decodeWithDictionary(stream, &dict, want.size(), &st, 1);
  ASSERT_EQ(st, GCOMP_OK)
      << "the second frame must start from the dictionary again";
  EXPECT_EQ(back, want);
}

// Decodes with liblz4, giving it the dictionary.
bool realLz4DecodeWithDict(const std::vector<uint8_t> & frame,
    const std::vector<uint8_t> & dict, std::vector<uint8_t> * out,
    std::string * err) {
  const RealLz4 & lib = realLz4();
  void * ctx = nullptr;
  if (lib.isError && lib.isError(lib.createDctx(&ctx, 100))) {
    *err = "context";
    return false;
  }
  out->assign(65536, 0);
  size_t produced = 0, consumed = 0;
  bool ok = true;
  while (consumed < frame.size()) {
    if (out->size() - produced < 65536) {
      out->resize(out->size() * 2 + 65536);
    }
    size_t osz = out->size() - produced;
    size_t isz = frame.size() - consumed;
    size_t r = lib.decompressUsingDict(ctx, out->data() + produced, &osz,
        frame.data() + consumed, &isz, dict.data(), dict.size(), nullptr);
    if (lib.isError && lib.isError(r)) {
      *err = lib.errorName ? lib.errorName(r) : "error";
      ok = false;
      break;
    }
    produced += osz;
    consumed += isz;
    if (r == 0 && consumed >= frame.size()) {
      break;
    }
    if (osz == 0 && isz == 0) {
      *err = "stalled";
      ok = false;
      break;
    }
  }
  lib.freeDctx(ctx);
  out->resize(ok ? produced : 0);
  return ok;
}

std::vector<uint8_t> encodeWithDictionary(gcomp_registry_t * registry,
    const std::vector<uint8_t> & data, const std::vector<uint8_t> * dict,
    int independent, uint64_t block_size) {
  gcomp_options_t * opts = nullptr;
  if (gcomp_options_create(&opts) != GCOMP_OK) {
    return {};
  }
  if (dict && !dict->empty()) {
    gcomp_options_set_bytes(opts, "lz4.dictionary", dict->data(), dict->size());
  }
  gcomp_options_set_bool(opts, "lz4.independent_blocks", independent);
  gcomp_options_set_uint64(opts, "lz4.block_size", block_size);
  gcomp_options_set_bool(opts, "lz4.content_checksum", 1);

  std::vector<uint8_t> out(data.size() * 2 + 65536);
  size_t used = 0;
  gcomp_status_t st = gcomp_encode_buffer(registry, "lz4", opts, data.data(),
      data.size(), out.data(), out.size(), &used);
  gcomp_options_destroy(opts);
  if (st != GCOMP_OK) {
    return {};
  }
  out.resize(used);
  return out;
}

TEST_F(Lz4SpecOracleTest, StaleHistoryDoesNotLeakAcrossFrames) {
  const RealLz4 & lib = realLz4();
  if (!lib.dictOk()) {
    GTEST_SKIP() << "liblz4 does not export the dictionary API here";
  }

  // A frame needing a dictionary, decoded without one, must be refused --
  // including part-way through a stream, where a previous frame has left a
  // full 64 KB of history behind.  Otherwise the out-of-range offsets resolve
  // into the earlier frame's output and the stream decodes to plausible
  // nonsense.
  //
  // Worth saying plainly: this does NOT cover the reseed at the frame
  // boundary in lz4_decoder.c.  Removing that reseed fails no test here, and
  // several attempts to build an input that distinguishes it did not manage
  // it -- the seed at header time and the decoder's offset range check appear
  // to cover every case reachable from the public API.  The reseed is kept
  // because it costs one call per frame and removing it on the strength of
  // "no test noticed" is how holes get made.  What this test does establish
  // is the property above, which is worth having on its own.
  std::vector<uint8_t> dict =
      dictionaryText(70000, "the quick brown fox jumps over the lazy dog ");
  // Large enough that decoding it fills the whole 64 KB history window, so
  // the offsets in the second frame fall inside what stale history could
  // wrongly satisfy rather than being rejected on range alone.
  std::vector<uint8_t> first =
      dictionaryText(200000, "PACK MY BOX WITH FIVE DOZEN LIQUOR JUGS -- ");
  std::vector<uint8_t> second =
      dictionaryText(4000, "the quick brown fox jumps over the lazy dog ");

  // First frame: ordinary dependent blocks, no dictionary, so decoding it
  // leaves its own output as history.
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_bool(opts, "lz4.independent_blocks", 0);
  gcomp_options_set_uint64(opts, "lz4.block_size", 65536u);
  std::vector<uint8_t> f1(first.size() * 2 + 65536);
  size_t used = 0;
  ASSERT_EQ(gcomp_encode_buffer(registry_, "lz4", opts, first.data(),
                first.size(), f1.data(), f1.size(), &used),
      GCOMP_OK);
  gcomp_options_destroy(opts);
  f1.resize(used);

  // Second frame: independent blocks against the dictionary, so without the
  // dictionary its offsets reach outside anything it may legally see.
  std::vector<uint8_t> f2 = realLz4CompressWithDict(second, dict, 1, 0, 4);
  ASSERT_FALSE(f2.empty());
  MatchStats stats;
  ASSERT_TRUE(countCrossBlockMatches(f2, &stats));
  ASSERT_GT(stats.cross_block, 0)
      << "the second frame does not reach outside itself, so there is nothing "
         "for stale history to wrongly satisfy";

  gcomp_status_t st = GCOMP_OK;
  std::vector<uint8_t> back = decodeWithDictionary(
      f1 + f2, nullptr, first.size() + second.size(), &st, 1);
  EXPECT_EQ(st, GCOMP_ERR_CORRUPT)
      << "a frame needing a dictionary was accepted because the frame before "
         "it had left history the decoder was willing to match against";
}

TEST_F(Lz4SpecOracleTest, AReaderCanDiscoverWhichDictionaryAStreamNeeds) {
  // The whole reason gcomp_lz4_peek_frame_info() exists.  A decoder needs the
  // dictionary before it can start, and the only place the dictionary's
  // identity appears is the frame header -- so without a way to read the
  // header first, a reader holding several dictionaries cannot tell which one
  // the stream in front of it wants.
  struct Entry {
    uint32_t id;
    const char * phrase;
  };
  static const Entry kLibrary[] = {
      {0x1001u, "the quick brown fox jumps over the lazy dog "},
      {0x1002u, "pack my box with five dozen liquor jugs! "},
      {0x1003u, "how vexingly quick daft zebras jump; "},
  };

  for (const Entry & chosen : kLibrary) {
    std::vector<uint8_t> dict = dictionaryText(70000, chosen.phrase);
    std::vector<uint8_t> data = dictionaryText(6000, chosen.phrase);

    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    gcomp_options_set_bytes(opts, "lz4.dictionary", dict.data(), dict.size());
    gcomp_options_set_uint64(opts, "lz4.dictionary_id", chosen.id);
    gcomp_options_set_bool(opts, "lz4.content_checksum", 1);
    std::vector<uint8_t> frame(data.size() * 2 + 65536);
    size_t used = 0;
    ASSERT_EQ(gcomp_encode_buffer(registry_, "lz4", opts, data.data(),
                  data.size(), frame.data(), frame.size(), &used),
        GCOMP_OK);
    gcomp_options_destroy(opts);
    frame.resize(used);

    // A reader that has never seen this stream before.  It has the header and
    // nothing else -- not even all of the first block.
    gcomp_lz4_frame_info_t info;
    ASSERT_EQ(gcomp_lz4_peek_frame_info(frame.data(),
                  std::min(frame.size(), (size_t)32), &info, nullptr),
        GCOMP_OK);
    ASSERT_TRUE(info.dict_id_present)
        << "the frame does not name its dictionary, so a reader cannot choose";
    ASSERT_EQ(info.dict_id, chosen.id);

    // Pick the dictionary the frame asked for, out of the ones held.
    const Entry * picked = nullptr;
    for (const Entry & e : kLibrary) {
      if (e.id == info.dict_id) {
        picked = &e;
      }
    }
    ASSERT_NE(picked, nullptr);
    std::vector<uint8_t> picked_dict = dictionaryText(70000, picked->phrase);

    gcomp_status_t st = GCOMP_OK;
    std::vector<uint8_t> back =
        decodeWithDictionary(frame, &picked_dict, data.size(), &st);
    ASSERT_EQ(st, GCOMP_OK);
    EXPECT_EQ(back, data);

    // And picking a different one fails, which is what makes the choice
    // meaningful rather than incidental.  The content checksum is what
    // detects it -- see WrongDictionaryIsCaughtByTheContentChecksum.
    for (const Entry & e : kLibrary) {
      if (e.id == chosen.id) {
        continue;
      }
      std::vector<uint8_t> other = dictionaryText(70000, e.phrase);
      gcomp_status_t bad_st = GCOMP_OK;
      decodeWithDictionary(frame, &other, data.size(), &bad_st);
      EXPECT_NE(bad_st, GCOMP_OK)
          << "dictionary 0x" << std::hex << e.id << " decoded a frame that "
          << "asked for 0x" << chosen.id;
    }
  }
}

TEST_F(Lz4SpecOracleTest, OurDictionaryFramesReadInRealLz4) {
  const RealLz4 & lib = realLz4();
  if (!lib.ok() || !lib.decompressUsingDict) {
    GTEST_SKIP() << "liblz4 does not export LZ4F_decompress_usingDict here";
  }

  std::vector<uint8_t> dict =
      dictionaryText(70000, "the quick brown fox jumps over the lazy dog ");

  for (size_t n : {(size_t)1, (size_t)44, (size_t)500, (size_t)5000,
           (size_t)70000, (size_t)200000}) {
    std::vector<uint8_t> data =
        dictionaryText(n, "the quick brown fox jumps over the lazy dog ");
    for (int independent = 0; independent <= 1; independent++) {
      for (uint64_t bs : {65536u, 262144u}) {
        std::vector<uint8_t> frame =
            encodeWithDictionary(registry_, data, &dict, independent, bs);
        std::string where = "n=" + std::to_string(n) + " " +
            (independent ? "independent" : "linked") + " bs=" +
            std::to_string(bs);
        ASSERT_FALSE(frame.empty()) << where << ": encode failed";

        // The real library, given the same dictionary, has to agree.
        std::vector<uint8_t> back;
        std::string err;
        ASSERT_TRUE(realLz4DecodeWithDict(frame, dict, &back, &err))
            << where << ": liblz4 refused our dictionary frame (" << err
            << ")";
        ASSERT_EQ(back, data) << where;

        // And so does ours.
        gcomp_status_t st = GCOMP_OK;
        std::vector<uint8_t> ours =
            decodeWithDictionary(frame, &dict, data.size(), &st);
        ASSERT_EQ(st, GCOMP_OK) << where;
        ASSERT_EQ(ours, data) << where;
      }
    }
  }
}

TEST_F(Lz4SpecOracleTest, TheDictionaryActuallyPays) {
  // A dictionary that is never consulted still produces a valid frame, so
  // "it round-trips" cannot tell a working dictionary from an ignored one.
  // The frame has to get smaller.
  std::vector<uint8_t> dict =
      dictionaryText(70000, "the quick brown fox jumps over the lazy dog ");

  struct Case {
    size_t n;
    int independent;
    double min_saving; // percent
  };
  // Small inputs gain most: there is nothing else to match against yet.  With
  // independent blocks the gain persists at size, because every block gets
  // the dictionary; with linked blocks the frame's own history takes over.
  static const Case kCases[] = {
      {500, 0, 25.0},
      {500, 1, 25.0},
      {5000, 0, 20.0},
      {5000, 1, 20.0},
      {70000, 1, 8.0},
      {200000, 1, 8.0},
  };

  for (const Case & c : kCases) {
    std::vector<uint8_t> data =
        dictionaryText(c.n, "the quick brown fox jumps over the lazy dog ");
    std::vector<uint8_t> plain =
        encodeWithDictionary(registry_, data, nullptr, c.independent, 65536);
    std::vector<uint8_t> with =
        encodeWithDictionary(registry_, data, &dict, c.independent, 65536);
    std::string where = "n=" + std::to_string(c.n) + " " +
        (c.independent ? "independent" : "linked");
    ASSERT_FALSE(plain.empty()) << where;
    ASSERT_FALSE(with.empty()) << where;

    double saving =
        100.0 * (double)(plain.size() - with.size()) / (double)plain.size();
    EXPECT_GE(saving, c.min_saving)
        << where << ": the dictionary saved only " << saving << "% ("
        << plain.size() << " -> " << with.size()
        << "), so it is barely being consulted";
  }

  // A dictionary with nothing in common with the data must not make things
  // worse than having none -- the encoder simply finds no matches in it.
  std::vector<uint8_t> unrelated = incompressible(70000, 99u);
  std::vector<uint8_t> data =
      dictionaryText(5000, "the quick brown fox jumps over the lazy dog ");
  std::vector<uint8_t> plain =
      encodeWithDictionary(registry_, data, nullptr, 0, 65536);
  std::vector<uint8_t> useless =
      encodeWithDictionary(registry_, data, &unrelated, 0, 65536);
  ASSERT_FALSE(useless.empty());
  EXPECT_LE(useless.size(), plain.size() + 8)
      << "an unrelated dictionary should cost nothing to speak of";
}

TEST_F(Lz4SpecOracleTest, DictionaryEncodingSurvivesResetAndStreaming) {
  const RealLz4 & lib = realLz4();
  std::vector<uint8_t> dict =
      dictionaryText(70000, "the quick brown fox jumps over the lazy dog ");
  std::vector<uint8_t> data =
      dictionaryText(150000, "the quick brown fox jumps over the lazy dog ");

  for (int independent = 0; independent <= 1; independent++) {
    gcomp_options_t * opts = nullptr;
    ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
    gcomp_options_set_bytes(opts, "lz4.dictionary", dict.data(), dict.size());
    gcomp_options_set_bool(opts, "lz4.independent_blocks", independent);
    gcomp_options_set_uint64(opts, "lz4.block_size", 65536u);
    gcomp_options_set_bool(opts, "lz4.content_checksum", 1);

    gcomp_encoder_t * enc = nullptr;
    ASSERT_EQ(gcomp_encoder_create(registry_, "lz4", opts, &enc), GCOMP_OK);
    gcomp_options_destroy(opts);

    // Encoding twice from one encoder must give the same frame both times:
    // reset has to put the window back to the dictionary, not to nothing and
    // not to whatever the last frame left behind.
    std::vector<std::vector<uint8_t>> runs;
    for (int pass = 0; pass < 2; pass++) {
      std::vector<uint8_t> out(data.size() * 2 + 65536);
      size_t out_len = 0, in_used = 0;
      // Awkward chunking, so the window slides mid-flush with a dictionary in
      // front of it.
      while (in_used < data.size()) {
        size_t take = std::min((size_t)4097, data.size() - in_used);
        gcomp_buffer_t ib = {data.data() + in_used, take, 0};
        while (ib.used < ib.size) {
          gcomp_buffer_t ob = {out.data() + out_len, out.size() - out_len, 0};
          ASSERT_EQ(gcomp_encoder_update(enc, &ib, &ob), GCOMP_OK);
          out_len += ob.used;
          ASSERT_FALSE(ob.used == 0 && ib.used == 0);
        }
        in_used += ib.used;
      }
      for (;;) {
        gcomp_buffer_t ob = {out.data() + out_len, out.size() - out_len, 0};
        gcomp_status_t st = gcomp_encoder_finish(enc, &ob);
        out_len += ob.used;
        if (st == GCOMP_OK) {
          break;
        }
        ASSERT_EQ(st, GCOMP_ERR_LIMIT);
        ASSERT_GT(ob.used, 0u);
      }
      out.resize(out_len);
      runs.push_back(out);
      if (pass == 0) {
        ASSERT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);
      }
    }
    gcomp_encoder_destroy(enc);

    std::string where = independent ? "independent" : "linked";
    EXPECT_EQ(runs[0], runs[1])
        << where
        << ": the same input through the same encoder gave different frames "
           "across a reset, so the window is not going back to the dictionary";

    for (const std::vector<uint8_t> & frame : runs) {
      gcomp_status_t st = GCOMP_OK;
      std::vector<uint8_t> back =
          decodeWithDictionary(frame, &dict, data.size(), &st);
      ASSERT_EQ(st, GCOMP_OK) << where;
      ASSERT_EQ(back, data) << where;
      if (lib.ok() && lib.decompressUsingDict) {
        std::vector<uint8_t> real_back;
        std::string err;
        ASSERT_TRUE(realLz4DecodeWithDict(frame, dict, &real_back, &err))
            << where << ": " << err;
        ASSERT_EQ(real_back, data) << where;
      }
    }
  }
}

TEST_F(Lz4SpecOracleTest, RealLz4_ReadsTheReferenceFrames) {
  // Checks the oracle, not the library.  Without this, the reference above is
  // only known to agree with the implementation it is testing -- which is the
  // same standing a round-trip has.
  const RealLz4 & lib = realLz4();
  if (!lib.ok()) {
    GTEST_SKIP() << "liblz4 is not installed";
  }

  static const char * kStyles[] = {"literals_only", "offset1_only", "first_match"};
  static const char * kFlagSets[] = {"", "C", "SC", "BC", "SBC"};

  std::vector<std::vector<uint8_t>> originals;
  std::vector<std::string> labels;
  std::vector<BatchCase> batch;
  std::vector<std::string> paths;

  for (size_t n : sweepSizes()) {
    if (n > 70000) {
      continue;
    }
    for (const Shape & sh : shapes(n)) {
      for (const char * style : kStyles) {
        for (const char * flags : kFlagSets) {
          std::string in = tempPath(".raw");
          ASSERT_TRUE(writeFile(in, sh.data));
          batch.push_back({"encode", std::string(style) + ":65536:" + flags, in,
              in + ".lz4"});
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

  int bad = 0;
  std::string detail;
  for (size_t i = 0; i < batch.size() && ran; i++) {
    if (report[i] != "ok") {
      continue;
    }
    std::vector<uint8_t> framed;
    if (!readFile(batch[i].out_path, &framed)) {
      continue;
    }
    std::vector<uint8_t> back;
    std::string err;
    if (!realLz4Decode(framed, &back, &err) || back != originals[i]) {
      bad++;
      if (detail.size() < 300) {
        detail += (detail.empty() ? "" : "; ") + labels[i] + " (" + err + ")";
      }
    }
    unlink(batch[i].out_path.c_str());
  }
  for (const std::string & p : paths) {
    unlink(p.c_str());
  }

  ASSERT_TRUE(ran) << "the reference encoder did not run";
  EXPECT_EQ(bad, 0) << bad << " of " << batch.size()
                    << " reference frames liblz4 could not read, so the "
                       "reference is not trustworthy as an oracle; "
                    << detail;
}

TEST_F(Lz4SpecOracleTest, OurDecoder_ReadsRealLz4Frames) {
  const RealLz4 & lib = realLz4();
  if (!lib.ok() || !lib.compressBound) {
    GTEST_SKIP() << "liblz4 is not installed";
  }

  for (size_t n : sweepSizes()) {
    for (const Shape & sh : shapes(n)) {
      // Default preferences: passing a preferences struct would mean pinning
      // liblz4's layout, which is not worth the fragility here -- our own
      // encoder covers the option matrix in RealLz4_ReadsOurFrames.
      size_t cap = lib.compressBound(sh.data.size(), nullptr);
      std::vector<uint8_t> framed(cap ? cap : 1);
      size_t r = lib.compressFrame(framed.data(), framed.size(),
          sh.data.empty() ? "" : (const void *)sh.data.data(), sh.data.size(),
          nullptr);
      ASSERT_FALSE(lib.isError && lib.isError(r))
          << sh.name << " n=" << n << ": liblz4 failed to compress";
      framed.resize(r);

      bool ok = false;
      std::vector<uint8_t> back = gcompDecode(framed, sh.data.size(), &ok);
      ASSERT_TRUE(ok) << sh.name << " n=" << n
                      << ": our decoder refused a genuine liblz4 frame";
      ASSERT_EQ(back, sh.data) << sh.name << " n=" << n;
    }
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
