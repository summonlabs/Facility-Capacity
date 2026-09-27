// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_capacity/reason.hpp"

#include <algorithm>
#include <array>
#include <ostream>

#include "dccp/facility_capacity/limits.hpp"
#include "internal/text.hpp"

namespace dccp::facility_capacity {
namespace {

struct ReasonEntry {
  ReasonCode code;
  std::string_view name;
};

constexpr std::array<ReasonEntry, 31> kReasons{{
    {ReasonCode::none, "none"},
    {ReasonCode::no_source_for_dimension, "no_source_for_dimension"},
    {ReasonCode::source_not_reported, "source_not_reported"},
    {ReasonCode::dimension_not_offered, "dimension_not_offered"},
    {ReasonCode::coverage_contract_changed, "coverage_contract_changed"},
    {ReasonCode::source_stale, "source_stale"},
    {ReasonCode::source_superseded, "source_superseded"},
    {ReasonCode::source_epoch_mismatch, "source_epoch_mismatch"},
    {ReasonCode::source_incarnation_mismatch, "source_incarnation_mismatch"},
    {ReasonCode::source_identity_mismatch, "source_identity_mismatch"},
    {ReasonCode::snapshot_expired, "snapshot_expired"},
    {ReasonCode::capacity_generation_advanced, "capacity_generation_advanced"},
    {ReasonCode::quantity_not_measured, "quantity_not_measured"},
    {ReasonCode::source_state_unknown, "source_state_unknown"},
    {ReasonCode::source_degraded, "source_degraded"},
    {ReasonCode::source_unavailable, "source_unavailable"},
    {ReasonCode::composed_unmeasured, "composed_unmeasured"},
    {ReasonCode::source_closure_violated, "source_closure_violated"},
    {ReasonCode::reserve_itemisation_mismatch, "reserve_itemisation_mismatch"},
    {ReasonCode::allocation_over_committed, "allocation_over_committed"},
    {ReasonCode::service_floor_violated, "service_floor_violated"},
    {ReasonCode::service_floor_indeterminate, "service_floor_indeterminate"},
    {ReasonCode::allocatable_positive, "allocatable_positive"},
    {ReasonCode::allocatable_exhausted, "allocatable_exhausted"},
    {ReasonCode::limiting_by_unavailable, "limiting_by_unavailable"},
    {ReasonCode::limiting_by_protection, "limiting_by_protection"},
    {ReasonCode::limiting_by_reserve, "limiting_by_reserve"},
    {ReasonCode::limiting_by_residual, "limiting_by_residual"},
    {ReasonCode::limiting_indeterminate, "limiting_indeterminate"},
    {ReasonCode::recovered_not_revalidated, "recovered_not_revalidated"},
    {ReasonCode::recovered_revalidated, "recovered_revalidated"},
}};

}  // namespace

std::string_view reason_code_name(ReasonCode code) noexcept {
  for (const ReasonEntry& entry : kReasons) {
    if (entry.code == code) {
      return entry.name;
    }
  }
  return "unknown_reason_code";
}

bool parse_reason_code(std::string_view name, ReasonCode& out) noexcept {
  for (const ReasonEntry& entry : kReasons) {
    if (entry.name == name) {
      out = entry.code;
      return true;
    }
  }
  return false;
}

std::ostream& operator<<(std::ostream& out, ReasonCode code) { return out << reason_code_name(code); }

CapacityNote::CapacityNote(ReasonCode code_in, CapacityDimension dimension_in, CapacitySourceId source_in,
                           std::string detail_in)
    : code(code_in),
      dimension(dimension_in),
      has_dimension(true),
      source(std::move(source_in)),
      detail(internal::truncate(detail_in, limits::max_detail_length)) {}

CapacityNote CapacityNote::plain(ReasonCode code, std::string detail) {
  CapacityNote note;
  note.code = code;
  note.detail = internal::truncate(detail, limits::max_detail_length);
  return note;
}

std::string CapacityNote::to_string() const {
  std::string out(reason_code_name(code));
  if (has_dimension) {
    out.append(" dimension=");
    out.append(capacity_dimension_name(dimension));
  }
  if (!source.empty()) {
    out.append(" source=");
    out.append(source.value());
  }
  if (!detail.empty()) {
    out.append(" detail=");
    out.append(detail);
  }
  return out;
}

std::ostream& operator<<(std::ostream& out, const CapacityNote& note) { return out << note.to_string(); }

bool NoteSet::add(CapacityNote note) {
  if (notes_.size() >= limits::max_reasons_per_note) {
    return false;
  }
  for (const CapacityNote& existing : notes_) {
    if (existing == note) {
      return true;
    }
  }
  notes_.push_back(std::move(note));
  sorted_ = false;
  return true;
}

void NoteSet::ensure_sorted() const {
  if (sorted_) {
    return;
  }
  std::sort(notes_.begin(), notes_.end(), [](const CapacityNote& lhs, const CapacityNote& rhs) {
    if (lhs.code != rhs.code) {
      return static_cast<std::uint16_t>(lhs.code) < static_cast<std::uint16_t>(rhs.code);
    }
    const std::size_t lhs_ordinal = lhs.has_dimension ? dimension_ordinal(lhs.dimension) : 0;
    const std::size_t rhs_ordinal = rhs.has_dimension ? dimension_ordinal(rhs.dimension) : 0;
    if (lhs_ordinal != rhs_ordinal) {
      return lhs_ordinal < rhs_ordinal;
    }
    if (lhs.source != rhs.source) {
      return lhs.source < rhs.source;
    }
    return lhs.detail < rhs.detail;
  });
  sorted_ = true;
}

bool NoteSet::empty() const {
  ensure_sorted();
  return notes_.empty();
}

std::size_t NoteSet::size() const {
  ensure_sorted();
  return notes_.size();
}

const std::vector<CapacityNote>& NoteSet::notes() const {
  ensure_sorted();
  return notes_;
}

const CapacityNote& NoteSet::operator[](std::size_t index) const {
  ensure_sorted();
  return notes_[index];
}

bool NoteSet::contains(ReasonCode code) const {
  ensure_sorted();
  for (const CapacityNote& note : notes_) {
    if (note.code == code) {
      return true;
    }
  }
  return false;
}

const CapacityNote* NoteSet::find(ReasonCode code) const {
  ensure_sorted();
  for (const CapacityNote& note : notes_) {
    if (note.code == code) {
      return &note;
    }
  }
  return nullptr;
}

bool NoteSet::merge(const NoteSet& other) {
  for (const CapacityNote& note : other.notes()) {
    if (!add(note)) {
      return false;
    }
  }
  return true;
}

std::string NoteSet::to_string() const {
  ensure_sorted();
  std::string out;
  for (const CapacityNote& note : notes_) {
    out.append(note.to_string());
    out.push_back('\n');
  }
  return out;
}

}  // namespace dccp::facility_capacity
