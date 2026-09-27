// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Time is injected, never read implicitly.
//
// Nothing in this library consults a wall clock, a locale or a timezone to make
// an authority decision. Expiry, reserve windows and provenance instants are
// compared against an exact tick supplied by the clock the caller installed.

#ifndef DCCP_FACILITY_CAPACITY_CLOCK_HPP
#define DCCP_FACILITY_CAPACITY_CLOCK_HPP

#include <memory>

#include "dccp/facility_capacity/generation.hpp"
#include "dccp/facility_capacity/result.hpp"

namespace dccp::facility_capacity {

/// Source of exact ticks.
class Clock {
 public:
  Clock() = default;
  virtual ~Clock();
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;

  /// The current instant, in nanoseconds.
  virtual Result<Tick> now() const = 0;
};

/// A clock the caller advances explicitly. Deterministic.
class ManualClock final : public Clock {
 public:
  ManualClock() = default;

  explicit ManualClock(Tick start) : current_(start) {}

  Result<Tick> now() const override { return current_; }

  /// Moves the clock forward by `delta`. Fails with `invalid_argument` when
  /// `delta` is zero and with `limit_exceeded` when the result would wrap.
  Status advance(Tick delta);

  /// The current instant.
  Tick current() const noexcept { return current_; }

 private:
  Tick current_{};
};

/// The operating system's monotonic-to-epoch wall clock, in nanoseconds.
///
/// Provided for operator tooling. It is never used for an authority decision
/// unless the caller explicitly installs it, and it is never used by the test
/// suite.
class SystemClock final : public Clock {
 public:
  SystemClock() = default;
  Result<Tick> now() const override;
};

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_CLOCK_HPP
