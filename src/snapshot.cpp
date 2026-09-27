// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Snapshot assembly, accessors and the derivation entry point.
//
// `CapacitySnapshot::build` validates the identity of the input, delegates the
// exact composition to the derivation engine, then binds the derived content to
// a content digest. Nothing about a snapshot is supplied by a caller except its
// inputs, so a snapshot is always internally consistent.

#include "dccp/facility_capacity/snapshot.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "dccp/facility_capacity/explain.hpp"
#include "dccp/facility_capacity/limits.hpp"
#include "internal/derive.hpp"
#include "internal/documents.hpp"
#include "internal/text.hpp"

namespace dccp::facility_capacity {

// ---------------------------------------------------------------------------
// Enum names and parsing
// ---------------------------------------------------------------------------

std::string_view capacity_outcome_name(CapacityOutcome outcome) noexcept {
  switch (outcome) {
    case CapacityOutcome::unavailable:
      return "unavailable";
    case CapacityOutcome::incomplete:
      return "incomplete";
    case CapacityOutcome::unknown:
      return "unknown";
    case CapacityOutcome::exhausted:
      return "exhausted";
    case CapacityOutcome::usable:
      return "usable";
  }
  return "unknown";
}

bool parse_capacity_outcome(std::string_view name, CapacityOutcome& out) noexcept {
  if (name == "unavailable") {
    out = CapacityOutcome::unavailable;
    return true;
  }
  if (name == "incomplete") {
    out = CapacityOutcome::incomplete;
    return true;
  }
  if (name == "unknown") {
    out = CapacityOutcome::unknown;
    return true;
  }
  if (name == "exhausted") {
    out = CapacityOutcome::exhausted;
    return true;
  }
  if (name == "usable") {
    out = CapacityOutcome::usable;
    return true;
  }
  return false;
}

std::ostream& operator<<(std::ostream& out, CapacityOutcome outcome) {
  return out << capacity_outcome_name(outcome);
}

std::string_view constraint_status_name(ConstraintStatus status) noexcept {
  switch (status) {
    case ConstraintStatus::satisfied:
      return "satisfied";
    case ConstraintStatus::violated:
      return "violated";
    case ConstraintStatus::indeterminate:
      return "indeterminate";
    case ConstraintStatus::inactive:
      return "inactive";
  }
  return "inactive";
}

bool parse_constraint_status(std::string_view name, ConstraintStatus& out) noexcept {
  if (name == "satisfied") {
    out = ConstraintStatus::satisfied;
    return true;
  }
  if (name == "violated") {
    out = ConstraintStatus::violated;
    return true;
  }
  if (name == "indeterminate") {
    out = ConstraintStatus::indeterminate;
    return true;
  }
  if (name == "inactive") {
    out = ConstraintStatus::inactive;
    return true;
  }
  return false;
}

std::ostream& operator<<(std::ostream& out, ConstraintStatus status) {
  return out << constraint_status_name(status);
}

std::string_view completeness_class_name(CompletenessClass value) noexcept {
  switch (value) {
    case CompletenessClass::complete:
      return "complete";
    case CompletenessClass::incomplete:
      return "incomplete";
  }
  return "incomplete";
}

std::ostream& operator<<(std::ostream& out, CompletenessClass value) {
  return out << completeness_class_name(value);
}

std::string_view snapshot_freshness_name(SnapshotFreshness value) noexcept {
  switch (value) {
    case SnapshotFreshness::issued:
      return "issued";
    case SnapshotFreshness::recovered:
      return "recovered";
  }
  return "recovered";
}

bool parse_snapshot_freshness(std::string_view name, SnapshotFreshness& out) noexcept {
  if (name == "issued") {
    out = SnapshotFreshness::issued;
    return true;
  }
  if (name == "recovered") {
    out = SnapshotFreshness::recovered;
    return true;
  }
  return false;
}

std::ostream& operator<<(std::ostream& out, SnapshotFreshness value) {
  return out << snapshot_freshness_name(value);
}

namespace {

/// Sorts the input into canonical order and refuses duplicated identities.
///
/// The order is fixed here rather than inherited from the caller so that the
/// same logical input always produces the same bytes, and therefore the same
/// digest, regardless of the order the caller happened to build it in.
Status normalize_input(CapacitySnapshotInput& input) {
  if (input.requirements.size() > limits::max_requirements) {
    return Error::make(ErrorCode::limit_exceeded, "the snapshot declares too many coverage requirements");
  }
  if (input.evidence.size() > limits::max_evidence_sources * capacity_dimension_count) {
    return Error::make(ErrorCode::limit_exceeded, "the snapshot carries too many evidence records");
  }
  if (input.reserves.size() > limits::max_reserves) {
    return Error::make(ErrorCode::limit_exceeded, "the snapshot carries too many declared reserves");
  }
  if (input.constraints.size() > limits::max_service_constraints) {
    return Error::make(ErrorCode::limit_exceeded, "the snapshot carries too many service constraints");
  }

  for (CapacityRequirement& requirement : input.requirements) {
    const Status status = requirement.normalize();
    if (!status.has_value()) {
      return status.error();
    }
  }
  std::sort(input.requirements.begin(), input.requirements.end(),
            [](const CapacityRequirement& lhs, const CapacityRequirement& rhs) {
              return dimension_ordinal(lhs.dimension) < dimension_ordinal(rhs.dimension);
            });
  for (std::size_t index = 1; index < input.requirements.size(); ++index) {
    if (input.requirements[index].dimension == input.requirements[index - 1].dimension) {
      return Error::make(ErrorCode::duplicate_identity,
                         "a dimension has more than one coverage requirement");
    }
  }

  std::sort(input.evidence.begin(), input.evidence.end(),
            [](const SourceEvidence& lhs, const SourceEvidence& rhs) {
              if (lhs.dimension() != rhs.dimension()) {
                return dimension_ordinal(lhs.dimension()) < dimension_ordinal(rhs.dimension());
              }
              return lhs.source() < rhs.source();
            });
  for (std::size_t index = 1; index < input.evidence.size(); ++index) {
    if (input.evidence[index].dimension() == input.evidence[index - 1].dimension() &&
        input.evidence[index].source() == input.evidence[index - 1].source()) {
      return Error::make(ErrorCode::duplicate_identity,
                         "a source declared more than one evidence record for the same dimension");
    }
  }

  std::sort(input.reserves.begin(), input.reserves.end(),
            [](const CapacityReserve& lhs, const CapacityReserve& rhs) {
              if (lhs.dimension() != rhs.dimension()) {
                return dimension_ordinal(lhs.dimension()) < dimension_ordinal(rhs.dimension());
              }
              if (lhs.source() != rhs.source()) {
                return lhs.source() < rhs.source();
              }
              return lhs.id() < rhs.id();
            });
  for (std::size_t index = 1; index < input.reserves.size(); ++index) {
    if (input.reserves[index].id() == input.reserves[index - 1].id()) {
      return Error::make(ErrorCode::duplicate_identity, "a reserve identity was declared twice");
    }
  }

  std::sort(input.constraints.begin(), input.constraints.end(),
            [](const ServiceConstraint& lhs, const ServiceConstraint& rhs) {
              if (lhs.dimension() != rhs.dimension()) {
                return dimension_ordinal(lhs.dimension()) < dimension_ordinal(rhs.dimension());
              }
              return lhs.id() < rhs.id();
            });
  for (std::size_t index = 1; index < input.constraints.size(); ++index) {
    if (input.constraints[index].id() == input.constraints[index - 1].id()) {
      return Error::make(ErrorCode::duplicate_identity, "a service constraint identity was declared twice");
    }
  }

  return Status::success();
}

SnapshotId make_snapshot_id(CapacityGeneration generation, const std::string& digest) {
  std::string value = "g";
  value.append(generation.to_string());
  value.push_back('-');
  value.append(digest.substr(0, 16));
  Result<SnapshotId> id = SnapshotId::parse(value);
  if (!id.has_value()) {
    // The construction above cannot produce an invalid identifier; a failure
    // here would mean the identifier alphabet and this function disagree.
    return SnapshotId();
  }
  return id.value();
}

}  // namespace

Result<std::shared_ptr<const CapacitySnapshot>> CapacitySnapshot::build(CapacitySnapshotInput input) {
  if (input.facility.empty()) {
    return Error::make(ErrorCode::invalid_argument, "a snapshot must name a facility");
  }
  if (input.site.empty()) {
    return Error::make(ErrorCode::invalid_argument, "a snapshot must name a site");
  }
  const Status normalized = normalize_input(input);
  if (!normalized.has_value()) {
    return normalized.error();
  }
  return derive(std::move(input));
}

Result<std::shared_ptr<const CapacitySnapshot>> CapacitySnapshot::derive(CapacitySnapshotInput input) {
  const Result<internal::DerivedSnapshot> derived = internal::derive_content(input);
  if (!derived.has_value()) {
    return derived.error();
  }

  std::shared_ptr<CapacitySnapshot> snapshot(new CapacitySnapshot());
  snapshot->input_ = std::move(input);
  snapshot->facility_ = snapshot->input_.facility;
  snapshot->site_ = snapshot->input_.site;
  snapshot->generation_ = snapshot->input_.generation;
  snapshot->epoch_ = snapshot->input_.epoch;
  snapshot->incarnation_ = snapshot->input_.incarnation;
  snapshot->revision_ = snapshot->input_.revision;
  snapshot->built_at_ = snapshot->input_.built_at;
  snapshot->valid_until_ = snapshot->input_.valid_until;
  snapshot->freshness_ = snapshot->input_.freshness;
  snapshot->requirements_ = snapshot->input_.requirements;
  snapshot->totals_ = derived.value().totals;
  snapshot->outcomes_ = derived.value().outcomes;
  snapshot->sources_ = derived.value().sources;
  snapshot->reserves_ = derived.value().reserves;
  snapshot->constraints_ = derived.value().constraints;
  snapshot->limits_ = derived.value().limits;
  snapshot->notes_ = derived.value().notes;
  snapshot->completeness_ = derived.value().completeness;
  snapshot->limiting_ = derived.value().limiting;
  snapshot->accounting_closed_ = derived.value().accounting_closed;

  internal::CanonicalWriter writer;
  internal::encode_snapshot_fields(writer, *snapshot);
  snapshot->digest_ = writer.digest_hex();
  snapshot->id_ = make_snapshot_id(snapshot->generation_, snapshot->digest_);
  if (snapshot->id_.empty()) {
    return Error::make(ErrorCode::invariant_violation, "a snapshot identifier could not be derived");
  }
  return std::shared_ptr<const CapacitySnapshot>(std::move(snapshot));
}

const DimensionTotals* CapacitySnapshot::find_totals(CapacityDimension dimension) const noexcept {
  for (const DimensionTotals& totals : totals_) {
    if (totals.dimension == dimension) {
      return &totals;
    }
  }
  return nullptr;
}

CapacityOutcome CapacitySnapshot::outcome(CapacityDimension dimension) const noexcept {
  const std::size_t ordinal = dimension_ordinal(dimension);
  if (ordinal < outcomes_.size()) {
    return outcomes_[ordinal];
  }
  return CapacityOutcome::unavailable;
}

Measured CapacitySnapshot::allocatable(CapacityDimension dimension) const {
  const DimensionTotals* totals = find_totals(dimension);
  return totals != nullptr ? totals->allocatable : Measured::unknown();
}

std::string CapacitySnapshot::describe() const { return explain::render_snapshot(*this); }

std::string CapacitySnapshot::explain(CapacityDimension dimension) const {
  return explain::render_dimension(*this, dimension);
}

std::string CapacityAnswer::describe() const { return explain::render_answer(*this); }

std::ostream& operator<<(std::ostream& out, const CapacityAnswer& answer) {
  return out << answer.describe();
}

}  // namespace dccp::facility_capacity
