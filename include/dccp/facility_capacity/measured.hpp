// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Measured: an exact non-negative quantity that is either measured or not
// measured.
//
// This type is the whole of the "unknown is not zero" rule. A `Measured` that
// is not known carries no magnitude at all, so there is no code path that can
// read a zero out of an unmeasured value. Arithmetic propagates the absence:
// anything combined with an unmeasured value is unmeasured, never zero.

#ifndef DCCP_FACILITY_CAPACITY_MEASURED_HPP
#define DCCP_FACILITY_CAPACITY_MEASURED_HPP

#include <cstdint>
#include <iosfwd>
#include <string>

#include "dccp/facility_capacity/result.hpp"
#include "dccp/facility_capacity/units.hpp"

namespace dccp::facility_capacity {

class Measured {
 public:
  constexpr Measured() noexcept = default;

  /// An unmeasured value. Not zero.
  static constexpr Measured unknown() noexcept { return Measured(); }

  /// A measured value. Requires a non-negative magnitude and a representable
  /// magnitude; otherwise fails with `invalid_argument` or `limit_exceeded`.
  static Result<Measured> known(Unit unit, std::int64_t magnitude);

  /// A measured value from an existing quantity, requiring the same rules.
  static Result<Measured> known(const Quantity& quantity);

  /// A measured zero in `unit`.
  static constexpr Measured known_zero(Unit unit) noexcept {
    Measured measured;
    measured.unit_ = unit;
    measured.magnitude_ = 0;
    measured.known_ = true;
    return measured;
  }

  constexpr bool is_known() const noexcept { return known_; }

  constexpr bool is_unknown() const noexcept { return !known_; }

  /// True only when the value is measured and exactly zero. An unmeasured value
  /// is never reported as zero.
  constexpr bool is_known_zero() const noexcept { return known_ && magnitude_ == 0; }

  /// True only when the value is measured and strictly positive.
  constexpr bool is_known_positive() const noexcept { return known_ && magnitude_ > 0; }

  /// The unit. A default-constructed value reports `rack_unit`; callers must
  /// check `is_known()` first and take the unit from the surrounding evidence.
  constexpr Unit unit() const noexcept { return unit_; }

  /// The exact magnitude. Throws `ResultMisuse` when the value is unmeasured,
  /// so an unmeasured value can never be read as a number by accident.
  std::int64_t magnitude() const;

  /// The quantity, or a failure when unmeasured.
  Result<Quantity> to_quantity() const;

  /// Exact sum. Unmeasured plus anything is unmeasured.
  ///
  /// This is a total function, so it has no way to report a failure. When the
  /// two operands are measured in *different units* it returns unmeasured
  /// rather than a number: a dimension can never be silently conflated with
  /// another. Use `checked_add` when the mismatch itself has to be reported.
  Measured add(const Measured& other) const;

  /// Exact sum, reporting `unit_mismatch` when the operands are measured in
  /// different units. An unmeasured operand yields an unmeasured result, which
  /// is not a failure.
  Result<Measured> checked_add(const Measured& other) const;

  /// Exact difference. Unmeasured in either operand yields unmeasured. Two
  /// measured operands in different units fail with `unit_mismatch`. A measured
  /// result below zero fails with `account_mismatch` and names the violated
  /// constraint, because a negative capacity is not a value.
  Result<Measured> subtract(const Measured& other, std::string_view constraint) const;

  /// Exact scaling. Unmeasured scales to unmeasured.
  Result<Measured> scale(std::int64_t factor) const;

  /// Returns this value when measured, otherwise `fallback`.
  Measured or_else(const Measured& fallback) const;

  friend constexpr bool operator==(const Measured& lhs, const Measured& rhs) noexcept {
    if (lhs.known_ != rhs.known_) {
      return false;
    }
    if (!lhs.known_) {
      return true;
    }
    return lhs.unit_ == rhs.unit_ && lhs.magnitude_ == rhs.magnitude_;
  }

  /// `unknown` renders as the literal `unknown`.
  std::string to_string() const;

 private:
  Unit unit_ = Unit::rack_unit;
  std::int64_t magnitude_ = 0;
  bool known_ = false;
};

std::ostream& operator<<(std::ostream& out, const Measured& measured);

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_MEASURED_HPP
