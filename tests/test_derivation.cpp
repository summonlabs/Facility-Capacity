// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The derivation contract: outcome precedence, the separation of unknown from
// zero, exact accounting closure, limiting-constraint attribution and
// completeness.

#include "test_framework.hpp"
#include "test_snapshot_builder.hpp"
#include "test_support.hpp"

namespace {

using namespace dccp::facility_capacity;
using fsup::EvidenceSpec;
using fsup::ReserveSpec;
using fsup::SnapshotBuilder;

EvidenceSpec power_source(const std::string& source, std::int64_t installed, std::int64_t usable,
                          std::int64_t unavailable) {
  EvidenceSpec spec;
  spec.source = source;
  spec.dimension = CapacityDimension::power;
  spec.unit = Unit::milli_watt;
  spec.installed = installed;
  spec.usable = usable;
  spec.unavailable = unavailable;
  spec.derive_residual = true;
  spec.protected_capacity = 0;
  spec.reserved = 0;
  return spec;
}

EvidenceSpec space_source(const std::string& source, std::int64_t installed, std::int64_t usable) {
  EvidenceSpec spec;
  spec.source = source;
  spec.dimension = CapacityDimension::space;
  spec.unit = Unit::rack_unit;
  spec.installed = installed;
  spec.usable = usable;
  spec.unavailable = 0;
  spec.derive_residual = true;
  spec.protected_capacity = 0;
  spec.reserved = 0;
  return spec;
}

}  // namespace

// ---------------------------------------------------------------------------
// Outcome precedence
// ---------------------------------------------------------------------------

FT_TEST(derivation, unoffered_dimension_is_unavailable) {
  SnapshotBuilder builder;
  const auto snapshot = builder.build_ok();
  FT_CHECK_EQ(snapshot->outcome(CapacityDimension::power), CapacityOutcome::unavailable);
  FT_CHECK(snapshot->notes().contains(ReasonCode::dimension_not_offered));
  FT_CHECK(snapshot->completeness().is_complete());
}

FT_TEST(derivation, required_dimension_without_evidence_is_incomplete) {
  SnapshotBuilder builder;
  builder.require(CapacityDimension::power);
  const auto snapshot = builder.build_ok();
  FT_CHECK_EQ(snapshot->outcome(CapacityDimension::power), CapacityOutcome::incomplete);
  FT_CHECK(snapshot->notes().contains(ReasonCode::no_source_for_dimension));
  FT_CHECK(!snapshot->completeness().is_complete());
  FT_CHECK_EQ(snapshot->completeness().required_dimensions, std::size_t{1});
  FT_CHECK_EQ(snapshot->completeness().complete_dimensions, std::size_t{0});
}

FT_TEST(derivation, absent_required_source_is_incomplete) {
  SnapshotBuilder builder;
  builder.require(CapacityDimension::power, true, {"power-capacity", "power-capacity-secondary"});
  builder.add(fsup::make_evidence(power_source("power-capacity", 10000, 9000, 1000)));
  const auto snapshot = builder.build_ok();
  FT_CHECK_EQ(snapshot->outcome(CapacityDimension::power), CapacityOutcome::incomplete);
  const CapacityNote* note = snapshot->notes().find(ReasonCode::source_not_reported);
  FT_REQUIRE(note != nullptr);
  FT_CHECK_EQ(note->source.value(), std::string("power-capacity-secondary"));
}

FT_TEST(derivation, unmeasured_composition_is_unknown_not_zero) {
  SnapshotBuilder builder;
  EvidenceSpec spec;
  spec.source = "power-capacity";
  spec.dimension = CapacityDimension::power;
  spec.installed = 10000;
  spec.usable = 9000;
  spec.unavailable = 1000;
  spec.derive_residual = true;
  spec.protected_capacity = std::nullopt;  // never measured
  spec.reserved = 0;
  builder.add(fsup::make_evidence(spec));
  const auto snapshot = builder.build_ok();
  FT_CHECK_EQ(snapshot->outcome(CapacityDimension::power), CapacityOutcome::unknown);
  const DimensionTotals* totals = snapshot->find_totals(CapacityDimension::power);
  FT_REQUIRE(totals != nullptr);
  FT_CHECK(totals->allocatable.is_unknown());
  FT_CHECK(!totals->allocatable.is_known_zero());
  FT_CHECK_EQ(totals->installed.magnitude(), std::int64_t{10000});
  FT_CHECK(snapshot->notes().contains(ReasonCode::quantity_not_measured));
  FT_CHECK(snapshot->notes().contains(ReasonCode::composed_unmeasured));
}

FT_TEST(derivation, unavailable_source_still_answers_exhausted) {
  SnapshotBuilder builder;
  EvidenceSpec spec;
  spec.source = "power-capacity";
  spec.dimension = CapacityDimension::power;
  spec.state = OperationalState::unavailable;
  spec.installed = 10000;
  spec.usable = 0;
  spec.unavailable = 10000;
  spec.derive_residual = true;
  spec.protected_capacity = 0;
  spec.reserved = 0;
  builder.add(fsup::make_evidence(spec));
  const auto snapshot = builder.build_ok();
  // The evidence is present and current, so the answer is a measured zero, not
  // an incomplete or unknown answer.
  FT_CHECK_EQ(snapshot->outcome(CapacityDimension::power), CapacityOutcome::exhausted);
  FT_CHECK(snapshot->completeness().is_complete());
  FT_CHECK(snapshot->notes().contains(ReasonCode::source_unavailable));
  const DimensionTotals* totals = snapshot->find_totals(CapacityDimension::power);
  FT_REQUIRE(totals != nullptr);
  FT_CHECK(totals->allocatable.is_known_zero());
}

FT_TEST(derivation, positive_allocatable_is_usable) {
  SnapshotBuilder builder;
  builder.add(fsup::make_evidence(power_source("power-capacity", 10000, 9000, 1000)));
  const auto snapshot = builder.build_ok();
  FT_CHECK_EQ(snapshot->outcome(CapacityDimension::power), CapacityOutcome::usable);
  FT_CHECK(snapshot->notes().contains(ReasonCode::allocatable_positive));
  FT_CHECK_EQ(snapshot->allocatable(CapacityDimension::power).magnitude(), std::int64_t{9000});
}

FT_TEST(derivation, excluded_source_makes_the_dimension_incomplete) {
  SnapshotBuilder builder;
  builder.add(fsup::make_evidence(power_source("power-capacity", 10000, 9000, 1000)));
  // The declared roll-up says 500 is protected, but the declared reserve is
  // 7000: the two disagree and the source is excluded rather than merged.
  ReserveSpec reserve;
  reserve.id = "protection-1";
  reserve.source = "power-capacity";
  reserve.dimension = CapacityDimension::power;
  reserve.kind = ReserveKind::protection;
  reserve.amount = 7000;
  builder.add(fsup::make_reserve(reserve));
  const auto snapshot = builder.build_ok();
  FT_CHECK_EQ(snapshot->outcome(CapacityDimension::power), CapacityOutcome::incomplete);
  FT_CHECK(snapshot->notes().contains(ReasonCode::reserve_itemisation_mismatch));
  const DimensionTotals* totals = snapshot->find_totals(CapacityDimension::power);
  FT_REQUIRE(totals != nullptr);
  FT_CHECK_EQ(totals->contributing_sources, std::size_t{0});
  FT_CHECK(totals->installed.is_unknown());
}

// ---------------------------------------------------------------------------
// Exact accounting closure
// ---------------------------------------------------------------------------

FT_TEST(derivation, physical_and_allocation_closure_hold) {
  SnapshotBuilder builder;
  EvidenceSpec first = power_source("power-capacity", 10000, 9000, 400);
  first.protected_capacity = 1000;
  first.reserved = 500;
  builder.add(fsup::make_evidence(first));
  EvidenceSpec second = power_source("power-capacity-2", 5000, 4000, 250);
  second.protected_capacity = 300;
  second.reserved = 200;
  builder.add(fsup::make_evidence(second));
  const auto snapshot = builder.build_ok();
  const DimensionTotals* totals = snapshot->find_totals(CapacityDimension::power);
  FT_REQUIRE(totals != nullptr);
  FT_CHECK(totals->physical_closed);
  FT_CHECK(totals->allocation_closed);
  FT_CHECK(snapshot->accounting_closed());

  // installed == usable + unavailable + residual
  const std::int64_t parts = totals->usable.magnitude() + totals->unavailable.magnitude() +
                             totals->residual.magnitude();
  FT_CHECK_EQ(parts, totals->installed.magnitude());
  // usable == protected + reserved + allocatable
  const std::int64_t allocation = totals->protected_capacity.magnitude() + totals->reserved.magnitude() +
                                  totals->allocatable.magnitude();
  FT_CHECK_EQ(allocation, totals->usable.magnitude());
  FT_CHECK(totals->allocatable.magnitude() <= totals->usable.magnitude());
  FT_CHECK(totals->usable.magnitude() <= totals->installed.magnitude());
}

FT_TEST(derivation, unknown_part_makes_closure_unproven_but_total_exact) {
  SnapshotBuilder builder;
  EvidenceSpec spec = power_source("power-capacity", 10000, 9000, 1000);
  spec.usable = std::nullopt;
  spec.residual = std::nullopt;
  spec.derive_residual = false;
  builder.add(fsup::make_evidence(spec));
  const auto snapshot = builder.build_ok();
  const DimensionTotals* totals = snapshot->find_totals(CapacityDimension::power);
  FT_REQUIRE(totals != nullptr);
  FT_CHECK(!totals->physical_closed);
  FT_CHECK(!totals->allocation_closed);
  FT_CHECK_EQ(totals->installed.magnitude(), std::int64_t{10000});
  FT_CHECK(totals->usable.is_unknown());
  FT_CHECK(totals->allocatable.is_unknown());
}

// ---------------------------------------------------------------------------
// Limiting constraint attribution
// ---------------------------------------------------------------------------

FT_TEST(derivation, limiting_constraint_picks_the_smallest_exact_headroom) {
  SnapshotBuilder builder;
  // power: usable 10000, allocatable 2500 -> 250 permille
  EvidenceSpec heavy = power_source("power-capacity", 10000, 10000, 0);
  heavy.protected_capacity = 7500;
  builder.add(fsup::make_evidence(heavy));
  // space: usable 100, allocatable 50 -> 500 permille
  EvidenceSpec light = space_source("space-capacity", 100, 100);
  light.protected_capacity = 50;
  builder.add(fsup::make_evidence(light));
  const auto snapshot = builder.build_ok();
  FT_CHECK(snapshot->limiting().present);
  FT_CHECK_EQ(snapshot->limiting().dimension, CapacityDimension::power);
  FT_CHECK_EQ(snapshot->limiting().headroom_permille, std::int64_t{250});
  FT_CHECK_EQ(snapshot->limiting().reason, ReasonCode::limiting_by_protection);
  FT_CHECK_EQ(snapshot->limiting().source.value(), std::string("power-capacity"));
}

FT_TEST(derivation, limiting_is_absent_when_no_dimension_is_measured) {
  SnapshotBuilder builder;
  const auto snapshot = builder.build_ok();
  FT_CHECK(!snapshot->limiting().present);
  FT_CHECK_EQ(snapshot->limiting().reason, ReasonCode::none);
}

FT_TEST(derivation, limit_reason_names_the_dominant_reduction) {
  SnapshotBuilder builder;
  EvidenceSpec spec = power_source("power-capacity", 10000, 2000, 8000);
  spec.protected_capacity = 0;
  builder.add(fsup::make_evidence(spec));
  const auto snapshot = builder.build_ok();
  const DimensionLimit* limit = nullptr;
  for (const DimensionLimit& candidate : snapshot->limits()) {
    if (candidate.dimension == CapacityDimension::power) {
      limit = &candidate;
    }
  }
  FT_REQUIRE(limit != nullptr);
  FT_CHECK_EQ(limit->reason, ReasonCode::limiting_by_unavailable);
  FT_CHECK(limit->amount.is_known());
  FT_CHECK_EQ(limit->amount.magnitude(), std::int64_t{8000});
  FT_CHECK_EQ(limit->source.value(), std::string("power-capacity"));
}

FT_TEST(derivation, limit_is_indeterminate_when_allocatable_is_unmeasured) {
  SnapshotBuilder builder;
  EvidenceSpec spec = power_source("power-capacity", 10000, 9000, 1000);
  spec.reserved = std::nullopt;
  spec.derive_residual = false;
  builder.add(fsup::make_evidence(spec));
  const auto snapshot = builder.build_ok();
  DimensionLimit limit;
  for (const DimensionLimit& candidate : snapshot->limits()) {
    if (candidate.dimension == CapacityDimension::power) {
      limit = candidate;
    }
  }
  FT_CHECK_EQ(limit.reason, ReasonCode::limiting_indeterminate);
  FT_CHECK(limit.amount.is_unknown());
}

// ---------------------------------------------------------------------------
// Reserves, windows and service constraints
// ---------------------------------------------------------------------------

FT_TEST(derivation, inactive_reserve_does_not_reduce_allocatable) {
  SnapshotBuilder builder;
  builder.at(1000);
  EvidenceSpec spec = power_source("power-capacity", 10000, 9000, 1000);
  spec.protected_capacity = 0;
  builder.add(fsup::make_evidence(spec));
  ReserveSpec reserve;
  reserve.id = "protection-later";
  reserve.source = "power-capacity";
  reserve.dimension = CapacityDimension::power;
  reserve.kind = ReserveKind::protection;
  reserve.amount = 5000;
  reserve.window_start = 5000;
  reserve.window_end = 6000;
  builder.add(fsup::make_reserve(reserve));
  const auto snapshot = builder.build_ok();
  FT_CHECK_EQ(snapshot->allocatable(CapacityDimension::power).magnitude(), std::int64_t{9000});
  FT_CHECK_EQ(snapshot->outcome(CapacityDimension::power), CapacityOutcome::usable);
}

FT_TEST(derivation, active_itemised_reserve_reduces_allocatable_exactly) {
  SnapshotBuilder builder;
  builder.at(1000);
  EvidenceSpec spec = power_source("power-capacity", 10000, 9000, 1000);
  spec.protected_capacity = 2000;
  spec.reserved = 1000;
  builder.add(fsup::make_evidence(spec));
  ReserveSpec protection;
  protection.id = "protection-active";
  protection.source = "power-capacity";
  protection.dimension = CapacityDimension::power;
  protection.kind = ReserveKind::protection;
  protection.amount = 2000;
  protection.window_start = 500;
  protection.window_end = 1500;
  builder.add(fsup::make_reserve(protection));
  ReserveSpec operational;
  operational.id = "operational-active";
  operational.source = "power-capacity";
  operational.dimension = CapacityDimension::power;
  operational.kind = ReserveKind::operational;
  operational.amount = 1000;
  builder.add(fsup::make_reserve(operational));

  const auto snapshot = builder.build_ok();
  const DimensionTotals* totals = snapshot->find_totals(CapacityDimension::power);
  FT_REQUIRE(totals != nullptr);
  FT_CHECK_EQ(totals->protected_capacity.magnitude(), std::int64_t{2000});
  FT_CHECK_EQ(totals->reserved.magnitude(), std::int64_t{1000});
  FT_CHECK_EQ(totals->allocatable.magnitude(), std::int64_t{6000});
}

FT_TEST(derivation, service_floor_states) {
  SnapshotBuilder builder;
  builder.add(fsup::make_evidence(power_source("power-capacity", 10000, 9000, 1000)));

  fsup::ConstraintSpec satisfied;
  satisfied.id = "floor-satisfied";
  satisfied.dimension = CapacityDimension::power;
  satisfied.floor_value = 8000;
  builder.add(fsup::make_constraint(satisfied));

  fsup::ConstraintSpec violated;
  violated.id = "floor-violated";
  violated.dimension = CapacityDimension::power;
  violated.floor_value = 9500;
  builder.add(fsup::make_constraint(violated));

  fsup::ConstraintSpec indeterminate;
  indeterminate.id = "floor-indeterminate";
  indeterminate.dimension = CapacityDimension::cooling;
  indeterminate.unit = canonical_unit(CapacityDimension::cooling);
  indeterminate.floor_value = 1;
  builder.add(fsup::make_constraint(indeterminate));

  fsup::ConstraintSpec inactive;
  inactive.id = "floor-inactive";
  inactive.dimension = CapacityDimension::power;
  inactive.floor_value = 1;
  inactive.window_start = 5000;
  inactive.window_end = 6000;
  builder.add(fsup::make_constraint(inactive));

  const auto snapshot = builder.build_ok();
  FT_CHECK_EQ(snapshot->constraints().size(), std::size_t{4});
  for (const ConstraintEvaluation& evaluation : snapshot->constraints()) {
    if (evaluation.id.value() == "floor-satisfied") {
      FT_CHECK_EQ(evaluation.status, ConstraintStatus::satisfied);
    } else if (evaluation.id.value() == "floor-violated") {
      FT_CHECK_EQ(evaluation.status, ConstraintStatus::violated);
      FT_CHECK_EQ(evaluation.reason, ReasonCode::service_floor_violated);
    } else if (evaluation.id.value() == "floor-indeterminate") {
      FT_CHECK_EQ(evaluation.status, ConstraintStatus::indeterminate);
      FT_CHECK_EQ(evaluation.reason, ReasonCode::service_floor_indeterminate);
    } else {
      FT_CHECK_EQ(evaluation.status, ConstraintStatus::inactive);
    }
  }
  FT_CHECK(snapshot->notes().contains(ReasonCode::service_floor_violated));
  FT_CHECK(snapshot->notes().contains(ReasonCode::service_floor_indeterminate));
}

FT_TEST(derivation, accounting_closed_is_recorded_and_reported) {
  SnapshotBuilder builder;
  builder.add(fsup::make_evidence(power_source("power-capacity", 10000, 9000, 400)));
  EvidenceSpec second = power_source("power-capacity-2", 4000, 3000, 500);
  second.protected_capacity = 100;
  builder.add(fsup::make_evidence(second));
  const auto snapshot = builder.build_ok();
  FT_CHECK(snapshot->accounting_closed());
  const std::string rendered = snapshot->describe();
  FT_CHECK(rendered.find("accounting_closed=1") != std::string::npos);
}

FT_TEST(derivation, snapshot_identity_is_derived_from_generation_and_digest) {
  SnapshotBuilder builder(42);
  const auto snapshot = builder.build_ok();
  FT_CHECK_EQ(snapshot->generation().value(), std::uint64_t{42});
  FT_CHECK_EQ(snapshot->digest().size(), std::size_t{64});
  FT_CHECK(snapshot->id().value().rfind("g42-", 0) == 0);
  FT_CHECK(snapshot->id().value().size() <= limits::max_identifier_length);
}

FT_TEST(derivation, build_refuses_a_missing_identity) {
  SnapshotBuilder builder;
  CapacitySnapshotInput input = builder.input();
  input.facility = FacilityId();
  FT_CHECK_ERROR(CapacitySnapshot::build(input), ErrorCode::invalid_argument);
  input = builder.input();
  input.site = SiteId();
  FT_CHECK_ERROR(CapacitySnapshot::build(input), ErrorCode::invalid_argument);
}

FT_TEST(derivation, build_refuses_a_duplicated_source_record) {
  SnapshotBuilder builder;
  builder.add(fsup::make_evidence(power_source("power-capacity", 10000, 9000, 1000)));
  builder.add(fsup::make_evidence(power_source("power-capacity", 20000, 18000, 2000)));
  FT_CHECK_ERROR(builder.build(), ErrorCode::duplicate_identity);
}

FT_TEST(derivation, build_refuses_a_duplicated_reserve_identity) {
  SnapshotBuilder builder;
  builder.add(fsup::make_evidence(power_source("power-capacity", 10000, 9000, 1000)));
  ReserveSpec reserve;
  reserve.id = "protection-1";
  reserve.dimension = CapacityDimension::power;
  reserve.amount = 100;
  builder.add(fsup::make_reserve(reserve));
  builder.add(fsup::make_reserve(reserve));
  FT_CHECK_ERROR(builder.build(), ErrorCode::duplicate_identity);
}

// ---------------------------------------------------------------------------
// Multi-source composition
// ---------------------------------------------------------------------------

FT_TEST(derivation, totals_sum_exactly_across_sources) {
  SnapshotBuilder builder;
  builder.add(fsup::make_evidence(power_source("power-capacity-a", 10000, 9000, 400)));
  EvidenceSpec second = power_source("power-capacity-b", 5000, 4000, 250);
  second.protected_capacity = 300;
  builder.add(fsup::make_evidence(second));
  const auto snapshot = builder.build_ok();
  const DimensionTotals* totals = snapshot->find_totals(CapacityDimension::power);
  FT_REQUIRE(totals != nullptr);
  FT_CHECK_EQ(totals->contributing_sources, std::size_t{2});
  FT_CHECK_EQ(totals->measured_usable_sources, std::size_t{2});
  FT_CHECK_EQ(totals->installed.magnitude(), std::int64_t{15000});
  FT_CHECK_EQ(totals->usable.magnitude(), std::int64_t{13000});
  FT_CHECK_EQ(totals->unavailable.magnitude(), std::int64_t{650});
  FT_CHECK_EQ(totals->protected_capacity.magnitude(), std::int64_t{300});
  FT_CHECK_EQ(totals->allocatable.magnitude(), std::int64_t{12700});
  FT_CHECK_EQ(snapshot->sources().size(), std::size_t{2});
}

FT_TEST(derivation, one_unmeasured_source_makes_the_dimension_total_unmeasured) {
  SnapshotBuilder builder;
  builder.add(fsup::make_evidence(power_source("power-capacity-a", 10000, 9000, 400)));
  EvidenceSpec second = power_source("power-capacity-b", 5000, 4000, 250);
  second.usable = std::nullopt;
  second.residual = std::nullopt;
  second.derive_residual = false;
  builder.add(fsup::make_evidence(second));
  const auto snapshot = builder.build_ok();
  const DimensionTotals* totals = snapshot->find_totals(CapacityDimension::power);
  FT_REQUIRE(totals != nullptr);
  FT_CHECK_EQ(totals->contributing_sources, std::size_t{2});
  FT_CHECK(totals->installed.is_known());
  FT_CHECK(totals->usable.is_unknown());
  FT_CHECK(totals->allocatable.is_unknown());
  FT_CHECK_EQ(snapshot->outcome(CapacityDimension::power), CapacityOutcome::unknown);
}
