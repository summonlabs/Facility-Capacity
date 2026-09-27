// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Checked integer arithmetic. Private to the library.
//
// Every capacity computation goes through these helpers. An overflow is a
// reported failure naming the violated constraint, never a wrapped value.

#ifndef DCCP_FACILITY_CAPACITY_SRC_INTERNAL_CHECKED_HPP
#define DCCP_FACILITY_CAPACITY_SRC_INTERNAL_CHECKED_HPP

#include <cstdint>
#include <string_view>

#include "dccp/facility_capacity/result.hpp"

namespace dccp::facility_capacity::internal {

/// `lhs + rhs` with overflow detection.
Result<std::int64_t> add_i64(std::int64_t lhs, std::int64_t rhs, std::string_view constraint);

/// `lhs - rhs` with overflow detection.
Result<std::int64_t> sub_i64(std::int64_t lhs, std::int64_t rhs, std::string_view constraint);

/// `lhs * rhs` with overflow detection.
Result<std::int64_t> mul_i64(std::int64_t lhs, std::int64_t rhs, std::string_view constraint);

/// `lhs + rhs` on unsigned values with overflow detection.
Result<std::uint64_t> add_u64(std::uint64_t lhs, std::uint64_t rhs, std::string_view constraint);

/// `(value * numerator) / denominator` with checked multiplication. The result
/// is truncated toward zero. `denominator` must be non-zero.
Result<std::int64_t> mul_div_i64(std::int64_t value, std::int64_t numerator, std::int64_t denominator,
                                 std::string_view constraint);

/// True when a magnitude is representable as an authoritative quantity.
bool fits_quantity(std::int64_t value) noexcept;

}  // namespace dccp::facility_capacity::internal

#endif  // DCCP_FACILITY_CAPACITY_SRC_INTERNAL_CHECKED_HPP
