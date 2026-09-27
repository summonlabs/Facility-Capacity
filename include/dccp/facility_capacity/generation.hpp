// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Generation, epoch, incarnation, revision, attempt and time counters.
//
// Each family is a distinct type over an unsigned 64-bit magnitude. Advancing a
// counter is checked: a counter that would wrap is reported as `limit_exceeded`
// rather than silently returning to zero.

#ifndef DCCP_FACILITY_CAPACITY_GENERATION_HPP
#define DCCP_FACILITY_CAPACITY_GENERATION_HPP

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

#include "dccp/facility_capacity/result.hpp"

namespace dccp::facility_capacity {

struct CapacityGenerationTag {};
struct EvidenceGenerationTag {};
struct RevisionTag {};
struct EpochIdTag {};
struct IncarnationIdTag {};
struct AttemptIdTag {};
struct TickTag {};

template <class Tag>
class Counter {
 public:
  using tag_type = Tag;

  constexpr Counter() noexcept = default;

  static constexpr Counter from_value(std::uint64_t value) noexcept {
    Counter counter;
    counter.value_ = value;
    return counter;
  }

  /// Parses an unsigned decimal magnitude. Rejects signs, padding, overflow and
  /// non-digit input.
  static Result<Counter> parse(std::string_view text) {
    if (text.empty() || text.size() > 20) {
      return Error::make(ErrorCode::invalid_argument, "invalid counter text");
    }
    std::uint64_t value = 0;
    for (const char character : text) {
      if (character < '0' || character > '9') {
        return Error::make(ErrorCode::invalid_argument, "counter text is not decimal");
      }
      const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
      if (value > (UINT64_MAX - digit) / 10u) {
        Error error = Error::make(ErrorCode::limit_exceeded, "counter magnitude overflow");
        error.with_constraint("value <= 18446744073709551615");
        return error;
      }
      value = value * 10u + digit;
    }
    return Counter::from_value(value);
  }

  constexpr std::uint64_t value() const noexcept { return value_; }

  constexpr bool is_zero() const noexcept { return value_ == 0; }

  /// The next value. Fails with `limit_exceeded` at the top of the range.
  Result<Counter> next() const {
    if (value_ == UINT64_MAX) {
      Error error = Error::make(ErrorCode::limit_exceeded, "counter exhausted");
      error.with_generations(value_, value_);
      return error;
    }
    return Counter::from_value(value_ + 1u);
  }

  /// The next value without a bounds check, for callers that have already
  /// established `value() != UINT64_MAX`.
  constexpr Counter unchecked_next() const noexcept { return Counter::from_value(value_ + 1u); }

  friend constexpr bool operator==(const Counter& lhs, const Counter& rhs) noexcept = default;

  friend constexpr std::strong_ordering operator<=>(const Counter& lhs, const Counter& rhs) noexcept = default;

  std::string to_string() const { return std::to_string(value_); }

 private:
  std::uint64_t value_ = 0;
};

using CapacityGeneration = Counter<CapacityGenerationTag>;
using EvidenceGeneration = Counter<EvidenceGenerationTag>;
using Revision = Counter<RevisionTag>;
using EpochId = Counter<EpochIdTag>;
using IncarnationId = Counter<IncarnationIdTag>;
using AttemptId = Counter<AttemptIdTag>;

/// An exact integer instant on the injected clock's timeline, in nanoseconds.
///
/// The library never reads a wall clock for authority. A `Tick` is comparable
/// and exact; it is not a calendar time and no timezone or locale takes part in
/// any decision.
using Tick = Counter<TickTag>;

/// The largest representable tick, used as the open end of an unbounded window.
inline constexpr Tick max_tick = Tick::from_value(UINT64_MAX);

template <class Tag>
std::ostream& operator<<(std::ostream& out, const Counter<Tag>& counter) {
  return out << counter.value();
}

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_GENERATION_HPP
