// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The fcap command-line reader.
//
// Nothing here allocates a locale, reads an environment variable, opens a file
// or consults a clock. Parsing is a pure function of the argument vector, which
// is what makes a failure line reproducible from the command line alone.

#include "script.hpp"

#include <utility>

namespace fcapctl {
namespace {

/// True when `token` is written as an option rather than as a value.
bool is_option_token(std::string_view token) noexcept {
  if (token == "-h") {
    return true;
  }
  return token.size() >= 2 && token[0] == '-' && token[1] == '-';
}

std::string option_label(std::string_view name) {
  std::string label = "--";
  label.append(name);
  return label;
}

}  // namespace

Result<std::uint64_t> parse_u64(std::string_view text) {
  if (text.empty()) {
    return Error::make(ErrorCode::invalid_argument, "an empty value is not a decimal integer");
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return Error::make(ErrorCode::invalid_argument, "the value is not a decimal integer");
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (value > (UINT64_MAX - digit) / 10u) {
      return Error::make(ErrorCode::invalid_argument, "the value is out of range for a 64-bit counter");
    }
    value = value * 10u + digit;
  }
  return value;
}

Result<std::int64_t> parse_i64(std::string_view text) {
  bool negative = false;
  if (!text.empty() && text[0] == '-') {
    negative = true;
    text.remove_prefix(1);
  }
  if (text.empty()) {
    return Error::make(ErrorCode::invalid_argument, "the value is not a decimal integer");
  }
  const std::uint64_t limit =
      negative ? static_cast<std::uint64_t>(INT64_MAX) + 1u : static_cast<std::uint64_t>(INT64_MAX);
  std::uint64_t magnitude = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return Error::make(ErrorCode::invalid_argument, "the value is not a decimal integer");
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (magnitude > (limit - digit) / 10u) {
      return Error::make(ErrorCode::invalid_argument, "the value is out of range for a signed 64-bit integer");
    }
    magnitude = magnitude * 10u + digit;
  }
  if (!negative) {
    return static_cast<std::int64_t>(magnitude);
  }
  if (magnitude == static_cast<std::uint64_t>(INT64_MAX) + 1u) {
    return INT64_MIN;
  }
  return -static_cast<std::int64_t>(magnitude);
}

Result<Script> Script::parse(const std::vector<std::string>& tokens) {
  Script script;
  std::size_t index = 0;

  // The command path: the leading positional words, at most two.
  while (index < tokens.size() && script.command_.size() < 2 && !is_option_token(tokens[index])) {
    script.command_.push_back(tokens[index]);
    ++index;
  }

  while (index < tokens.size()) {
    const std::string& token = tokens[index];
    if (token == "--help" || token == "-h") {
      script.help_requested_ = true;
      ++index;
      continue;
    }
    if (token.size() < 2 || token[0] != '-' || token[1] != '-') {
      return Error::make(ErrorCode::invalid_argument, "unexpected argument '" + token + "'");
    }
    const std::string body = token.substr(2);
    const std::size_t equals = body.find('=');
    if (equals != std::string::npos) {
      Entry entry;
      entry.name = body.substr(0, equals);
      entry.value = body.substr(equals + 1);
      if (entry.name.empty()) {
        return Error::make(ErrorCode::invalid_argument, "an option needs a name before '='");
      }
      script.entries_.push_back(std::move(entry));
      ++index;
      continue;
    }
    if (body.empty()) {
      return Error::make(ErrorCode::invalid_argument, "an option is written --name or --name=value");
    }
    Entry entry;
    entry.name = body;
    if (index + 1 < tokens.size() && !is_option_token(tokens[index + 1])) {
      entry.value = tokens[index + 1];
      script.entries_.push_back(std::move(entry));
      index += 2;
      continue;
    }
    script.entries_.push_back(std::move(entry));
    ++index;
  }
  return script;
}

bool Script::command_is(std::string_view first) const noexcept {
  return command_.size() == 1 && std::string_view(command_[0]) == first;
}

bool Script::command_is(std::string_view first, std::string_view second) const noexcept {
  return command_.size() == 2 && std::string_view(command_[0]) == first && std::string_view(command_[1]) == second;
}

std::string Script::command_text() const {
  std::string text;
  for (std::size_t index = 0; index < command_.size(); ++index) {
    if (index != 0) {
      text.push_back(' ');
    }
    text.append(command_[index]);
  }
  return text;
}

bool Script::has_option(std::string_view name) const {
  for (const Entry& entry : entries_) {
    if (std::string_view(entry.name) == name) {
      return true;
    }
  }
  return false;
}

Script::Entry* Script::find_unconsumed(std::string_view name) noexcept {
  for (Entry& entry : entries_) {
    if (!entry.consumed && std::string_view(entry.name) == name) {
      return &entry;
    }
  }
  return nullptr;
}

void Script::note_shape_error(Error error) const {
  if (!shape_error_.has_value()) {
    shape_error_ = std::move(error);
  }
}

std::optional<std::string> Script::option(std::string_view name) {
  Entry* entry = find_unconsumed(name);
  if (entry == nullptr) {
    return std::nullopt;
  }
  entry->consumed = true;
  if (!entry->value.has_value()) {
    note_shape_error(Error::make(ErrorCode::invalid_argument, "option " + option_label(name) + " requires a value"));
    return std::nullopt;
  }
  return entry->value;
}

Result<std::optional<std::string>> Script::option_text(std::string_view name) {
  Entry* entry = find_unconsumed(name);
  if (entry == nullptr) {
    return std::optional<std::string>();
  }
  entry->consumed = true;
  if (!entry->value.has_value()) {
    return Error::make(ErrorCode::invalid_argument, "option " + option_label(name) + " requires a value");
  }
  return std::optional<std::string>(entry->value.value());
}

std::string Script::option_or(std::string_view name, std::string fallback) {
  std::optional<std::string> value = option(name);
  if (!value.has_value()) {
    return fallback;
  }
  return value.value();
}

std::vector<std::string> Script::option_all(std::string_view name) {
  std::vector<std::string> values;
  for (Entry& entry : entries_) {
    if (entry.consumed || std::string_view(entry.name) != name) {
      continue;
    }
    entry.consumed = true;
    if (!entry.value.has_value()) {
      note_shape_error(Error::make(ErrorCode::invalid_argument, "option " + option_label(name) + " requires a value"));
      continue;
    }
    values.push_back(entry.value.value());
  }
  return values;
}

bool Script::flag(std::string_view name) {
  Entry* entry = find_unconsumed(name);
  if (entry == nullptr) {
    return false;
  }
  entry->consumed = true;
  if (entry->value.has_value()) {
    note_shape_error(
        Error::make(ErrorCode::invalid_argument, "flag " + option_label(name) + " does not take a value"));
  }
  return true;
}

Result<std::string> Script::required(std::string_view name) {
  std::optional<std::string> value = option(name);
  if (value.has_value()) {
    return value.value();
  }
  if (shape_error_.has_value()) {
    return shape_error_.value();
  }
  return make_error<std::string>(ErrorCode::invalid_argument, "missing required option " + option_label(name));
}

Result<std::uint64_t> Script::option_u64(std::string_view name) {
  Result<std::optional<std::string>> text = option_text(name);
  if (!text.has_value()) {
    return text.error();
  }
  if (!text.value().has_value()) {
    return make_error<std::uint64_t>(ErrorCode::invalid_argument,
                                     "option " + option_label(name) + " was not given");
  }
  Result<std::uint64_t> parsed = parse_u64(text.value().value());
  if (!parsed.has_value()) {
    return make_error<std::uint64_t>(ErrorCode::invalid_argument,
                                     "option " + option_label(name) + ": " + parsed.error().message());
  }
  return parsed;
}

Result<std::uint64_t> Script::required_u64(std::string_view name) { return option_u64(name); }

Result<std::int64_t> Script::option_i64(std::string_view name) {
  Result<std::optional<std::string>> text = option_text(name);
  if (!text.has_value()) {
    return text.error();
  }
  if (!text.value().has_value()) {
    return make_error<std::int64_t>(ErrorCode::invalid_argument,
                                    "option " + option_label(name) + " was not given");
  }
  Result<std::int64_t> parsed = parse_i64(text.value().value());
  if (!parsed.has_value()) {
    return make_error<std::int64_t>(ErrorCode::invalid_argument,
                                    "option " + option_label(name) + ": " + parsed.error().message());
  }
  return parsed;
}

Status Script::finish() const {
  if (shape_error_.has_value()) {
    return shape_error_.value();
  }
  for (const Entry& entry : entries_) {
    if (!entry.consumed) {
      return Error::make(ErrorCode::invalid_argument, "unknown option " + option_label(entry.name));
    }
  }
  return Status::success();
}

}  // namespace fcapctl
