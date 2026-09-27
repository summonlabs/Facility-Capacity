// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Bounded text helpers. Private to the library.
//
// Everything here is ASCII-only and locale-independent. No helper consults the
// C locale, a code page or a timezone.

#ifndef DCCP_FACILITY_CAPACITY_SRC_INTERNAL_TEXT_HPP
#define DCCP_FACILITY_CAPACITY_SRC_INTERNAL_TEXT_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_capacity/result.hpp"

namespace dccp::facility_capacity::internal {

/// True when every byte is in `[0x20, 0x7E]`.
bool is_ascii_printable(std::string_view text) noexcept;

/// Truncates to at most `max` bytes without splitting an escape sequence; used
/// only for operator-facing detail text, never for an authoritative value.
std::string truncate(std::string_view text, std::size_t max);

/// Parses an unsigned decimal integer. Rejects empty input, signs, leading
/// spaces and overflow.
Result<std::uint64_t> parse_u64(std::string_view text);

/// Parses a signed decimal integer. Rejects empty input, lone signs, leading
/// spaces and overflow.
Result<std::int64_t> parse_i64(std::string_view text);

/// Parses `0` or `1`.
Result<bool> parse_bool(std::string_view text);

/// Renders a boolean as `0` or `1`.
std::string_view render_bool(bool value) noexcept;

/// Splits on `\n`. A trailing terminator does not produce an extra empty line.
/// Any `\r` is rejected by the caller, not stripped here.
std::vector<std::string_view> split_lines(std::string_view text);

/// True when `text` starts with `prefix`.
bool starts_with(std::string_view text, std::string_view prefix) noexcept;

/// Escapes a value for the canonical encoding: printable ASCII except `\` and
/// `=` is emitted unchanged, everything else becomes `\xHH`.
std::string escape_value(std::string_view value);

/// Reverses `escape_value`. Fails with `corruption` on a malformed escape.
Result<std::string> unescape_value(std::string_view text);

/// True when `text` matches `gen-<digits>.fcs` with no other characters.
bool is_generation_file_name(std::string_view text) noexcept;

/// True when `text` matches `staging-<digits>-<digits>.tmp`.
bool is_staging_file_name(std::string_view text) noexcept;

}  // namespace dccp::facility_capacity::internal

#endif  // DCCP_FACILITY_CAPACITY_SRC_INTERNAL_TEXT_HPP
