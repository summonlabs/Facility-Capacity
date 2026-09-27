// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Every rule SourceEvidence::create enforces, checked at the boundary where it
// is enforced rather than through a later stage.

#include <optional>
#include <string>

#include "test_framework.hpp"
#include "test_rng.hpp"
#include "test_support.hpp"

namespace {

using namespace dccp::facility_capacity;

SourceEvidenceFields base_fields() {
  SourceEvidenceFields fields;
  fields.source = *CapacitySourceId::parse("power-capacity");
  fields.dimension = CapacityDimension::power;
  fields.unit = Unit::milli_watt;
  fields.generation = EvidenceGeneration::from_value(1);
  fields.observed_at = Tick::from_value(100);
  fields.provenance = fsup::provenance("power-capacity", 7, 3, 100);
  fields.state = OperationalState::nominal;
  fields.installed = fsup::known(CapacityDimension::power, 10000);
  fields.observed = fsup::known(CapacityDimension::power, 10000);
  fields.usable = fsup::known(CapacityDimension::power, 9000);
  fields.unavailable = fsup::known(CapacityDimension::power, 400);
  fields.residual = fsup::known(CapacityDimension::power, 600);
  fields.protected_capacity = fsup::known(CapacityDimension::power, 1000);
  fields.reserved = fsup::known(CapacityDimension::power, 500);
  return fields;
}

}  // namespace

FT_TEST(evidence, a_valid_record_is_created_and_digest_bound) {
  auto first = SourceEvidence::create(base_fields());
  FT_REQUIRE_OK(first);
  auto second = SourceEvidence::create(base_fields());
  FT_REQUIRE_OK(second);
  FT_CHECK_EQ(first.value().digest(), second.value().digest());
  FT_CHECK_EQ(first.value().digest().size(), std::size_t{64});
  FT_CHECK(first.value().closure_proven());
  FT_CHECK_EQ(first.value().allocatable().magnitude(), std::int64_t{7500});
  FT_CHECK_EQ(first.value().residual().magnitude(), std::int64_t{600});
}

FT_TEST(evidence, the_digest_changes_when_any_field_changes) {
  auto reference = SourceEvidence::create(base_fields());
  FT_REQUIRE_OK(reference);

  const auto differs = [&reference](SourceEvidenceFields fields) {
    auto other = SourceEvidence::create(std::move(fields));
    FT_REQUIRE_OK(other);
    FT_CHECK_NE(other.value().digest(), reference.value().digest());
  };

  SourceEvidenceFields fields = base_fields();
  fields.generation = EvidenceGeneration::from_value(2);
  differs(fields);

  fields = base_fields();
  fields.observed_at = Tick::from_value(101);
  differs(fields);

  fields = base_fields();
  fields.state = OperationalState::degraded;
  differs(fields);

  fields = base_fields();
  fields.installed = fsup::known(CapacityDimension::power, 10001);
  fields.usable = fsup::known(CapacityDimension::power, 9001);
  differs(fields);

  fields = base_fields();
  fields.protected_capacity = fsup::known(CapacityDimension::power, 1001);
  fields.reserved = fsup::known(CapacityDimension::power, 499);
  differs(fields);

  fields = base_fields();
  fields.provenance.revision = "test/2.0.0";
  differs(fields);

  fields = base_fields();
  fields.provenance.evidence_digest = std::string(64, 'a');
  differs(fields);
}

FT_TEST(evidence, identity_and_unit_are_validated) {
  SourceEvidenceFields fields = base_fields();
  fields.source = CapacitySourceId();
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::invalid_argument);

  fields = base_fields();
  fields.unit = Unit::rack_slot;
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::unit_mismatch);

  fields = base_fields();
  fields.dimension = CapacityDimension::space;
  fields.unit = Unit::milli_watt;
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::unit_mismatch);

  fields = base_fields();
  fields.generation = EvidenceGeneration::from_value(0);
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::invalid_argument);
}

FT_TEST(evidence, a_measured_value_in_another_unit_is_refused) {
  SourceEvidenceFields fields = base_fields();
  fields.usable = fsup::known_unit(Unit::rack_unit, 9000);
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::unit_mismatch);
}

FT_TEST(evidence, installed_must_be_measured) {
  SourceEvidenceFields fields = base_fields();
  fields.installed = Measured::unknown();
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::invalid_argument);
}

FT_TEST(evidence, observed_cannot_exceed_installed) {
  SourceEvidenceFields fields = base_fields();
  fields.observed = fsup::known(CapacityDimension::power, 10001);
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::account_mismatch);
}

FT_TEST(evidence, the_physical_closure_must_hold_exactly) {
  auto good = SourceEvidence::create(base_fields());
  FT_REQUIRE_OK(good);
  FT_CHECK(good.value().closure_proven());

  SourceEvidenceFields over = base_fields();
  over.residual = fsup::known(CapacityDimension::power, 601);
  FT_CHECK_ERROR(SourceEvidence::create(over), ErrorCode::account_mismatch);

  SourceEvidenceFields under = base_fields();
  under.residual = fsup::known(CapacityDimension::power, 599);
  FT_CHECK_ERROR(SourceEvidence::create(under), ErrorCode::account_mismatch);
}

FT_TEST(evidence, deriving_the_residual_closes_the_identity) {
  SourceEvidenceFields fields = base_fields();
  fields.residual = Measured::unknown();
  fields.derive_residual = true;
  auto derived = SourceEvidence::create(fields);
  FT_REQUIRE_OK(derived);
  FT_CHECK(derived.value().closure_proven());
  FT_CHECK_EQ(derived.value().residual().magnitude(), std::int64_t{600});

  // A residual that would be negative is refused rather than reported.
  fields = base_fields();
  fields.usable = fsup::known(CapacityDimension::power, 9700);
  fields.residual = Measured::unknown();
  fields.derive_residual = true;
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::account_mismatch);

  // Deriving requires both parts to be measured.
  fields = base_fields();
  fields.usable = Measured::unknown();
  fields.residual = Measured::unknown();
  fields.derive_residual = true;
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::invalid_argument);
}

FT_TEST(evidence, protected_and_reserved_cannot_exceed_usable) {
  SourceEvidenceFields fields = base_fields();
  fields.protected_capacity = fsup::known(CapacityDimension::power, 9000);
  fields.reserved = fsup::known(CapacityDimension::power, 1);
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::account_mismatch);
}

FT_TEST(evidence, an_unavailable_source_must_report_zero_usable) {
  SourceEvidenceFields fields = base_fields();
  fields.state = OperationalState::unavailable;
  fields.usable = fsup::known(CapacityDimension::power, 100);
  fields.protected_capacity = fsup::known(CapacityDimension::power, 0);
  fields.reserved = fsup::known(CapacityDimension::power, 0);
  fields.unavailable = fsup::known(CapacityDimension::power, 9300);
  fields.residual = fsup::known(CapacityDimension::power, 600);
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::invalid_argument);

  fields = base_fields();
  fields.state = OperationalState::unavailable;
  fields.usable = Measured::unknown();
  fields.unavailable = Measured::unknown();
  fields.residual = Measured::unknown();
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::invalid_argument);

  fields = base_fields();
  fields.state = OperationalState::unavailable;
  fields.usable = fsup::known(CapacityDimension::power, 0);
  fields.protected_capacity = fsup::known(CapacityDimension::power, 0);
  fields.reserved = fsup::known(CapacityDimension::power, 0);
  fields.unavailable = fsup::known(CapacityDimension::power, 9400);
  fields.residual = fsup::known(CapacityDimension::power, 600);
  auto accepted = SourceEvidence::create(fields);
  FT_REQUIRE_OK(accepted);
  FT_CHECK(accepted.value().usable().is_known_zero());
}

FT_TEST(evidence, a_nominal_source_cannot_carry_an_unavailable_reason) {
  SourceEvidenceFields fields = base_fields();
  fields.state_reason = ReasonCode::source_unavailable;
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::invalid_argument);

  fields = base_fields();
  fields.state_reason = ReasonCode::source_state_unknown;
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::invalid_argument);

  fields = base_fields();
  fields.state = OperationalState::degraded;
  fields.state_reason = ReasonCode::source_degraded;
  fields.state_detail = "one feed out";
  FT_CHECK_OK(SourceEvidence::create(fields));
}

FT_TEST(evidence, provenance_must_name_the_same_source) {
  SourceEvidenceFields fields = base_fields();
  fields.provenance.source = *CapacitySourceId::parse("cooling-capacity");
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::conflict);

  fields = base_fields();
  fields.provenance = Provenance();
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::invalid_argument);

  fields = base_fields();
  fields.provenance.evidence_digest = std::string(63, 'a');
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::invalid_argument);
}

FT_TEST(evidence, detail_text_is_bounded_and_must_be_printable) {
  SourceEvidenceFields fields = base_fields();
  fields.state = OperationalState::degraded;
  fields.state_detail = std::string(limits::max_detail_length + 1, 'x');
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::limit_exceeded);

  fields = base_fields();
  fields.state = OperationalState::degraded;
  fields.state_detail = std::string("tab\there");
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::invalid_argument);

  fields = base_fields();
  fields.state = OperationalState::degraded;
  fields.state_detail = std::string("\xC3\xA9");
  FT_CHECK_ERROR(SourceEvidence::create(fields), ErrorCode::invalid_argument);
}

FT_TEST(evidence, closure_is_unproven_exactly_when_a_part_is_unmeasured) {
  SourceEvidenceFields fields = base_fields();
  fields.usable = Measured::unknown();
  fields.residual = Measured::unknown();
  auto record = SourceEvidence::create(fields);
  FT_REQUIRE_OK(record);
  FT_CHECK(!record.value().closure_proven());
  FT_CHECK(record.value().usable().is_unknown());
  FT_CHECK(record.value().allocatable().is_unknown());
  FT_CHECK(!record.value().allocatable().is_known_zero());
  FT_CHECK_EQ(record.value().installed().magnitude(), std::int64_t{10000});
}

FT_TEST(evidence, an_unmeasured_rollup_poisons_allocatable) {
  SourceEvidenceFields fields = base_fields();
  fields.protected_capacity = Measured::unknown();
  auto record = SourceEvidence::create(fields);
  FT_REQUIRE_OK(record);
  FT_CHECK(record.value().closure_proven());
  FT_CHECK(record.value().allocatable().is_unknown());
  FT_CHECK_EQ(record.value().usable().magnitude(), std::int64_t{9000});
}

FT_TEST(evidence, randomized_records_obey_the_invariants) {
  ftest::Rng rng(ftest::current_seed());
  for (int iteration = 0; iteration < 300; ++iteration) {
    SourceEvidenceFields fields = base_fields();
    const std::int64_t installed = rng.range(0, 100000);
    const std::int64_t unavailable = rng.range(0, installed);
    const std::int64_t usable = rng.range(0, installed - unavailable);
    fields.installed = fsup::known(CapacityDimension::power, installed);
    fields.observed = Measured::unknown();
    fields.unavailable = fsup::known(CapacityDimension::power, unavailable);
    fields.usable = fsup::known(CapacityDimension::power, usable);
    fields.residual = Measured::unknown();
    fields.derive_residual = true;
    fields.protected_capacity = fsup::known(CapacityDimension::power, rng.range(0, usable));
    fields.reserved =
        fsup::known(CapacityDimension::power, rng.range(0, usable - fields.protected_capacity.magnitude()));
    fields.state = rng.chance(1, 5) ? OperationalState::degraded : OperationalState::nominal;

    auto record = SourceEvidence::create(fields);
    FT_REQUIRE_OK(record);
    FT_CHECK(record.value().closure_proven());
    FT_CHECK_EQ(record.value().residual().magnitude(), installed - unavailable - usable);
    FT_CHECK_EQ(record.value().usable().magnitude() + record.value().unavailable().magnitude() +
                    record.value().residual().magnitude(),
                installed);
    if (record.value().allocatable().is_known()) {
      FT_CHECK(record.value().allocatable().magnitude() <= record.value().usable().magnitude());
      FT_CHECK(record.value().allocatable().magnitude() <= installed);
    }
  }
}
