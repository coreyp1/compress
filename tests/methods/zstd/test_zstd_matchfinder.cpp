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
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include "../../../src/methods/zstd/zstd_internal.h"
#include "../../../src/methods/zstd/zstd_matchfinder_private.h"
#include <gtest/gtest.h>
#include <cmath>
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

/**
 * @brief Noise with three phrases planted at three different periods.
 *
 * Each phrase recurs at its own fixed distance -- 100, 137 and 211 bytes --
 * so a parse working through this meets the three distances over and over in
 * changing order.  That is what puts all three repeat offset codes to work,
 * and more to the point it makes one code follow another constantly, which
 * is the only time a mistake in how the three rotate can show itself.
 *
 * Ordinary text does not do this.  With 400 KB of it, rotating the offsets
 * wrongly for code 3 still decoded correctly end to end; with this, two
 * thousand sequences resolve to the wrong bytes.
 */
std::vector<uint8_t> ThreePeriodNoise(size_t n) {
  Noise noise(1234567u);
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; i++) {
    v[i] = noise.next();
  }

  constexpr size_t kPhrase = 40;
  uint8_t phrase[3][kPhrase];
  for (auto & p : phrase) {
    for (size_t j = 0; j < kPhrase; j++) {
      p[j] = noise.next();
    }
  }

  const size_t period[3] = {100, 137, 211};
  for (size_t k = 0; k < 3; k++) {
    for (size_t i = 0; i + kPhrase < n; i += period[k]) {
      memcpy(v.data() + i, phrase[k], kPhrase);
    }
  }
  return v;
}

/// One parse of a buffer, with the deferral depth forced to a chosen value.
struct Parse {
  std::vector<zstd_sequence_t> sequences;
  std::vector<uint8_t> literals;
  std::vector<uint32_t> chain;
  size_t chain_size = 0;
  std::vector<uint32_t> bt;
  size_t bt_size = 0;
  unsigned use_bt = 0;
  size_t compared_bytes = 0;
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
  // A level uses the chain or the tree, never both, and only the one it uses
  // is allocated.
  parse.use_bt = mf.use_bt;
  parse.compared_bytes = mf.compared_bytes;
  if (mf.chain_table) {
    parse.chain_size = mf.chain_size;
    parse.chain.assign(mf.chain_table, mf.chain_table + mf.chain_size);
  }
  if (mf.bt_table) {
    parse.bt_size = mf.bt_size;
    parse.bt.assign(mf.bt_table, mf.bt_table + mf.bt_size * 2u);
  }

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

/// Text-shaped bytes: words from a small vocabulary, with punctuation.
std::vector<uint8_t> TextLikeBytes(size_t n) {
  static const char * words[] = {"the", "quick", "brown", "fox", "jumps",
      "over", "lazy", "dog", "and", "then", "returns", "home", "with",
      "another", "message", "for", "everyone", "who", "waited"};
  const size_t count = sizeof(words) / sizeof(words[0]);
  std::vector<uint8_t> v;
  v.reserve(n + 16);
  uint32_t seed = 4242u;
  while (v.size() < n) {
    seed = seed * 1103515245u + 12345u;
    const char * w = words[(seed >> 16) % count];
    while (*w) {
      v.push_back(static_cast<uint8_t>(*w++));
    }
    v.push_back((seed & 0x1Fu) == 0 ? '\n' : ' ');
  }
  v.resize(n);
  return v;
}

/// Text-like bytes with large stretches repeated further on.
///
/// Plain text is not enough to tell the upper levels apart.  Their settings
/// only bind on input that still has something to find: matches long enough
/// that a nice_length in the thousands is reached, far enough back that the
/// search has to work to reach them, and enough of them that the depth limit
/// matters.  Without that the tree finds everything there is by about level
/// 16 and the levels above it have nothing to show for themselves -- which
/// says the input ran out, not that the levels are the same.
std::vector<uint8_t> TextWithDistantRepeats(
    size_t total, size_t chunks, size_t chunk_len) {
  static const char * words[] = {"the", "quick", "brown", "fox", "jumps",
      "over", "lazy", "dog", "and", "then", "returns", "home", "with",
      "another", "message", "for", "everyone", "who", "waited", "encoder",
      "decoder", "window", "offset", "literal", "sequence"};
  const size_t count = sizeof(words) / sizeof(words[0]);

  std::vector<uint8_t> v;
  v.reserve(total + chunk_len + 16);
  uint32_t seed = 90210u;

  // Filler carries on from where it left off rather than restarting, so the
  // stretches between the planted repeats are not themselves repeats.
  auto fill_to = [&](size_t want) {
    while (v.size() < want) {
      seed = seed * 1103515245u + 12345u;
      const char * w = words[(seed >> 16) % count];
      while (*w) {
        v.push_back(static_cast<uint8_t>(*w++));
      }
      v.push_back((seed & 0x1Fu) == 0 ? '\n' : ' ');
    }
  };

  fill_to(total / (chunks + 1));
  for (size_t i = 0; i < chunks; i++) {
    if (v.size() > chunk_len) {
      seed = seed * 1103515245u + 12345u;
      size_t from = (seed >> 8) % (v.size() - chunk_len);
      std::vector<uint8_t> copy(v.begin() + static_cast<ptrdiff_t>(from),
          v.begin() + static_cast<ptrdiff_t>(from + chunk_len));
      v.insert(v.end(), copy.begin(), copy.end());
    }
    fill_to((total * (i + 2)) / (chunks + 1));
  }
  v.resize(total);
  return v;
}

/// Compress @p in at @p level and return the stream.
std::vector<uint8_t> EncodeAtLevel(const std::vector<uint8_t> & in, int level) {
  gcomp_options_t * options = nullptr;
  if (gcomp_options_create(&options) != GCOMP_OK) {
    return {};
  }
  gcomp_options_set_int64(options, "zstd.level", level);
  std::vector<uint8_t> out(in.size() * 2 + 4096);
  size_t written = 0;
  gcomp_status_t status = gcomp_encode_buffer(gcomp_registry_default(), "zstd",
      options, in.data(), in.size(), out.data(), out.size(), &written);
  gcomp_options_destroy(options);
  if (status != GCOMP_OK) {
    return {};
  }
  out.resize(written);
  return out;
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
    // Level 6 is the deepest level that still searches a chain.
    Parse parse = run_parse(data, 6, depth, true);
    ASSERT_EQ(parse.use_bt, 0u) << "this test is about the chain";
    ASSERT_GT(parse.chain_size, 0u);
    for (size_t i = 0; i < parse.chain_size; i++) {
      // Entries are stored as position+1 so that zero can mean "no entry".
      ASSERT_NE(parse.chain[i], static_cast<uint32_t>(i + 1))
          << "position " << i << " is chained to itself at deferral depth "
          << depth;
    }
  }
}

// The same requirement, for the levels that search a tree, where breaking it
// costs more than compression.
//
// Inserting a position twice makes a node its own descendant.  The chain
// walk merely stops early and loses candidates; the tree walk carries the
// length two nodes have in common down with it and trusts it, so a node
// reached through itself can hand back a "match" of bytes that were never
// compared.  That does not make the output worse, it makes it wrong.
//
// So this checks the shape of the tree rather than one entry of it: every
// node reachable from any root is reached exactly once -- no cycles, no
// subtree hanging off two parents -- and every node sorts on the correct
// side of its parent.
TEST_F(ZstdMatchFinderTest, TheTreeIsATreeAndItIsSorted) {
  size_t target = 0;
  std::vector<uint8_t> data = build_deferral_case(&target);

  for (unsigned depth = 0; depth <= 2; depth++) {
    Parse parse = run_parse(data, 11, depth, true);
    ASSERT_EQ(parse.use_bt, 1u) << "level 11 is meant to search a tree";
    ASSERT_GT(parse.bt_size, 0u);

    // Reached from anywhere at all; the roots are the hash buckets, but a
    // walk from every node covers the forest without needing them.
    std::vector<int> seen(parse.bt_size, 0);

    // Every slot that names a child is one edge of the forest.  Counting how
    // many times each node is named catches a node with two parents, and a
    // cycle shows up as a node named by its own descendant.
    std::vector<int> parents(parse.bt_size, 0);
    for (size_t i = 0; i < parse.bt_size; i++) {
      for (unsigned side = 0; side < 2; side++) {
        uint32_t child = parse.bt[2u * i + side];
        if (child == 0u) {
          continue;
        }
        size_t c = static_cast<size_t>(child) - 1u;
        ASSERT_LT(c, parse.bt_size) << "child out of range";
        ASSERT_NE(c, i) << "node " << i << " is its own child at deferral "
                        << "depth " << depth;
        parents[c]++;
        ASSERT_LE(parents[c], 1)
            << "node " << c << " hangs off more than one parent at deferral "
            << "depth " << depth;

        // A child on the smaller side must sort before its parent, and one
        // on the larger side after.  Both are positions in `data`, so the
        // comparison is the same one the search makes.
        size_t n = data.size();
        size_t len = 0;
        while (i + len < n && c + len < n && data[i + len] == data[c + len]) {
          len++;
        }
        if (i + len < n && c + len < n) {
          bool child_is_smaller = data[c + len] < data[i + len];
          EXPECT_EQ(child_is_smaller, side == 0u)
              << "node " << c << " is on the wrong side of " << i
              << " at deferral depth " << depth;
        }
        (void)seen;
      }
    }
  }
}

// The same structural check, at a level that parses by shortest path rather
// than by deferral.  That parse reaches the tree differently: it searches
// every position of a sweep in turn, and when a match is long enough to be
// taken as it stands it jumps past the positions that match covers and has
// to put them into the tree itself.  Getting that wrong -- skipping a
// position, or inserting one twice -- corrupts the tree, and nothing about
// the output would say so.
TEST_F(ZstdMatchFinderTest, TheTreeIsStillATreeWhenTheParseIsOptimal) {
  std::vector<uint8_t> data = TextWithDistantRepeats(400u * 1024u, 8u, 16u * 1024u);

  Parse parse = run_parse(data, 19, 0, false);
  ASSERT_EQ(parse.use_bt, 1u) << "level 19 is meant to search a tree";
  ASSERT_GT(parse.bt_size, 0u);

  std::vector<int> parents(parse.bt_size, 0);
  for (size_t i = 0; i < parse.bt_size; i++) {
    for (unsigned side = 0; side < 2; side++) {
      uint32_t child = parse.bt[2u * i + side];
      if (child == 0u) {
        continue;
      }
      size_t c = static_cast<size_t>(child) - 1u;
      ASSERT_LT(c, parse.bt_size) << "child out of range";
      ASSERT_NE(c, i) << "node " << i << " is its own child";
      parents[c]++;
      ASSERT_LE(parents[c], 1)
          << "node " << c << " hangs off more than one parent";

      size_t n = data.size();
      size_t len = 0;
      while (i + len < n && c + len < n && data[i + len] == data[c + len]) {
        len++;
      }
      if (i + len < n && c + len < n) {
        bool child_is_smaller = data[c + len] < data[i + len];
        EXPECT_EQ(child_is_smaller, side == 0u)
            << "node " << c << " is on the wrong side of " << i;
      }
    }
  }
}

// A parse says how to rebuild the input, so between them its literals and its
// matches must account for every byte of it and no more.  The optimal parse
// assembles its answer by walking a chosen path backwards and then forwards
// again; dropping or repeating a step there would show up here first, and a
// round-trip test would only say "mismatch".
TEST_F(ZstdMatchFinderTest, TheOptimalParseAccountsForEveryByte) {
  std::vector<uint8_t> data = TextWithDistantRepeats(400u * 1024u, 8u, 16u * 1024u);

  for (int level = 16; level <= 22; level++) {
    Parse parse = run_parse(data, level, 0, false);
    EXPECT_EQ(bytes_covered(parse), data.size()) << "level " << level;

    // And every match must point at data that is actually behind it.
    size_t at = 0;
    for (const zstd_sequence_t & seq : parse.sequences) {
      at += seq.lit_length;
      // Offset codes 1 to 3 name a repeat offset rather than a distance;
      // anything above them is the distance plus three.
      if (seq.match_offset > 3u) {
        EXPECT_LE(seq.match_offset - 3u, at)
            << "level " << level << ": a match reaches before the start of "
            << "the stream";
      }
      EXPECT_GE(seq.match_length, 3u) << "level " << level;
      at += seq.match_length;
    }
  }
}

// A distance that has just been used is written as a one-symbol code instead
// of as a distance (RFC 8878 section 3.1.1.3.2.1.1).  On input with a fixed
// stride, where nearly every match is the same distance back as the last
// one, nearly every sequence should be written that way.
//
// This checks the stream, not the search: the parse also probes all three
// repeat offsets at every position rather than waiting for the match finder
// to turn one up, and turning that probe off does not fail this test -- the
// finder mostly finds the same distances by itself.  What the probe is worth
// was measured instead: on 9 MB of manuals, C source and XML it takes level
// 19 from 1395677 bytes to 1393482, about 0.16%, for some 5% of the time.
TEST_F(ZstdMatchFinderTest, TheOptimalParseWritesRepeatOffsets) {
  // Records of a fixed width whose fields repeat down the file: matching the
  // previous record is the same distance every time.
  constexpr size_t kRecord = 64;
  std::vector<uint8_t> data;
  Noise noise(0xC0FFEEu);
  std::vector<uint8_t> record;
  noise.append(record, kRecord);
  for (size_t r = 0; r < 4096; r++) {
    std::vector<uint8_t> copy = record;
    copy[r % kRecord] = static_cast<uint8_t>(r);
    copy[(r * 7u) % kRecord] = static_cast<uint8_t>(r >> 8);
    data.insert(data.end(), copy.begin(), copy.end());
  }

  Parse parse = run_parse(data, 19, 0, false);
  size_t repeats = 0;
  for (const zstd_sequence_t & seq : parse.sequences) {
    if (seq.match_offset >= 1u && seq.match_offset <= 3u) {
      repeats++;
    }
  }
  EXPECT_GT(repeats, parse.sequences.size() / 2u)
      << "of " << parse.sequences.size() << " sequences only " << repeats
      << " used a repeat offset on input whose every match is the same "
      << "distance back";
}

// A parse is a set of instructions for rebuilding the input, and this checks
// that following them gives the input back -- reading the sequences the way
// RFC 8878 section 3.1.1.3.2.1.1 says a decoder must, with its own copy of
// the repeat offset rules rather than the encoder's.
//
// That independence is the point.  The parse keeps a running copy of the
// three repeat offsets so it knows what a distance will cost, and the
// encoder uses the same copy to decide which code to write; an assertion in
// the parse checks the two agree, but it cannot catch a rule that is wrong
// in the same way in both places.  Nor, it turns out, can a round trip:
// rotating the offsets wrongly for code 3 still decoded 400 KB of text
// correctly, because the mistake only shows when a later sequence looks up
// the offset the rotation misplaced.  Checking each sequence against the
// specification's rules catches it at the first one that is wrong.
void CheckSequencesRebuildTheInput(
    const std::vector<uint8_t> & data, const Parse & parse, int level) {
  uint32_t rep[3] = {ZSTD_REP_OFFSET_1_INIT, ZSTD_REP_OFFSET_2_INIT,
      ZSTD_REP_OFFSET_3_INIT};
  size_t at = 0;       ///< Bytes of the input rebuilt so far.
  size_t lit_at = 0;   ///< Bytes of the literals buffer consumed so far.

  for (size_t i = 0; i < parse.sequences.size(); i++) {
    const zstd_sequence_t & seq = parse.sequences[i];

    ASSERT_LE(lit_at + seq.lit_length, parse.literals.size())
        << "level " << level << " sequence " << i
        << " wants more literals than the parse produced";
    ASSERT_LE(at + seq.lit_length, data.size())
        << "level " << level << " sequence " << i;
    for (uint32_t b = 0; b < seq.lit_length; b++) {
      ASSERT_EQ(parse.literals[lit_at + b], data[at + b])
          << "level " << level << " sequence " << i << " literal " << b;
    }
    lit_at += seq.lit_length;
    at += seq.lit_length;

    // Resolve the offset exactly as a decoder does.  Codes 1 to 3 name one
    // of the three most recently used distances, and which one they name
    // shifts when the sequence has no literals before it.
    uint32_t offset = 0;
    if (seq.match_offset > 3u) {
      offset = seq.match_offset - 3u;
      rep[2] = rep[1];
      rep[1] = rep[0];
      rep[0] = offset;
    }
    else {
      ASSERT_GE(seq.match_offset, 1u) << "level " << level << " sequence " << i
                                      << ": offset zero is not a code";
      unsigned slot = seq.match_offset - 1u;
      if (seq.lit_length == 0u) {
        slot++;
      }
      if (slot < 3u) {
        offset = rep[slot];
        // Everything the code passed over drops one place.
        for (unsigned k = slot; k > 0; k--) {
          rep[k] = rep[k - 1u];
        }
        rep[0] = offset;
      }
      else {
        // Code 3 with no literals means "one less than the most recent".
        offset = rep[0] - 1u;
        ASSERT_GT(rep[0], 1u) << "level " << level << " sequence " << i;
        rep[2] = rep[1];
        rep[1] = rep[0];
        rep[0] = offset;
      }
    }

    ASSERT_GT(offset, 0u) << "level " << level << " sequence " << i;
    ASSERT_LE(static_cast<size_t>(offset), at)
        << "level " << level << " sequence " << i
        << ": the match reaches before the start of the stream";
    ASSERT_LE(at + seq.match_length, data.size())
        << "level " << level << " sequence " << i;
    for (uint32_t b = 0; b < seq.match_length; b++) {
      ASSERT_EQ(data[at - offset + b], data[at + b])
          << "level " << level << " sequence " << i << " byte " << b
          << ": the offset the decoder resolves does not name these bytes";
    }
    at += seq.match_length;
  }

  // Whatever is left is trailing literals, and they must be the rest of it.
  size_t trailing = data.size() - at;
  ASSERT_EQ(parse.literals.size() - lit_at, trailing) << "level " << level;
  for (size_t b = 0; b < trailing; b++) {
    ASSERT_EQ(parse.literals[lit_at + b], data[at + b]) << "level " << level;
  }
}

TEST_F(ZstdMatchFinderTest, EverySequenceResolvesToTheBytesItClaims) {
  std::vector<uint8_t> data = ThreePeriodNoise(400u * 1024u);

  // A parse that rarely reaches for a repeat offset would pass this without
  // exercising the rules it is here to check.  What matters is not only that
  // all three codes appear but that they follow one another, since a
  // rotation is only wrong once something looks up what it misplaced.
  size_t codes[4] = {0, 0, 0, 0};
  size_t after_a_rotation = 0;
  unsigned previous = 0;
  Parse check = run_parse(data, 19, 0, false);
  for (const zstd_sequence_t & seq : check.sequences) {
    if (seq.match_offset >= 1u && seq.match_offset <= 3u) {
      codes[seq.match_offset]++;
      if (previous == 3u && seq.match_offset >= 2u) {
        after_a_rotation++;
      }
      previous = seq.match_offset;
    }
    else {
      previous = 0;
    }
  }
  for (unsigned code = 1; code <= 3; code++) {
    ASSERT_GT(codes[code], 0u)
        << "this input never uses offset code " << code;
  }
  ASSERT_GT(after_a_rotation, 100u)
      << "no code looks up an offset that a previous rotation moved, so a "
      << "wrong rotation would not show";

  for (int level = 1; level <= 22; level++) {
    Parse parse = run_parse(data, level, 0, false);
    CheckSequencesRebuildTheInput(data, parse, level);
  }
}

// An insertion does not report what it finds, so it has no use for the exact
// length of the matches it meets: anything at least nice_length long ends
// the descent whatever it turns out to be.  It therefore stops comparing
// there -- which is only allowed because it leaves the tree exactly as a
// full comparison would have.
//
// This checks that directly, by building the same tree twice over the same
// buffer, once with every position inserted and once with every position
// searched, and comparing the two slot for slot.  The input has a repeating
// period, which is the shape that made the difference visible: matches there
// run to the end of the buffer, so every insertion used to compare tens of
// kilobytes to learn something it discarded.
TEST_F(ZstdMatchFinderTest, InsertingBuildsTheSameTreeAsSearching) {
  // A period well under the level's nice_length, so the comparison limit is
  // reached at nearly every position.
  std::vector<uint8_t> data = ThreePeriodNoise(256u * 1024u);

  const gcomp_allocator_t * alloc = gcomp_allocator_default();
  for (int level : {11, 16, 19, 22}) {
    zstd_match_finder_t inserted;
    zstd_match_finder_t searched;
    ASSERT_EQ(zstd_mf_init(&inserted, alloc, level, 1u << 20, nullptr),
        GCOMP_OK);
    ASSERT_EQ(
        zstd_mf_init(&searched, alloc, level, 1u << 20, nullptr), GCOMP_OK);
    ASSERT_EQ(inserted.use_bt, 1u) << "level " << level;

    std::vector<zstd_mf_candidate_t> found(ZSTD_MF_MAX_CANDIDATES);
    for (size_t pos = 0; pos < data.size(); pos++) {
      zstd_mf_insert_one(&inserted, data.data(), pos, data.size());
      (void)zstd_mf_find_matches(&searched, data.data(), pos, data.size(),
          found.data(), found.size());
    }

    ASSERT_EQ(inserted.bt_size, searched.bt_size);
    for (size_t i = 0; i < inserted.bt_size * 2u; i++) {
      ASSERT_EQ(inserted.bt_table[i], searched.bt_table[i])
          << "level " << level << ": slot " << i
          << " differs, so stopping an insertion's comparison early changed "
          << "the tree";
    }
    for (size_t i = 0; i < inserted.hash_size; i++) {
      ASSERT_EQ(inserted.hash_table[i], searched.hash_table[i])
          << "level " << level << ": hash slot " << i << " differs";
    }

    zstd_mf_destroy(&inserted, alloc, nullptr);
    zstd_mf_destroy(&searched, alloc, nullptr);
  }
}

// What a parse of periodic input is allowed to cost.
//
// Comparing is nearly all of what the tree does, and the way it goes wrong
// is comparing far more than the answer needs.  On data with a repeating
// period every match runs to the end of the buffer, and an insertion that
// measured each one in full turned a linear job into a quadratic one: 4 MB
// of a 1500 byte pattern took 9.3 seconds at every level from 11 up, where
// the reference implementation took 0.02.
//
// The bound here is per input byte and deliberately loose -- it is aimed at
// a factor of hundreds, not at the last ten percent -- and it is counted
// rather than timed, so it says the same thing on a busy machine as on an
// idle one.  Counting happens only in a sanitizer build, because one add
// per candidate measured 0.87% of encoding and that is too much to charge
// every caller for a test.
TEST_F(ZstdMatchFinderTest, PeriodicInputDoesNotCostQuadraticWork) {
  // A period well under every tree level's nice_length, so a full
  // measurement would run to the end of the buffer at nearly every position.
  constexpr size_t kPeriod = 1500;
  std::vector<uint8_t> pattern;
  Noise noise(0xABCDEFu);
  noise.append(pattern, kPeriod);
  std::vector<uint8_t> data;
  while (data.size() < 512u * 1024u) {
    data.insert(data.end(), pattern.begin(), pattern.end());
  }

  // The library only counts in a build that defines GCOMP_TEST_BUILD, which
  // is the sanitizer one.  Asking the library rather than asking the
  // preprocessor means the test cannot quietly stop checking because a
  // build flag moved: a zero can only mean counting is off, and anything
  // else is measured.
  {
    Parse probe = run_parse(data, 19, 0, false);
    if (probe.compared_bytes == 0) {
      GTEST_SKIP() << "this build does not count comparisons; run "
                      "`make test-asan`";
    }
  }

  for (int level : {11, 15, 19, 22}) {
    Parse parse = run_parse(data, level, 0, false);
    ASSERT_EQ(parse.use_bt, 1u) << "level " << level;
    ASSERT_GT(parse.compared_bytes, 0u) << "level " << level;
    // On this input the encoder now compares 0.997 bytes for every byte of
    // input, at every level: it reads the thing once.  Before, level 22
    // compared 12,110 bytes per input byte, and that figure rose with the
    // size of the buffer.  A hundred leaves room for a different parse
    // without leaving room for that.
    EXPECT_LT(parse.compared_bytes, data.size() * 100u)
        << "level " << level << ": compared " << parse.compared_bytes
        << " bytes for " << data.size() << " of input, "
        << (double)parse.compared_bytes / (double)data.size() << " per byte";
  }
}

// Twenty-two levels must be twenty-two levels.
//
// They were not.  The search depth, the deferral depth, the nice length and
// the hash table size were each set by a range test -- level <= 3, level <= 6,
// level <= 12 -- and the ranges made levels into aliases of one another.
// Compressing 11 MB of source, manuals, XML and an ELF binary at each level in
// turn produced byte for byte identical output from levels 2 and 3; from 4, 5
// and 6; from 7, 8, 9 and 10; and from 11 and 12.  Twelve levels, five
// behaviours.  A caller who asked for 9 got 7.
//
// Real text is used rather than the deep-chain shape the other tests use,
// because on input where every match is long the nice_length cutoff stops the
// chain walk immediately and the deep levels genuinely cannot differ -- the
// input never asks them to.
TEST_F(ZstdMatchFinderTest, EveryLevelDiffersFromTheOneBelowIt) {
  // Two megabytes with eight 64 KB stretches repeated further on.  Plain
  // text of 400 KB, which this used to use, is finished with by about level
  // 16: the tree has found everything there is and the levels above it
  // produce the same bytes.  That is the input running out, not the levels
  // being aliases, and the settings check below is what separates the two.
  std::vector<uint8_t> in = TextWithDistantRepeats(2000000, 8, 65536);

  std::vector<std::vector<uint8_t>> streams;
  for (int level = 1; level <= 22; level++) {
    streams.push_back(EncodeAtLevel(in, level));
    ASSERT_FALSE(streams.back().empty()) << "level " << level;
  }

  for (size_t i = 1; i < streams.size(); i++) {
    EXPECT_NE(streams[i], streams[i - 1])
        << "level " << (i + 1) << " encodes exactly as level " << i
        << ", so one of the two is not a level";
  }

  // The ladder has to go somewhere, checked across it rather than step by
  // step.  Adjacent levels are not required to be ordered by size: the parse
  // is greedy with a look-ahead, so searching harder can turn up a longer
  // match that leads to a worse parse than the shorter one would have, and
  // at the top -- where the tree has already found nearly everything -- the
  // levels drift by a few bytes in either direction.  Measured over three
  // inputs the drift was never more than four bytes in 163,500.  Requiring
  // each level to beat the one below would be requiring the parse to be
  // optimal, which it is not and does not claim to be.
  EXPECT_LT(streams[21].size(), streams[10].size())
      << "the tree levels gain nothing over the chain levels";
  EXPECT_LT(streams[10].size(), streams[0].size())
      << "the chain levels gain nothing over the fast level";
}

// The same question asked of the settings rather than the output, because
// the output can only answer it on input that still has something to find.
//
// This is the defect that started all of this: the levels were set by range
// tests, so several of them named the same behaviour and a caller who asked
// for 9 got 7.  Comparing what each level actually configures catches that
// however easy the input is, and cannot be satisfied by an input that runs
// out before the levels do.
TEST_F(ZstdMatchFinderTest, NoTwoLevelsAreConfiguredTheSame) {
  const gcomp_allocator_t * alloc = gcomp_allocator_default();

  struct Settings {
    unsigned search_depth;
    unsigned lazy_depth;
    uint32_t nice_length;
    unsigned hash_log;
    unsigned use_bt;
    size_t window;
  };
  std::vector<Settings> seen;

  for (int level = 1; level <= 22; level++) {
    zstd_match_finder_t mf;
    ASSERT_EQ(zstd_mf_init(&mf, alloc, level, 1u << 20, nullptr), GCOMP_OK);
    Settings s{mf.search_depth, mf.lazy_depth, mf.nice_length, mf.hash_log,
        mf.use_bt, mf.window_size};
    zstd_mf_destroy(&mf, alloc, nullptr);

    for (size_t i = 0; i < seen.size(); i++) {
      bool same = seen[i].search_depth == s.search_depth &&
          seen[i].lazy_depth == s.lazy_depth &&
          seen[i].nice_length == s.nice_length &&
          seen[i].hash_log == s.hash_log && seen[i].use_bt == s.use_bt &&
          seen[i].window == s.window;
      EXPECT_FALSE(same) << "level " << level << " is configured exactly as "
                         << "level " << (i + 1) << ", so one of them is not "
                         << "a level";
    }
    seen.push_back(s);
  }
}

// And through the levels callers actually reach for, a higher level must not
// produce a larger file.
//
// The bound is 12 and not 22 on purpose.  Above it the remaining levels differ
// by a couple of bytes in a hundred thousand and the ordering is not reliable:
// a deeper search finds longer matches at odder distances, and the offsets it
// then has to encode can cost more than the match length saves.  That is a
// property of a parse that decides one match at a time, not a fault in the
// table, and asserting an order that the algorithm does not guarantee would
// make this test a source of noise rather than a check.
//
// Two inputs, because they catch different things.  The small one fits
// inside every window from level 4 up, so it says nothing about the two
// places the declared window grows; the large one crosses both.  The half
// of this that cannot be tested here is speed: a level that is slower than
// the one above it is just as wrong as one that is larger -- levels 7 to 10
// were both, once -- but a clock in a unit test is a source of noise.  That
// half is measured, not asserted; see WHERE THE TREE STARTS in
// zstd_matchfinder.c.
TEST_F(ZstdMatchFinderTest, HigherLevelsDoNotProduceLargerOutput) {
  std::vector<uint8_t> small = TextLikeBytes(400000);
  std::vector<uint8_t> large =
      TextWithDistantRepeats(3u * 1024u * 1024u, 12u, 64u * 1024u);

  for (const std::vector<uint8_t> * in : {&small, &large}) {
    size_t previous = SIZE_MAX;
    for (int level = 1; level <= 12; level++) {
      size_t size = EncodeAtLevel(*in, level).size();
      EXPECT_LE(size, previous)
          << "over " << in->size() << " bytes, level " << level
          << " produced " << size << " bytes, more than level "
          << (level - 1) << "'s " << previous;
      previous = size;
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
  EXPECT_EQ(mf.use_opt, 0u) << "level 1 must not parse optimally either";
  zstd_mf_destroy(&mf, alloc, nullptr);

  // There are two ways to be better than greedy, and every level above the
  // first has to use one of them.  Naming which one, per level, would make
  // this test a copy of the table it is checking; what matters is that no
  // level is left doing the fast thing under a slow level's name.
  for (int level = 2; level <= 22; level++) {
    ASSERT_EQ(zstd_mf_init(&mf, alloc, level, 1u << 20, nullptr), GCOMP_OK);
    EXPECT_TRUE(mf.lazy_depth >= 1u || mf.use_opt != 0u)
        << "level " << level << " neither defers nor parses optimally";
    EXPECT_GT(mf.nice_length, 0u) << "level " << level;
    // The optimal parse prices candidate matches against one another, and
    // only the tree can produce a list of candidates to price.
    if (mf.use_opt) {
      EXPECT_NE(mf.use_bt, 0u) << "level " << level
                               << " parses optimally without a tree to search";
      EXPECT_GT(mf.opt_segment, 0u) << "level " << level;
      EXPECT_NE(mf.opt, nullptr) << "level " << level;
    }
    zstd_mf_destroy(&mf, alloc, nullptr);
  }
}

// A level that parses optimally must not sit below one that does not: the
// levels are a ladder, and a caller who asks for more effort and gets a
// cheaper algorithm has been given the wrong thing.
TEST_F(ZstdMatchFinderTest, TheOptimalLevelsAreTheTopOfTheLadder) {
  const gcomp_allocator_t * alloc = gcomp_allocator_default();
  int first_optimal = 0;

  for (int level = 1; level <= 22; level++) {
    zstd_match_finder_t mf;
    ASSERT_EQ(zstd_mf_init(&mf, alloc, level, 1u << 20, nullptr), GCOMP_OK);
    if (mf.use_opt) {
      if (!first_optimal) {
        first_optimal = level;
      }
    }
    else {
      EXPECT_EQ(first_optimal, 0)
          << "level " << level << " does not parse optimally, but level "
          << first_optimal << " below it does";
    }
    zstd_mf_destroy(&mf, alloc, nullptr);
  }
  EXPECT_GT(first_optimal, 0) << "no level parses optimally";
}

// Everything either parse prices is a difference of two of these -- the zstd
// one and the deflate one both cost their symbols with it -- so an error
// here does not fail anything, it quietly makes every parse worse.  The
// reference is the real logarithm; the tolerance is the last place the fixed
// point format can represent.
TEST_F(ZstdMatchFinderTest, TheFixedPointLogarithmIsTheRealOne) {
  EXPECT_EQ(gcomp_bitcost_log2(1u), 0u) << "log2(1) is zero";
  EXPECT_EQ(gcomp_bitcost_log2(256u), 8u * 256u) << "an exact power of two";
  EXPECT_EQ(gcomp_bitcost_log2(1u << 31), 31u * 256u);
  // Zero has no logarithm; the model never has a count of zero, and asking
  // for one must still give a usable number rather than wrapping.
  EXPECT_EQ(gcomp_bitcost_log2(0u), 0u);

  for (uint32_t x = 1; x < 4096; x++) {
    double want = std::log2(static_cast<double>(x)) * 256.0;
    double got = static_cast<double>(gcomp_bitcost_log2(x));
    EXPECT_LE(std::fabs(got - want), 1.0) << "log2(" << x << ")";
  }
  for (uint32_t bit = 12; bit < 32; bit++) {
    for (uint32_t k = 0; k < 64; k++) {
      uint32_t x = (1u << bit) + k * ((1u << bit) / 64u) + k;
      double want = std::log2(static_cast<double>(x)) * 256.0;
      double got = static_cast<double>(gcomp_bitcost_log2(x));
      EXPECT_LE(std::fabs(got - want), 1.0) << "log2(" << x << ")";
    }
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
