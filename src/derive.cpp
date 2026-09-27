// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Exact composition of capacity dimensions.
//
// The accounting laws, per source and per dimension:
//
//   installed  ==  usable + unavailable + residual          (physical closure)
//   usable     ==  protected + reserved + allocatable       (allocation closure)
//
// and therefore, over the sources that contribute:
//
//   allocatable  <=  usable  <=  installed
//
// Every quantity is either exact or unmeasured. An unmeasured value poisons
// every total it takes part in, so a total is never quietly reduced to zero and
// capacity can never appear from nowhere: the most a dimension can report is
// what its sources measured as installed, minus what they measured as
// unavailable, residual, protected and reserved.

#include "internal/derive.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "dccp/facility_capacity/limits.hpp"
#include "internal/checked.hpp"

namespace dccp::facility_capacity::internal {
namespace {

struct DimensionComposition {
  DimensionTotals totals;
  DimensionLimit limit;
  CapacityOutcome outcome = CapacityOutcome::unavailable;
  NoteSet notes;
  bool coverage_satisfied = false;
  bool offered = false;
  std::vector<SourceGenerationStamp> sources;
  std::vector<ReserveStamp> reserves;
};

struct SourceContribution {
  CapacitySourceId source;
  Measured unavailable;
  Measured residual;
  Measured protection;
  Measured reserved;
};

struct ReductionCandidate {
  ReasonCode reason = ReasonCode::none;
  Measured amount;
  CapacitySourceId source;
  bool eligible = false;
};

/// Reason codes that make an answer incomplete rather than merely imprecise.
bool is_coverage_reason(ReasonCode code) noexcept {
  switch (code) {
    case ReasonCode::no_source_for_dimension:
    case ReasonCode::source_not_reported:
    case ReasonCode::coverage_contract_changed:
    case ReasonCode::source_stale:
    case ReasonCode::source_superseded:
    case ReasonCode::source_epoch_mismatch:
    case ReasonCode::source_incarnation_mismatch:
    case ReasonCode::source_identity_mismatch:
    case ReasonCode::source_closure_violated:
    case ReasonCode::reserve_itemisation_mismatch:
    case ReasonCode::allocation_over_committed:
    case ReasonCode::snapshot_expired:
    case ReasonCode::capacity_generation_advanced:
      return true;
    default:
      return false;
  }
}

void add_note(NoteSet& notes, ReasonCode code, CapacityDimension dimension, const CapacitySourceId& source,
              std::string detail) {
  (void)notes.add(CapacityNote(code, dimension, source, std::move(detail)));
}

Measured sum_reserve_amounts(const std::vector<const CapacityReserve*>& items, Tick instant, Unit unit) {
  Measured total = Measured::known_zero(unit);
  for (const CapacityReserve* reserve : items) {
    total = total.add(reserve->amount_at(instant));
  }
  return total;
}

void consider(ReductionCandidate& best, ReasonCode reason, const Measured& amount) {
  if (!amount.is_known()) {
    return;
  }
  if (!best.eligible || amount.magnitude() > best.amount.magnitude()) {
    best.reason = reason;
    best.amount = amount;
    best.eligible = true;
  }
}

/// Attributes the dominant reduction to the source that contributes most to it.
CapacitySourceId attribute(const std::vector<SourceContribution>& contributions, ReasonCode reason) {
  CapacitySourceId best;
  std::int64_t best_amount = -1;
  for (const SourceContribution& contribution : contributions) {
    const Measured* value = nullptr;
    switch (reason) {
      case ReasonCode::limiting_by_unavailable:
        value = &contribution.unavailable;
        break;
      case ReasonCode::limiting_by_residual:
        value = &contribution.residual;
        break;
      case ReasonCode::limiting_by_protection:
        value = &contribution.protection;
        break;
      case ReasonCode::limiting_by_reserve:
        value = &contribution.reserved;
        break;
      default:
        return CapacitySourceId();
    }
    if (value->is_known() && value->magnitude() > best_amount) {
      best_amount = value->magnitude();
      best = contribution.source;
    }
  }
  return best;
}

void choose_limit(DimensionComposition& composition, const std::vector<SourceContribution>& contributions) {
  DimensionLimit& limit = composition.limit;
  limit.dimension = composition.totals.dimension;
  limit.unit = composition.totals.unit;
  limit.installed = composition.totals.installed;
  limit.usable = composition.totals.usable;
  limit.allocatable = composition.totals.allocatable;
  limit.amount = Measured::unknown();

  if (!composition.offered) {
    limit.reason = ReasonCode::none;
    return;
  }
  if (composition.totals.allocatable.is_unknown()) {
    limit.reason = ReasonCode::limiting_indeterminate;
    return;
  }

  const Measured& protection = composition.totals.protected_capacity;
  const Measured& reserved = composition.totals.reserved;

  ReductionCandidate best;
  consider(best, ReasonCode::limiting_by_unavailable, composition.totals.unavailable);
  consider(best, ReasonCode::limiting_by_residual, composition.totals.residual);
  if (protection.is_known() && reserved.is_known()) {
    if (protection.magnitude() >= reserved.magnitude()) {
      consider(best, ReasonCode::limiting_by_protection, protection);
    } else {
      consider(best, ReasonCode::limiting_by_reserve, reserved);
    }
  } else {
    consider(best, ReasonCode::limiting_by_protection, protection);
    consider(best, ReasonCode::limiting_by_reserve, reserved);
  }

  if (!best.eligible) {
    limit.reason = ReasonCode::limiting_indeterminate;
    return;
  }
  limit.reason = best.reason;
  limit.amount = best.amount;
  limit.source = attribute(contributions, best.reason);
}

DimensionComposition compose_dimension(const CapacitySnapshotInput& input, CapacityDimension dimension) {
  const Unit unit = canonical_unit(dimension);
  DimensionComposition result;
  result.totals.dimension = dimension;
  result.totals.unit = unit;
  result.limit.dimension = dimension;
  result.limit.unit = unit;

  std::vector<const SourceEvidence*> records;
  for (const SourceEvidence& record : input.evidence) {
    if (record.dimension() == dimension) {
      records.push_back(&record);
    }
  }

  const CapacityRequirement* requirement = nullptr;
  for (const CapacityRequirement& candidate : input.requirements) {
    if (candidate.dimension == dimension) {
      requirement = &candidate;
      break;
    }
  }
  const bool required = requirement != nullptr && requirement->required;

  if (records.empty()) {
    if (required) {
      add_note(result.notes, ReasonCode::no_source_for_dimension, dimension, CapacitySourceId(),
               "no evidence covers a required dimension");
      result.outcome = CapacityOutcome::incomplete;
      result.coverage_satisfied = false;
    } else {
      add_note(result.notes, ReasonCode::dimension_not_offered, dimension, CapacitySourceId(),
               "no evidence covers the dimension and it is not required");
      result.outcome = CapacityOutcome::unavailable;
      result.coverage_satisfied = true;
    }
    return result;
  }

  result.offered = true;
  bool coverage_satisfied = true;

  if (requirement != nullptr) {
    for (const CapacitySourceId& required_source : requirement->required_sources) {
      const bool found =
          std::any_of(records.begin(), records.end(),
                      [&required_source](const SourceEvidence* record) {
                        return record->source() == required_source;
                      });
      if (!found) {
        add_note(result.notes, ReasonCode::source_not_reported, dimension, required_source,
                 "a required source reported no evidence");
        coverage_satisfied = false;
      }
    }
  }

  Measured installed = Measured::known_zero(unit);
  Measured observed = Measured::known_zero(unit);
  Measured usable = Measured::known_zero(unit);
  Measured unavailable = Measured::known_zero(unit);
  Measured residual = Measured::known_zero(unit);
  Measured protected_total = Measured::known_zero(unit);
  Measured reserved_total = Measured::known_zero(unit);
  bool physical_closed = true;
  std::size_t contributing = 0;
  std::size_t measured_usable = 0;
  bool any_excluded = false;
  std::vector<SourceContribution> contributions;

  for (const SourceEvidence* record : records) {
    std::vector<const CapacityReserve*> protection_items;
    std::vector<const CapacityReserve*> reserve_items;
    for (const CapacityReserve& reserve : input.reserves) {
      if (reserve.dimension() != dimension || reserve.source() != record->source()) {
        continue;
      }
      if (!reserve.active_at(input.built_at)) {
        continue;
      }
      if (reserve.supplies_protection()) {
        protection_items.push_back(&reserve);
      } else {
        reserve_items.push_back(&reserve);
      }
    }

    const Measured itemised_protection = sum_reserve_amounts(protection_items, input.built_at, unit);
    const Measured itemised_reserved = sum_reserve_amounts(reserve_items, input.built_at, unit);

    bool excluded = false;
    Measured resolved_protection;
    if (protection_items.empty()) {
      resolved_protection = record->protected_capacity();
    } else {
      if (record->protected_capacity().is_known() && itemised_protection.is_known() &&
          record->protected_capacity().magnitude() != itemised_protection.magnitude()) {
        add_note(result.notes, ReasonCode::reserve_itemisation_mismatch, dimension, record->source(),
                 "the declared protection roll-up does not equal the itemised protection reserves");
        excluded = true;
      }
      resolved_protection =
          record->protected_capacity().is_known() ? record->protected_capacity() : itemised_protection;
    }

    Measured resolved_reserved;
    if (reserve_items.empty()) {
      resolved_reserved = record->reserved();
    } else {
      if (record->reserved().is_known() && itemised_reserved.is_known() &&
          record->reserved().magnitude() != itemised_reserved.magnitude()) {
        add_note(result.notes, ReasonCode::reserve_itemisation_mismatch, dimension, record->source(),
                 "the declared reserved roll-up does not equal the itemised reserves");
        excluded = true;
      }
      resolved_reserved = record->reserved().is_known() ? record->reserved() : itemised_reserved;
    }

    if (!excluded && record->usable().is_known() && resolved_protection.is_known() &&
        resolved_reserved.is_known()) {
      const Measured held = resolved_protection.add(resolved_reserved);
      if (held.is_known() && held.magnitude() > record->usable().magnitude()) {
        add_note(result.notes, ReasonCode::allocation_over_committed, dimension, record->source(),
                 "protected and reserved capacity exceeds usable capacity");
        excluded = true;
      }
    }
    if (!excluded && !record->closure_proven() && record->usable().is_known() &&
        record->unavailable().is_known() && record->residual().is_known()) {
      add_note(result.notes, ReasonCode::source_closure_violated, dimension, record->source(),
               "usable, unavailable and residual do not sum to installed");
      excluded = true;
    }

    if (excluded) {
      any_excluded = true;
      continue;
    }

    installed = installed.add(record->installed());
    observed = observed.add(record->observed());
    usable = usable.add(record->usable());
    unavailable = unavailable.add(record->unavailable());
    residual = residual.add(record->residual());
    protected_total = protected_total.add(resolved_protection);
    reserved_total = reserved_total.add(resolved_reserved);
    if (!record->closure_proven()) {
      physical_closed = false;
    }
    ++contributing;
    if (record->usable().is_known()) {
      ++measured_usable;
    }

    SourceContribution contribution;
    contribution.source = record->source();
    contribution.unavailable = record->unavailable();
    contribution.residual = record->residual();
    contribution.protection = resolved_protection;
    contribution.reserved = resolved_reserved;
    contributions.push_back(std::move(contribution));

    switch (record->state()) {
      case OperationalState::degraded:
        add_note(result.notes, ReasonCode::source_degraded, dimension, record->source(),
                 record->state_detail());
        break;
      case OperationalState::unavailable:
        add_note(result.notes, ReasonCode::source_unavailable, dimension, record->source(),
                 record->state_detail());
        break;
      case OperationalState::unknown:
        add_note(result.notes, ReasonCode::source_state_unknown, dimension, record->source(),
                 record->state_detail());
        break;
      case OperationalState::nominal:
        break;
    }

    std::string unmeasured;
    if (record->usable().is_unknown()) {
      unmeasured.append("usable");
    }
    if (resolved_protection.is_unknown()) {
      unmeasured.append(unmeasured.empty() ? "" : ",").append("protected");
    }
    if (resolved_reserved.is_unknown()) {
      unmeasured.append(unmeasured.empty() ? "" : ",").append("reserved");
    }
    if (!unmeasured.empty()) {
      add_note(result.notes, ReasonCode::quantity_not_measured, dimension, record->source(),
               "the source did not measure: " + unmeasured);
    }

    SourceGenerationStamp stamp;
    stamp.source = record->source();
    stamp.dimension = dimension;
    stamp.generation = record->generation();
    stamp.evidence_digest = record->digest();
    stamp.source_evidence_digest = record->provenance().evidence_digest;
    stamp.source_revision = record->provenance().revision;
    stamp.epoch = record->provenance().epoch;
    stamp.incarnation = record->provenance().incarnation;
    stamp.observed_at = record->observed_at();
    stamp.state = record->state();
    stamp.itemised_reserves = protection_items.size() + reserve_items.size();
    stamp.used_declared_rollup = stamp.itemised_reserves == 0;
    result.sources.push_back(std::move(stamp));

    const auto stamp_reserve = [&result, &dimension, &unit, &input](const CapacityReserve* reserve) {
      ReserveStamp reserve_stamp;
      reserve_stamp.id = reserve->id();
      reserve_stamp.source = reserve->source();
      reserve_stamp.dimension = dimension;
      reserve_stamp.kind = reserve->kind();
      reserve_stamp.unit = unit;
      reserve_stamp.amount = reserve->amount_at(input.built_at);
      reserve_stamp.window = reserve->window();
      reserve_stamp.owner = reserve->owner();
      reserve_stamp.service_class = reserve->service_class();
      reserve_stamp.reason = reserve->reason();
      reserve_stamp.reserve_digest = reserve->digest();
      result.reserves.push_back(std::move(reserve_stamp));
    };
    for (const CapacityReserve* reserve : protection_items) {
      stamp_reserve(reserve);
    }
    for (const CapacityReserve* reserve : reserve_items) {
      stamp_reserve(reserve);
    }
  }

  if (any_excluded) {
    coverage_satisfied = false;
  }

  // A dimension whose every source was excluded reports no total at all. The
  // accumulators start at a measured zero, but publishing that zero would say
  // "this dimension has no capacity" when the truth is "this dimension's
  // sources could not be used", which is the difference between zero and
  // unknown that this product exists to preserve.
  if (contributing != 0) {
    result.totals.installed = installed;
    result.totals.observed = observed;
    result.totals.usable = usable;
    result.totals.unavailable = unavailable;
    result.totals.residual = residual;
    result.totals.protected_capacity = protected_total;
    result.totals.reserved = reserved_total;
  }
  result.totals.contributing_sources = contributing;
  result.totals.measured_usable_sources = measured_usable;

  bool allocation_closed = false;
  if (usable.is_known() && protected_total.is_known() && reserved_total.is_known()) {
    const Measured held = protected_total.add(reserved_total);
    if (held.is_known()) {
      const Result<Measured> remaining = usable.subtract(held, "usable - protected - reserved >= 0");
      if (!remaining.has_value()) {
        add_note(result.notes, ReasonCode::allocation_over_committed, dimension, CapacitySourceId(),
                 "the composed protected and reserved capacity exceeds the composed usable capacity");
        coverage_satisfied = false;
      } else {
        result.totals.allocatable = remaining.value();
        allocation_closed = true;
      }
    }
  }
  result.totals.allocation_closed = allocation_closed;
  result.totals.physical_closed = physical_closed && contributing != 0;

  choose_limit(result, contributions);

  if (contributing == 0) {
    result.outcome = CapacityOutcome::incomplete;
    result.coverage_satisfied = false;
    return result;
  }
  if (!coverage_satisfied) {
    result.outcome = CapacityOutcome::incomplete;
  } else if (result.totals.allocatable.is_unknown()) {
    result.outcome = CapacityOutcome::unknown;
    add_note(result.notes, ReasonCode::composed_unmeasured, dimension, CapacitySourceId(),
             "every covering source is present and current, and the composed allocatable quantity is unmeasured");
  } else if (result.totals.allocatable.is_known_zero()) {
    result.outcome = CapacityOutcome::exhausted;
    add_note(result.notes, ReasonCode::allocatable_exhausted, dimension, CapacitySourceId(),
             "the composed allocatable quantity is measured and is exactly zero");
  } else {
    result.outcome = CapacityOutcome::usable;
    add_note(result.notes, ReasonCode::allocatable_positive, dimension, CapacitySourceId(),
             "the composed allocatable quantity is measured and is positive");
  }

  result.coverage_satisfied = coverage_satisfied;
  return result;
}

}  // namespace

Result<DerivedSnapshot> derive_content(const CapacitySnapshotInput& input) {
  DerivedSnapshot derived;
  derived.notes.merge(input.coverage_findings);

  bool accounting_closed = true;
  std::size_t required_dimensions = 0;
  std::size_t complete_dimensions = 0;

  for (std::size_t ordinal = 0; ordinal < capacity_dimension_count; ++ordinal) {
    const CapacityDimension dimension = all_capacity_dimensions()[ordinal];
    DimensionComposition composition = compose_dimension(input, dimension);

    // Exact accounting closure, asserted rather than assumed.
    const DimensionTotals& totals = composition.totals;
    if (totals.installed.is_known() && totals.unavailable.is_known() && totals.residual.is_known() &&
        totals.usable.is_known()) {
      const Measured parts = totals.unavailable.add(totals.residual).add(totals.usable);
      if (!parts.is_known() || parts.magnitude() != totals.installed.magnitude()) {
        Error error = Error::make(ErrorCode::invariant_violation,
                                  "the composed dimension does not satisfy installed == usable + "
                                  "unavailable + residual");
        error.with_constraint("unavailable + residual + usable == installed");
        return error;
      }
    }
    if (totals.usable.is_known() && totals.protected_capacity.is_known() && totals.reserved.is_known() &&
        totals.allocatable.is_known()) {
      const Measured parts = totals.protected_capacity.add(totals.reserved).add(totals.allocatable);
      if (!parts.is_known() || parts.magnitude() != totals.usable.magnitude()) {
        Error error = Error::make(ErrorCode::invariant_violation,
                                  "the composed dimension does not satisfy usable == protected + "
                                  "reserved + allocatable");
        error.with_constraint("protected + reserved + allocatable == usable");
        return error;
      }
    }
    if (totals.installed.is_known() && totals.allocatable.is_known() &&
        totals.allocatable.magnitude() > totals.installed.magnitude()) {
      Error error = Error::make(ErrorCode::invariant_violation,
                                "the composed allocatable capacity exceeds the composed installed capacity");
      error.with_constraint("allocatable <= installed");
      return error;
    }

    if (totals.contributing_sources != 0) {
      accounting_closed = accounting_closed && totals.allocation_closed && totals.physical_closed;
    }

    derived.totals.push_back(totals);
    derived.outcomes.push_back(composition.outcome);
    derived.limits.push_back(composition.limit);
    for (const SourceGenerationStamp& stamp : composition.sources) {
      derived.sources.push_back(stamp);
    }
    for (const ReserveStamp& stamp : composition.reserves) {
      derived.reserves.push_back(stamp);
    }
    derived.notes.merge(composition.notes);

    const CapacityRequirement* requirement = nullptr;
    for (const CapacityRequirement& candidate : input.requirements) {
      if (candidate.dimension == dimension) {
        requirement = &candidate;
        break;
      }
    }
    if (requirement != nullptr && requirement->required) {
      ++required_dimensions;
      if (composition.coverage_satisfied) {
        ++complete_dimensions;
      }
    }
  }

  // Limiting constraint: the dimension with the smallest exact permille
  // headroom, ties broken by dimension ordinal.
  bool found = false;
  std::int64_t best_permille = 0;

  for (std::size_t ordinal = 0; ordinal < derived.totals.size(); ++ordinal) {
    const DimensionTotals& totals = derived.totals[ordinal];
    if (!totals.usable.is_known() || !totals.allocatable.is_known()) {
      continue;
    }
    std::int64_t value = 0;
    if (!totals.usable.is_known_zero()) {
      const Result<std::int64_t> permille = mul_div_i64(totals.allocatable.magnitude(), 1000,
                                                        totals.usable.magnitude(),
                                                        "headroom_permille is exact");
      if (!permille.has_value()) {
        return permille.error();
      }
      value = permille.value() < 0 ? 0 : permille.value();
    }
    if (!found || value < best_permille) {
      found = true;
      best_permille = value;
      derived.limiting.present = true;
      derived.limiting.dimension = totals.dimension;
      derived.limiting.unit = totals.unit;
      derived.limiting.headroom_permille = value;
      derived.limiting.allocatable = totals.allocatable;
      derived.limiting.usable = totals.usable;
      derived.limiting.reason = derived.limits[ordinal].reason;
      derived.limiting.source = derived.limits[ordinal].source;
    }
  }
  if (!found) {
    derived.limiting.present = false;
  }

  // Service constraints.
  for (const ServiceConstraint& constraint : input.constraints) {
    ConstraintEvaluation evaluation;
    evaluation.id = constraint.id();
    evaluation.dimension = constraint.dimension();
    evaluation.service_class = constraint.service_class();
    evaluation.unit = constraint.unit();
    evaluation.floor_value = constraint.floor_value();

    const DimensionTotals* totals = nullptr;
    for (const DimensionTotals& candidate : derived.totals) {
      if (candidate.dimension == constraint.dimension()) {
        totals = &candidate;
        break;
      }
    }
    evaluation.allocatable = totals != nullptr ? totals->allocatable : Measured::unknown();

    if (!constraint.active_at(input.built_at)) {
      evaluation.status = ConstraintStatus::inactive;
      evaluation.reason = ReasonCode::none;
    } else if (!evaluation.floor_value.is_known() || !evaluation.allocatable.is_known()) {
      evaluation.status = ConstraintStatus::indeterminate;
      evaluation.reason = ReasonCode::service_floor_indeterminate;
      evaluation.detail = "the floor or the composed allocatable quantity is unmeasured";
      add_note(derived.notes, ReasonCode::service_floor_indeterminate, constraint.dimension(),
               CapacitySourceId(), constraint.id().value() + ": " + evaluation.detail);
    } else if (evaluation.allocatable.magnitude() >= evaluation.floor_value.magnitude()) {
      evaluation.status = ConstraintStatus::satisfied;
      evaluation.reason = ReasonCode::none;
    } else {
      evaluation.status = ConstraintStatus::violated;
      evaluation.reason = ReasonCode::service_floor_violated;
      evaluation.detail = "the composed allocatable quantity is below the declared floor";
      add_note(derived.notes, ReasonCode::service_floor_violated, constraint.dimension(),
               CapacitySourceId(), constraint.id().value() + ": " + evaluation.detail);
    }
    derived.constraints.push_back(std::move(evaluation));
  }


  derived.completeness.required_dimensions = required_dimensions;
  derived.completeness.complete_dimensions = complete_dimensions;
  bool complete = complete_dimensions == required_dimensions && input.coverage_findings.empty();
  for (const CapacityNote& finding : derived.notes.notes()) {
    if (is_coverage_reason(finding.code)) {
      complete = false;
      (void)derived.completeness.findings.add(finding);
    }
  }
  derived.completeness.classification =
      complete ? CompletenessClass::complete : CompletenessClass::incomplete;
  derived.accounting_closed = accounting_closed;

  return derived;
}

}  // namespace dccp::facility_capacity::internal
