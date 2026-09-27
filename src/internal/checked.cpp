// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "internal/checked.hpp"

#include <cstdint>
#include <limits>

#include "dccp/facility_capacity/limits.hpp"

namespace dccp::facility_capacity::internal {
namespace {

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();
constexpr std::uint64_t kUInt64Max = std::numeric_limits<std::uint64_t>::max();

Error overflow_error(std::string_view constraint) {
  Error error = Error::make(ErrorCode::limit_exceeded, "checked arithmetic overflow");
  error.with_constraint(std::string(constraint));
  return error;
}

/// Absolute value of a signed 64-bit value as an unsigned magnitude. Correct
/// for INT64_MIN, which has no positive counterpart.
std::uint64_t magnitude_of(std::int64_t value) noexcept {
  if (value < 0) {
    return static_cast<std::uint64_t>(-(value + 1)) + 1u;
  }
  return static_cast<std::uint64_t>(value);
}

}  // namespace

Result<std::int64_t> add_i64(std::int64_t lhs, std::int64_t rhs, std::string_view constraint) {
  if (rhs > 0 && lhs > kInt64Max - rhs) {
    return overflow_error(constraint);
  }
  if (rhs < 0 && lhs < kInt64Min - rhs) {
    return overflow_error(constraint);
  }
  return lhs + rhs;
}

Result<std::int64_t> sub_i64(std::int64_t lhs, std::int64_t rhs, std::string_view constraint) {
  if (rhs > 0 && lhs < kInt64Min + rhs) {
    return overflow_error(constraint);
  }
  if (rhs < 0 && lhs > kInt64Max + rhs) {
    return overflow_error(constraint);
  }
  return lhs - rhs;
}

Result<std::int64_t> mul_i64(std::int64_t lhs, std::int64_t rhs, std::string_view constraint) {
  if (lhs == 0 || rhs == 0) {
    return std::int64_t{0};
  }
  const std::uint64_t lhs_magnitude = magnitude_of(lhs);
  const std::uint64_t rhs_magnitude = magnitude_of(rhs);
  if (lhs_magnitude > kUInt64Max / rhs_magnitude) {
    return overflow_error(constraint);
  }
  const std::uint64_t product = lhs_magnitude * rhs_magnitude;
  const bool negative = (lhs < 0) != (rhs < 0);
  if (negative) {
    const std::uint64_t limit = static_cast<std::uint64_t>(kInt64Max) + 1u;
    if (product > limit) {
      return overflow_error(constraint);
    }
    if (product == limit) {
      return kInt64Min;
    }
    return -static_cast<std::int64_t>(product);
  }
  if (product > static_cast<std::uint64_t>(kInt64Max)) {
    return overflow_error(constraint);
  }
  return static_cast<std::int64_t>(product);
}

Result<std::uint64_t> add_u64(std::uint64_t lhs, std::uint64_t rhs, std::string_view constraint) {
  if (lhs > kUInt64Max - rhs) {
    return overflow_error(constraint);
  }
  return lhs + rhs;
}

Result<std::int64_t> mul_div_i64(std::int64_t value, std::int64_t numerator, std::int64_t denominator,
                                 std::string_view constraint) {
  if (denominator == 0) {
    Error error = Error::make(ErrorCode::invalid_argument, "division by zero in checked arithmetic");
    error.with_constraint(std::string(constraint));
    return error;
  }
  const Result<std::int64_t> product = mul_i64(value, numerator, constraint);
  if (!product.has_value()) {
    return product.error();
  }
  return product.value() / denominator;
}

bool fits_quantity(std::int64_t value) noexcept {
  return value >= -limits::max_quantity_magnitude && value <= limits::max_quantity_magnitude;
}

}  // namespace dccp::facility_capacity::internal
