// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "internal/text.hpp"

#include <array>
#include <limits>

#include "dccp/facility_capacity/limits.hpp"

namespace dccp::facility_capacity::internal {
namespace {

Error corruption(std::string_view message) { return Error::make(ErrorCode::corruption, message); }

int hex_digit_value(char character) noexcept {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  return -1;
}

bool all_digits(std::string_view text) noexcept {
  if (text.empty()) {
    return false;
  }
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
  }
  return true;
}

}  // namespace

bool is_ascii_printable(std::string_view text) noexcept {
  for (const char character : text) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte < 0x20u || byte > 0x7Eu) {
      return false;
    }
  }
  return true;
}

std::string truncate(std::string_view text, std::size_t max) {
  if (text.size() <= max) {
    return std::string(text);
  }
  return std::string(text.substr(0, max));
}

Result<std::uint64_t> parse_u64(std::string_view text) {
  if (text.empty() || text.size() > 20) {
    return corruption("integer field is empty or too long");
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return corruption("integer field contains a non-digit");
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
      return corruption("integer field overflows");
    }
    value = value * 10u + digit;
  }
  return value;
}

Result<std::int64_t> parse_i64(std::string_view text) {
  if (text.empty()) {
    return corruption("integer field is empty");
  }
  bool negative = false;
  std::string_view digits = text;
  if (digits.front() == '-') {
    negative = true;
    digits.remove_prefix(1);
  }
  const Result<std::uint64_t> magnitude = parse_u64(digits);
  if (!magnitude.has_value()) {
    return magnitude.error();
  }
  constexpr std::uint64_t kMaxPositive = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  constexpr std::uint64_t kMaxNegative = kMaxPositive + 1u;
  if (negative) {
    if (magnitude.value() > kMaxNegative) {
      return corruption("integer field overflows");
    }
    if (magnitude.value() == kMaxNegative) {
      return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(magnitude.value());
  }
  if (magnitude.value() > kMaxPositive) {
    return corruption("integer field overflows");
  }
  return static_cast<std::int64_t>(magnitude.value());
}

Result<bool> parse_bool(std::string_view text) {
  if (text == "0") {
    return false;
  }
  if (text == "1") {
    return true;
  }
  return corruption("boolean field is not 0 or 1");
}

std::string_view render_bool(bool value) noexcept { return value ? std::string_view("1") : std::string_view("0"); }

std::vector<std::string_view> split_lines(std::string_view text) {
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t newline = text.find('\n', start);
    if (newline == std::string_view::npos) {
      lines.push_back(text.substr(start));
      return lines;
    }
    lines.push_back(text.substr(start, newline - start));
    start = newline + 1;
  }
  return lines;
}

bool starts_with(std::string_view text, std::string_view prefix) noexcept {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string escape_value(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size());
  for (const char character : value) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte >= 0x20u && byte <= 0x7Eu && character != '\\' && character != '=') {
      out.push_back(character);
      continue;
    }
    out.push_back('\\');
    out.push_back('x');
    out.push_back(kHex[(byte >> 4) & 0x0Fu]);
    out.push_back(kHex[byte & 0x0Fu]);
  }
  return out;
}

Result<std::string> unescape_value(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char character = text[index];
    if (character != '\\') {
      out.push_back(character);
      continue;
    }
    if (index + 3 >= text.size() || text[index + 1] != 'x') {
      return corruption("malformed escape sequence in a field value");
    }
    const int high = hex_digit_value(text[index + 2]);
    const int low = hex_digit_value(text[index + 3]);
    if (high < 0 || low < 0) {
      return corruption("malformed escape sequence in a field value");
    }
    out.push_back(static_cast<char>((high << 4) | low));
    index += 3;
  }
  return out;
}

bool is_generation_file_name(std::string_view text) noexcept {
  constexpr std::string_view kPrefix = "gen-";
  constexpr std::string_view kSuffix = ".fcs";
  if (text.size() <= kPrefix.size() + kSuffix.size()) {
    return false;
  }
  if (!starts_with(text, kPrefix)) {
    return false;
  }
  if (text.substr(text.size() - kSuffix.size()) != kSuffix) {
    return false;
  }
  const std::string_view digits = text.substr(kPrefix.size(), text.size() - kPrefix.size() - kSuffix.size());
  return all_digits(digits) && digits.size() <= 20;
}

bool is_staging_file_name(std::string_view text) noexcept {
  constexpr std::string_view kPrefix = "staging-";
  constexpr std::string_view kSuffix = ".tmp";
  if (text.size() <= kPrefix.size() + kSuffix.size()) {
    return false;
  }
  if (!starts_with(text, kPrefix)) {
    return false;
  }
  if (text.substr(text.size() - kSuffix.size()) != kSuffix) {
    return false;
  }
  const std::string_view body = text.substr(kPrefix.size(), text.size() - kPrefix.size() - kSuffix.size());
  const std::size_t separator = body.find('-');
  if (separator == std::string_view::npos) {
    return false;
  }
  const std::string_view first = body.substr(0, separator);
  const std::string_view second = body.substr(separator + 1);
  return all_digits(first) && all_digits(second);
}

}  // namespace dccp::facility_capacity::internal
