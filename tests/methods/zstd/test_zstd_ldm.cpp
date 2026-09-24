/**
 * @file test_zstd_ldm.cpp
 *
 * Long-distance matching (`zstd.long`), the encoder strategy in zstd_ldm.c.
 *
 * Every ratio test here is built the same way: a block of incompressible
 * bytes, a long stretch of different incompressible bytes, then the first
 * block again.  Nothing in such a file compresses at all except the repeat,
 * and the repeat is further back than MF_MAX_DISTANCE, so the ordinary match
 * finder cannot reach it.  The output size therefore says exactly one thing -
 * whether the long match was found - which is what makes these tests worth
 * having rather than merely green.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../../common/test_helpers.h"
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace {

/// MF_MAX_DISTANCE from zstd_matchfinder_private.h: what the ordinary finder
/// can reach.  A test that wants to prove LDM found something has to place
/// the repeat beyond this.
constexpr size_t kOrdinaryReach = 0x7FFFFF;

/**
 * @brief The encoder's block size, which these tests must avoid landing on.
 *
 * Every block begins with a rolling hash computed from scratch, so if the two
 * copies of a repeated stretch sit a whole number of blocks apart, their
 * block-start hashes agree no matter what the rolling update does in between.
 * A corpus built that way finds the repeat even when the rolling hash is
 * wrong, and says nothing about whether it works.
 *
 * This is not hypothetical: it is how the first version of this file passed
 * against a broken hash.  @ref farRepeat therefore forces the distance off
 * that grid, and the tests assert that it did.
 */
constexpr size_t kEncoderBlock = 128u * 1024u;

/**
 * @brief Incompressible bytes, deterministically.
 *
 * A fixed seed, so a failure is reproducible and a ratio is comparable
 * between runs.
 */
std::vector<uint8_t> noise(size_t n, uint32_t seed) {
  std::vector<uint8_t> v(n);
  std::mt19937 rng(seed);
  for (size_t i = 0; i < n; i++) {
    v[i] = static_cast<uint8_t>(rng() >> 24);
  }
  return v;
}

/**
 * @brief block | filler | block, with the two copies more than @p gap apart.
 *
 * The filler is a short pattern repeated rather than more noise.  Both make
 * the point - what matters is only that the two copies of @p block sit far
 * enough apart - but the ordinary finder disposes of a repeated pattern in a
 * few sequences, where eight megabytes of noise has to be searched position
 * by position and found wanting.  That difference was most of a minute in
 * this file alone, paid on every run of the suite, to test nothing extra.
 *
 * The block itself stays incompressible, because it is the block that has to
 * be worth finding.
 */
std::vector<uint8_t> farRepeat(size_t block_size, size_t gap) {
  const std::vector<uint8_t> block = noise(block_size, 1u);
  const std::vector<uint8_t> pattern = noise(8u * 1024u, 2u);

  std::vector<uint8_t> out;
  out.reserve(block_size * 2 + gap + pattern.size() + kEncoderBlock);
  out.insert(out.end(), block.begin(), block.end());
  while (out.size() < block_size + gap) {
    out.insert(out.end(), pattern.begin(), pattern.end());
  }
  // Push the second copy off the block grid; see kEncoderBlock.  1021 is
  // prime and smaller than any block, so no amount of filler can put the
  // distance back on a multiple.
  while (out.size() % kEncoderBlock == 0u || out.size() % kEncoderBlock < 1021u) {
    out.push_back(static_cast<uint8_t>(out.size() * 31u));
  }
  out.insert(out.end(), block.begin(), block.end());
  return out;
}

/// The distance between the two copies @ref farRepeat placed.
size_t repeatDistance(size_t input_size, size_t block_size) {
  return input_size - block_size;
}

struct EncodeResult {
  gcomp_status_t status;
  std::vector<uint8_t> bytes;
};

EncodeResult encode(const std::vector<uint8_t> & input, int level,
    uint64_t window_log, bool use_long, uint64_t threads = 0,
    const std::string & extra_key = "", uint64_t extra_value = 0) {
  gcomp_options_t * opts = nullptr;
  EXPECT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_int64(opts, "zstd.level", level);
  gcomp_options_set_uint64(opts, "zstd.window_log", window_log);
  gcomp_options_set_bool(opts, "zstd.long", use_long);
  // The window these tests ask for is larger than the default ceiling.
  gcomp_options_set_uint64(
      opts, "limits.max_memory_bytes", 4096ull * 1024u * 1024u);
  if (threads > 0) {
    gcomp_options_set_uint64(opts, "threads.count", threads);
  }
  if (!extra_key.empty()) {
    gcomp_options_set_uint64(opts, extra_key.c_str(), extra_value);
  }

  void * out = nullptr;
  size_t out_len = 0;
  gcomp_status_t s = gcomp_encode_alloc(
      nullptr, "zstd", opts, input.data(), input.size(), &out, &out_len);
  gcomp_options_destroy(opts);

  EncodeResult r;
  r.status = s;
  if (s == GCOMP_OK) {
    r.bytes.assign(static_cast<uint8_t *>(out),
        static_cast<uint8_t *>(out) + out_len);
    gcomp_buffer_free(nullptr, out);
  }
  return r;
}

/// Decode, and say so loudly if it does not round-trip.
::testing::AssertionResult decodesTo(
    const std::vector<uint8_t> & stream, const std::vector<uint8_t> & want) {
  gcomp_options_t * opts = nullptr;
  if (gcomp_options_create(&opts) != GCOMP_OK) {
    return ::testing::AssertionFailure() << "options";
  }
  gcomp_options_set_uint64(
      opts, "limits.max_output_bytes", 512ull * 1024u * 1024u);
  void * out = nullptr;
  size_t out_len = 0;
  gcomp_status_t s = gcomp_decode_alloc(
      nullptr, "zstd", opts, stream.data(), stream.size(), &out, &out_len);
  gcomp_options_destroy(opts);
  if (s != GCOMP_OK) {
    return ::testing::AssertionFailure()
        << "decode: " << gcomp_status_to_string(s);
  }
  // An empty vector's data() may be null, and memcmp is not allowed a null
  // pointer even for a length of zero.  UBSan says so; the zero-byte case in
  // HandlesInputShorterThanTheMinimumMatch is how it was found.
  bool same = out_len == want.size() &&
      (want.empty() || memcmp(out, want.data(), want.size()) == 0);
  size_t got = out_len;
  gcomp_buffer_free(nullptr, out);
  if (!same) {
    return ::testing::AssertionFailure()
        << "decoded " << got << " bytes, wanted " << want.size();
  }
  return ::testing::AssertionSuccess();
}

} // namespace

/**
 * The central claim.  Without `zstd.long` the second copy is new data; with
 * it, it costs almost nothing.
 */
TEST(ZstdLdm, FindsARepeatTheOrdinaryFinderCannotReach) {
  const size_t block = 256u * 1024u;
  const std::vector<uint8_t> input = farRepeat(block, kOrdinaryReach + block);
  ASSERT_GT(repeatDistance(input.size(), block), kOrdinaryReach)
      << "the repeat must be beyond what the ordinary finder reaches";
  ASSERT_NE(repeatDistance(input.size(), block) % kEncoderBlock, 0u)
      << "a whole number of blocks apart would pass on a broken rolling hash";

  EncodeResult without = encode(input, 9, 25, false);
  ASSERT_EQ(without.status, GCOMP_OK);
  EncodeResult with = encode(input, 9, 25, true);
  ASSERT_EQ(with.status, GCOMP_OK);

  EXPECT_TRUE(decodesTo(with.bytes, input));
  EXPECT_TRUE(decodesTo(without.bytes, input));

  // The whole of the repeated block, less the bytes a sequence costs.
  EXPECT_LT(with.bytes.size() + block / 2u, without.bytes.size())
      << "with long: " << with.bytes.size()
      << ", without: " << without.bytes.size();
}

/// The same, through the shortest-path parse, which is a separate code path.
TEST(ZstdLdm, WorksAtTheLevelsThatParseOptimally) {
  const size_t block = 256u * 1024u;
  const std::vector<uint8_t> input = farRepeat(block, kOrdinaryReach + block);
  ASSERT_NE(repeatDistance(input.size(), block) % kEncoderBlock, 0u);

  EncodeResult without = encode(input, 17, 25, false);
  ASSERT_EQ(without.status, GCOMP_OK);
  EncodeResult with = encode(input, 17, 25, true);
  ASSERT_EQ(with.status, GCOMP_OK);

  EXPECT_TRUE(decodesTo(with.bytes, input));
  EXPECT_LT(with.bytes.size() + block / 2u, without.bytes.size())
      << "with long: " << with.bytes.size()
      << ", without: " << without.bytes.size();
}

/**
 * Off by default.
 *
 * Asked for explicitly or not at all, the bytes must be the same: a strategy
 * that changed the output of every caller who never asked for it would be a
 * silent format change for them.
 */
TEST(ZstdLdm, IsOffUnlessAskedFor) {
  const std::vector<uint8_t> input = noise(256u * 1024u, 7u);

  gcomp_options_t * bare = nullptr;
  ASSERT_EQ(gcomp_options_create(&bare), GCOMP_OK);
  gcomp_options_set_int64(bare, "zstd.level", 9);
  void * out = nullptr;
  size_t out_len = 0;
  ASSERT_EQ(gcomp_encode_alloc(nullptr, "zstd", bare, input.data(),
                input.size(), &out, &out_len),
      GCOMP_OK);
  std::vector<uint8_t> untouched(static_cast<uint8_t *>(out),
      static_cast<uint8_t *>(out) + out_len);
  gcomp_buffer_free(nullptr, out);
  gcomp_options_destroy(bare);

  EncodeResult explicit_off = encode(input, 9, 0, false);
  ASSERT_EQ(explicit_off.status, GCOMP_OK);
  EXPECT_EQ(explicit_off.bytes, untouched);
}

/**
 * A window that slides.
 *
 * The index names absolute positions, so an entry from before the window is
 * behind the base and must be rejected rather than read: reading it would
 * either point outside the buffer or produce an offset past the window the
 * frame header declares, which RFC 8878 section 3.1.1.1.2 forbids.  The only
 * way to be sure that check works is to make the window move.
 */
TEST(ZstdLdm, SurvivesTheWindowSliding) {
  // 20 MB through a 16 MB window: the front of the stream is pushed out.
  const size_t block = 256u * 1024u;
  const std::vector<uint8_t> input = farRepeat(block, 20u * 1024u * 1024u);
  EncodeResult r = encode(input, 9, 24, true);
  ASSERT_EQ(r.status, GCOMP_OK);
  EXPECT_TRUE(decodesTo(r.bytes, input));
}

/// Nothing to match, and nothing to crash on either.
TEST(ZstdLdm, HandlesInputShorterThanTheMinimumMatch) {
  for (size_t n : {size_t{0}, size_t{1}, size_t{63}, size_t{64}, size_t{65}}) {
    const std::vector<uint8_t> input = noise(n, 11u);
    EncodeResult r = encode(input, 9, 20, true);
    ASSERT_EQ(r.status, GCOMP_OK) << "n=" << n;
    EXPECT_TRUE(decodesTo(r.bytes, input)) << "n=" << n;
  }
}

/// A shorter minimum finds more, a longer one finds less; both must decode.
TEST(ZstdLdm, HonoursTheMinimumMatchOption) {
  const size_t block = 256u * 1024u;
  const std::vector<uint8_t> input = farRepeat(block, kOrdinaryReach + block);

  EncodeResult small =
      encode(input, 9, 25, true, 0, "zstd.ldm_min_match", 32);
  EncodeResult large =
      encode(input, 9, 25, true, 0, "zstd.ldm_min_match", 1024);
  ASSERT_EQ(small.status, GCOMP_OK);
  ASSERT_EQ(large.status, GCOMP_OK);
  EXPECT_TRUE(decodesTo(small.bytes, input));
  EXPECT_TRUE(decodesTo(large.bytes, input));

  // Both minimums are far below the size of the repeat, so both must find it:
  // measured against the same encode with the strategy off, not against the
  // input, which the filler alone would satisfy.
  EncodeResult without = encode(input, 9, 25, false);
  ASSERT_EQ(without.status, GCOMP_OK);
  EXPECT_LT(small.bytes.size() + block / 2u, without.bytes.size());
  EXPECT_LT(large.bytes.size() + block / 2u, without.bytes.size());
}

/// A coarser or finer sampling rate changes the cost, never the correctness.
TEST(ZstdLdm, HonoursTheHashRateOption) {
  const size_t block = 256u * 1024u;
  const std::vector<uint8_t> input = farRepeat(block, kOrdinaryReach + block);
  EncodeResult without = encode(input, 9, 25, false);
  ASSERT_EQ(without.status, GCOMP_OK);
  for (uint64_t rate : {uint64_t{0}, uint64_t{6}, uint64_t{12}}) {
    EncodeResult r =
        encode(input, 9, 25, true, 0, "zstd.ldm_hash_rate_log", rate);
    ASSERT_EQ(r.status, GCOMP_OK) << "rate=" << rate;
    EXPECT_TRUE(decodesTo(r.bytes, input)) << "rate=" << rate;
    EXPECT_LT(r.bytes.size() + block / 2u, without.bytes.size())
        << "rate=" << rate;
  }
}

/// An explicit table size is accepted and still finds the match.
TEST(ZstdLdm, HonoursAnExplicitHashLog) {
  const size_t block = 256u * 1024u;
  const std::vector<uint8_t> input = farRepeat(block, kOrdinaryReach + block);
  EncodeResult r = encode(input, 9, 25, true, 0, "zstd.ldm_hash_log", 20);
  ASSERT_EQ(r.status, GCOMP_OK);
  EXPECT_TRUE(decodesTo(r.bytes, input));
  EncodeResult without = encode(input, 9, 25, false);
  ASSERT_EQ(without.status, GCOMP_OK);
  EXPECT_LT(r.bytes.size() + block / 2u, without.bytes.size());
}

/**
 * Refused rather than ignored, with more than one thread.
 *
 * This used to be RefusesToRunWithMoreThanOneThread, on the stated grounds that
 * "a parallel job compresses its block against its own window, so it cannot see
 * tens of megabytes back". That was a misreading of the encoder. A job is
 * seeded with a **whole window** of the preceding stream - zstd_parallel.c
 * sizes the overlap from window_log and deliberately does not cap it at the job
 * size - and a window is as far as a long match may reach in any case, because
 * RFC 8878 section 3.1.1.1.2 forbids an offset past the declared window. So a
 * job's reach is the single-threaded encoder's reach.
 *
 * **The ratio is the assertion, not the round trip.** What was actually missing
 * was that the overlap went into the match finder's hash tables and not into
 * the long-distance table, so every job's long scan started from an empty table
 * and could only match inside its own content. That produces a perfectly valid
 * stream containing no long matches - so a test that encoded, decoded and
 * compared would have passed throughout, and did: the first version of this
 * change was measured only by round trip and looked finished.
 */
TEST(ZstdLdm, WorksWithMoreThanOneThread) {
  const size_t block = 256u * 1024u;
  const std::vector<uint8_t> input = farRepeat(block, kOrdinaryReach + block);
  ASSERT_GT(repeatDistance(input.size(), block), kOrdinaryReach)
      << "the repeat must be beyond what the ordinary finder reaches, or "
         "threading is not what is being measured";

  // The single-threaded figures are the yardstick: one with the long scan and
  // one without, so "it worked" is bounded on both sides rather than compared
  // against a number written down here.
  const EncodeResult serial_off = encode(input, 9, 25, false);
  const EncodeResult serial_on = encode(input, 9, 25, true);
  ASSERT_EQ(serial_off.status, GCOMP_OK);
  ASSERT_EQ(serial_on.status, GCOMP_OK);
  ASSERT_LT(serial_on.bytes.size() + block / 2u, serial_off.bytes.size())
      << "the serial control did not find the repeat, so this test cannot "
         "tell whether the threaded runs did";

  for (uint64_t threads : {(uint64_t)2, (uint64_t)4, (uint64_t)8}) {
    const EncodeResult off = encode(input, 9, 25, false, threads);
    const EncodeResult on = encode(input, 9, 25, true, threads);
    ASSERT_EQ(off.status, GCOMP_OK) << "threads " << threads;
    ASSERT_EQ(on.status, GCOMP_OK) << "threads " << threads;

    EXPECT_TRUE(decodesTo(on.bytes, input)) << "threads " << threads;
    EXPECT_TRUE(decodesTo(off.bytes, input)) << "threads " << threads;

    // The long scan found the repeat: most of a 256 KB block, less what a
    // sequence costs to name it.
    EXPECT_LT(on.bytes.size() + block / 2u, off.bytes.size())
        << "threads " << threads << ": with long " << on.bytes.size()
        << ", without " << off.bytes.size()
        << " - the long scan found nothing, which is what an empty "
           "long-distance table looks like";

    // And it found about as much of it as the serial encoder did. Within 1%,
    // which is slack for the seams between jobs and not for a missed match.
    const size_t slack = serial_on.bytes.size() / 100u;
    EXPECT_LT(on.bytes.size(), serial_on.bytes.size() + slack)
        << "threads " << threads << ": " << on.bytes.size()
        << " against the serial encoder's " << serial_on.bytes.size();
  }
}

/**
 * @brief The job size must not decide whether the long scan works.
 *
 * A job's history is the overlap, which is a whole window and is not capped by
 * the job size. If that ever changed - if the overlap were trimmed to the job,
 * say - small jobs would lose their reach while large ones kept it, and nothing
 * else here would notice.
 */
TEST(ZstdLdm, TheJobSizeDoesNotDecideTheReach) {
  const size_t block = 256u * 1024u;
  const std::vector<uint8_t> input = farRepeat(block, kOrdinaryReach + block);

  const EncodeResult serial_off = encode(input, 9, 25, false);
  ASSERT_EQ(serial_off.status, GCOMP_OK);

  for (uint64_t job : {(uint64_t)(1u << 18), (uint64_t)(1u << 20),
           (uint64_t)(1u << 22)}) {
    const EncodeResult r =
        encode(input, 9, 25, true, 4, "zstd.job_size", job);
    ASSERT_EQ(r.status, GCOMP_OK) << "job_size " << job;
    EXPECT_TRUE(decodesTo(r.bytes, input)) << "job_size " << job;
    EXPECT_LT(r.bytes.size() + block / 2u, serial_off.bytes.size())
        << "job_size " << job << ": " << r.bytes.size()
        << " against " << serial_off.bytes.size() << " with no long scan";
  }
}

/// One thread is the normal case and is not refused.
TEST(ZstdLdm, OneThreadIsAccepted) {
  const std::vector<uint8_t> input = noise(256u * 1024u, 3u);
  EncodeResult r = encode(input, 9, 24, true, 1);
  EXPECT_EQ(r.status, GCOMP_OK);
  EXPECT_TRUE(decodesTo(r.bytes, input));
}

/**
 * Out-of-range values are refused when the encoder is built.
 *
 * Not when the option is set: the options object holds what the caller put
 * there and the method that will use it decides what is legal, which is how
 * zstd.window_log is handled too.
 */
TEST(ZstdLdm, RefusesOptionValuesOutOfRange) {
  const std::vector<uint8_t> input = noise(4096u, 13u);
  struct Bad {
    const char * key;
    uint64_t value;
  };
  const Bad bad[] = {
      {"zstd.ldm_min_match", 4},
      {"zstd.ldm_min_match", 1u << 20},
      {"zstd.ldm_hash_log", 3},
      {"zstd.ldm_hash_log", 40},
      {"zstd.ldm_hash_rate_log", 40},
  };
  for (const Bad & b : bad) {
    EncodeResult r = encode(input, 9, 20, true, 0, b.key, b.value);
    EXPECT_EQ(r.status, GCOMP_ERR_INVALID_ARG) << b.key << "=" << b.value;
  }
  // And the edges of each range are accepted.
  EXPECT_EQ(encode(input, 9, 20, true, 0, "zstd.ldm_min_match", 16).status,
      GCOMP_OK);
  EXPECT_EQ(encode(input, 9, 20, true, 0, "zstd.ldm_hash_log", 0).status,
      GCOMP_OK);
  EXPECT_EQ(encode(input, 9, 20, true, 0, "zstd.ldm_hash_rate_log", 0).status,
      GCOMP_OK);
}

/**
 * The index is charged for.
 *
 * A window log the caller chose can ask for a table of gigabytes, and a limit
 * that is not told about it has not limited anything.
 */
TEST(ZstdLdm, IsChargedAgainstTheMemoryLimit) {
  const std::vector<uint8_t> input = noise(64u * 1024u, 5u);

  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_int64(opts, "zstd.level", 9);
  gcomp_options_set_uint64(opts, "zstd.window_log", 27);
  gcomp_options_set_bool(opts, "zstd.long", true);
  // Enough for the match finder at this window, not enough for it plus a
  // long-distance table sized for 128 MB.
  gcomp_options_set_uint64(opts, "limits.max_memory_bytes", 8ull * 1024u * 1024u);

  void * out = nullptr;
  size_t out_len = 0;
  gcomp_status_t s = gcomp_encode_alloc(
      nullptr, "zstd", opts, input.data(), input.size(), &out, &out_len);
  gcomp_options_destroy(opts);
  if (s == GCOMP_OK) {
    gcomp_buffer_free(nullptr, out);
  }
  EXPECT_EQ(s, GCOMP_ERR_LIMIT)
      << "a table for a 128 MB window went unnoticed by an 8 MB limit";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
