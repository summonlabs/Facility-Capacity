// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Result<T>: a value or a typed Error. The library never signals an expected
// failure with an exception; exceptions are reserved for programming errors
// such as dereferencing a failed Result.

#ifndef DCCP_FACILITY_CAPACITY_RESULT_HPP
#define DCCP_FACILITY_CAPACITY_RESULT_HPP

#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "dccp/facility_capacity/error.hpp"

namespace dccp::facility_capacity {

/// Thrown only when a caller misuses the API, never for an expected failure.
class ResultMisuse : public std::logic_error {
 public:
  explicit ResultMisuse(const char* what) : std::logic_error(what) {}
};

template <class T>
class Result {
 public:
  using value_type = T;
  using error_type = Error;

  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}  // NOLINT(google-explicit-constructor)

  Result(const Result&) = default;
  Result(Result&&) noexcept(std::is_nothrow_move_constructible_v<T>) = default;
  Result& operator=(const Result&) = default;
  Result& operator=(Result&&) noexcept(std::is_nothrow_move_assignable_v<T>) = default;
  ~Result() = default;

  bool has_value() const noexcept { return storage_.index() == 0; }

  explicit operator bool() const noexcept { return has_value(); }

  T& value() & {
    require_value();
    return std::get<0>(storage_);
  }

  const T& value() const& {
    require_value();
    return std::get<0>(storage_);
  }

  T&& value() && {
    require_value();
    return std::get<0>(std::move(storage_));
  }

  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

  const Error& error() const& {
    require_error();
    return std::get<1>(storage_);
  }

  Error&& error() && {
    require_error();
    return std::get<1>(std::move(storage_));
  }

  /// Returns the contained value, or `fallback` when the result is a failure.
  template <class U>
  T value_or(U&& fallback) const& {
    return has_value() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
  }

 private:
  void require_value() const {
    if (!has_value()) {
      throw ResultMisuse("Result::value() called on a failed Result");
    }
  }

  void require_error() const {
    if (has_value()) {
      throw ResultMisuse("Result::error() called on a successful Result");
    }
  }

  std::variant<T, Error> storage_;
};

/// Specialisation for operations that only report success or failure.
template <>
class Result<void> {
 public:
  using value_type = void;
  using error_type = Error;

  Result() noexcept = default;
  Result(Error error) : error_(std::move(error)), failed_(true) {}  // NOLINT(google-explicit-constructor)

  static Result success() noexcept { return Result(); }

  bool has_value() const noexcept { return !failed_; }

  explicit operator bool() const noexcept { return has_value(); }

  void value() const {
    if (failed_) {
      throw ResultMisuse("Result<void>::value() called on a failed Result");
    }
  }

  const Error& error() const& {
    if (!failed_) {
      throw ResultMisuse("Result<void>::error() called on a successful Result");
    }
    return error_;
  }

 private:
  Error error_;
  bool failed_ = false;
};

using Status = Result<void>;

/// Builds a failed Status with a message.
inline Status make_status(ErrorCode code, std::string_view message) {
  return Status(Error::make(code, message));
}

/// Builds a failed Result<T> with a message.
template <class T>
Result<T> make_error(ErrorCode code, std::string_view message) {
  return Result<T>(Error::make(code, message));
}

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_RESULT_HPP
