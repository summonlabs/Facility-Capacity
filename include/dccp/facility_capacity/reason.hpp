// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Stable reason codes and the notes that carry them.
//
// A reason code is a machine-readable statement about why an answer is what it
// is. Reason codes are part of the public contract: they are never reused for a
// different meaning and never renamed within a major version.

#ifndef DCCP_FACILITY_CAPACITY_REASON_HPP
#define DCCP_FACILITY_CAPACITY_REASON_HPP

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_capacity/dimension.hpp"
#include "dccp/facility_capacity/strong_id.hpp"

namespace dccp::facility_capacity {

/// Why the aggregate model answered the way it did.
enum class ReasonCode : std::uint16_t {
  /// No reason applies.
  none = 0,

  // --- Coverage and completeness -----------------------------------------
  /// The dimension is declared required but no evidence source covers it.
  no_source_for_dimension = 1,
  /// A source named as required by the coverage contract is absent.
  source_not_reported = 2,
  /// The dimension is not declared required and no source covers it.
  dimension_not_offered = 3,
  /// The coverage contract requires a different source set than the evidence.
  coverage_contract_changed = 4,

  // --- Authority and freshness -------------------------------------------
  /// The evidence generation is older than the generation already applied.
  source_stale = 10,
  /// A newer evidence record exists for the same source and dimension.
  source_superseded = 11,
  /// The evidence was produced under a different control-plane epoch.
  source_epoch_mismatch = 12,
  /// The evidence was produced by a different controller incarnation.
  source_incarnation_mismatch = 13,
  /// The evidence belongs to a different facility or site.
  source_identity_mismatch = 14,
  /// The snapshot's declared validity window has passed.
  snapshot_expired = 15,
  /// A newer authoritative capacity generation has been published.
  capacity_generation_advanced = 16,

  // --- Observation --------------------------------------------------------
  /// The source declared the dimension but never measured this quantity.
  quantity_not_measured = 20,
  /// The source reports its own operational state as unknown.
  source_state_unknown = 21,
  /// The source reports itself degraded.
  source_degraded = 22,
  /// The source reports itself unavailable; its usable capacity is zero.
  source_unavailable = 23,

  // --- Composition and accounting ----------------------------------------
  /// Every covering source is present and current, and the composed
  /// allocatable capacity is unmeasured.
  composed_unmeasured = 30,
  /// A source's usable, unavailable and residual parts do not sum to its
  /// installed capacity.
  source_closure_violated = 31,
  /// A source's declared protected and reserved roll-ups do not equal the
  /// itemised reserves declared for it.
  reserve_itemisation_mismatch = 32,
  /// A dimension's protected and reserved capacity exceeds its usable capacity.
  allocation_over_committed = 33,
  /// A service constraint floor is not met by the composed allocatable.
  service_floor_violated = 34,
  /// A service constraint floor cannot be evaluated because the composed
  /// allocatable capacity is unmeasured.
  service_floor_indeterminate = 35,

  // --- Outcome attribution ------------------------------------------------
  /// The dimension's allocatable capacity is measured and strictly positive.
  allocatable_positive = 40,
  /// The dimension's allocatable capacity is measured and exactly zero.
  allocatable_exhausted = 41,
  /// A source is the largest single reduction of the dimension's usable
  /// capacity.
  limiting_by_unavailable = 42,
  /// Declared protection reserves are the largest single reduction of usable
  /// capacity.
  limiting_by_protection = 43,
  /// Declared reserves are the largest single reduction of usable capacity.
  limiting_by_reserve = 44,
  /// Unusable residual is the largest reduction between installed and usable.
  limiting_by_residual = 45,
  /// The binding reduction could not be determined because a value is
  /// unmeasured.
  limiting_indeterminate = 46,

  // --- Persistence --------------------------------------------------------
  /// State was recovered from durable storage and has not been revalidated
  /// against current evidence.
  recovered_not_revalidated = 50,
  /// The recovered state was revalidated and is current.
  recovered_revalidated = 51,
};

/// Stable machine-readable name, e.g. `source_stale`.
std::string_view reason_code_name(ReasonCode code) noexcept;

/// Parses a name produced by `reason_code_name`.
bool parse_reason_code(std::string_view name, ReasonCode& out) noexcept;

std::ostream& operator<<(std::ostream& out, ReasonCode code);

/// One machine-readable reason, with the exact attribution that produced it.
struct CapacityNote {
  ReasonCode code = ReasonCode::none;
  /// Dimension the reason is about, if any.
  CapacityDimension dimension = CapacityDimension::space;
  bool has_dimension = false;
  /// Evidence source the reason is about, if any.
  CapacitySourceId source;
  /// Exact constraint or free-text detail, bounded and empty when not needed.
  std::string detail;

  CapacityNote() = default;

  CapacityNote(ReasonCode code_in, CapacityDimension dimension_in, CapacitySourceId source_in,
               std::string detail_in);

  /// A note with no dimension and no source.
  static CapacityNote plain(ReasonCode code, std::string detail);

  /// Deterministic single-line rendering.
  std::string to_string() const;

  friend bool operator==(const CapacityNote& lhs, const CapacityNote& rhs) noexcept {
    return lhs.code == rhs.code && lhs.has_dimension == rhs.has_dimension &&
           (!lhs.has_dimension || lhs.dimension == rhs.dimension) && lhs.source == rhs.source &&
           lhs.detail == rhs.detail;
  }
};

std::ostream& operator<<(std::ostream& out, const CapacityNote& note);

/// An ordered, duplicate-free, deterministically sorted note set.
///
/// Ordering is by reason code, then dimension ordinal, then source identity,
/// then detail, so two runs that reach the same conclusion render the same
/// bytes.
///
/// Notes are appended cheaply and the ordering is established once, on the
/// first read after a change. Sorting on every insertion would make building a
/// note set quadratic in the number of notes, which is on the hot path of every
/// snapshot composition.
class NoteSet {
 public:
  NoteSet() = default;

  /// Adds a note. Returns false when the bound on note count was reached; the
  /// caller turns that into `limit_exceeded`.
  bool add(CapacityNote note);

  bool empty() const;

  std::size_t size() const;

  const std::vector<CapacityNote>& notes() const;

  const CapacityNote& operator[](std::size_t index) const;

  /// True when a note with this code is present.
  bool contains(ReasonCode code) const;

  /// The first note with this code, or nullptr.
  const CapacityNote* find(ReasonCode code) const;

  /// Merges another set, preserving the ordering and the bound.
  bool merge(const NoteSet& other);

  /// Deterministic multi-line rendering, one note per line.
  std::string to_string() const;

  /// Compares the ordered note sequences. Not `noexcept`: establishing the
  /// ordering on first read can allocate.
  friend bool operator==(const NoteSet& lhs, const NoteSet& rhs) { return lhs.notes() == rhs.notes(); }

 private:
  void ensure_sorted() const;

  // Mutable so that the ordering can be established lazily on the first read
  // after a change, from a const context.
  mutable std::vector<CapacityNote> notes_;
  mutable bool sorted_ = true;
};

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_REASON_HPP
