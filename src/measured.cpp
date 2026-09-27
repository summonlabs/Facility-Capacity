// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity/measured.hpp"

#include <ostream>

#include "internal/checked.hpp"

namespace dccp::facility_capacity {
namespace {

Error negative_error(std::string_view constraint) {
  Error error = Error::make(ErrorCode::account_mismatch, "capacity accounting produced a negative quantity");
  error.with_constraint(std::string(constraint));
  return error;
}

}  // namespace

Result<Measured> Measured::known(Unit unit, std::int64_t magnitude) {
  if (magnitude < 0) {
    Error error = Error::make(ErrorCode::invalid_argument, "a measured capacity quantity cannot be negative");
    error.with_constraint("magnitude >= 0");
    return error;
  }
  if (!internal::fits_quantity(magnitude)) {
    Error error = Error::make(ErrorCode::limit_exceeded, "measured magnitude is outside the accepted range");
    error.with_constraint("magnitude <= max_quantity_magnitude");
    return error;
  }
  Measured measured;
  measured.unit_ = unit;
  measured.magnitude_ = magnitude;
  measured.known_ = true;
  return measured;
}

Result<Measured> Measured::known(const Quantity& quantity) {
  return Measured::known(quantity.unit(), quantity.magnitude());
}

std::int64_t Measured::magnitude() const {
  if (!known_) {
    throw ResultMisuse("Measured::magnitude() called on an unmeasured value");
  }
  return magnitude_;
}

Result<Quantity> Measured::to_quantity() const {
  if (!known_) {
    return Error::make(ErrorCode::not_measured, "the value was never measured");
  }
  return Quantity::make(unit_, magnitude_);
}

Measured Measured::add(const Measured& other) const {
  if (!known_ || !other.known_) {
    return Measured::unknown();
  }
  if (unit_ != other.unit_) {
    // A dimension can never be silently conflated with another dimension. This
    // function cannot report a failure, so it reports the absence of an answer.
    return Measured::unknown();
  }
  const Result<std::int64_t> sum = internal::add_i64(magnitude_, other.magnitude_, "sum within int64");
  if (!sum.has_value()) {
    return Measured::unknown();
  }
  Measured result;
  result.unit_ = unit_;
  result.magnitude_ = sum.value();
  result.known_ = true;
  if (!internal::fits_quantity(result.magnitude_)) {
    // A composed total outside the accepted range is not a usable answer. The
    // sum stays exact but is reported as unmeasured rather than as a wrapped
    // number, so the caller sees "unknown", never a wrong magnitude.
    return Measured::unknown();
  }
  return result;
}

Result<Measured> Measured::checked_add(const Measured& other) const {
  if (!known_ || !other.known_) {
    return Measured::unknown();
  }
  if (unit_ != other.unit_) {
    Error error = Error::make(ErrorCode::unit_mismatch, "measured quantities in different units were combined");
    error.with_constraint("lhs.unit == rhs.unit");
    return error;
  }
  const Measured sum = add(other);
  if (!sum.is_known()) {
    Error error = Error::make(ErrorCode::limit_exceeded,
                              "the exact sum is outside the accepted quantity range");
    error.with_constraint("sum <= max_quantity_magnitude");
    return error;
  }
  return sum;
}

Result<Measured> Measured::subtract(const Measured& other, std::string_view constraint) const {
  if (!known_ || !other.known_) {
    return Measured::unknown();
  }
  if (unit_ != other.unit_) {
    Error error = Error::make(ErrorCode::unit_mismatch, "measured quantities in different units were combined");
    error.with_constraint(std::string(constraint));
    return error;
  }
  const Result<std::int64_t> difference = internal::sub_i64(magnitude_, other.magnitude_, constraint);
  if (!difference.has_value()) {
    return difference.error();
  }
  if (difference.value() < 0) {
    return negative_error(constraint);
  }
  return Measured::known(unit_, difference.value());
}

Result<Measured> Measured::scale(std::int64_t factor) const {
  if (!known_) {
    return Measured::unknown();
  }
  const Result<std::int64_t> product = internal::mul_i64(magnitude_, factor, "product within int64");
  if (!product.has_value()) {
    return product.error();
  }
  if (product.value() < 0) {
    return negative_error("scaled magnitude >= 0");
  }
  return Measured::known(unit_, product.value());
}

Measured Measured::or_else(const Measured& fallback) const { return known_ ? *this : fallback; }

std::string Measured::to_string() const {
  if (!known_) {
    return "unknown";
  }
  return std::to_string(magnitude_) + " " + std::string(unit_symbol(unit_));
}

std::ostream& operator<<(std::ostream& out, const Measured& measured) { return out << measured.to_string(); }

}  // namespace dccp::facility_capacity
