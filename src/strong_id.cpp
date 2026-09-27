// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity/strong_id.hpp"

namespace dccp::facility_capacity {
namespace {

bool is_alphanumeric(char character) noexcept {
  return (character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') ||
         (character >= 'a' && character <= 'z');
}

bool is_allowed(char character) noexcept {
  return is_alphanumeric(character) || character == '.' || character == '_' || character == ':' ||
         character == '-';
}

}  // namespace

bool is_valid_identifier(std::string_view text) noexcept {
  return identifier_rejection_reason(text).empty();
}

std::string_view identifier_rejection_reason(std::string_view text) noexcept {
  if (text.empty()) {
    return "identifier is empty";
  }
  if (text.size() > limits::max_identifier_length) {
    return "identifier is longer than max_identifier_length";
  }
  if (!is_alphanumeric(text.front())) {
    return "identifier must begin with a letter or a digit";
  }
  for (const char character : text) {
    if (!is_allowed(character)) {
      return "identifier contains a character outside the accepted alphabet";
    }
  }
  if (text == "." || text == "..") {
    return "identifier is a relative path component";
  }
  return std::string_view();
}

}  // namespace dccp::facility_capacity
