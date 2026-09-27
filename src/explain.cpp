// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Deterministic explanations.
//
// Every rendering here is a pure function of its argument: no clock, no
// locale, no address, no floating point and no iteration over an unordered
// container. Two runs over equal state produce equal bytes, which is what makes
// an explanation usable as evidence and diffable.

#include "dccp/facility_capacity/explain.hpp"

#include <cstddef>
#include <ostream>
#include <string>
#include <string_view>

#include "dccp/facility_capacity/dimension.hpp"
#include "dccp/facility_capacity/evidence.hpp"
#include "dccp/facility_capacity/generation.hpp"
#include "dccp/facility_capacity/measured.hpp"
#include "dccp/facility_capacity/reason.hpp"
#include "dccp/facility_capacity/reserve.hpp"
#include "dccp/facility_capacity/units.hpp"
#include "internal/text.hpp"

namespace dccp::facility_capacity {
namespace {

/// Renders empty text as `-`, the placeholder used throughout an explanation.
std::string_view or_dash(std::string_view text) noexcept {
  return text.empty() ? std::string_view("-") : text;
}

/// Appends ` name=value`.
void append_named(std::string& out, std::string_view name, std::string_view value) {
  out.push_back(' ');
  out.append(name);
  out.push_back('=');
  out.append(value);
}

/// Appends `name=value\n`.
void append_field(std::string& out, std::string_view name, std::string_view value) {
  out.append(name);
  out.push_back('=');
  out.append(value);
  out.push_back('\n');
}

/// Appends `  name=value\n`.
void append_indented_field(std::string& out, std::string_view name, std::string_view value) {
  out.append("  ");
  append_field(out, name, value);
}

/// The recorded attribution of one dimension, or nullptr when the snapshot
/// carries none.
const DimensionLimit* find_limit(const CapacitySnapshot& snapshot, CapacityDimension dimension) {
  for (const DimensionLimit& limit : snapshot.limits()) {
    if (limit.dimension == dimension) {
      return &limit;
    }
  }
  return nullptr;
}

/// One composed quantity of a dimension, or the literal `unknown` when the
/// snapshot carries no totals for that dimension at all.
std::string quantity_or_unknown(const DimensionTotals* totals, const Measured DimensionTotals::*field) {
  if (totals == nullptr) {
    return "unknown";
  }
  return (totals->*field).to_string();
}

/// One dimension's answer: the classification, the composed totals, the exact
/// provenance counts and the recorded attribution of the dominant reduction.
std::string render_dimension_block(const CapacitySnapshot& snapshot, CapacityDimension dimension) {
  const DimensionTotals* totals = snapshot.find_totals(dimension);
  const Unit unit = totals != nullptr ? totals->unit : canonical_unit(dimension);

  std::string out("dimension ");
  out.append(capacity_dimension_name(dimension));
  out.append(" outcome=");
  out.append(capacity_outcome_name(snapshot.outcome(dimension)));
  out.push_back('\n');

  append_indented_field(out, "unit", unit_name(unit));
  append_indented_field(out, "installed", quantity_or_unknown(totals, &DimensionTotals::installed));
  append_indented_field(out, "observed", quantity_or_unknown(totals, &DimensionTotals::observed));
  append_indented_field(out, "usable", quantity_or_unknown(totals, &DimensionTotals::usable));
  append_indented_field(out, "protected", quantity_or_unknown(totals, &DimensionTotals::protected_capacity));
  append_indented_field(out, "reserved", quantity_or_unknown(totals, &DimensionTotals::reserved));
  append_indented_field(out, "unavailable", quantity_or_unknown(totals, &DimensionTotals::unavailable));
  append_indented_field(out, "residual", quantity_or_unknown(totals, &DimensionTotals::residual));
  append_indented_field(out, "allocatable", quantity_or_unknown(totals, &DimensionTotals::allocatable));
  append_indented_field(
      out, "sources",
      std::to_string(totals != nullptr ? totals->contributing_sources : std::size_t{0}));
  append_indented_field(out, "allocation_closed",
                        internal::render_bool(totals != nullptr && totals->allocation_closed));
  append_indented_field(out, "physical_closed",
                        internal::render_bool(totals != nullptr && totals->physical_closed));

  const DimensionLimit* limit = find_limit(snapshot, dimension);
  out.append("  limit reason=");
  out.append(limit != nullptr ? reason_code_name(limit->reason) : reason_code_name(ReasonCode::none));
  out.append(" source=");
  out.append(limit != nullptr ? or_dash(limit->source.value()) : std::string_view("-"));
  out.append(" amount=");
  out.append(limit != nullptr ? limit->amount.to_string() : std::string("unknown"));
  out.push_back('\n');
  return out;
}

}  // namespace

// --- composable parts -------------------------------------------------------

std::string DimensionTotals::to_string() const {
  std::string out(capacity_dimension_name(dimension));
  append_named(out, "unit", unit_name(unit));
  append_named(out, "installed", installed.to_string());
  append_named(out, "observed", observed.to_string());
  append_named(out, "usable", usable.to_string());
  append_named(out, "protected", protected_capacity.to_string());
  append_named(out, "reserved", reserved.to_string());
  append_named(out, "unavailable", unavailable.to_string());
  append_named(out, "residual", residual.to_string());
  append_named(out, "allocatable", allocatable.to_string());
  append_named(out, "sources", std::to_string(contributing_sources));
  append_named(out, "measured_usable", std::to_string(measured_usable_sources));
  append_named(out, "allocation_closed", internal::render_bool(allocation_closed));
  append_named(out, "physical_closed", internal::render_bool(physical_closed));
  return out;
}

std::string SourceGenerationStamp::to_string() const {
  std::string out = source.value();
  append_named(out, "dimension", capacity_dimension_name(dimension));
  append_named(out, "generation", generation.to_string());
  append_named(out, "state", operational_state_name(state));
  append_named(out, "observed_at", observed_at.to_string());
  append_named(out, "epoch", epoch.to_string());
  append_named(out, "incarnation", incarnation.to_string());
  append_named(out, "evidence_digest", or_dash(evidence_digest));
  append_named(out, "source_revision", or_dash(source_revision));
  append_named(out, "source_digest", or_dash(source_evidence_digest));
  append_named(out, "itemised_reserves", std::to_string(itemised_reserves));
  append_named(out, "rollup", internal::render_bool(used_declared_rollup));
  return out;
}

std::string ReserveStamp::to_string() const {
  std::string out = id.value();
  append_named(out, "source", source.value());
  append_named(out, "dimension", capacity_dimension_name(dimension));
  append_named(out, "kind", reserve_kind_name(kind));
  append_named(out, "unit", unit_name(unit));
  append_named(out, "amount", amount.to_string());
  append_named(out, "window", window.to_string());
  append_named(out, "owner", owner.value());
  append_named(out, "service_class", or_dash(service_class.value()));
  append_named(out, "reason", reason_code_name(reason));
  append_named(out, "digest", or_dash(reserve_digest));
  return out;
}

std::string ConstraintEvaluation::to_string() const {
  std::string out = id.value();
  append_named(out, "dimension", capacity_dimension_name(dimension));
  append_named(out, "unit", unit_name(unit));
  append_named(out, "status", constraint_status_name(status));
  append_named(out, "floor", floor_value.to_string());
  append_named(out, "allocatable", allocatable.to_string());
  append_named(out, "reason", reason_code_name(reason));
  if (!service_class.empty()) {
    append_named(out, "service_class", service_class.value());
  }
  if (!detail.empty()) {
    append_named(out, "detail", detail);
  }
  return out;
}

std::string DimensionLimit::to_string() const {
  std::string out(capacity_dimension_name(dimension));
  append_named(out, "unit", unit_name(unit));
  append_named(out, "reason", reason_code_name(reason));
  append_named(out, "source", or_dash(source.value()));
  append_named(out, "amount", amount.to_string());
  append_named(out, "installed", installed.to_string());
  append_named(out, "usable", usable.to_string());
  append_named(out, "allocatable", allocatable.to_string());
  return out;
}

std::string LimitingConstraint::to_string() const {
  if (!present) {
    return "none";
  }
  std::string out("dimension=");
  out.append(capacity_dimension_name(dimension));
  out.append(" headroom_permille=");
  out.append(std::to_string(headroom_permille));
  return out;
}

// --- renderings -------------------------------------------------------------

namespace explain {

std::string render_notes(const NoteSet& notes) { return notes.to_string(); }

std::string render_snapshot(const CapacitySnapshot& snapshot) {
  std::string out;
  append_field(out, "facility", snapshot.facility().value());
  append_field(out, "site", snapshot.site().value());
  append_field(out, "generation", snapshot.generation().to_string());
  append_field(out, "revision", snapshot.revision().to_string());
  append_field(out, "epoch", snapshot.epoch().to_string());
  append_field(out, "incarnation", snapshot.incarnation().to_string());
  append_field(out, "built_at", snapshot.built_at().to_string());
  append_field(out, "valid_until", snapshot.valid_until() == max_tick ? std::string("unbounded")
                                                                     : snapshot.valid_until().to_string());
  append_field(out, "freshness", snapshot_freshness_name(snapshot.freshness()));
  append_field(out, "id", or_dash(snapshot.id().value()));
  append_field(out, "digest", or_dash(snapshot.digest()));

  std::string completeness;
  completeness.append(completeness_class_name(snapshot.completeness().classification));
  completeness.append(" required=");
  completeness.append(std::to_string(snapshot.completeness().required_dimensions));
  completeness.append(" complete=");
  completeness.append(std::to_string(snapshot.completeness().complete_dimensions));
  append_field(out, "completeness", completeness);

  append_field(out, "accounting_closed", internal::render_bool(snapshot.accounting_closed()));

  const CapacityDimension* dimensions = all_capacity_dimensions();
  for (std::size_t index = 0; index < capacity_dimension_count; ++index) {
    out.append(render_dimension_block(snapshot, dimensions[index]));
  }

  out.append("limiting ");
  out.append(snapshot.limiting().to_string());
  out.push_back('\n');

  for (const CapacityNote& note : snapshot.notes().notes()) {
    out.append("note ");
    out.append(note.to_string());
    out.push_back('\n');
  }
  for (const SourceGenerationStamp& stamp : snapshot.sources()) {
    out.append("source ");
    out.append(stamp.to_string());
    out.push_back('\n');
  }
  if (!snapshot.reserves().empty()) {
    for (const ReserveStamp& reserve : snapshot.reserves()) {
      out.append("reserve ");
      out.append(reserve.to_string());
      out.push_back('\n');
    }
  }
  for (const ConstraintEvaluation& evaluation : snapshot.constraints()) {
    out.append("constraint ");
    out.append(evaluation.to_string());
    out.push_back('\n');
  }
  return out;
}

std::string render_dimension(const CapacitySnapshot& snapshot, CapacityDimension dimension) {
  std::string out = render_dimension_block(snapshot, dimension);
  for (const CapacityNote& note : snapshot.notes().notes()) {
    if (note.has_dimension && note.dimension == dimension) {
      out.append("note ");
      out.append(note.to_string());
      out.push_back('\n');
    }
  }
  for (const ReserveStamp& reserve : snapshot.reserves()) {
    if (reserve.dimension == dimension) {
      out.append("reserve ");
      out.append(reserve.to_string());
      out.push_back('\n');
    }
  }
  for (const SourceGenerationStamp& stamp : snapshot.sources()) {
    if (stamp.dimension == dimension) {
      out.append("source ");
      out.append(stamp.to_string());
      out.push_back('\n');
    }
  }
  return out;
}

std::string render_answer(const CapacityAnswer& answer) {
  std::string out;
  append_field(out, "outcome", capacity_outcome_name(answer.outcome));
  append_field(out, "dimension", capacity_dimension_name(answer.dimension));
  append_field(out, "unit", unit_name(answer.unit));
  append_field(out, "generation", answer.generation.to_string());
  append_field(out, "snapshot_digest", or_dash(answer.snapshot_digest));
  append_field(out, "installed", answer.totals.installed.to_string());
  append_field(out, "observed", answer.totals.observed.to_string());
  append_field(out, "usable", answer.totals.usable.to_string());
  append_field(out, "protected", answer.totals.protected_capacity.to_string());
  append_field(out, "reserved", answer.totals.reserved.to_string());
  append_field(out, "unavailable", answer.totals.unavailable.to_string());
  append_field(out, "residual", answer.totals.residual.to_string());
  append_field(out, "allocatable", answer.totals.allocatable.to_string());
  append_field(out, "sources", std::to_string(answer.totals.contributing_sources));
  append_field(out, "measured_usable", std::to_string(answer.totals.measured_usable_sources));
  append_field(out, "allocation_closed", internal::render_bool(answer.totals.allocation_closed));
  append_field(out, "physical_closed", internal::render_bool(answer.totals.physical_closed));
  for (const CapacityNote& note : answer.reasons.notes()) {
    out.append("note ");
    out.append(note.to_string());
    out.push_back('\n');
  }
  return out;
}

std::string render_revalidation(const RevalidationReport& report) {
  std::string out;
  append_field(out, "status", revalidation_status_name(report.status));
  append_field(out, "snapshot_generation", report.snapshot_generation.to_string());
  append_field(out, "current_generation", report.current_generation.to_string());
  append_field(out, "current_revision", report.current_revision.to_string());
  for (const CapacityNote& note : report.findings.notes()) {
    out.append("finding ");
    out.append(note.to_string());
    out.push_back('\n');
  }
  return out;
}

std::string render_diff(const SnapshotDiff& diff) { return diff.describe(); }

std::string render_quantity_change(const QuantityChange& change) { return change.to_string(); }

}  // namespace explain

}  // namespace dccp::facility_capacity
