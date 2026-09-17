/**
 * @file test_huffman_lengths.cpp
 *
 * Tests for the shared length-limited Huffman code length builder.
 *
 * The requirement being tested is the one stated in huffman_lengths.h: the
 * lengths minimise sum(freq[i] * length[i]) over every prefix-free code whose
 * longest code word fits the cap, and they always describe a complete code.
 * "Optimal" is checked against an exhaustive search over small alphabets
 * rather than against this implementation's own output, so a change that
 * makes the code worse cannot pass by agreeing with itself.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "../../src/core/huffman_lengths.h"
#include <cstdint>
#include <cstring>
#include <ghoti.io/compress/errors.h>
#include <gtest/gtest.h>
#include <limits>
#include <queue>
#include <random>
#include <utility>
#include <vector>

namespace {

// Sum of 2^-length over the code, scaled by 2^cap.  A complete code sums to
// exactly 2^cap; less is incomplete, more is over-subscribed and not a code
// at all.
uint64_t KraftSum(const std::vector<uint8_t> & lengths, unsigned cap) {
  uint64_t sum = 0;
  for (uint8_t l : lengths) {
    if (l > 0) {
      sum += 1ull << (cap - l);
    }
  }
  return sum;
}

uint64_t CodeCost(
    const std::vector<uint32_t> & freq, const std::vector<uint8_t> & lengths) {
  uint64_t cost = 0;
  for (size_t i = 0; i < freq.size(); i++) {
    cost += (uint64_t)freq[i] * lengths[i];
  }
  return cost;
}

// Exhaustive search over every complete code under the cap.  Only usable for
// a handful of symbols, which is the point: it is an independent answer.
struct BruteForce {
  const std::vector<uint32_t> & freq;
  unsigned cap;
  uint64_t best = std::numeric_limits<uint64_t>::max();

  void search(size_t i, uint64_t kraft, uint64_t cost) {
    uint64_t budget = 1ull << cap;
    if (kraft > budget) {
      return;
    }
    if (i == freq.size()) {
      if (kraft == budget && cost < best) {
        best = cost;
      }
      return;
    }
    for (unsigned l = 1; l <= cap; l++) {
      search(i + 1, kraft + (1ull << (cap - l)), cost + (uint64_t)freq[i] * l);
    }
  }
};

std::vector<uint8_t> Build(
    const std::vector<uint32_t> & freq, unsigned cap, gcomp_status_t expect) {
  std::vector<uint8_t> lengths(freq.size(), 0xEE);
  EXPECT_EQ(gcomp_huffman_code_lengths(
                nullptr, freq.data(), freq.size(), cap, lengths.data()),
      expect);
  return lengths;
}

// Code lengths from a plain Huffman tree, with no cap.  Used only to build
// the "before" answer below.
std::vector<uint8_t> PlainHuffman(const std::vector<uint32_t> & freq) {
  size_t n = freq.size();
  std::vector<uint64_t> weight;
  std::vector<int> parent;
  for (size_t i = 0; i < n; i++) {
    weight.push_back(freq[i]);
    parent.push_back(-1);
  }
  using Item = std::pair<uint64_t, int>;
  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
  for (size_t i = 0; i < n; i++) {
    if (freq[i] > 0) {
      pq.push({freq[i], (int)i});
    }
  }
  while (pq.size() > 1) {
    Item a = pq.top();
    pq.pop();
    Item b = pq.top();
    pq.pop();
    int node = (int)weight.size();
    weight.push_back(a.first + b.first);
    parent.push_back(-1);
    parent[a.second] = node;
    parent[b.second] = node;
    pq.push({a.first + b.first, node});
  }
  std::vector<uint8_t> lengths(n, 0);
  for (size_t i = 0; i < n; i++) {
    if (freq[i] == 0) {
      continue;
    }
    uint8_t depth = 0;
    for (int at = (int)i; parent[at] >= 0; at = parent[at]) {
      depth++;
    }
    lengths[i] = depth ? depth : 1;
  }
  return lengths;
}

// The limiter this module replaced: clamp everything past the cap, then walk
// the symbols in index order lengthening codes until the Kraft sum fits, then
// shorten until the code is complete again.  Kept here so the claim that
// package-merge costs fewer bits is checked against the real alternative
// rather than asserted.
std::vector<uint8_t> ClampAndRepair(
    const std::vector<uint32_t> & freq, unsigned cap) {
  std::vector<uint8_t> lengths = PlainHuffman(freq);
  uint64_t budget = 1ull << cap;
  for (uint8_t & l : lengths) {
    if (l > cap) {
      l = (uint8_t)cap;
    }
  }
  uint64_t kraft = KraftSum(lengths, cap);
  while (kraft > budget) {
    bool moved = false;
    for (size_t i = 0; i < lengths.size() && kraft > budget; i++) {
      if (lengths[i] > 0 && lengths[i] < cap) {
        kraft -= 1ull << (cap - lengths[i]);
        lengths[i]++;
        kraft += 1ull << (cap - lengths[i]);
        moved = true;
      }
    }
    if (!moved) {
      break;
    }
  }
  while (kraft < budget) {
    size_t best = lengths.size();
    for (size_t i = 0; i < lengths.size(); i++) {
      if (lengths[i] <= 1) {
        continue;
      }
      if (1ull << (cap - lengths[i]) > budget - kraft) {
        continue;
      }
      if (best == lengths.size() || lengths[i] < lengths[best]) {
        best = i;
      }
    }
    if (best == lengths.size()) {
      break;
    }
    kraft += 1ull << (cap - lengths[best]);
    lengths[best]--;
  }
  return lengths;
}

} // namespace

TEST(HuffmanLengths, UnusedSymbolsGetNoCode) {
  std::vector<uint32_t> freq = {0, 5, 0, 3, 0};
  auto lengths = Build(freq, 15, GCOMP_OK);
  EXPECT_EQ(lengths[0], 0);
  EXPECT_EQ(lengths[2], 0);
  EXPECT_EQ(lengths[4], 0);
  EXPECT_GT(lengths[1], 0);
  EXPECT_GT(lengths[3], 0);
}

TEST(HuffmanLengths, EmptyAlphabetProducesNoCode) {
  std::vector<uint32_t> freq(20, 0);
  auto lengths = Build(freq, 15, GCOMP_OK);
  for (uint8_t l : lengths) {
    EXPECT_EQ(l, 0);
  }
}

TEST(HuffmanLengths, SingleUsedSymbolGetsOneBit) {
  // A zero-bit code word could not be told apart from the absence of one, and
  // DEFLATE's decoder builds a one-entry table from a single length-1 code.
  std::vector<uint32_t> freq(30, 0);
  freq[7] = 99;
  auto lengths = Build(freq, 15, GCOMP_OK);
  EXPECT_EQ(lengths[7], 1);
}

TEST(HuffmanLengths, TwoUsedSymbolsEachGetOneBit) {
  std::vector<uint32_t> freq(30, 0);
  freq[2] = 1000;
  freq[29] = 1;
  auto lengths = Build(freq, 15, GCOMP_OK);
  EXPECT_EQ(lengths[2], 1);
  EXPECT_EQ(lengths[29], 1);
}

TEST(HuffmanLengths, RejectsAnAlphabetTooLargeForTheCap) {
  // Four bits can name sixteen code words; seventeen symbols cannot fit.
  std::vector<uint32_t> freq(17, 1);
  Build(freq, 4, GCOMP_ERR_INVALID_ARG);
}

TEST(HuffmanLengths, RejectsNullArguments) {
  std::vector<uint32_t> freq(4, 1);
  std::vector<uint8_t> lengths(4, 0);
  EXPECT_EQ(
      gcomp_huffman_code_lengths(nullptr, nullptr, 4, 15, lengths.data()),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(
      gcomp_huffman_code_lengths(nullptr, freq.data(), 4, 15, nullptr),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_huffman_code_lengths(
                nullptr, freq.data(), 4, 0, lengths.data()),
      GCOMP_ERR_INVALID_ARG);
  EXPECT_EQ(gcomp_huffman_code_lengths(
                nullptr, freq.data(), 4, 33, lengths.data()),
      GCOMP_ERR_INVALID_ARG);
}

TEST(HuffmanLengths, GeometricFrequenciesStayUnderTheCap) {
  // Halving frequencies drive a plain Huffman code to one bit per symbol, so
  // 256 symbols want up to 255 bits.  This is the shape that makes the cap
  // bind, and both callers' caps have to hold.
  for (unsigned cap : {7u, 11u, 15u}) {
    // A cap of c bits has 2^c code words, so the alphabet has to fit in it.
    std::vector<uint32_t> freq(cap == 7 ? 128 : 256);
    uint32_t f = 1u << 30;
    for (size_t i = 0; i < freq.size(); i++) {
      freq[i] = f > 1 ? f : 1;
      f /= 2;
    }
    auto lengths = Build(freq, cap, GCOMP_OK);
    for (uint8_t l : lengths) {
      EXPECT_LE(l, cap);
      EXPECT_GT(l, 0);
    }
    EXPECT_EQ(KraftSum(lengths, cap), 1ull << cap);
  }
}

TEST(HuffmanLengths, LengthsAlwaysDescribeACompleteCode) {
  std::mt19937 rng(20260917);
  for (int trial = 0; trial < 500; trial++) {
    size_t n = 2 + rng() % 286;
    unsigned cap = 3 + rng() % 13;
    if ((1u << cap) < n) {
      continue;
    }
    std::vector<uint32_t> freq(n);
    for (size_t i = 0; i < n; i++) {
      // A mixture of flat and wildly skewed shapes; zeros are allowed, but
      // at least two symbols have to be in use for a code to exist.
      freq[i] = (rng() % 4 == 0) ? 0 : (1u << (rng() % 24)) + rng() % 97;
    }
    freq[0] = 1;
    freq[n - 1] = 1u << 20;
    auto lengths = Build(freq, cap, GCOMP_OK);
    EXPECT_EQ(KraftSum(lengths, cap), 1ull << cap)
        << "n=" << n << " cap=" << cap;
    for (size_t i = 0; i < n; i++) {
      EXPECT_LE(lengths[i], cap);
      EXPECT_EQ(lengths[i] == 0, freq[i] == 0);
    }
  }
}

TEST(HuffmanLengths, MatchesTheBruteForceOptimumOnSmallAlphabets) {
  std::mt19937 rng(4242);
  int checked = 0;
  for (int trial = 0; trial < 600; trial++) {
    size_t n = 2 + rng() % 7;
    unsigned cap = 3 + rng() % 4;
    if ((1u << cap) < n) {
      continue;
    }
    std::vector<uint32_t> freq(n);
    int shape = rng() % 3;
    for (size_t i = 0; i < n; i++) {
      if (shape == 0) {
        freq[i] = 1 + rng() % 100;
      }
      else if (shape == 1) {
        freq[i] = 1u << (rng() % 20); // spread wide enough to bind the cap
      }
      else {
        freq[i] = (i == 0) ? 1000000u : 1 + rng() % 3;
      }
    }
    auto lengths = Build(freq, cap, GCOMP_OK);
    BruteForce bf{freq, cap};
    bf.search(0, 0, 0);
    EXPECT_EQ(CodeCost(freq, lengths), bf.best)
        << "n=" << n << " cap=" << cap;
    checked++;
  }
  EXPECT_GT(checked, 400);
}

TEST(HuffmanLengths, EqualFrequenciesGiveABalancedCode) {
  // Eight equally likely symbols cost three bits each; anything else wastes
  // code space.
  std::vector<uint32_t> freq(8, 17);
  auto lengths = Build(freq, 15, GCOMP_OK);
  for (uint8_t l : lengths) {
    EXPECT_EQ(l, 3);
  }
}

TEST(HuffmanLengths, MoreFrequentSymbolsAreNeverLonger) {
  std::mt19937 rng(777);
  for (int trial = 0; trial < 200; trial++) {
    size_t n = 4 + rng() % 60;
    unsigned cap = 6 + rng() % 10;
    if ((1u << cap) < n) {
      continue;
    }
    std::vector<uint32_t> freq(n);
    for (size_t i = 0; i < n; i++) {
      freq[i] = 1 + rng() % 100000;
    }
    auto lengths = Build(freq, cap, GCOMP_OK);
    for (size_t i = 0; i < n; i++) {
      for (size_t j = 0; j < n; j++) {
        if (freq[i] > freq[j]) {
          EXPECT_LE(lengths[i], lengths[j]) << "i=" << i << " j=" << j;
        }
      }
    }
  }
}

TEST(HuffmanLengths, TiedFrequenciesResolveTheSameWayEveryTime) {
  // Ties are broken by symbol index, so the code a block gets depends on the
  // frequencies alone and not on the sort implementation.
  std::vector<uint32_t> freq(40, 3);
  freq[11] = 900;
  auto first = Build(freq, 15, GCOMP_OK);
  for (int i = 0; i < 5; i++) {
    EXPECT_EQ(Build(freq, 15, GCOMP_OK), first);
  }
}

TEST(HuffmanLengths, ACapThatDoesNotBindCostsNothing) {
  // With room to spare the answer is the plain Huffman code, so raising the
  // cap further must not change the cost.
  std::mt19937 rng(31337);
  for (int trial = 0; trial < 100; trial++) {
    size_t n = 2 + rng() % 40;
    std::vector<uint32_t> freq(n);
    for (size_t i = 0; i < n; i++) {
      freq[i] = 1 + rng() % 1000;
    }
    auto tight = Build(freq, 20, GCOMP_OK);
    auto loose = Build(freq, 30, GCOMP_OK);
    EXPECT_EQ(CodeCost(freq, tight), CodeCost(freq, loose));
  }
}

TEST(HuffmanLengths, CostsFewerBitsThanClampAndRepairWould) {
  // Frequencies that halve: a plain Huffman code runs one bit longer per
  // symbol and so shoots far past any useful cap.  This is the shape that
  // shows up in a skewed literal block, and the shape on which the limiter
  // this replaced lost the most.
  std::vector<uint32_t> freq = {1000000, 500000, 250000, 120000, 60000, 30000,
      15000, 7000, 3000, 1500, 700, 300, 150, 70, 30, 15, 7, 3, 1, 1};

  auto lengths = Build(freq, 7, GCOMP_OK);
  EXPECT_EQ(KraftSum(lengths, 7), 1ull << 7);
  for (uint8_t l : lengths) {
    EXPECT_LE(l, 7u);
    EXPECT_GT(l, 0);
  }
  EXPECT_LT(CodeCost(freq, lengths), CodeCost(freq, ClampAndRepair(freq, 7)));
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
