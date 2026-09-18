/**
 * @file test_zstd_matchfinder.cpp
 *
 * Match finder tests for the Zstd implementation in the Ghoti.io Compress
 * library.
 *
 * WHAT IS BEING TESTED, AND WHY IT NEEDS ITS OWN FILE
 * ==================================================
 *
 * Deferred ("lazy") matching changes which matches the parse chooses.  It
 * cannot make the output wrong -- every parse of the same input decodes back
 * to that input -- so a round-trip test cannot tell a working deferral from
 * one that never defers, or from one that defers every time.  The encoder
 * tests would go on passing either way.
 *
 * So these tests reach the match finder directly and assert on the parse it
 * produces: which matches it picked, and what it left behind in its tables.
 *
 * Two failures in particular have to be catchable:
 *
 * 1. The deferral never happens (a greedy parse wearing its name).  Caught by
 *    ADeferredMatchIsTakenWhenItIsWorthMore, which feeds the finder an input
 *    built so that the greedy choice is provably the worse one.
 *
 * 2. A position is inserted into the hash chain twice, because the look-ahead
 *    search already covered it.  Its chain entry then points at itself, and
 *    the chain walk reads that as the end of the chain and throws away every
 *    older candidate behind it.  Nothing fails; the output is simply worse.
 *    Caught by NoPositionIsChainedToItself, which checks the invariant that
 *    double insertion violates.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include "../../../src/methods/zstd/zstd_internal.h"
#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

/// Bytes in the long phrase the deferred match is supposed to find.
constexpr size_t kLongPhrase = 40;

/// Bytes in the short phrase the greedy parse settles for.
constexpr size_t kShortPhrase = 5;

/// Filler between the planted phrases, so that each sits at its own offset.
constexpr size_t kFiller = 4096;

/// A repeatable byte source.  Not random enough to be cryptography, varied
/// enough that a four byte hash of one stretch does not collide with another.
class Noise {
public:
  explicit Noise(uint32_t seed) : state_(seed) {}

  uint8_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 17;
    state_ ^= state_ << 5;
    return static_cast<uint8_t>(state_ >> 19);
  }

  void append(std::vector<uint8_t> & out, size_t count) {
    for (size_t i = 0; i < count; i++) {
      out.push_back(next());
    }
  }

private:
  uint32_t state_;
};

/**
 * @brief Build an input on which the greedy parse is the wrong choice.
 *
 * The shape is this.  Let Y be a long phrase and let X be the five bytes
 * `c, Y[0], Y[1], Y[2], Y[3]` -- one lead byte followed by the start of Y.
 * Both X and Y are planted in the first part of the buffer, separated by
 * filler.  The last part of the buffer is `c` followed by the whole of Y.
 *
 * At the position of that trailing `c`, the four byte hash is the hash of
 * `c, Y[0], Y[1], Y[2]`, which finds the planted X: a match of exactly
 * kShortPhrase bytes, because what follows the planted X is filler.
 *
 * One byte later the hash is the hash of `Y[0..3]`, which finds the planted
 * Y: a match of the whole kLongPhrase.
 *
 * A greedy parse commits to the five byte match and never looks at the
 * position after it.  A parse that defers writes one literal and takes the
 * long match instead.  The two are trivially distinguishable, which is the
 * point.
 *
 * @param target_out Receives the offset of the trailing `c`.
 */
std::vector<uint8_t> build_deferral_case(size_t * target_out) {
  Noise noise(0x5eed1234u);

  std::vector<uint8_t> phrase;
  noise.append(phrase, kLongPhrase);

  const uint8_t lead = static_cast<uint8_t>(phrase[0] ^ 0xA5u);

  std::vector<uint8_t> data;
  noise.append(data, kFiller);

  // The planted X: the lead byte and the first four bytes of the phrase.
  data.push_back(lead);
  data.insert(data.end(), phrase.begin(), phrase.begin() + (kShortPhrase - 1));

  noise.append(data, kFiller);

  // The planted Y.
  data.insert(data.end(), phrase.begin(), phrase.end());

  noise.append(data, kFiller);

  // The target: the lead byte, then the whole phrase.
  *target_out = data.size();
  data.push_back(lead);
  data.insert(data.end(), phrase.begin(), phrase.end());

  noise.append(data, kFiller);
  return data;
}

/// One parse of a buffer, with the deferral depth forced to a chosen value.
struct Parse {
  std::vector<zstd_sequence_t> sequences;
  std::vector<uint8_t> literals;
  std::vector<uint32_t> chain;
  size_t chain_size = 0;
};

Parse run_parse(const std::vector<uint8_t> & data, int level,
    unsigned lazy_depth, bool force_depth) {
  const gcomp_allocator_t * alloc = gcomp_allocator_default();
  zstd_match_finder_t mf;
  EXPECT_EQ(zstd_mf_init(&mf, alloc, level, 1u << 20, nullptr), GCOMP_OK);
  if (force_depth) {
    mf.lazy_depth = lazy_depth;
  }

  Parse parse;
  parse.sequences.resize(data.size() + 1);
  parse.literals.resize(data.size() + 64);

  size_t num_sequences = 0;
  size_t literals_size = 0;
  uint32_t rep1 = ZSTD_REP_OFFSET_1_INIT;
  uint32_t rep2 = ZSTD_REP_OFFSET_2_INIT;
  uint32_t rep3 = ZSTD_REP_OFFSET_3_INIT;

  EXPECT_EQ(zstd_mf_generate_sequences(&mf, data.data(), data.size(), 0,
                parse.sequences.data(), parse.sequences.size(),
                &num_sequences, parse.literals.data(), &literals_size, &rep1,
                &rep2, &rep3),
      GCOMP_OK);

  parse.sequences.resize(num_sequences);
  parse.literals.resize(literals_size);
  parse.chain_size = mf.chain_size;
  parse.chain.assign(mf.chain_table, mf.chain_table + mf.chain_size);

  zstd_mf_destroy(&mf, alloc, nullptr);
  return parse;
}

/// The longest match any sequence in the parse settled on.
uint32_t longest_match(const Parse & parse) {
  uint32_t longest = 0;
  for (const zstd_sequence_t & seq : parse.sequences) {
    if (seq.match_length > longest) {
      longest = seq.match_length;
    }
  }
  return longest;
}

/// Every byte the parse accounts for: literals plus matched bytes.
size_t bytes_covered(const Parse & parse) {
  size_t covered = parse.literals.size();
  for (const zstd_sequence_t & seq : parse.sequences) {
    covered += seq.match_length;
  }
  return covered;
}

} // namespace

class ZstdMatchFinderTest : public ::testing::Test {};

// A deferred match is taken when it is worth more than the one in hand.
//
// This is the test that fails if the deferral is removed, is never reached,
// or is gated behind a condition that is never true.
TEST_F(ZstdMatchFinderTest, ADeferredMatchIsTakenWhenItIsWorthMore) {
  size_t target = 0;
  std::vector<uint8_t> data = build_deferral_case(&target);

  Parse greedy = run_parse(data, 9, 0, true);
  Parse deferred = run_parse(data, 9, 1, true);

  // The greedy parse commits to the short match at the target and so never
  // sees the long one.
  EXPECT_LT(longest_match(greedy), kLongPhrase)
      << "the greedy parse found the long match anyway, so this input no "
         "longer distinguishes the two and the test proves nothing";

  // The deferring parse gives up that byte and takes the long match.
  EXPECT_GE(longest_match(deferred), kLongPhrase)
      << "the deferral did not happen";
}

// Deferring costs a literal and is only done when the longer match pays for
// it, so the deferred parse must still describe every byte of the input.
TEST_F(ZstdMatchFinderTest, DeferringDoesNotLoseBytes) {
  size_t target = 0;
  std::vector<uint8_t> data = build_deferral_case(&target);

  for (unsigned depth = 0; depth <= 3; depth++) {
    Parse parse = run_parse(data, 9, depth, true);
    EXPECT_EQ(bytes_covered(parse), data.size())
        << "at deferral depth " << depth;
  }
}

// A position must be inserted into the hash chain exactly once.  Inserting it
// twice makes its chain entry name itself, which the chain walk reads as the
// end of the chain -- every older candidate behind it becomes unreachable and
// compression quietly gets worse.
TEST_F(ZstdMatchFinderTest, NoPositionIsChainedToItself) {
  size_t target = 0;
  std::vector<uint8_t> data = build_deferral_case(&target);

  for (unsigned depth = 0; depth <= 2; depth++) {
    Parse parse = run_parse(data, 9, depth, true);
    for (size_t i = 0; i < parse.chain_size; i++) {
      // Entries are stored as position+1 so that zero can mean "no entry".
      ASSERT_NE(parse.chain[i], static_cast<uint32_t>(i + 1))
          << "position " << i << " is chained to itself at deferral depth "
          << depth;
    }
  }
}

// The levels that are meant to defer do, and level 1 -- the fast one -- does
// not.  Without this, the whole feature could be switched off by a level
// table with the wrong bounds and nothing would say so.
TEST_F(ZstdMatchFinderTest, LevelOneIsGreedyAndTheRestAreNot) {
  const gcomp_allocator_t * alloc = gcomp_allocator_default();

  zstd_match_finder_t mf;
  ASSERT_EQ(zstd_mf_init(&mf, alloc, 1, 1u << 20, nullptr), GCOMP_OK);
  EXPECT_EQ(mf.lazy_depth, 0u) << "level 1 is the fast level and must not "
                                  "spend extra searches deferring";
  zstd_mf_destroy(&mf, alloc, nullptr);

  for (int level = 2; level <= 22; level++) {
    ASSERT_EQ(zstd_mf_init(&mf, alloc, level, 1u << 20, nullptr), GCOMP_OK);
    EXPECT_GE(mf.lazy_depth, 1u) << "level " << level << " does not defer";
    EXPECT_GT(mf.nice_length, 0u) << "level " << level;
    zstd_mf_destroy(&mf, alloc, nullptr);
  }
}

// The deferral is a ratio improvement, and the levels that do it should come
// out ahead of the level that does not on input built to reward it.
TEST_F(ZstdMatchFinderTest, DeferringDescribesTheInputInFewerSequences) {
  size_t target = 0;
  std::vector<uint8_t> data = build_deferral_case(&target);

  Parse greedy = run_parse(data, 9, 0, true);
  Parse deferred = run_parse(data, 9, 1, true);

  // The long match replaces the short match and the run that followed it.
  EXPECT_LT(deferred.sequences.size(), greedy.sequences.size());
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
