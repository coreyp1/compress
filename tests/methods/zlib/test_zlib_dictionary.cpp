/**
 * @file test_zlib_dictionary.cpp
 *
 * Preset dictionaries, RFC 1950 section 2.2, against the reference.
 *
 * A zlib stream may be compressed against a dictionary the decompressor "must
 * be presented with" before decoding, with FDICT set in the header and DICTID
 * carrying the Adler-32 of that dictionary. RFC 1951 has no notion of one: a
 * dictionary is simply history, so decoding it means loading those bytes into
 * the window before the first block and letting distances reach back into them.
 *
 * This library could not read such a stream at all. It can now, and the only
 * way to know it reads them *correctly* is to decode streams somebody else
 * wrote: a dictionary loaded at the wrong offset, or truncated from the wrong
 * end, still produces output - just not the right output, and not in a way any
 * round-trip test of our own would notice.
 *
 * The reference is Python's zlib, which is CPython's binding to zlib itself.
 *
 * Writing such a stream is not implemented; the encoder says so rather than
 * quietly dropping the dictionary, and the last tests here hold it to that.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/deflate.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zlib.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#define popen _popen
#define pclose _pclose
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
  const char * dir = getenv("TMPDIR");
  if (!dir || !*dir) {
    dir = "/tmp";
  }
  char name[512];
  snprintf(name, sizeof(name), "%s/gcomp_zlib_dict_%d_%d%s", dir, (int)getpid(),
      counter++, suffix.c_str());
  return name;
}

bool writeFile(const std::string & path, const std::vector<uint8_t> & v) {
  FILE * f = fopen(path.c_str(), "wb");
  if (!f) {
    return false;
  }
  bool ok = v.empty() || fwrite(v.data(), 1, v.size(), f) == v.size();
  fclose(f);
  return ok;
}

/**
 * @brief Compress with the reference, against a dictionary.
 *
 * The dictionary and payload go through files rather than the command line:
 * a 32 KB dictionary is 128 KB once escaped, which is past what a command
 * line holds - and the failure mode is an empty result that looks like
 * "python is missing" rather than "the argument was too long".
 *
 * @param wbits 15 for a zlib wrapper, -15 for a raw deflate stream.
 */
std::vector<uint8_t> referenceCompress(const std::vector<uint8_t> & data,
    const std::vector<uint8_t> & dict, int level, int wbits) {
  const std::string dict_path = tempPath(".dict");
  const std::string data_path = tempPath(".raw");
  if (!writeFile(dict_path, dict) || !writeFile(data_path, data)) {
    return {};
  }

  std::string cmd = std::string(pythonCommand()) +
      " -c \"import sys,zlib;"
      "d=open(sys.argv[1],'rb').read();"
      "p=open(sys.argv[2],'rb').read();"
      "c=zlib.compressobj(" +
      std::to_string(level) + ",zlib.DEFLATED," + std::to_string(wbits) +
      ",zdict=d);"
      "sys.stdout.buffer.write(c.compress(p)+c.flush())\" " +
      dict_path + " " + data_path;
  std::vector<uint8_t> out = runCapture(cmd);
  unlink(dict_path.c_str());
  unlink(data_path.c_str());
  return out;
}

/// Deterministic content from a small vocabulary, so a dictionary can pay.
std::vector<uint8_t> corpus(size_t len, uint32_t seed) {
  static const char * words[] = {
      "alpha", "bravo", "charlie", "delta", "echo", "foxtrot"};
  std::string s;
  uint32_t st = seed;
  while (s.size() < len) {
    st = st * 1103515245u + 12345u;
    s += words[(st >> 16) % 6];
    s += ",";
  }
  s.resize(len);
  return std::vector<uint8_t>(s.begin(), s.end());
}

/// Decode with our library, with the dictionary supplied under @p key.
gcomp_status_t decodeWithDict(const char * method, const char * key,
    const std::vector<uint8_t> & stream, const std::vector<uint8_t> * dict,
    std::vector<uint8_t> * out, std::string * detail = nullptr) {
  gcomp_options_t * o = nullptr;
  EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
  if (dict) {
    EXPECT_EQ(gcomp_options_set_bytes(o, key, dict->data(), dict->size()),
        GCOMP_OK);
  }
  // A dictionary makes small payloads compress very hard, which is exactly
  // what the ratio heuristic refuses; the caller here knows what it is doing.
  EXPECT_EQ(gcomp_options_set_uint64(o, "limits.max_expansion_ratio", 0),
      GCOMP_OK);

  gcomp_decoder_t * dec = nullptr;
  // The stream API wants a registry; unlike gcomp_decode_buffer() it does not
  // fall back to the default one for a NULL.
  gcomp_status_t s =
      gcomp_decoder_create(gcomp_registry_default(), method, o, &dec);
  if (s != GCOMP_OK) {
    gcomp_options_destroy(o);
    return s;
  }

  out->clear();
  std::vector<uint8_t> buf(4096);
  gcomp_buffer_t in = {stream.data(), stream.size(), 0};
  for (;;) {
    gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
    size_t before = in.used;
    s = gcomp_decoder_update(dec, &in, &ob);
    if (s != GCOMP_OK) {
      break;
    }
    out->insert(out->end(), buf.begin(), buf.begin() + ob.used);
    if (ob.used == 0 && in.used == before) {
      break;
    }
  }
  if (s == GCOMP_OK) {
    for (;;) {
      gcomp_buffer_t ob = {buf.data(), buf.size(), 0};
      s = gcomp_decoder_finish(dec, &ob);
      out->insert(out->end(), buf.begin(), buf.begin() + ob.used);
      if (s == GCOMP_OK || s != GCOMP_ERR_LIMIT) {
        break;
      }
    }
  }
  if (detail) {
    const char * d = gcomp_decoder_get_error_detail(dec);
    *detail = d ? d : "";
  }
  gcomp_decoder_destroy(dec);
  gcomp_options_destroy(o);
  return s;
}

//
// Tests
//

TEST(ZlibDictionary, OracleIsActuallyAvailable) {
  // A skipped oracle test and an absent one look the same in the summary.
  // zlib is part of Python's standard library, so this failing means python3
  // itself is missing - worth reporting rather than quietly passing a suite
  // that compared nothing.
  const std::vector<uint8_t> dict = corpus(64, 1);
  const std::vector<uint8_t> data = corpus(64, 2);
  std::vector<uint8_t> packed = referenceCompress(data, dict, 6, 15);
  ASSERT_FALSE(packed.empty())
      << "Could not run the reference zlib implementation. This suite needs "
         "python3 (the zlib module it uses is part of the standard library). "
         "Nothing here compares against a reference without it.";
  ASSERT_GE(packed.size(), 6u);
  // RFC 1950 2.2: FDICT is bit 5 of FLG, and DICTID follows the two header
  // bytes.  If the reference did not set it, the rest of this file is testing
  // nothing.
  EXPECT_TRUE(packed[1] & 0x20) << "the reference did not set FDICT";
}

/**
 * @brief Streams the reference wrote, decoded byte for byte.
 *
 * The dictionary sizes straddle the 32 KB window on purpose: only the last
 * window_size bytes of a longer dictionary are reachable, and loading the
 * wrong end of one is the mistake that produces plausible wrong output.
 */
TEST(ZlibDictionary, DecodesReferenceStreams) {
  struct Case {
    size_t dict_len;
    size_t data_len;
  };
  const Case cases[] = {
      {1, 64},
      {64, 64},
      {256, 1000},
      {4096, 4096},
      {4096, 200},
      {32767, 5000},
      {32768, 5000},
      {32769, 5000},   // one byte past the window
      {70000, 4096},   // and well past it
  };

  for (const Case & c : cases) {
    for (int level : {1, 6, 9}) {
      const std::vector<uint8_t> dict = corpus(c.dict_len, 100);
      const std::vector<uint8_t> data = corpus(c.data_len, 200);
      std::vector<uint8_t> stream = referenceCompress(data, dict, level, 15);
      ASSERT_FALSE(stream.empty())
          << "dict=" << c.dict_len << " data=" << c.data_len;

      std::vector<uint8_t> out;
      gcomp_status_t s =
          decodeWithDict("zlib", "zlib.dictionary", stream, &dict, &out);
      ASSERT_EQ(s, GCOMP_OK)
          << "dict=" << c.dict_len << " data=" << c.data_len
          << " level=" << level;
      ASSERT_EQ(out.size(), data.size())
          << "dict=" << c.dict_len << " data=" << c.data_len;
      EXPECT_EQ(std::memcmp(out.data(), data.data(), out.size()), 0)
          << "dict=" << c.dict_len << " data=" << c.data_len
          << ": decoded the right number of bytes, but not the right ones";
    }
  }
}

/// Raw deflate with a dictionary, through `deflate.dictionary`.
TEST(ZlibDictionary, RawDeflateAcceptsADictionary) {
  const std::vector<uint8_t> dict = corpus(4096, 7);
  const std::vector<uint8_t> data = corpus(2000, 8);
  // wbits -15: a bare deflate stream, no zlib header and no DICTID, so the
  // caller is the only thing that knows a dictionary is needed.
  std::vector<uint8_t> stream = referenceCompress(data, dict, 6, -15);
  ASSERT_FALSE(stream.empty());

  std::vector<uint8_t> out;
  ASSERT_EQ(
      decodeWithDict("deflate", "deflate.dictionary", stream, &dict, &out),
      GCOMP_OK);
  ASSERT_EQ(out.size(), data.size());
  EXPECT_EQ(std::memcmp(out.data(), data.data(), out.size()), 0);
}

/**
 * @brief The wrong dictionary is refused, not decoded into wrong bytes.
 *
 * DICTID is the Adler-32 of the dictionary (RFC 1950 2.2), which exists so
 * that this can be caught.
 */
TEST(ZlibDictionary, WrongDictionaryIsRefused) {
  const std::vector<uint8_t> dict = corpus(4096, 11);
  const std::vector<uint8_t> data = corpus(3000, 12);
  std::vector<uint8_t> stream = referenceCompress(data, dict, 6, 15);
  ASSERT_FALSE(stream.empty());

  std::vector<uint8_t> wrong = dict;
  wrong[0] = (uint8_t)(wrong[0] ^ 0xFF);

  std::vector<uint8_t> out;
  std::string detail;
  EXPECT_EQ(
      decodeWithDict("zlib", "zlib.dictionary", stream, &wrong, &out, &detail),
      GCOMP_ERR_CORRUPT);
  EXPECT_NE(detail.find("0x"), std::string::npos)
      << "the message should name both identifiers: " << detail;

  // A dictionary of a different length is equally wrong.
  std::vector<uint8_t> shorter(dict.begin(), dict.end() - 1);
  EXPECT_EQ(decodeWithDict("zlib", "zlib.dictionary", stream, &shorter, &out),
      GCOMP_ERR_CORRUPT);
}

/// With no dictionary at all, say which one is wanted.
TEST(ZlibDictionary, MissingDictionaryNamesWhatIsNeeded) {
  const std::vector<uint8_t> dict = corpus(4096, 21);
  const std::vector<uint8_t> data = corpus(3000, 22);
  std::vector<uint8_t> stream = referenceCompress(data, dict, 6, 15);
  ASSERT_FALSE(stream.empty());

  std::vector<uint8_t> out;
  std::string detail;
  EXPECT_EQ(
      decodeWithDict("zlib", "zlib.dictionary", stream, nullptr, &out, &detail),
      GCOMP_ERR_UNSUPPORTED);
  EXPECT_NE(detail.find("FDICT"), std::string::npos) << detail;
  EXPECT_NE(detail.find("zlib.dictionary"), std::string::npos)
      << "the message should say what to supply: " << detail;
}

/// gcomp_peek() reports the requirement before any of this is attempted.
TEST(ZlibDictionary, PeekReportsTheRequirement) {
  const std::vector<uint8_t> dict = corpus(4096, 31);
  const std::vector<uint8_t> data = corpus(3000, 32);
  std::vector<uint8_t> stream = referenceCompress(data, dict, 6, 15);
  ASSERT_FALSE(stream.empty());

  gcomp_stream_info_t info;
  ASSERT_EQ(gcomp_peek(nullptr, "zlib", nullptr, stream.data(), stream.size(),
                &info, nullptr),
      GCOMP_OK);
  EXPECT_TRUE(info.has_dictionary);
  EXPECT_NE(info.dictionary_id, 0u);
  EXPECT_EQ(info.header_size, 6u) << "FDICT makes the header six bytes";
}

//
// The half that is not implemented
//

/**
 * @brief Encoding against a dictionary is refused, not silently ignored.
 *
 * Dropping the dictionary and encoding anyway would produce a stream that
 * decodes to the wrong bytes for anyone who supplied it - the failure would
 * land on the reader, who did nothing wrong.
 */
TEST(ZlibDictionary, EncoderRefusesRatherThanIgnoring) {
  const std::vector<uint8_t> dict = corpus(1024, 41);

  struct Case {
    const char * method;
    const char * key;
  };
  for (const Case & c :
      {Case{"zlib", "zlib.dictionary"}, Case{"deflate", "deflate.dictionary"}}) {
    gcomp_options_t * o = nullptr;
    ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
    ASSERT_EQ(
        gcomp_options_set_bytes(o, c.key, dict.data(), dict.size()), GCOMP_OK);

    gcomp_encoder_t * enc = nullptr;
    EXPECT_EQ(
        gcomp_encoder_create(gcomp_registry_default(), c.method, o, &enc),
        GCOMP_ERR_UNSUPPORTED)
        << c.method << ": a dictionary the encoder cannot use must be "
                       "refused, not ignored";
    if (enc) {
      gcomp_encoder_destroy(enc);
    }
    gcomp_options_destroy(o);
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
