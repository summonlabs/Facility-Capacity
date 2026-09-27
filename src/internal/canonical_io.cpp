// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "internal/canonical_io.hpp"

#include <charconv>
#include <string>

#include "dccp/facility_capacity/digest.hpp"
#include "dccp/facility_capacity/limits.hpp"
#include "internal/text.hpp"

namespace dccp::facility_capacity::internal {
namespace {

Error corruption(std::string_view message) { return Error::make(ErrorCode::corruption, message); }

Error missing_field(std::string_view key) {
  Error error = Error::make(ErrorCode::corruption, "required canonical field is missing");
  error.with_constraint(std::string(key));
  return error;
}

Error duplicate_field(std::string_view key) {
  Error error = Error::make(ErrorCode::corruption, "canonical field appears more than once");
  error.with_constraint(std::string(key));
  return error;
}

bool is_valid_key(std::string_view key) noexcept {
  if (key.empty() || key.size() > 96) {
    return false;
  }
  for (const char character : key) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte < 0x21u || byte > 0x7Eu || character == '=' || character == '\\') {
      return false;
    }
  }
  return true;
}

}  // namespace

std::string render_u64(std::uint64_t value) { return std::to_string(value); }

std::string indexed_key(std::string_view prefix, std::size_t index, std::string_view suffix) {
  std::string key(prefix);
  key.push_back('.');
  key.append(std::to_string(index));
  key.push_back('.');
  key.append(suffix);
  return key;
}

void CanonicalWriter::field(std::string_view key, std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  buffer_.append(key);
  buffer_.push_back('=');
  // The escaping is applied inline. Building an intermediate string per value
  // would put two heap allocations on every one of the several thousand fields
  // a full snapshot document carries.
  for (const char character : value) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte >= 0x20u && byte <= 0x7Eu && character != '\\' && character != '=') {
      buffer_.push_back(character);
      continue;
    }
    buffer_.push_back('\\');
    buffer_.push_back('x');
    buffer_.push_back(kHex[(byte >> 4) & 0x0Fu]);
    buffer_.push_back(kHex[byte & 0x0Fu]);
  }
  buffer_.push_back('\n');
}

void CanonicalWriter::field(std::string_view key, std::uint64_t value) {
  char digits[24];
  const std::to_chars_result result = std::to_chars(digits, digits + sizeof(digits), value);
  field(key, std::string_view(digits, static_cast<std::size_t>(result.ptr - digits)));
}

void CanonicalWriter::field(std::string_view key, std::int64_t value) {
  char digits[24];
  const std::to_chars_result result = std::to_chars(digits, digits + sizeof(digits), value);
  field(key, std::string_view(digits, static_cast<std::size_t>(result.ptr - digits)));
}

void CanonicalWriter::field(std::string_view key, bool value) { field(key, render_bool(value)); }

void CanonicalWriter::field(std::string_view key, Unit value) { field(key, unit_name(value)); }

void CanonicalWriter::field(std::string_view key, CapacityDimension value) {
  field(key, capacity_dimension_name(value));
}

void CanonicalWriter::field(std::string_view key, ReasonCode value) { field(key, reason_code_name(value)); }

void CanonicalWriter::field(std::string_view key, OperationalState value) {
  field(key, operational_state_name(value));
}

void CanonicalWriter::field(std::string_view key, ReserveKind value) { field(key, reserve_kind_name(value)); }

void CanonicalWriter::field(std::string_view key, CapacityOutcome value) {
  field(key, capacity_outcome_name(value));
}

void CanonicalWriter::field(std::string_view key, ConstraintStatus value) {
  field(key, constraint_status_name(value));
}

void CanonicalWriter::field(std::string_view key, SnapshotFreshness value) {
  field(key, snapshot_freshness_name(value));
}

void CanonicalWriter::field(std::string_view key, const Measured& value) {
  if (value.is_known()) {
    field(key, value.magnitude());
  } else {
    field(key, std::string_view("unknown"));
  }
}

std::string CanonicalWriter::digest_hex() const { return digest::sha256_hex(buffer_); }

std::string CanonicalWriter::finish() {
  // The digest covers every byte before the digest line, so it is computed
  // before the digest line is appended. Computing it afterwards would make the
  // document cover its own digest and no reader could ever verify it.
  const std::string declared = digest_hex();
  buffer_.append("digest=");
  buffer_.append(declared);
  buffer_.push_back('\n');
  return buffer_;
}

Result<CanonicalReader> CanonicalReader::parse(std::string_view body) {
  if (body.size() > limits::max_store_body_bytes) {
    Error error = Error::make(ErrorCode::limit_exceeded, "canonical document exceeds the body bound");
    error.with_constraint("body_bytes <= max_store_body_bytes");
    return error;
  }
  if (body.empty() || body.back() != '\n') {
    return corruption("canonical document is not LF terminated");
  }

  const std::vector<std::string_view> lines = split_lines(body);
  if (lines.size() > limits::max_body_lines) {
    Error error = Error::make(ErrorCode::limit_exceeded, "canonical document has too many lines");
    error.with_constraint("lines <= max_body_lines");
    return error;
  }
  if (lines.empty()) {
    return corruption("canonical document is empty");
  }

  for (const std::string_view line : lines) {
    if (line.size() > limits::max_line_length) {
      Error error = Error::make(ErrorCode::limit_exceeded, "canonical line exceeds the line bound");
      error.with_constraint("line_bytes <= max_line_length");
      return error;
    }
    if (line.find('\r') != std::string_view::npos) {
      return corruption("canonical document contains a carriage return");
    }
  }

  // The final line carries the digest over everything before it.
  const std::string_view digest_line = lines.back();
  constexpr std::string_view kDigestPrefix = "digest=";
  if (!starts_with(digest_line, kDigestPrefix)) {
    return corruption("canonical document does not end with a digest field");
  }
  const std::string_view declared = digest_line.substr(kDigestPrefix.size());
  if (declared.size() != digest::sha256_bytes * 2) {
    return corruption("canonical digest field is not a SHA-256 hex value");
  }
  const std::size_t digest_offset = body.size() - digest_line.size() - 1;
  const std::string computed = digest::sha256_hex(body.substr(0, digest_offset));
  if (computed != declared) {
    Error error = Error::make(ErrorCode::checksum_mismatch, "canonical document digest does not match its bytes");
    error.with_constraint("sha256(body_without_digest) == digest_field");
    return error;
  }

  CanonicalReader reader;
  const std::size_t field_count = lines.size() - 1;
  reader.fields_.reserve(field_count);
  reader.consumed_.assign(field_count, false);
  reader.index_.reserve(field_count);

  for (std::size_t index = 0; index < field_count; ++index) {
    const std::string_view line = lines[index];
    const std::size_t separator = line.find('=');
    if (separator == std::string_view::npos) {
      return corruption("canonical line has no separator");
    }
    const std::string_view key = line.substr(0, separator);
    const std::string_view raw_value = line.substr(separator + 1);
    if (!is_valid_key(key)) {
      return corruption("canonical field name is not acceptable");
    }
    if (reader.index_.find(std::string(key)) != reader.index_.end()) {
      return duplicate_field(key);
    }
    const Result<std::string> value = unescape_value(raw_value);
    if (!value.has_value()) {
      return value.error();
    }
    reader.index_.emplace(std::string(key), index);
    reader.fields_.emplace_back(std::string(key), value.value());
  }

  return reader;
}

Result<std::string_view> CanonicalReader::locate(std::string_view key) const {
  const auto found = index_.find(std::string(key));
  if (found == index_.end()) {
    return missing_field(key);
  }
  const std::size_t position = found->second;
  if (consumed_[position]) {
    return duplicate_field(key);
  }
  consumed_[position] = true;
  return std::string_view(fields_[position].second);
}

Result<std::string_view> CanonicalReader::take(std::string_view key) const { return locate(key); }

Result<std::string> CanonicalReader::take_string(std::string_view key) const {
  const Result<std::string_view> value = locate(key);
  if (!value.has_value()) {
    return value.error();
  }
  return std::string(value.value());
}

Result<std::uint64_t> CanonicalReader::take_u64(std::string_view key) const {
  const Result<std::string_view> value = locate(key);
  if (!value.has_value()) {
    return value.error();
  }
  return parse_u64(value.value());
}

Result<std::int64_t> CanonicalReader::take_i64(std::string_view key) const {
  const Result<std::string_view> value = locate(key);
  if (!value.has_value()) {
    return value.error();
  }
  return parse_i64(value.value());
}

Result<bool> CanonicalReader::take_bool(std::string_view key) const {
  const Result<std::string_view> value = locate(key);
  if (!value.has_value()) {
    return value.error();
  }
  return parse_bool(value.value());
}

Result<Unit> CanonicalReader::take_unit(std::string_view key) const {
  const Result<std::string_view> value = locate(key);
  if (!value.has_value()) {
    return value.error();
  }
  Unit unit = Unit::rack_unit;
  if (!parse_unit(value.value(), unit)) {
    Error error = Error::make(ErrorCode::corruption, "unknown unit name in canonical document");
    error.with_constraint(std::string(key));
    return error;
  }
  return unit;
}

Result<CapacityDimension> CanonicalReader::take_dimension(std::string_view key) const {
  const Result<std::string_view> value = locate(key);
  if (!value.has_value()) {
    return value.error();
  }
  CapacityDimension dimension = CapacityDimension::space;
  if (!parse_capacity_dimension(value.value(), dimension)) {
    Error error = Error::make(ErrorCode::corruption, "unknown capacity dimension in canonical document");
    error.with_constraint(std::string(key));
    return error;
  }
  return dimension;
}

Result<ReasonCode> CanonicalReader::take_reason(std::string_view key) const {
  const Result<std::string_view> value = locate(key);
  if (!value.has_value()) {
    return value.error();
  }
  ReasonCode reason = ReasonCode::none;
  if (!parse_reason_code(value.value(), reason)) {
    Error error = Error::make(ErrorCode::corruption, "unknown reason code in canonical document");
    error.with_constraint(std::string(key));
    return error;
  }
  return reason;
}

Result<OperationalState> CanonicalReader::take_state(std::string_view key) const {
  const Result<std::string_view> value = locate(key);
  if (!value.has_value()) {
    return value.error();
  }
  OperationalState state = OperationalState::nominal;
  if (!parse_operational_state(value.value(), state)) {
    Error error = Error::make(ErrorCode::corruption, "unknown operational state in canonical document");
    error.with_constraint(std::string(key));
    return error;
  }
  return state;
}

Result<ReserveKind> CanonicalReader::take_reserve_kind(std::string_view key) const {
  const Result<std::string_view> value = locate(key);
  if (!value.has_value()) {
    return value.error();
  }
  ReserveKind kind = ReserveKind::protection;
  if (!parse_reserve_kind(value.value(), kind)) {
    Error error = Error::make(ErrorCode::corruption, "unknown reserve kind in canonical document");
    error.with_constraint(std::string(key));
    return error;
  }
  return kind;
}

Result<CapacityOutcome> CanonicalReader::take_outcome(std::string_view key) const {
  const Result<std::string_view> value = locate(key);
  if (!value.has_value()) {
    return value.error();
  }
  CapacityOutcome outcome = CapacityOutcome::unavailable;
  if (!parse_capacity_outcome(value.value(), outcome)) {
    Error error = Error::make(ErrorCode::corruption, "unknown capacity outcome in canonical document");
    error.with_constraint(std::string(key));
    return error;
  }
  return outcome;
}

Result<ConstraintStatus> CanonicalReader::take_constraint_status(std::string_view key) const {
  const Result<std::string_view> value = locate(key);
  if (!value.has_value()) {
    return value.error();
  }
  ConstraintStatus status = ConstraintStatus::inactive;
  if (!parse_constraint_status(value.value(), status)) {
    Error error = Error::make(ErrorCode::corruption, "unknown constraint status in canonical document");
    error.with_constraint(std::string(key));
    return error;
  }
  return status;
}

Result<SnapshotFreshness> CanonicalReader::take_freshness(std::string_view key) const {
  const Result<std::string_view> value = locate(key);
  if (!value.has_value()) {
    return value.error();
  }
  SnapshotFreshness freshness = SnapshotFreshness::issued;
  if (!parse_snapshot_freshness(value.value(), freshness)) {
    Error error = Error::make(ErrorCode::corruption, "unknown snapshot freshness in canonical document");
    error.with_constraint(std::string(key));
    return error;
  }
  return freshness;
}

Result<Measured> CanonicalReader::take_measured(std::string_view key, Unit unit) const {
  const Result<std::string_view> value = locate(key);
  if (!value.has_value()) {
    return value.error();
  }
  if (value.value() == "unknown") {
    return Measured::unknown();
  }
  const Result<std::int64_t> magnitude = parse_i64(value.value());
  if (!magnitude.has_value()) {
    return magnitude.error();
  }
  if (magnitude.value() < 0) {
    return corruption("a measured capacity field is negative");
  }
  return Measured::known(unit, magnitude.value());
}

Result<Quantity> CanonicalReader::take_quantity(std::string_view key, Unit unit) const {
  const Result<std::string_view> value = locate(key);
  if (!value.has_value()) {
    return value.error();
  }
  const Result<std::int64_t> magnitude = parse_i64(value.value());
  if (!magnitude.has_value()) {
    return magnitude.error();
  }
  return Quantity::make(unit, magnitude.value());
}

Result<void> CanonicalReader::expect(std::string_view key, std::string_view expected) const {
  const Result<std::string_view> value = locate(key);
  if (!value.has_value()) {
    return value.error();
  }
  if (value.value() != expected) {
    Error error = Error::make(ErrorCode::corruption, "canonical field has an unexpected value");
    error.with_constraint(std::string(key) + " == " + std::string(expected));
    return error;
  }
  return Status::success();
}

bool CanonicalReader::has(std::string_view key) const {
  return index_.find(std::string(key)) != index_.end();
}

Result<void> CanonicalReader::finish() const {
  for (std::size_t index = 0; index < fields_.size(); ++index) {
    if (!consumed_[index]) {
      Error error = Error::make(ErrorCode::corruption, "canonical document has an unconsumed field");
      error.with_constraint(fields_[index].first);
      return error;
    }
  }
  return Status::success();
}

}  // namespace dccp::facility_capacity::internal
