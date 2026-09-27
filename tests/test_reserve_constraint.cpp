// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Suites: reserve, constraint, reason.
//
// Declared reserves and service floors are evidence, not authority: they are
// validated once at creation, digest-bound, and immutable thereafter. Reason
// codes are part of the public contract and are checked here name by name.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fcap = dccp::facility_capacity;

using fcap::CapacityDimension;
using fcap::CapacityNote;
using fcap::CapacityRequirement;
using fcap::CapacityReserve;
using fcap::CapacityReserveFields;
using fcap::CapacitySourceId;
using fcap::ConstraintId;
using fcap::ErrorCode;
using fcap::Measured;
using fcap::NoteSet;
using fcap::ReasonCode;
using fcap::ReserveId;
using fcap::ReserveKind;
using fcap::ServiceConstraint;
using fcap::ServiceConstraintFields;
using fcap::Tick;
using fcap::Unit;
using fcap::ValidityWindow;

namespace {

constexpr Unit kCanonicalPowerUnit = Unit::milli_watt;

template <class Id>
Id make_id(std::string_view text) {
  const auto parsed = Id::parse(text);
  if (!parsed.has_value()) {
    FT_FAIL("test identifier is invalid: " + std::string(text) + " (" + parsed.error().to_string() + ")");
    throw ::ftest::TestAborted{};
  }
  return parsed.value();
}

Measured measured(std::int64_t magnitude) {
  const auto value = Measured::known(kCanonicalPowerUnit, magnitude);
  if (!value.has_value()) {
    FT_FAIL("test measured value is invalid: " + value.error().to_string());
    throw ::ftest::TestAborted{};
  }
  return value.value();
}

ValidityWindow window(std::uint64_t start, std::uint64_t end) {
  const auto value = ValidityWindow::create(Tick::from_value(start), Tick::from_value(end));
  if (!value.has_value()) {
    FT_FAIL("test window is invalid: " + value.error().to_string());
    throw ::ftest::TestAborted{};
  }
  return value.value();
}

std::string long_detail(std::size_t size) { return std::string(size, 'd'); }

/// A reserve specification that satisfies every documented precondition.
fsup::ReserveSpec baseline_reserve_spec() {
  fsup::ReserveSpec spec;
  spec.amount = 500;
  return spec;
}

/// A constraint specification that satisfies every documented precondition.
fsup::ConstraintSpec baseline_constraint_spec() {
  fsup::ConstraintSpec spec;
  spec.floor_value = 250;
  return spec;
}

/// The fields of a reserve that satisfies every documented precondition, built
/// directly so a single field can be made invalid.
CapacityReserveFields valid_reserve_fields() {
  CapacityReserveFields fields;
  fields.id = make_id<ReserveId>("protection-1");
  fields.source = make_id<CapacitySourceId>("power-capacity");
  fields.dimension = CapacityDimension::power;
  fields.unit = kCanonicalPowerUnit;
  fields.kind = ReserveKind::protection;
  fields.amount = measured(500);
  fields.window = window(10, 20);
  fields.owner = make_id<fcap::OwnerId>("facility-ops");
  fields.service_class = make_id<fcap::ServiceClassId>("tier-1");
  fields.reason = ReasonCode::limiting_by_protection;
  fields.detail = "redundancy held for N+1";
  fields.provenance = fsup::provenance("power-capacity", 7, 3, 100);
  return fields;
}

/// The fields of a service constraint that satisfies every documented
/// precondition, built directly so a single field can be made invalid.
ServiceConstraintFields valid_constraint_fields() {
  ServiceConstraintFields fields;
  fields.id = make_id<ConstraintId>("floor-1");
  fields.dimension = CapacityDimension::power;
  fields.unit = kCanonicalPowerUnit;
  fields.service_class = make_id<fcap::ServiceClassId>("tier-1");
  fields.floor = measured(250);
  fields.window = window(10, 20);
  fields.owner = make_id<fcap::OwnerId>("facility-ops");
  fields.reason = ReasonCode::service_floor_violated;
  fields.detail = "tier-1 floor";
  fields.provenance = fsup::provenance("facility-ops", 7, 3, 100);
  return fields;
}

/// Every `ReserveKind`, written out explicitly so a new enumerator cannot pass
/// unnoticed.
const ReserveKind kAllReserveKinds[] = {ReserveKind::protection, ReserveKind::operational,
                                        ReserveKind::service_obligation, ReserveKind::contingency};

/// Every `ReasonCode`, written out explicitly so a new enumerator cannot pass
/// unnoticed.
const ReasonCode kAllReasonCodes[] = {
    ReasonCode::none,
    ReasonCode::no_source_for_dimension,
    ReasonCode::source_not_reported,
    ReasonCode::dimension_not_offered,
    ReasonCode::coverage_contract_changed,
    ReasonCode::source_stale,
    ReasonCode::source_superseded,
    ReasonCode::source_epoch_mismatch,
    ReasonCode::source_incarnation_mismatch,
    ReasonCode::source_identity_mismatch,
    ReasonCode::snapshot_expired,
    ReasonCode::capacity_generation_advanced,
    ReasonCode::quantity_not_measured,
    ReasonCode::source_state_unknown,
    ReasonCode::source_degraded,
    ReasonCode::source_unavailable,
    ReasonCode::composed_unmeasured,
    ReasonCode::source_closure_violated,
    ReasonCode::reserve_itemisation_mismatch,
    ReasonCode::allocation_over_committed,
    ReasonCode::service_floor_violated,
    ReasonCode::service_floor_indeterminate,
    ReasonCode::allocatable_positive,
    ReasonCode::allocatable_exhausted,
    ReasonCode::limiting_by_unavailable,
    ReasonCode::limiting_by_protection,
    ReasonCode::limiting_by_reserve,
    ReasonCode::limiting_by_residual,
    ReasonCode::limiting_indeterminate,
    ReasonCode::recovered_not_revalidated,
    ReasonCode::recovered_revalidated,
};

/// Every `ErrorCode`, written out explicitly so a new enumerator cannot pass
/// unnoticed.
const ErrorCode kAllErrorCodes[] = {
    ErrorCode::invalid_argument,     ErrorCode::stale_generation,     ErrorCode::stale_authority,
    ErrorCode::conflict,             ErrorCode::not_found,            ErrorCode::already_exists,
    ErrorCode::incompatible_version, ErrorCode::corruption,           ErrorCode::limit_exceeded,
    ErrorCode::unsupported,          ErrorCode::unavailable,          ErrorCode::indeterminate,
    ErrorCode::permission_denied,    ErrorCode::io_failure,           ErrorCode::lock_conflict,
    ErrorCode::invariant_violation,  ErrorCode::unit_mismatch,        ErrorCode::duplicate_identity,
    ErrorCode::precondition_failed,  ErrorCode::checksum_mismatch,    ErrorCode::truncated_input,
    ErrorCode::path_rejected,        ErrorCode::account_mismatch,     ErrorCode::not_measured,
};

}  // namespace

// ---------------------------------------------------------------------------
// reserve
// ---------------------------------------------------------------------------

FT_TEST(reserve, kind_name_and_parse_round_trip) {
  static_assert(sizeof(kAllReserveKinds) / sizeof(kAllReserveKinds[0]) == 4);
  FT_CHECK_EQ(kAllReserveKinds[0], ReserveKind::protection);
  FT_CHECK_EQ(kAllReserveKinds[1], ReserveKind::operational);
  FT_CHECK_EQ(kAllReserveKinds[2], ReserveKind::service_obligation);
  FT_CHECK_EQ(kAllReserveKinds[3], ReserveKind::contingency);

  for (const ReserveKind kind : kAllReserveKinds) {
    const std::string name(fcap::reserve_kind_name(kind));
    FT_CHECK(!name.empty());
    FT_CHECK(name != "unknown");

    ReserveKind parsed = ReserveKind::operational;
    FT_REQUIRE(fcap::parse_reserve_kind(name, parsed));
    FT_CHECK_EQ(parsed, kind);

    FT_CHECK_EQ(ftest::render(kind), name);
  }
}

FT_TEST(reserve, published_kind_names) {
  FT_CHECK_EQ(std::string(fcap::reserve_kind_name(ReserveKind::protection)), "protection");
  FT_CHECK_EQ(std::string(fcap::reserve_kind_name(ReserveKind::operational)), "operational");
  FT_CHECK_EQ(std::string(fcap::reserve_kind_name(ReserveKind::service_obligation)), "service_obligation");
  FT_CHECK_EQ(std::string(fcap::reserve_kind_name(ReserveKind::contingency)), "contingency");
  FT_CHECK_EQ(std::string(fcap::reserve_kind_name(static_cast<ReserveKind>(200))), "unknown");
}

FT_TEST(reserve, unknown_kind_name_fails_to_parse) {
  ReserveKind parsed = ReserveKind::protection;
  FT_CHECK(!fcap::parse_reserve_kind("protections", parsed));
  FT_CHECK_EQ(parsed, ReserveKind::protection);
  FT_CHECK(!fcap::parse_reserve_kind("", parsed));
  FT_CHECK_EQ(parsed, ReserveKind::protection);
  FT_CHECK(!fcap::parse_reserve_kind("PROTECTION", parsed));
  FT_CHECK_EQ(parsed, ReserveKind::protection);
}

FT_TEST(reserve, only_protection_is_protection) {
  FT_CHECK(fcap::reserve_kind_is_protection(ReserveKind::protection));
  FT_CHECK(!fcap::reserve_kind_is_protection(ReserveKind::operational));
  FT_CHECK(!fcap::reserve_kind_is_protection(ReserveKind::service_obligation));
  FT_CHECK(!fcap::reserve_kind_is_protection(ReserveKind::contingency));
}

FT_TEST(reserve, create_accepts_a_well_formed_declaration) {
  const auto reserve = CapacityReserve::create(valid_reserve_fields());
  FT_REQUIRE_OK(reserve);
  FT_CHECK_EQ(reserve.value().id().value(), "protection-1");
  FT_CHECK_EQ(reserve.value().source().value(), "power-capacity");
  FT_CHECK_EQ(reserve.value().dimension(), CapacityDimension::power);
  FT_CHECK_EQ(reserve.value().unit(), Unit::milli_watt);
  FT_CHECK_EQ(reserve.value().kind(), ReserveKind::protection);
  FT_CHECK(reserve.value().supplies_protection());
  FT_CHECK_EQ(reserve.value().owner().value(), "facility-ops");
  FT_CHECK_EQ(reserve.value().service_class().value(), "tier-1");
  FT_CHECK_EQ(reserve.value().reason(), ReasonCode::limiting_by_protection);
  FT_CHECK_EQ(reserve.value().detail(), "redundancy held for N+1");
  FT_CHECK(reserve.value().amount().is_known());
  FT_CHECK_EQ(reserve.value().amount().magnitude(), 500);
  FT_CHECK(reserve.value().provenance() == fsup::provenance("power-capacity", 7, 3, 100));
}

FT_TEST(reserve, create_rejects_an_empty_identity) {
  CapacityReserveFields fields = valid_reserve_fields();
  fields.id = ReserveId();
  const auto reserve = CapacityReserve::create(fields);
  FT_CHECK_ERROR(reserve, ErrorCode::invalid_argument);
  FT_REQUIRE(!reserve.has_value());
  FT_CHECK_EQ(reserve.error().message(), "a reserve has no identity");
}

FT_TEST(reserve, create_rejects_an_empty_source) {
  CapacityReserveFields fields = valid_reserve_fields();
  fields.source = CapacitySourceId();
  fields.provenance.source = CapacitySourceId();
  const auto reserve = CapacityReserve::create(fields);
  FT_CHECK_ERROR(reserve, ErrorCode::invalid_argument);
  FT_REQUIRE(!reserve.has_value());
  FT_CHECK_EQ(reserve.error().message(), "a reserve declares no source it is held against");
}

FT_TEST(reserve, create_rejects_a_unit_that_is_not_canonical) {
  CapacityReserveFields fields = valid_reserve_fields();
  fields.unit = Unit::rack_slot;
  const auto reserve = CapacityReserve::create(fields);
  FT_CHECK_ERROR(reserve, ErrorCode::unit_mismatch);
  FT_REQUIRE(!reserve.has_value());
  FT_CHECK_EQ(reserve.error().constraint(), "unit == canonical_unit(dimension)");
}

FT_TEST(reserve, create_rejects_an_amount_in_another_unit) {
  CapacityReserveFields fields = valid_reserve_fields();
  fields.amount = fsup::known_unit(Unit::milli_watt_thermal, 10);
  const auto reserve = CapacityReserve::create(fields);
  FT_CHECK_ERROR(reserve, ErrorCode::unit_mismatch);
  FT_REQUIRE(!reserve.has_value());
  FT_CHECK_EQ(reserve.error().constraint(), "amount.unit == unit");
}

FT_TEST(reserve, create_rejects_an_over_long_detail) {
  CapacityReserveFields fields = valid_reserve_fields();
  fields.detail = long_detail(fcap::limits::max_detail_length + 1u);
  const auto reserve = CapacityReserve::create(fields);
  FT_CHECK_ERROR(reserve, ErrorCode::limit_exceeded);
  FT_REQUIRE(!reserve.has_value());
  FT_CHECK_EQ(reserve.error().message(), "a reserve detail exceeds the length bound");
}

FT_TEST(reserve, create_rejects_a_non_printable_detail) {
  CapacityReserveFields fields = valid_reserve_fields();
  fields.detail = std::string("held") + '\t';
  const auto reserve = CapacityReserve::create(fields);
  FT_CHECK_ERROR(reserve, ErrorCode::invalid_argument);
  FT_REQUIRE(!reserve.has_value());
  FT_CHECK_EQ(reserve.error().message(), "a reserve detail contains non-printable characters");
}

FT_TEST(reserve, create_accepts_a_detail_exactly_at_the_bound) {
  CapacityReserveFields fields = valid_reserve_fields();
  fields.detail = long_detail(fcap::limits::max_detail_length);
  const auto reserve = CapacityReserve::create(fields);
  FT_REQUIRE_OK(reserve);
  FT_CHECK_EQ(reserve.value().detail().size(), fcap::limits::max_detail_length);
}

FT_TEST(reserve, create_rejects_provenance_naming_a_different_source) {
  CapacityReserveFields fields = valid_reserve_fields();
  fields.provenance = fsup::provenance("cooling-capacity", 7, 3, 100);
  const auto reserve = CapacityReserve::create(fields);
  FT_CHECK_ERROR(reserve, ErrorCode::conflict);
  FT_REQUIRE(!reserve.has_value());
  FT_CHECK_EQ(reserve.error().message(), "reserve provenance names a different source");
}

FT_TEST(reserve, create_reports_a_malformed_provenance_with_its_own_code) {
  CapacityReserveFields fields = valid_reserve_fields();
  fields.provenance.source = CapacitySourceId();
  const auto no_source = CapacityReserve::create(fields);
  FT_CHECK_ERROR(no_source, ErrorCode::invalid_argument);
  FT_REQUIRE(!no_source.has_value());
  FT_CHECK_EQ(no_source.error().message(), "provenance has no source identity");

  fields = valid_reserve_fields();
  fields.provenance.revision = "power=1";
  FT_CHECK_ERROR(CapacityReserve::create(fields), ErrorCode::invalid_argument);

  fields = valid_reserve_fields();
  fields.provenance.revision = std::string(fcap::limits::max_revision_length + 1u, 'r');
  FT_CHECK_ERROR(CapacityReserve::create(fields), ErrorCode::limit_exceeded);

  fields = valid_reserve_fields();
  fields.provenance.evidence_digest = std::string(64, 'Z');
  FT_CHECK_ERROR(CapacityReserve::create(fields), ErrorCode::invalid_argument);
}

FT_TEST(reserve, amount_at_is_the_declared_amount_inside_a_half_open_window) {
  fsup::ReserveSpec spec;
  spec.amount = 400;
  spec.window_start = 10;
  spec.window_end = 20;
  const CapacityReserve reserve = fsup::make_reserve(spec);

  FT_CHECK(reserve.active_at(Tick::from_value(10)));
  FT_CHECK(reserve.active_at(Tick::from_value(19)));
  FT_CHECK(!reserve.active_at(Tick::from_value(20)));

  FT_CHECK_EQ(reserve.amount_at(Tick::from_value(10)), measured(400));
  FT_CHECK_EQ(reserve.amount_at(Tick::from_value(15)), measured(400));
  FT_CHECK_EQ(reserve.amount_at(Tick::from_value(19)), measured(400));
  FT_CHECK(!reserve.amount_at(Tick::from_value(9)).is_known());
  FT_CHECK(!reserve.amount_at(Tick::from_value(20)).is_known());
  FT_CHECK(!reserve.amount_at(Tick::from_value(21)).is_known());
}

FT_TEST(reserve, an_unmeasured_amount_stays_unknown_inside_the_window) {
  fsup::ReserveSpec spec;
  spec.amount = std::nullopt;
  spec.window_start = 10;
  spec.window_end = 20;
  const CapacityReserve reserve = fsup::make_reserve(spec);

  FT_CHECK(reserve.amount().is_unknown());
  FT_CHECK(!reserve.amount_at(Tick::from_value(15)).is_known());
  FT_CHECK(!reserve.amount_at(Tick::from_value(15)).is_known_zero());
}

FT_TEST(reserve, digest_is_a_lowercase_hex_sha256) {
  const CapacityReserve reserve = fsup::make_reserve(baseline_reserve_spec());
  FT_CHECK_EQ(reserve.digest().size(), std::size_t{64});
  FT_CHECK(fcap::is_hex_digest(reserve.digest()));
}

FT_TEST(reserve, digest_is_stable_across_identical_constructions) {
  const CapacityReserve first = fsup::make_reserve(baseline_reserve_spec());
  const CapacityReserve second = fsup::make_reserve(baseline_reserve_spec());
  FT_CHECK_EQ(first.digest(), second.digest());
  FT_CHECK(first == second);
}

FT_TEST(reserve, digest_changes_with_the_identity_and_kind) {
  const std::string baseline = fsup::make_reserve(baseline_reserve_spec()).digest();

  fsup::ReserveSpec other_id = baseline_reserve_spec();
  other_id.id = "protection-2";
  FT_CHECK_NE(fsup::make_reserve(other_id).digest(), baseline);

  fsup::ReserveSpec other_kind = baseline_reserve_spec();
  other_kind.kind = ReserveKind::contingency;
  FT_CHECK_NE(fsup::make_reserve(other_kind).digest(), baseline);

  fsup::ReserveSpec other_source = baseline_reserve_spec();
  other_source.source = "cooling-capacity";
  FT_CHECK_NE(fsup::make_reserve(other_source).digest(), baseline);

  fsup::ReserveSpec other_owner = baseline_reserve_spec();
  other_owner.owner = "other-ops";
  FT_CHECK_NE(fsup::make_reserve(other_owner).digest(), baseline);

  fsup::ReserveSpec other_class = baseline_reserve_spec();
  other_class.service_class = "tier-2";
  FT_CHECK_NE(fsup::make_reserve(other_class).digest(), baseline);
}

FT_TEST(reserve, digest_changes_with_the_amount_and_window) {
  const std::string baseline = fsup::make_reserve(baseline_reserve_spec()).digest();

  fsup::ReserveSpec other_amount = baseline_reserve_spec();
  other_amount.amount = 1234;
  FT_CHECK_NE(fsup::make_reserve(other_amount).digest(), baseline);

  fsup::ReserveSpec unmeasured = baseline_reserve_spec();
  unmeasured.amount = std::nullopt;
  FT_CHECK_NE(fsup::make_reserve(unmeasured).digest(), baseline);

  fsup::ReserveSpec other_start = baseline_reserve_spec();
  other_start.window_start = 5;
  other_start.window_end = 20;
  FT_CHECK_NE(fsup::make_reserve(other_start).digest(), baseline);

  fsup::ReserveSpec other_end = baseline_reserve_spec();
  other_end.window_start = 10;
  other_end.window_end = 21;
  FT_CHECK_NE(fsup::make_reserve(other_end).digest(), baseline);
}

FT_TEST(reserve, digest_changes_with_the_reason_detail_and_provenance) {
  const std::string baseline = fsup::make_reserve(baseline_reserve_spec()).digest();

  fsup::ReserveSpec other_reason = baseline_reserve_spec();
  other_reason.reason = ReasonCode::source_degraded;
  FT_CHECK_NE(fsup::make_reserve(other_reason).digest(), baseline);

  fsup::ReserveSpec other_detail = baseline_reserve_spec();
  other_detail.detail = "a different detail";
  FT_CHECK_NE(fsup::make_reserve(other_detail).digest(), baseline);

  fsup::ReserveSpec other_epoch = baseline_reserve_spec();
  other_epoch.epoch = 8;
  FT_CHECK_NE(fsup::make_reserve(other_epoch).digest(), baseline);

  fsup::ReserveSpec other_incarnation = baseline_reserve_spec();
  other_incarnation.incarnation = 4;
  FT_CHECK_NE(fsup::make_reserve(other_incarnation).digest(), baseline);

  fsup::ReserveSpec other_instant = baseline_reserve_spec();
  other_instant.produced_at = 101;
  FT_CHECK_NE(fsup::make_reserve(other_instant).digest(), baseline);
}

FT_TEST(reserve, digest_changes_with_the_dimension) {
  const std::string baseline = fsup::make_reserve(baseline_reserve_spec()).digest();

  fsup::ReserveSpec cooling = baseline_reserve_spec();
  cooling.dimension = CapacityDimension::cooling;
  cooling.unit = Unit::milli_watt_thermal;
  FT_CHECK_NE(fsup::make_reserve(cooling).digest(), baseline);

  fsup::ReserveSpec rack = baseline_reserve_spec();
  rack.dimension = CapacityDimension::rack;
  rack.unit = Unit::rack_slot;
  FT_CHECK_NE(fsup::make_reserve(rack).digest(), baseline);
}

FT_TEST(reserve, to_string_renders_the_declaration) {
  const CapacityReserve reserve = fsup::make_reserve(baseline_reserve_spec());
  const std::string rendered = reserve.to_string();
  FT_CHECK(rendered.find("protection-1") != std::string::npos);
  FT_CHECK(rendered.find("kind=protection") != std::string::npos);
  FT_CHECK(rendered.find("dimension=power") != std::string::npos);
  FT_CHECK(rendered.find("source=power-capacity") != std::string::npos);
  FT_CHECK(rendered.find("amount=500 mW") != std::string::npos);
  FT_CHECK(rendered.find("window=") != std::string::npos);
  FT_CHECK(rendered.find("owner=facility-ops") != std::string::npos);
  FT_CHECK_EQ(ftest::render(reserve), rendered);
}

// ---------------------------------------------------------------------------
// constraint
// ---------------------------------------------------------------------------

FT_TEST(constraint, create_accepts_a_well_formed_declaration) {
  const auto constraint = ServiceConstraint::create(valid_constraint_fields());
  FT_REQUIRE_OK(constraint);
  FT_CHECK_EQ(constraint.value().id().value(), "floor-1");
  FT_CHECK_EQ(constraint.value().dimension(), CapacityDimension::power);
  FT_CHECK_EQ(constraint.value().unit(), Unit::milli_watt);
  FT_CHECK_EQ(constraint.value().service_class().value(), "tier-1");
  FT_CHECK_EQ(constraint.value().floor_value(), measured(250));
  FT_CHECK_EQ(constraint.value().owner().value(), "facility-ops");
  FT_CHECK_EQ(constraint.value().reason(), ReasonCode::service_floor_violated);
  FT_CHECK_EQ(constraint.value().detail(), "tier-1 floor");
  FT_CHECK(constraint.value().active_at(Tick::from_value(10)));
  FT_CHECK(constraint.value().active_at(Tick::from_value(19)));
  FT_CHECK(!constraint.value().active_at(Tick::from_value(20)));
}

FT_TEST(constraint, create_rejects_an_empty_identity) {
  ServiceConstraintFields fields = valid_constraint_fields();
  fields.id = ConstraintId();
  const auto constraint = ServiceConstraint::create(fields);
  FT_CHECK_ERROR(constraint, ErrorCode::invalid_argument);
  FT_REQUIRE(!constraint.has_value());
  FT_CHECK_EQ(constraint.error().message(), "a service constraint has no identity");
}

FT_TEST(constraint, create_rejects_a_unit_that_is_not_canonical) {
  ServiceConstraintFields fields = valid_constraint_fields();
  fields.unit = Unit::rack_unit;
  const auto constraint = ServiceConstraint::create(fields);
  FT_CHECK_ERROR(constraint, ErrorCode::unit_mismatch);
  FT_REQUIRE(!constraint.has_value());
  FT_CHECK_EQ(constraint.error().constraint(), "unit == canonical_unit(dimension)");
}

FT_TEST(constraint, create_rejects_a_floor_in_another_unit) {
  ServiceConstraintFields fields = valid_constraint_fields();
  fields.floor = fsup::known_unit(Unit::rack_unit, 10);
  const auto constraint = ServiceConstraint::create(fields);
  FT_CHECK_ERROR(constraint, ErrorCode::unit_mismatch);
  FT_REQUIRE(!constraint.has_value());
  FT_CHECK_EQ(constraint.error().constraint(), "floor.unit == unit");
}

FT_TEST(constraint, create_rejects_an_over_long_detail) {
  ServiceConstraintFields fields = valid_constraint_fields();
  fields.detail = long_detail(fcap::limits::max_detail_length + 1u);
  const auto constraint = ServiceConstraint::create(fields);
  FT_CHECK_ERROR(constraint, ErrorCode::limit_exceeded);
  FT_REQUIRE(!constraint.has_value());
  FT_CHECK_EQ(constraint.error().constraint(), "detail");
}

FT_TEST(constraint, create_rejects_a_non_printable_detail) {
  ServiceConstraintFields fields = valid_constraint_fields();
  fields.detail = std::string("floor") + static_cast<char>(0x1F);
  const auto constraint = ServiceConstraint::create(fields);
  FT_CHECK_ERROR(constraint, ErrorCode::invalid_argument);
  FT_REQUIRE(!constraint.has_value());
  FT_CHECK_EQ(constraint.error().constraint(), "detail");
}

FT_TEST(constraint, create_accepts_a_detail_exactly_at_the_bound) {
  ServiceConstraintFields fields = valid_constraint_fields();
  fields.detail = long_detail(fcap::limits::max_detail_length);
  const auto constraint = ServiceConstraint::create(fields);
  FT_REQUIRE_OK(constraint);
  FT_CHECK_EQ(constraint.value().detail().size(), fcap::limits::max_detail_length);
}

FT_TEST(constraint, create_reports_a_malformed_provenance_with_its_own_code) {
  ServiceConstraintFields fields = valid_constraint_fields();
  fields.provenance.source = CapacitySourceId();
  const auto no_source = ServiceConstraint::create(fields);
  FT_CHECK_ERROR(no_source, ErrorCode::invalid_argument);
  FT_REQUIRE(!no_source.has_value());
  FT_CHECK_EQ(no_source.error().message(), "provenance has no source identity");

  fields = valid_constraint_fields();
  fields.provenance.revision = std::string(fcap::limits::max_revision_length + 1u, 'r');
  FT_CHECK_ERROR(ServiceConstraint::create(fields), ErrorCode::limit_exceeded);

  fields = valid_constraint_fields();
  fields.provenance.evidence_digest = std::string(63, 'a');
  FT_CHECK_ERROR(ServiceConstraint::create(fields), ErrorCode::invalid_argument);
}

FT_TEST(constraint, create_accepts_an_unmeasured_floor) {
  ServiceConstraintFields fields = valid_constraint_fields();
  fields.floor = Measured::unknown();
  const auto constraint = ServiceConstraint::create(fields);
  FT_REQUIRE_OK(constraint);
  FT_CHECK(constraint.value().floor_value().is_unknown());
  FT_CHECK(constraint.value().active_at(Tick::from_value(10)));
  FT_CHECK(!constraint.value().active_at(Tick::from_value(20)));
}

FT_TEST(constraint, digest_is_a_lowercase_hex_sha256) {
  const ServiceConstraint constraint = fsup::make_constraint(baseline_constraint_spec());
  FT_CHECK_EQ(constraint.digest().size(), std::size_t{64});
  FT_CHECK(fcap::is_hex_digest(constraint.digest()));
}

FT_TEST(constraint, digest_is_stable_and_changes_with_any_field) {
  const std::string baseline = fsup::make_constraint(baseline_constraint_spec()).digest();
  FT_CHECK_EQ(fsup::make_constraint(baseline_constraint_spec()).digest(), baseline);

  fsup::ConstraintSpec other_id = baseline_constraint_spec();
  other_id.id = "floor-2";
  FT_CHECK_NE(fsup::make_constraint(other_id).digest(), baseline);

  fsup::ConstraintSpec other_floor = baseline_constraint_spec();
  other_floor.floor_value = 999;
  FT_CHECK_NE(fsup::make_constraint(other_floor).digest(), baseline);

  fsup::ConstraintSpec unmeasured = baseline_constraint_spec();
  unmeasured.floor_value = std::nullopt;
  FT_CHECK_NE(fsup::make_constraint(unmeasured).digest(), baseline);

  fsup::ConstraintSpec other_class = baseline_constraint_spec();
  other_class.service_class = "tier-2";
  FT_CHECK_NE(fsup::make_constraint(other_class).digest(), baseline);

  fsup::ConstraintSpec other_detail = baseline_constraint_spec();
  other_detail.detail = "another floor";
  FT_CHECK_NE(fsup::make_constraint(other_detail).digest(), baseline);

  fsup::ConstraintSpec other_window = baseline_constraint_spec();
  other_window.window_start = 11;
  FT_CHECK_NE(fsup::make_constraint(other_window).digest(), baseline);

  fsup::ConstraintSpec other_owner = baseline_constraint_spec();
  other_owner.owner = "other-ops";
  FT_CHECK_NE(fsup::make_constraint(other_owner).digest(), baseline);

  fsup::ConstraintSpec other_reason = baseline_constraint_spec();
  other_reason.reason = ReasonCode::service_floor_indeterminate;
  FT_CHECK_NE(fsup::make_constraint(other_reason).digest(), baseline);

  fsup::ConstraintSpec other_epoch = baseline_constraint_spec();
  other_epoch.epoch = 8;
  FT_CHECK_NE(fsup::make_constraint(other_epoch).digest(), baseline);

  fsup::ConstraintSpec other_dimension = baseline_constraint_spec();
  other_dimension.dimension = CapacityDimension::cooling;
  other_dimension.unit = Unit::milli_watt_thermal;
  FT_CHECK_NE(fsup::make_constraint(other_dimension).digest(), baseline);
}

FT_TEST(constraint, to_string_renders_the_declaration) {
  const ServiceConstraint constraint = fsup::make_constraint(baseline_constraint_spec());
  const std::string rendered = constraint.to_string();
  FT_CHECK(rendered.find("floor-1") != std::string::npos);
  FT_CHECK(rendered.find("dimension=power") != std::string::npos);
  FT_CHECK(rendered.find("floor=250 mW") != std::string::npos);
  FT_CHECK(rendered.find("window=") != std::string::npos);
  FT_CHECK(rendered.find("service_class=tier-1") != std::string::npos);
  FT_CHECK_EQ(ftest::render(constraint), rendered);
}

FT_TEST(constraint, requirement_normalize_sorts_and_de_duplicates) {
  CapacityRequirement requirement;
  requirement.dimension = CapacityDimension::power;
  requirement.required_sources = {make_id<CapacitySourceId>("src-c"), make_id<CapacitySourceId>("src-a"),
                                  make_id<CapacitySourceId>("src-b"), make_id<CapacitySourceId>("src-a"),
                                  make_id<CapacitySourceId>("src-c")};

  FT_CHECK_OK(requirement.normalize());
  FT_CHECK_EQ(requirement.required_sources.size(), std::size_t{3});
  FT_CHECK_EQ(requirement.required_sources[0].value(), "src-a");
  FT_CHECK_EQ(requirement.required_sources[1].value(), "src-b");
  FT_CHECK_EQ(requirement.required_sources[2].value(), "src-c");
}

FT_TEST(constraint, requirement_normalize_of_an_empty_list_succeeds) {
  CapacityRequirement requirement;
  FT_CHECK_OK(requirement.normalize());
  FT_CHECK(requirement.required_sources.empty());
  FT_CHECK_EQ(requirement.to_string(), "space required");
}

FT_TEST(constraint, requirement_normalize_rejects_an_empty_source_identity) {
  CapacityRequirement requirement;
  requirement.required_sources = {make_id<CapacitySourceId>("src-a"), CapacitySourceId()};
  const auto status = requirement.normalize();
  FT_CHECK_ERROR(status, ErrorCode::invalid_argument);
  FT_REQUIRE(!status.has_value());
  FT_CHECK_EQ(status.error().message(), "a coverage requirement declares an empty source identity");
}

FT_TEST(constraint, requirement_normalize_rejects_too_many_sources) {
  CapacityRequirement requirement;
  for (std::size_t index = 0; index <= fcap::limits::max_evidence_sources; ++index) {
    requirement.required_sources.push_back(
        make_id<CapacitySourceId>("src-" + std::to_string(static_cast<unsigned long long>(index))));
  }
  FT_CHECK_EQ(requirement.required_sources.size(), fcap::limits::max_evidence_sources + 1u);

  const auto status = requirement.normalize();
  FT_CHECK_ERROR(status, ErrorCode::limit_exceeded);
  FT_REQUIRE(!status.has_value());
  FT_CHECK_EQ(status.error().message(), "a coverage requirement declares too many sources");
}

FT_TEST(constraint, requirement_normalize_accepts_exactly_the_bound) {
  CapacityRequirement requirement;
  for (std::size_t index = 0; index < fcap::limits::max_evidence_sources; ++index) {
    requirement.required_sources.push_back(
        make_id<CapacitySourceId>("src-" + std::to_string(static_cast<unsigned long long>(index))));
  }
  FT_CHECK_OK(requirement.normalize());
  FT_CHECK_EQ(requirement.required_sources.size(), fcap::limits::max_evidence_sources);
}

FT_TEST(constraint, requirement_normalize_checks_the_count_before_the_identities) {
  CapacityRequirement requirement;
  for (std::size_t index = 0; index <= fcap::limits::max_evidence_sources; ++index) {
    requirement.required_sources.push_back(
        make_id<CapacitySourceId>("src-" + std::to_string(static_cast<unsigned long long>(index))));
  }
  requirement.required_sources.push_back(CapacitySourceId());
  FT_CHECK_ERROR(requirement.normalize(), ErrorCode::limit_exceeded);
}

FT_TEST(constraint, requirement_equality_and_rendering) {
  CapacityRequirement first;
  first.dimension = CapacityDimension::power;
  first.required = true;
  first.required_sources = {make_id<CapacitySourceId>("src-b"), make_id<CapacitySourceId>("src-a")};
  FT_CHECK_OK(first.normalize());

  const CapacityRequirement second = first;
  FT_CHECK(first == second);

  CapacityRequirement optional = first;
  optional.required = false;
  FT_CHECK(first != optional);
  FT_CHECK_EQ(optional.to_string(), "power optional sources=[src-a,src-b]");

  CapacityRequirement other_dimension = first;
  other_dimension.dimension = CapacityDimension::cooling;
  FT_CHECK(first != other_dimension);

  FT_CHECK_EQ(ftest::render(first), first.to_string());
}

// ---------------------------------------------------------------------------
// reason
// ---------------------------------------------------------------------------

FT_TEST(reason, every_reason_code_round_trips_through_its_name) {
  static_assert(sizeof(kAllReasonCodes) / sizeof(kAllReasonCodes[0]) == 31);

  for (const ReasonCode code : kAllReasonCodes) {
    const std::string name(fcap::reason_code_name(code));
    FT_CHECK(!name.empty());
    FT_CHECK(name != "unknown_reason_code");

    ReasonCode parsed = ReasonCode::none;
    FT_REQUIRE(fcap::parse_reason_code(name, parsed));
    FT_CHECK_EQ(parsed, code);
    FT_CHECK_EQ(ftest::render(code), name);
  }
}

FT_TEST(reason, reason_code_names_are_distinct) {
  std::vector<std::string> names;
  for (const ReasonCode code : kAllReasonCodes) {
    names.emplace_back(fcap::reason_code_name(code));
  }

  for (std::size_t index = 0; index < names.size(); ++index) {
    for (std::size_t other = index + 1; other < names.size(); ++other) {
      FT_CHECK(names[index] != names[other]);
    }
  }
}

FT_TEST(reason, unknown_reason_code_name_fails_to_parse) {
  ReasonCode parsed = ReasonCode::none;
  FT_CHECK(!fcap::parse_reason_code("", parsed));
  FT_CHECK(!fcap::parse_reason_code("unknown_reason_code", parsed));
  FT_CHECK(!fcap::parse_reason_code("source_stale ", parsed));
  FT_CHECK(!fcap::parse_reason_code("SOURCE_STALE", parsed));
  FT_CHECK_EQ(parsed, ReasonCode::none);
}

FT_TEST(reason, a_reason_code_with_no_name_renders_a_placeholder) {
  FT_CHECK_EQ(std::string(fcap::reason_code_name(static_cast<ReasonCode>(60000))), "unknown_reason_code");
  // Five is a gap in the published numbering.
  FT_CHECK_EQ(std::string(fcap::reason_code_name(static_cast<ReasonCode>(5))), "unknown_reason_code");
}

FT_TEST(reason, every_error_code_round_trips_through_its_name) {
  static_assert(sizeof(kAllErrorCodes) / sizeof(kAllErrorCodes[0]) == 24);

  for (const ErrorCode code : kAllErrorCodes) {
    const std::string name(fcap::error_code_name(code));
    FT_CHECK(!name.empty());
    FT_CHECK(name != "unknown_error_code");

    ErrorCode parsed = ErrorCode::indeterminate;
    FT_REQUIRE(fcap::parse_error_code(name, parsed));
    FT_CHECK_EQ(parsed, code);
    FT_CHECK_EQ(ftest::render(code), name);
    FT_CHECK(!fcap::error_code_description(code).empty());
  }
}

FT_TEST(reason, error_code_names_are_distinct) {
  std::vector<std::string> names;
  for (const ErrorCode code : kAllErrorCodes) {
    names.emplace_back(fcap::error_code_name(code));
  }

  for (std::size_t index = 0; index < names.size(); ++index) {
    for (std::size_t other = index + 1; other < names.size(); ++other) {
      FT_CHECK(names[index] != names[other]);
    }
  }
}

FT_TEST(reason, unknown_error_code_name_fails_to_parse) {
  ErrorCode parsed = ErrorCode::indeterminate;
  FT_CHECK(!fcap::parse_error_code("", parsed));
  FT_CHECK(!fcap::parse_error_code("unknown_error_code", parsed));
  FT_CHECK(!fcap::parse_error_code("no_such_code", parsed));
  FT_CHECK_EQ(parsed, ErrorCode::indeterminate);
}

FT_TEST(reason, an_error_code_with_no_name_renders_a_placeholder) {
  FT_CHECK_EQ(std::string(fcap::error_code_name(static_cast<ErrorCode>(60000))), "unknown_error_code");
  FT_CHECK_EQ(std::string(fcap::error_code_description(static_cast<ErrorCode>(60000))), "unrecognised");
}

FT_TEST(reason, note_set_adding_a_duplicate_does_not_grow_the_set) {
  NoteSet notes;
  FT_CHECK(notes.empty());
  FT_CHECK_EQ(notes.size(), std::size_t{0});

  const CapacityNote note = CapacityNote::plain(ReasonCode::source_stale, "stale evidence");
  FT_CHECK(notes.add(note));
  FT_CHECK_EQ(notes.size(), std::size_t{1});
  FT_CHECK(notes.add(note));
  FT_CHECK_EQ(notes.size(), std::size_t{1});
  FT_CHECK(notes.add(CapacityNote::plain(ReasonCode::source_stale, "stale evidence")));
  FT_CHECK_EQ(notes.size(), std::size_t{1});

  // A different detail is a different note.
  FT_CHECK(notes.add(CapacityNote::plain(ReasonCode::source_stale, "another detail")));
  FT_CHECK_EQ(notes.size(), std::size_t{2});
}

FT_TEST(reason, note_set_orders_by_code_then_dimension_then_source_then_detail) {
  const CapacitySourceId source_a = make_id<CapacitySourceId>("src-a");
  const CapacitySourceId source_b = make_id<CapacitySourceId>("src-b");

  const CapacityNote without_dimension = CapacityNote::plain(ReasonCode::source_stale, "z");
  const CapacityNote space_source_a_detail_a =
      CapacityNote(ReasonCode::source_stale, CapacityDimension::space, source_a, "a");
  const CapacityNote space_source_a_detail_c =
      CapacityNote(ReasonCode::source_stale, CapacityDimension::space, source_a, "c");
  const CapacityNote power_source_b =
      CapacityNote(ReasonCode::source_stale, CapacityDimension::power, source_b, "b");
  const CapacityNote higher_code =
      CapacityNote(ReasonCode::allocatable_positive, CapacityDimension::space, CapacitySourceId(), "");

  // Added out of order on purpose.
  NoteSet notes;
  FT_CHECK(notes.add(higher_code));
  FT_CHECK(notes.add(power_source_b));
  FT_CHECK(notes.add(space_source_a_detail_c));
  FT_CHECK(notes.add(without_dimension));
  FT_CHECK(notes.add(space_source_a_detail_a));

  FT_REQUIRE(notes.size() == 5u);
  FT_CHECK(notes[0] == without_dimension);
  FT_CHECK(notes[1] == space_source_a_detail_a);
  FT_CHECK(notes[2] == space_source_a_detail_c);
  FT_CHECK(notes[3] == power_source_b);
  FT_CHECK(notes[4] == higher_code);
}

FT_TEST(reason, note_set_find_and_contains) {
  NoteSet notes;
  FT_CHECK(!notes.contains(ReasonCode::source_stale));
  FT_CHECK(notes.find(ReasonCode::source_stale) == nullptr);

  const CapacityNote stale = CapacityNote::plain(ReasonCode::source_stale, "stale evidence");
  FT_CHECK(notes.add(stale));
  FT_CHECK(notes.add(CapacityNote::plain(ReasonCode::allocatable_exhausted, "zero")));

  FT_CHECK(notes.contains(ReasonCode::source_stale));
  FT_CHECK(notes.contains(ReasonCode::allocatable_exhausted));
  FT_CHECK(!notes.contains(ReasonCode::source_degraded));

  const CapacityNote* found = notes.find(ReasonCode::source_stale);
  FT_REQUIRE(found != nullptr);
  FT_CHECK(*found == stale);
  FT_CHECK(notes.find(ReasonCode::source_degraded) == nullptr);
}

FT_TEST(reason, note_set_find_returns_the_first_note_with_the_code) {
  const CapacitySourceId source = make_id<CapacitySourceId>("src-a");
  NoteSet notes;
  FT_CHECK(notes.add(CapacityNote(ReasonCode::source_stale, CapacityDimension::space, source, "first")));
  FT_CHECK(notes.add(CapacityNote(ReasonCode::source_stale, CapacityDimension::power, source, "second")));

  const CapacityNote* found = notes.find(ReasonCode::source_stale);
  FT_REQUIRE(found != nullptr);
  FT_CHECK_EQ(found->detail, "first");
}

FT_TEST(reason, note_set_merge_of_overlapping_sets_is_idempotent) {
  const CapacitySourceId source = make_id<CapacitySourceId>("src-a");
  const CapacityNote shared = CapacityNote::plain(ReasonCode::source_stale, "shared");
  const CapacityNote left_only = CapacityNote::plain(ReasonCode::source_degraded, "left");
  const CapacityNote right_only =
      CapacityNote(ReasonCode::source_unavailable, CapacityDimension::power, source, "right");

  NoteSet left;
  FT_CHECK(left.add(shared));
  FT_CHECK(left.add(left_only));

  NoteSet right;
  FT_CHECK(right.add(shared));
  FT_CHECK(right.add(right_only));

  FT_CHECK(left.merge(right));
  FT_CHECK_EQ(left.size(), std::size_t{3});
  FT_CHECK(left.contains(ReasonCode::source_stale));
  FT_CHECK(left.contains(ReasonCode::source_degraded));
  FT_CHECK(left.contains(ReasonCode::source_unavailable));

  const NoteSet after_first_merge = left;
  FT_CHECK(left.merge(right));
  FT_CHECK(left == after_first_merge);
  FT_CHECK_EQ(left.size(), std::size_t{3});

  FT_CHECK(left.merge(right));
  FT_CHECK(left == after_first_merge);
}

FT_TEST(reason, note_set_merge_of_an_empty_set_changes_nothing) {
  NoteSet notes;
  FT_CHECK(notes.add(CapacityNote::plain(ReasonCode::source_stale, "stale")));
  const NoteSet before = notes;

  const NoteSet empty;
  FT_CHECK(notes.merge(empty));
  FT_CHECK(notes == before);
}

FT_TEST(reason, note_set_to_string_is_one_line_per_note) {
  NoteSet notes;
  FT_CHECK(notes.add(CapacityNote::plain(ReasonCode::source_stale, "stale evidence")));
  FT_CHECK(notes.add(CapacityNote(ReasonCode::allocatable_exhausted, CapacityDimension::power,
                                  make_id<CapacitySourceId>("power-capacity"), "zero")));

  const std::string rendered = notes.to_string();
  FT_CHECK_EQ(rendered,
              "source_stale detail=stale evidence\n"
              "allocatable_exhausted dimension=power source=power-capacity detail=zero\n");
  FT_CHECK_EQ(notes[0].to_string(), "source_stale detail=stale evidence");
  FT_CHECK_EQ(notes[1].to_string(), "allocatable_exhausted dimension=power source=power-capacity detail=zero");

  const NoteSet empty;
  FT_CHECK_EQ(empty.to_string(), std::string());
}

FT_TEST(reason, note_constructor_records_the_dimension) {
  const CapacitySourceId source = make_id<CapacitySourceId>("power-capacity");
  const CapacityNote note(ReasonCode::source_stale, CapacityDimension::cooling, source, "detail");

  FT_CHECK(note.has_dimension);
  FT_CHECK_EQ(note.dimension, CapacityDimension::cooling);
  FT_CHECK_EQ(note.code, ReasonCode::source_stale);
  FT_CHECK_EQ(note.source, source);
  FT_CHECK_EQ(note.detail, "detail");
  FT_CHECK_EQ(note.to_string(), "source_stale dimension=cooling source=power-capacity detail=detail");
}

FT_TEST(reason, note_plain_sets_no_dimension) {
  const CapacityNote note = CapacityNote::plain(ReasonCode::source_stale, "detail");

  FT_CHECK(!note.has_dimension);
  FT_CHECK(note.source.empty());
  FT_CHECK_EQ(note.code, ReasonCode::source_stale);
  FT_CHECK_EQ(note.detail, "detail");
  FT_CHECK_EQ(note.to_string(), "source_stale detail=detail");

  const CapacityNote bare = CapacityNote::plain(ReasonCode::none, "");
  FT_CHECK(!bare.has_dimension);
  FT_CHECK(bare.source.empty());
  FT_CHECK(bare.detail.empty());
  FT_CHECK_EQ(bare.to_string(), "none");
}

FT_TEST(reason, note_constructor_truncates_an_over_long_detail) {
  const CapacitySourceId source = make_id<CapacitySourceId>("power-capacity");
  const CapacityNote note(ReasonCode::source_stale, CapacityDimension::power, source,
                          long_detail(fcap::limits::max_detail_length + 40u));

  FT_CHECK_EQ(note.detail.size(), fcap::limits::max_detail_length);
  FT_CHECK_EQ(note.detail, long_detail(fcap::limits::max_detail_length));
}

FT_TEST(reason, note_plain_truncates_an_over_long_detail) {
  const CapacityNote note = CapacityNote::plain(ReasonCode::source_stale,
                                                long_detail(fcap::limits::max_detail_length + 40u));
  FT_CHECK_EQ(note.detail.size(), fcap::limits::max_detail_length);
  FT_CHECK_EQ(note.detail, long_detail(fcap::limits::max_detail_length));
}

FT_TEST(reason, note_equality_covers_every_field) {
  const CapacitySourceId source = make_id<CapacitySourceId>("power-capacity");
  const CapacityNote base(ReasonCode::source_stale, CapacityDimension::power, source, "detail");

  FT_CHECK(base == CapacityNote(ReasonCode::source_stale, CapacityDimension::power, source, "detail"));
  FT_CHECK(base != CapacityNote(ReasonCode::source_degraded, CapacityDimension::power, source, "detail"));
  FT_CHECK(base != CapacityNote(ReasonCode::source_stale, CapacityDimension::cooling, source, "detail"));
  FT_CHECK(base != CapacityNote(ReasonCode::source_stale, CapacityDimension::power, CapacitySourceId(), "detail"));
  FT_CHECK(base != CapacityNote(ReasonCode::source_stale, CapacityDimension::power, source, "other"));

  // A plain note is not equal to one that records a dimension, even when the
  // dimension ordinal happens to match.
  const CapacityNote plain = CapacityNote::plain(ReasonCode::source_stale, "detail");
  const CapacityNote with_space(ReasonCode::source_stale, CapacityDimension::space, CapacitySourceId(), "detail");
  FT_CHECK(plain != with_space);
}
