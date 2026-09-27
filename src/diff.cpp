// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Structural differences between two published answers.
//
// A diff is exact and total. A change is reported only when the underlying
// values differ, a delta only when both sides were measured, and every
// rendering is a pure function of the two snapshots: no clock, no locale, no
// floating point and no iteration over an unordered container.

#include "dccp/facility_capacity/diff.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/facility_capacity/dimension.hpp"
#include "dccp/facility_capacity/limits.hpp"
#include "dccp/facility_capacity/measured.hpp"
#include "dccp/facility_capacity/units.hpp"
#include "internal/checked.hpp"

namespace dccp::facility_capacity {
namespace {

/// Violated constraint reported when an exact delta is not representable.
constexpr std::string_view kDeltaConstraint = "difference within int64";

/// The bound every materialised diff is checked against.
constexpr std::string_view kEntryBound = "dimensions + sources + constraints <= max_diff_entries";

/// Renders empty text as `-`, the placeholder used throughout the diff.
std::string_view or_dash(std::string_view text) noexcept {
  return text.empty() ? std::string_view("-") : text;
}

Result<SnapshotDiff> entry_limit_error() {
  Error error = Error::make(ErrorCode::limit_exceeded, "the diff exceeds the entry bound");
  error.with_constraint(std::string(kEntryBound));
  return error;
}

/// One measured value of a snapshot side, or an unmeasured value when the
/// dimension is absent from that snapshot.
Measured totals_value(const DimensionTotals* totals, const Measured DimensionTotals::*field) {
  if (totals == nullptr) {
    return Measured::unknown();
  }
  return totals->*field;
}

/// Differences one quantity. A side counts as known only when it was measured
/// *and* its magnitude is representable as a quantity.
QuantityChange diff_quantity(const Measured& before_value, const Measured& after_value, Unit unit) {
  QuantityChange change;
  change.before = Quantity::zero(unit);
  change.after = Quantity::zero(unit);

  if (before_value.is_known()) {
    const Result<Quantity> quantity = Quantity::make(unit, before_value.magnitude());
    if (quantity.has_value()) {
      change.before = quantity.value();
      change.before_known = true;
    }
  }
  if (after_value.is_known()) {
    const Result<Quantity> quantity = Quantity::make(unit, after_value.magnitude());
    if (quantity.has_value()) {
      change.after = quantity.value();
      change.after_known = true;
    }
  }

  if (change.before_known && change.after_known) {
    const Result<std::int64_t> delta =
        internal::sub_i64(change.after.magnitude(), change.before.magnitude(), kDeltaConstraint);
    if (delta.has_value()) {
      change.delta = delta.value();
      change.delta_known = true;
    }
  }
  return change;
}

DimensionChange diff_dimension(const CapacitySnapshot& before, const CapacitySnapshot& after,
                               CapacityDimension dimension) {
  const DimensionTotals* before_totals = before.find_totals(dimension);
  const DimensionTotals* after_totals = after.find_totals(dimension);

  DimensionChange change;
  change.dimension = dimension;
  if (before_totals != nullptr) {
    change.unit = before_totals->unit;
  } else if (after_totals != nullptr) {
    change.unit = after_totals->unit;
  } else {
    change.unit = canonical_unit(dimension);
  }

  change.installed = diff_quantity(totals_value(before_totals, &DimensionTotals::installed),
                                   totals_value(after_totals, &DimensionTotals::installed), change.unit);
  change.usable = diff_quantity(totals_value(before_totals, &DimensionTotals::usable),
                                totals_value(after_totals, &DimensionTotals::usable), change.unit);
  change.protected_capacity =
      diff_quantity(totals_value(before_totals, &DimensionTotals::protected_capacity),
                    totals_value(after_totals, &DimensionTotals::protected_capacity), change.unit);
  change.reserved = diff_quantity(totals_value(before_totals, &DimensionTotals::reserved),
                                  totals_value(after_totals, &DimensionTotals::reserved), change.unit);
  change.unavailable = diff_quantity(totals_value(before_totals, &DimensionTotals::unavailable),
                                     totals_value(after_totals, &DimensionTotals::unavailable), change.unit);
  change.residual = diff_quantity(totals_value(before_totals, &DimensionTotals::residual),
                                  totals_value(after_totals, &DimensionTotals::residual), change.unit);
  change.allocatable = diff_quantity(totals_value(before_totals, &DimensionTotals::allocatable),
                                     totals_value(after_totals, &DimensionTotals::allocatable), change.unit);

  change.outcome_before = before.outcome(dimension);
  change.outcome_after = after.outcome(dimension);
  return change;
}

/// Appends ` name=<change>` when the quantity changed.
void append_quantity_change(std::string& out, const char* name, const QuantityChange& change) {
  if (!change.changed()) {
    return;
  }
  out.push_back(' ');
  out.append(name);
  out.push_back('=');
  out.append(change.to_string());
}

/// One source and dimension pair observed in a snapshot.
struct SourceEntry {
  CapacityDimension dimension = CapacityDimension::space;
  CapacitySourceId source;
  EvidenceGeneration generation{};
  OperationalState state = OperationalState::nominal;
};

/// Orders two entries by (dimension ordinal, source id) only: the key a source
/// is unioned by.
bool source_key_less(const SourceEntry& lhs, const SourceEntry& rhs) noexcept {
  if (lhs.dimension != rhs.dimension) {
    return dimension_ordinal(lhs.dimension) < dimension_ordinal(rhs.dimension);
  }
  return lhs.source < rhs.source;
}

bool source_key_equal(const SourceEntry& lhs, const SourceEntry& rhs) noexcept {
  return lhs.dimension == rhs.dimension && lhs.source == rhs.source;
}

/// A total order over the entries, so that the retained entry of a duplicated
/// key never depends on the sort implementation.
bool source_entry_less(const SourceEntry& lhs, const SourceEntry& rhs) noexcept {
  if (source_key_less(lhs, rhs)) {
    return true;
  }
  if (source_key_less(rhs, lhs)) {
    return false;
  }
  if (lhs.generation != rhs.generation) {
    return lhs.generation < rhs.generation;
  }
  return static_cast<std::uint8_t>(lhs.state) < static_cast<std::uint8_t>(rhs.state);
}

std::vector<SourceEntry> collect_sources(const CapacitySnapshot& snapshot) {
  std::vector<SourceEntry> entries;
  entries.reserve(snapshot.sources().size());
  for (const SourceGenerationStamp& stamp : snapshot.sources()) {
    SourceEntry entry;
    entry.dimension = stamp.dimension;
    entry.source = stamp.source;
    entry.generation = stamp.generation;
    entry.state = stamp.state;
    entries.push_back(std::move(entry));
  }
  std::sort(entries.begin(), entries.end(), source_entry_less);
  entries.erase(std::unique(entries.begin(), entries.end(), source_key_equal), entries.end());
  return entries;
}

/// The union of both snapshots' sources, keyed by (dimension, source), holding
/// only the pairs whose presence, generation or state changed.
std::vector<SourceChange> diff_sources(const CapacitySnapshot& before, const CapacitySnapshot& after) {
  const std::vector<SourceEntry> before_entries = collect_sources(before);
  const std::vector<SourceEntry> after_entries = collect_sources(after);

  std::vector<SourceChange> changes;
  std::size_t before_index = 0;
  std::size_t after_index = 0;
  while (before_index < before_entries.size() || after_index < after_entries.size()) {
    bool take_before = false;
    bool take_after = false;
    if (before_index >= before_entries.size()) {
      take_after = true;
    } else if (after_index >= after_entries.size()) {
      take_before = true;
    } else if (source_key_less(before_entries[before_index], after_entries[after_index])) {
      take_before = true;
    } else if (source_key_less(after_entries[after_index], before_entries[before_index])) {
      take_after = true;
    } else {
      take_before = true;
      take_after = true;
    }

    SourceChange change;
    if (take_before) {
      const SourceEntry& entry = before_entries[before_index];
      change.source = entry.source;
      change.dimension = entry.dimension;
      change.present_before = true;
      change.generation_before = entry.generation;
      change.state_before = entry.state;
      ++before_index;
    }
    if (take_after) {
      const SourceEntry& entry = after_entries[after_index];
      change.source = entry.source;
      change.dimension = entry.dimension;
      change.present_after = true;
      change.generation_after = entry.generation;
      change.state_after = entry.state;
      ++after_index;
    }

    if (change.changed()) {
      changes.push_back(std::move(change));
    }
  }
  return changes;
}

/// One declared service constraint observed in a snapshot.
struct ConstraintEntry {
  ConstraintId id;
  CapacityDimension dimension = CapacityDimension::space;
  ConstraintStatus status = ConstraintStatus::inactive;
};

/// A total order over the entries, so that the retained entry of a duplicated
/// identity never depends on the sort implementation.
bool constraint_entry_less(const ConstraintEntry& lhs, const ConstraintEntry& rhs) noexcept {
  if (lhs.id != rhs.id) {
    return lhs.id < rhs.id;
  }
  if (lhs.dimension != rhs.dimension) {
    return dimension_ordinal(lhs.dimension) < dimension_ordinal(rhs.dimension);
  }
  return static_cast<std::uint8_t>(lhs.status) < static_cast<std::uint8_t>(rhs.status);
}

/// Constraints are keyed by identity alone: one entry per declared constraint.
bool constraint_entry_same_id(const ConstraintEntry& lhs, const ConstraintEntry& rhs) noexcept {
  return lhs.id == rhs.id;
}

std::vector<ConstraintEntry> collect_constraints(const CapacitySnapshot& snapshot) {
  std::vector<ConstraintEntry> entries;
  entries.reserve(snapshot.constraints().size());
  for (const ConstraintEvaluation& evaluation : snapshot.constraints()) {
    ConstraintEntry entry;
    entry.id = evaluation.id;
    entry.dimension = evaluation.dimension;
    entry.status = evaluation.status;
    entries.push_back(std::move(entry));
  }
  std::sort(entries.begin(), entries.end(), constraint_entry_less);
  entries.erase(std::unique(entries.begin(), entries.end(), constraint_entry_same_id), entries.end());
  return entries;
}

/// The union of both snapshots' constraints, keyed by `ConstraintId`, holding
/// only the constraints whose status changed, ordered by (dimension, id).
std::vector<ConstraintChange> diff_constraints(const CapacitySnapshot& before, const CapacitySnapshot& after) {
  const std::vector<ConstraintEntry> before_entries = collect_constraints(before);
  const std::vector<ConstraintEntry> after_entries = collect_constraints(after);

  std::vector<ConstraintChange> changes;
  std::size_t before_index = 0;
  std::size_t after_index = 0;
  while (before_index < before_entries.size() || after_index < after_entries.size()) {
    bool take_before = false;
    bool take_after = false;
    if (before_index >= before_entries.size()) {
      take_after = true;
    } else if (after_index >= after_entries.size()) {
      take_before = true;
    } else if (before_entries[before_index].id < after_entries[after_index].id) {
      take_before = true;
    } else if (after_entries[after_index].id < before_entries[before_index].id) {
      take_after = true;
    } else {
      take_before = true;
      take_after = true;
    }

    ConstraintChange change;
    if (take_before) {
      const ConstraintEntry& entry = before_entries[before_index];
      change.id = entry.id;
      change.dimension = entry.dimension;
      change.status_before = entry.status;
      ++before_index;
    }
    if (take_after) {
      const ConstraintEntry& entry = after_entries[after_index];
      change.id = entry.id;
      change.dimension = entry.dimension;
      change.status_after = entry.status;
      ++after_index;
    }

    if (change.changed()) {
      changes.push_back(std::move(change));
    }
  }

  std::sort(changes.begin(), changes.end(), [](const ConstraintChange& lhs, const ConstraintChange& rhs) {
    if (lhs.dimension != rhs.dimension) {
      return dimension_ordinal(lhs.dimension) < dimension_ordinal(rhs.dimension);
    }
    return lhs.id < rhs.id;
  });
  return changes;
}

}  // namespace

bool QuantityChange::changed() const noexcept {
  if (before_known != after_known) {
    return true;
  }
  if (!before_known) {
    return false;
  }
  return before != after;
}

std::string QuantityChange::to_string() const {
  std::string out = before_known ? before.to_string() : std::string("unknown");
  out.append(" -> ");
  out.append(after_known ? after.to_string() : std::string("unknown"));
  if (delta_known) {
    out.append(" (delta ");
    out.append(std::to_string(delta));
    out.push_back(')');
  }
  return out;
}

bool DimensionChange::changed() const noexcept {
  return installed.changed() || usable.changed() || protected_capacity.changed() || reserved.changed() ||
         unavailable.changed() || residual.changed() || allocatable.changed() || outcome_before != outcome_after;
}

std::string DimensionChange::to_string() const {
  std::string out(capacity_dimension_name(dimension));
  append_quantity_change(out, "installed", installed);
  append_quantity_change(out, "usable", usable);
  append_quantity_change(out, "protected_capacity", protected_capacity);
  append_quantity_change(out, "reserved", reserved);
  append_quantity_change(out, "unavailable", unavailable);
  append_quantity_change(out, "residual", residual);
  append_quantity_change(out, "allocatable", allocatable);
  out.append(" outcome=");
  out.append(capacity_outcome_name(outcome_before));
  if (outcome_before != outcome_after) {
    out.append(" -> ");
    out.append(capacity_outcome_name(outcome_after));
  }
  return out;
}

bool SourceChange::changed() const noexcept {
  return present_before != present_after || generation_before != generation_after || state_before != state_after;
}

std::string SourceChange::to_string() const {
  std::string out(capacity_dimension_name(dimension));
  out.push_back('/');
  out.append(source.value());
  out.push_back(' ');
  out.append(present_before ? "present" : "absent");
  if (present_before != present_after) {
    out.append(" -> ");
    out.append(present_after ? "present" : "absent");
  }
  if (present_before != present_after || generation_before != generation_after) {
    out.append(" gen=");
    out.append(generation_before.to_string());
    out.append(" -> gen=");
    out.append(generation_after.to_string());
  } else {
    out.append(" gen=");
    out.append(generation_before.to_string());
  }
  if (state_before != state_after) {
    out.append(" state=");
    out.append(operational_state_name(state_before));
    out.append(" -> ");
    out.append(operational_state_name(state_after));
  } else {
    out.append(" state=");
    out.append(operational_state_name(state_before));
  }
  return out;
}

bool ConstraintChange::changed() const noexcept { return status_before != status_after; }

std::string ConstraintChange::to_string() const {
  std::string out(capacity_dimension_name(dimension));
  out.push_back('/');
  out.append(id.value());
  out.push_back(' ');
  out.append(constraint_status_name(status_before));
  if (status_before != status_after) {
    out.append(" -> ");
    out.append(constraint_status_name(status_after));
  }
  return out;
}

bool SnapshotDiff::empty() const noexcept {
  return dimensions.empty() && sources.empty() && constraints.empty();
}

std::string SnapshotDiff::describe() const {
  std::string out("facility=");
  out.append(facility.value());
  out.push_back('\n');
  out.append("site=");
  out.append(site.value());
  out.push_back('\n');
  out.append("generation ");
  out.append(before_generation.to_string());
  out.append(" -> ");
  out.append(after_generation.to_string());
  out.push_back('\n');
  out.append("digest ");
  out.append(or_dash(before_digest));
  out.append(" -> ");
  out.append(or_dash(after_digest));
  out.push_back('\n');
  for (const DimensionChange& change : dimensions) {
    out.append("dimension: ");
    out.append(change.to_string());
    out.push_back('\n');
  }
  for (const SourceChange& change : sources) {
    out.append("source: ");
    out.append(change.to_string());
    out.push_back('\n');
  }
  for (const ConstraintChange& change : constraints) {
    out.append("constraint: ");
    out.append(change.to_string());
    out.push_back('\n');
  }
  return out;
}

Result<SnapshotDiff> diff_snapshots(const CapacitySnapshot& before, const CapacitySnapshot& after) {
  if (before.facility() != after.facility()) {
    Error error = Error::make(ErrorCode::conflict, "cannot diff snapshots that describe different facilities");
    error.with_constraint("before.facility == after.facility");
    return error;
  }
  if (before.site() != after.site()) {
    Error error = Error::make(ErrorCode::conflict, "cannot diff snapshots that describe different sites");
    error.with_constraint("before.site == after.site");
    return error;
  }

  SnapshotDiff diff;
  diff.facility = before.facility();
  diff.site = before.site();
  diff.before_generation = before.generation();
  diff.after_generation = after.generation();
  diff.before_digest = before.digest();
  diff.after_digest = after.digest();

  const CapacityDimension* dimensions = all_capacity_dimensions();
  for (std::size_t index = 0; index < capacity_dimension_count; ++index) {
    DimensionChange change = diff_dimension(before, after, dimensions[index]);
    if (change.changed()) {
      diff.dimensions.push_back(std::move(change));
    }
  }
  if (diff.dimensions.size() > limits::max_diff_entries) {
    return entry_limit_error();
  }

  diff.sources = diff_sources(before, after);
  if (diff.dimensions.size() + diff.sources.size() > limits::max_diff_entries) {
    return entry_limit_error();
  }

  diff.constraints = diff_constraints(before, after);
  if (diff.dimensions.size() + diff.sources.size() + diff.constraints.size() > limits::max_diff_entries) {
    return entry_limit_error();
  }

  return diff;
}

}  // namespace dccp::facility_capacity
