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
 * Both directions are covered. Our encoder's output is handed to the reference
 * to decode, because a dictionary stream only we can read is one nobody else
 * can - and every mistake available on that side (priming from the wrong end of
 * the dictionary, hashing positions that run off it, hashing the tail instead
 * of the whole for DICTID) produces a stream that round-trips through us and
 * fails there.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/adler32.h>
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

/**
 * @brief Decompress with the reference, against a dictionary.
 *
 * The direction that matters for our encoder: a stream only we can read is a
 * stream nobody else can.
 */
std::vector<uint8_t> referenceDecompress(const std::vector<uint8_t> & stream,
    const std::vector<uint8_t> & dict, int wbits) {
  const std::string dict_path = tempPath(".dict");
  const std::string in_path = tempPath(".z");
  if (!writeFile(dict_path, dict) || !writeFile(in_path, stream)) {
    return {};
  }
  std::string cmd = std::string(pythonCommand()) +
      " -c \"import sys,zlib;"
      "d=open(sys.argv[1],'rb').read();"
      "z=open(sys.argv[2],'rb').read();"
      "o=zlib.decompressobj(" +
      std::to_string(wbits) +
      ",zdict=d);"
      "sys.stdout.buffer.write(o.decompress(z)+o.flush())\" " +
      dict_path + " " + in_path;
  std::vector<uint8_t> out = runCapture(cmd);
  unlink(dict_path.c_str());
  unlink(in_path.c_str());
  return out;
}

/// Encode with our library, with the dictionary supplied under @p key.
gcomp_status_t encodeWithDict(const char * method, const char * key,
    const std::vector<uint8_t> & data, const std::vector<uint8_t> * dict,
    int level, std::vector<uint8_t> * out) {
  gcomp_options_t * o = nullptr;
  EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
  EXPECT_EQ(gcomp_options_set_int64(o, "deflate.level", level), GCOMP_OK);
  if (dict) {
    EXPECT_EQ(gcomp_options_set_bytes(o, key, dict->data(), dict->size()),
        GCOMP_OK);
  }
  void * buf = nullptr;
  size_t len = 0;
  gcomp_status_t s = gcomp_encode_alloc(
      nullptr, method, o, data.data(), data.size(), &buf, &len);
  if (s == GCOMP_OK) {
    out->assign((uint8_t *)buf, (uint8_t *)buf + len);
  }
  gcomp_buffer_free(nullptr, buf);
  gcomp_options_destroy(o);
  return s;
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
// Encoding
//

/**
 * @brief Streams we write, decoded by the reference.
 *
 * The direction that matters: a dictionary stream only we can read is one
 * nobody else can. Every mistake available here - priming the window with the
 * wrong end of the dictionary, hashing positions that are not all dictionary
 * bytes, writing DICTID over the tail instead of the whole - produces a stream
 * that round-trips through us and fails here.
 */
TEST(ZlibDictionary, ReferenceDecodesWhatWeWrite) {
  struct Case {
    size_t dict_len;
    size_t data_len;
  };
  const Case cases[] = {
      {1, 500},
      {64, 64},
      {1000, 5000},
      {4096, 2000},
      {32767, 5000},
      {32768, 5000},
      {32769, 5000},
      {70000, 5000},
  };

  for (const Case & c : cases) {
    for (int level : {1, 6, 9}) {
      const std::vector<uint8_t> dict = corpus(c.dict_len, 301);
      const std::vector<uint8_t> data = corpus(c.data_len, 302);

      std::vector<uint8_t> stream;
      ASSERT_EQ(encodeWithDict("zlib", "zlib.dictionary", data, &dict, level,
                    &stream),
          GCOMP_OK)
          << "dict=" << c.dict_len << " level=" << level;

      // RFC 1950 2.2: FDICT is bit 5 of FLG and DICTID is the four bytes after
      // the header, most significant byte first.
      ASSERT_GE(stream.size(), 6u);
      EXPECT_TRUE(stream[1] & 0x20)
          << "dict=" << c.dict_len << ": FDICT not set";
      uint32_t written = ((uint32_t)stream[2] << 24) |
          ((uint32_t)stream[3] << 16) | ((uint32_t)stream[4] << 8) |
          (uint32_t)stream[5];
      EXPECT_EQ(written, gcomp_adler32(dict.data(), dict.size()))
          << "dict=" << c.dict_len
          << ": DICTID is the Adler-32 of the whole dictionary supplied";

      std::vector<uint8_t> back = referenceDecompress(stream, dict, 15);
      ASSERT_EQ(back.size(), data.size())
          << "dict=" << c.dict_len << " level=" << level
          << ": the reference could not read it back";
      EXPECT_EQ(std::memcmp(back.data(), data.data(), back.size()), 0)
          << "dict=" << c.dict_len << " level=" << level;
    }
  }
}

/// The same for raw deflate, where the caller carries the dictionary itself.
TEST(ZlibDictionary, ReferenceDecodesOurRawDeflate) {
  for (size_t dict_len : {64u, 4096u, 70000u}) {
    const std::vector<uint8_t> dict = corpus(dict_len, 401);
    const std::vector<uint8_t> data = corpus(3000, 402);

    std::vector<uint8_t> stream;
    ASSERT_EQ(encodeWithDict(
                  "deflate", "deflate.dictionary", data, &dict, 6, &stream),
        GCOMP_OK);

    std::vector<uint8_t> back = referenceDecompress(stream, dict, -15);
    ASSERT_EQ(back.size(), data.size()) << "dict=" << dict_len;
    EXPECT_EQ(std::memcmp(back.data(), data.data(), back.size()), 0)
        << "dict=" << dict_len;
  }
}

/// And we read our own back, which the sweep above does not cover.
TEST(ZlibDictionary, RoundTripsThroughOurselves) {
  const std::vector<uint8_t> dict = corpus(4096, 501);
  const std::vector<uint8_t> data = corpus(4000, 502);

  for (const char * method : {"zlib", "deflate"}) {
    const char * key =
        std::strcmp(method, "zlib") == 0 ? "zlib.dictionary" : "deflate.dictionary";
    std::vector<uint8_t> stream;
    ASSERT_EQ(encodeWithDict(method, key, data, &dict, 6, &stream), GCOMP_OK);

    std::vector<uint8_t> out;
    ASSERT_EQ(decodeWithDict(method, key, stream, &dict, &out), GCOMP_OK)
        << method;
    ASSERT_EQ(out.size(), data.size()) << method;
    EXPECT_EQ(std::memcmp(out.data(), data.data(), out.size()), 0) << method;
  }
}

/**
 * @brief The dictionary has to actually make the output smaller.
 *
 * A dictionary that is copied into the window but never indexed into the hash
 * chains still round-trips and still passes every test above. Only the size
 * tells the difference.
 */
TEST(ZlibDictionary, DictionaryMakesOutputSmaller) {
  const std::vector<uint8_t> dict = corpus(8192, 601);
  // Drawn from the same generator, so the dictionary holds the phrases.
  const std::vector<uint8_t> data = corpus(3000, 601);

  for (int level : {1, 6, 9}) {
    std::vector<uint8_t> with, without;
    ASSERT_EQ(encodeWithDict("zlib", "zlib.dictionary", data, &dict, level,
                  &with),
        GCOMP_OK);
    ASSERT_EQ(
        encodeWithDict("zlib", "zlib.dictionary", data, nullptr, level,
            &without),
        GCOMP_OK);
    EXPECT_LT(with.size(), without.size())
        << "level " << level << ": a dictionary holding this very content did "
        << "not help (" << with.size() << " against " << without.size() << ")";
  }
}

/**
 * @brief A reset starts the next stream from the same history.
 *
 * The dictionary is the stream's starting history, so a reset has to lay it
 * down again - and the encoder keeps its own copy for that reason, since the
 * caller's options need not outlive it.
 */
TEST(ZlibDictionary, ResetPrimesAgain) {
  const std::vector<uint8_t> dict = corpus(4096, 701);
  const std::vector<uint8_t> data = corpus(2500, 702);

  gcomp_options_t * o = nullptr;
  ASSERT_EQ(gcomp_options_create(&o), GCOMP_OK);
  ASSERT_EQ(gcomp_options_set_bytes(
                o, "deflate.dictionary", dict.data(), dict.size()),
      GCOMP_OK);

  gcomp_encoder_t * enc = nullptr;
  ASSERT_EQ(
      gcomp_encoder_create(gcomp_registry_default(), "deflate", o, &enc),
      GCOMP_OK);

  auto encode_one = [&](std::vector<uint8_t> * out) {
    out->clear();
    uint8_t buf[512];
    gcomp_buffer_t in = {data.data(), data.size(), 0};
    while (in.used < in.size) {
      gcomp_buffer_t ob = {buf, sizeof(buf), 0};
      ASSERT_EQ(gcomp_encoder_update(enc, &in, &ob), GCOMP_OK);
      out->insert(out->end(), buf, buf + ob.used);
    }
    for (;;) {
      gcomp_buffer_t ob = {buf, sizeof(buf), 0};
      gcomp_status_t s = gcomp_encoder_finish(enc, &ob);
      out->insert(out->end(), buf, buf + ob.used);
      if (s == GCOMP_OK) {
        break;
      }
      ASSERT_EQ(s, GCOMP_ERR_LIMIT);
    }
  };

  std::vector<uint8_t> first;
  encode_one(&first);
  ASSERT_EQ(gcomp_encoder_reset(enc), GCOMP_OK);
  std::vector<uint8_t> second;
  encode_one(&second);

  gcomp_encoder_destroy(enc);
  gcomp_options_destroy(o);

  EXPECT_EQ(first, second)
      << "the stream after a reset differs from the one before it, so the "
         "dictionary was not laid down again";

  // And the reference reads the second one, which is the part that would go
  // wrong silently if reset primed the window but not the hash chains.
  std::vector<uint8_t> back = referenceDecompress(second, dict, -15);
  ASSERT_EQ(back.size(), data.size());
  EXPECT_EQ(std::memcmp(back.data(), data.data(), back.size()), 0);
}

/**
 * @brief Without a dictionary, nothing changes.
 *
 * The priming path runs for every encoder; it must be inert when there is
 * nothing to prime.
 */
TEST(ZlibDictionary, NoDictionaryIsUnchanged) {
  const std::vector<uint8_t> data = corpus(5000, 801);
  std::vector<uint8_t> stream;
  ASSERT_EQ(encodeWithDict("zlib", "zlib.dictionary", data, nullptr, 6,
                &stream),
      GCOMP_OK);
  ASSERT_GE(stream.size(), 2u);
  EXPECT_FALSE(stream[1] & 0x20) << "FDICT set without a dictionary";

  gcomp_stream_info_t info;
  ASSERT_EQ(gcomp_peek(nullptr, "zlib", nullptr, stream.data(), stream.size(),
                &info, nullptr),
      GCOMP_OK);
  EXPECT_FALSE(info.has_dictionary);
  EXPECT_EQ(info.header_size, 2u);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
