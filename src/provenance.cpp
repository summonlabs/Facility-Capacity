// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity/provenance.hpp"

#include <ostream>

#include "dccp/facility_capacity/limits.hpp"
#include "internal/text.hpp"

namespace dccp::facility_capacity {
namespace {

bool is_lower_hex(std::string_view text) noexcept {
  for (const char character : text) {
    const bool digit = character >= '0' && character <= '9';
    const bool lower = character >= 'a' && character <= 'f';
    if (!digit && !lower) {
      return false;
    }
  }
  return true;
}

}  // namespace

Status Provenance::validate() const {
  if (source.empty()) {
    return Error::make(ErrorCode::invalid_argument, "provenance has no source identity");
  }
  if (revision.size() > limits::max_revision_length) {
    Error error = Error::make(ErrorCode::limit_exceeded, "provenance revision exceeds the length bound");
    error.with_constraint("revision_length <= max_revision_length");
    return error;
  }
  if (!internal::is_ascii_printable(revision) || revision.find('=') != std::string::npos) {
    return Error::make(ErrorCode::invalid_argument, "provenance revision contains unacceptable characters");
  }
  if (!evidence_digest.empty() && !is_hex_digest(evidence_digest)) {
    return Error::make(ErrorCode::invalid_argument, "provenance evidence digest is not a SHA-256 hex value");
  }
  return Status::success();
}

std::string Provenance::to_string() const {
  std::string out = source.value();
  out.push_back('@');
  out.append(revision.empty() ? std::string("-") : revision);
  out.append(" epoch=");
  out.append(epoch.to_string());
  out.append(" incarnation=");
  out.append(incarnation.to_string());
  out.append(" produced_at=");
  out.append(produced_at.to_string());
  return out;
}

std::ostream& operator<<(std::ostream& out, const Provenance& provenance) {
  return out << provenance.to_string();
}

Result<ValidityWindow> ValidityWindow::create(Tick start, Tick end) {
  if (!(start < end)) {
    Error error = Error::make(ErrorCode::invalid_argument, "a validity window must be non-empty");
    error.with_constraint("start < end");
    return error;
  }
  ValidityWindow window;
  window.start_ = start;
  window.end_ = end;
  return window;
}

ValidityWindow ValidityWindow::unbounded_from(Tick start) {
  ValidityWindow window;
  window.start_ = start;
  window.end_ = max_tick;
  return window;
}

std::string ValidityWindow::to_string() const {
  return "[" + start_.to_string() + "," + end_.to_string() + ")";
}

std::ostream& operator<<(std::ostream& out, const ValidityWindow& window) { return out << window.to_string(); }

bool is_hex_digest(std::string_view text) noexcept {
  return text.size() == 64 && is_lower_hex(text);
}

}  // namespace dccp::facility_capacity
