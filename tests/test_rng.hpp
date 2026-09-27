// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Deterministic pseudo-random generator for property and randomized tests.
//
// SplitMix64: a fixed, fully specified sequence. The generator is seeded from
// the run's seed, so a failing case is reproducible by rerunning with the
// printed seed.

#ifndef FACILITY_CAPACITY_TESTS_TEST_RNG_HPP
#define FACILITY_CAPACITY_TESTS_TEST_RNG_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace ftest {

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  std::uint64_t next_u64() {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t value = state_;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
  }

  /// A value in `[0, bound)`. `bound` must be non-zero.
  std::uint64_t below(std::uint64_t bound) { return bound == 0 ? 0 : next_u64() % bound; }

  /// A value in `[low, high]`.
  std::int64_t range(std::int64_t low, std::int64_t high) {
    if (high <= low) {
      return low;
    }
    const std::uint64_t span = static_cast<std::uint64_t>(high - low) + 1u;
    return low + static_cast<std::int64_t>(below(span));
  }

  bool boolean() { return (next_u64() & 1u) != 0u; }

  /// True with probability `numerator / denominator`.
  bool chance(std::uint64_t numerator, std::uint64_t denominator) {
    return denominator != 0 && below(denominator) < numerator;
  }

  template <class T>
  const T& pick(const std::vector<T>& items) {
    return items[static_cast<std::size_t>(below(items.size()))];
  }

  std::string suffix() { return std::to_string(next_u64() % 100000u); }

 private:
  std::uint64_t state_;
};

}  // namespace ftest

#endif  // FACILITY_CAPACITY_TESTS_TEST_RNG_HPP
