// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity/clock.hpp"

#include <chrono>

#include "internal/checked.hpp"

namespace dccp::facility_capacity {

Clock::~Clock() = default;

Status ManualClock::advance(Tick delta) {
  if (delta.is_zero()) {
    return Error::make(ErrorCode::invalid_argument, "a manual clock cannot advance by zero");
  }
  const Result<std::uint64_t> next =
      internal::add_u64(current_.value(), delta.value(), "manual clock within uint64");
  if (!next.has_value()) {
    return next.error();
  }
  current_ = Tick::from_value(next.value());
  return Status::success();
}

Result<Tick> SystemClock::now() const {
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
  if (nanoseconds < 0) {
    return Error::make(ErrorCode::unavailable, "the system clock reports a negative epoch offset");
  }
  return Tick::from_value(static_cast<std::uint64_t>(nanoseconds));
}

}  // namespace dccp::facility_capacity
