// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// A small, deterministic command-line reader for the fcap tool.
//
// The reader knows three shapes and nothing else:
//
//   * a command path: at most two leading positional words, e.g. `store init`;
//   * a named option: `--name value` or `--name=value`;
//   * a boolean flag: `--name` with no value.
//
// Every accessor consumes what it returns, so `finish()` can report anything
// the command never asked for. A typo is therefore a usage error and never a
// silently ignored word.
//
// The reader consults no environment, no locale, no working directory and no
// shell: an identical argument vector always produces an identical result.
// `--name=value` is the escape hatch for a value that would otherwise look like
// an option, because `--name value` binds the following token only when that
// token does not itself begin with `--`.

#ifndef FCAPCTL_SCRIPT_HPP
#define FCAPCTL_SCRIPT_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_capacity/result.hpp"

namespace fcapctl {

using dccp::facility_capacity::Error;
using dccp::facility_capacity::ErrorCode;
using dccp::facility_capacity::Result;
using dccp::facility_capacity::Status;

/// A parsed command line: a command path plus the named options that followed.
class Script {
 public:
  Script() = default;

  /// Splits `tokens` (the words after the program name) into a command path and
  /// a list of named options.
  ///
  /// Fails with `invalid_argument` when a token is neither a leading positional
  /// word, `--name`, `--name value`, `--name=value` nor `-h`/`--help`.
  static Result<Script> parse(const std::vector<std::string>& tokens);

  // --- command path -------------------------------------------------------

  /// The leading positional words, at most two, in argument order.
  const std::vector<std::string>& command() const noexcept { return command_; }

  std::size_t command_size() const noexcept { return command_.size(); }

  /// True when the command path is exactly `first`.
  bool command_is(std::string_view first) const noexcept;

  /// True when the command path is exactly `first second`.
  bool command_is(std::string_view first, std::string_view second) const noexcept;

  /// The command path joined by single spaces, e.g. `store init`. Empty when no
  /// positional word was given.
  std::string command_text() const;

  /// True when `-h` or `--help` appeared anywhere on the line.
  bool help_requested() const noexcept { return help_requested_; }

  // --- options ------------------------------------------------------------

  /// True when `--name` was given, whether or not a command consumed it.
  bool has_option(std::string_view name) const;

  /// Consumes the next `--name` and returns its value.
  ///
  /// Returns `nullopt` when the option was not given at all, and also when it
  /// was given with no value - that shape violation is remembered and reported
  /// by `finish()`.
  std::optional<std::string> option(std::string_view name);

  /// Consumes the next `--name` and returns its value, or `fallback`.
  std::string option_or(std::string_view name, std::string fallback);

  /// Consumes `--name` and returns its text: `nullopt` when it was not given,
  /// the value when it was, and `invalid_argument` when it was given with no
  /// value at all.
  Result<std::optional<std::string>> option_text(std::string_view name);

  /// Consumes every `--name` and returns the values in argument order. Used for
  /// repeatable options such as `--require power --require cooling`.
  std::vector<std::string> option_all(std::string_view name);

  /// True when `--name` was given as a flag. A flag given a value
  /// (`--deep=1`) is a usage error reported by `finish()`.
  bool flag(std::string_view name);

  /// Consumes `--name`, which must be present and a decimal unsigned integer.
  Result<std::uint64_t> required_u64(std::string_view name);

  /// Consumes `--name`, which must be present and non-empty.
  Result<std::string> required(std::string_view name);

  /// Consumes `--name`, which must be present and a decimal unsigned integer.
  Result<std::uint64_t> option_u64(std::string_view name);

  /// Consumes `--name`, which must be present and a decimal signed integer.
  Result<std::int64_t> option_i64(std::string_view name);

  /// Every problem the option list still carries: the first shape violation
  /// raised by an accessor, or the first option that no command consumed.
  ///
  /// A command calls this once it has read every option it accepts and before
  /// it does any work, so an unknown or malformed option never reaches the
  /// library or the filesystem.
  Status finish() const;

 private:
  struct Entry {
    std::string name;
    std::optional<std::string> value;
    bool consumed = false;
  };

  void note_shape_error(Error error) const;
  Entry* find_unconsumed(std::string_view name) noexcept;

  std::vector<std::string> command_;
  std::vector<Entry> entries_;
  mutable std::optional<Error> shape_error_;
  bool help_requested_ = false;
};

/// Parses a decimal unsigned magnitude, rejecting signs, padding-free overflow
/// and non-digit input. Locale-independent: only the ASCII digits 0-9 count.
Result<std::uint64_t> parse_u64(std::string_view text);

/// Parses a decimal magnitude with an optional leading `-`.
Result<std::int64_t> parse_i64(std::string_view text);

}  // namespace fcapctl

#endif  // FCAPCTL_SCRIPT_HPP