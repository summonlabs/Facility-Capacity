// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The model contract: explicit preconditions, validation precedence, bounded
// idempotency, epoch advancement, publication and canonical state.

#include <stdexcept>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace dccp::facility_capacity;

struct Fixture {
  ManualClock clock{Tick::from_value(1000)};
  FacilityCapacityModel model;

  explicit Fixture(std::uint64_t validity_ticks = 0) : model(make_model(clock, validity_ticks)) {}

  static FacilityCapacityModel make_model(ManualClock& clock, std::uint64_t validity_ticks) {
    ModelConfig config;
    config.facility = fsup::facility_id();
    config.site = fsup::site_id();
    config.epoch = EpochId::from_value(7);
    config.incarnation = IncarnationId::from_value(3);
    config.snapshot_validity_ticks = Tick::from_value(validity_ticks);
    auto created = FacilityCapacityModel::create(config, clock);
    if (!created.has_value()) {
      throw std::runtime_error("fixture: " + created.error().to_string());
    }
    return std::move(created.value());
  }

  std::uint64_t next_attempt() { return ++attempts; }

  CapacityPrecondition precondition() {
    CapacityPrecondition value = model.current_precondition();
    value.attempt = AttemptId::from_value(next_attempt());
    return value;
  }

  std::uint64_t attempts = 0;
};

CapacitySourceId source(const char* text) {
  auto id = CapacitySourceId::parse(text);
  if (!id.has_value()) {
    throw std::runtime_error("fixture: invalid source identity");
  }
  return id.value();
}

fsup::EvidenceSpec power(std::int64_t installed, std::int64_t usable, std::int64_t unavailable,
                         std::uint64_t generation = 1) {
  fsup::EvidenceSpec spec;
  spec.source = "power-capacity";
  spec.dimension = CapacityDimension::power;
  spec.installed = installed;
  spec.usable = usable;
  spec.unavailable = unavailable;
  spec.derive_residual = true;
  spec.protected_capacity = 0;
  spec.reserved = 0;
  spec.generation = generation;
  return spec;
}

}  // namespace

FT_TEST(model, create_validates_its_configuration) {
  ManualClock clock(Tick::from_value(0));
  ModelConfig config;
  FT_CHECK_ERROR(FacilityCapacityModel::create(config, clock), ErrorCode::invalid_argument);

  config.facility = fsup::facility_id();
  FT_CHECK_ERROR(FacilityCapacityModel::create(config, clock), ErrorCode::invalid_argument);

  config.site = fsup::site_id();
  FT_CHECK_ERROR(FacilityCapacityModel::create(config, clock), ErrorCode::invalid_argument);

  config.epoch = EpochId::from_value(7);
  FT_CHECK_ERROR(FacilityCapacityModel::create(config, clock), ErrorCode::invalid_argument);

  config.incarnation = IncarnationId::from_value(3);
  auto created = FacilityCapacityModel::create(config, clock);
  FT_REQUIRE_OK(created);
  FT_CHECK_EQ(created.value().capacity_generation().value(), std::uint64_t{0});
  FT_CHECK_EQ(created.value().revision().value(), std::uint64_t{0});
  FT_CHECK_OK(created.value().validate());
}

FT_TEST(model, current_precondition_matches_the_state) {
  Fixture fixture;
  const CapacityPrecondition precondition = fixture.model.current_precondition();
  FT_CHECK_EQ(precondition.expected_epoch.value(), std::uint64_t{7});
  FT_CHECK_EQ(precondition.expected_incarnation.value(), std::uint64_t{3});
  FT_CHECK_EQ(precondition.expected_capacity_generation.value(), std::uint64_t{0});
  FT_CHECK_EQ(precondition.expected_revision.value(), std::uint64_t{0});
  FT_CHECK(precondition.attempt.is_zero());
}

FT_TEST(model, declaring_evidence_advances_the_revision_once) {
  Fixture fixture;
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  FT_CHECK_EQ(fixture.model.revision().value(), std::uint64_t{1});
  FT_CHECK_EQ(fixture.model.capacity_generation().value(), std::uint64_t{0});
  FT_CHECK_OK(fixture.model.validate());
}

FT_TEST(model, a_stale_revision_is_refused) {
  Fixture fixture;
  // Two distinct attempt tokens captured before the first mutation: one is
  // applied, the other is a genuinely stale precondition rather than an
  // idempotent replay of the first.
  const CapacityPrecondition applied = fixture.precondition();
  const CapacityPrecondition stale = fixture.precondition();
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)), applied));
  FT_CHECK_ERROR(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000, 2)), stale),
                 ErrorCode::stale_generation);
}
FT_TEST(model, a_stale_generation_is_refused) {
  Fixture fixture;
  CapacityPrecondition precondition = fixture.precondition();
  precondition.expected_capacity_generation = CapacityGeneration::from_value(9);
  FT_CHECK_ERROR(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)), precondition),
                 ErrorCode::stale_generation);
}

FT_TEST(model, validation_precedence_puts_authority_before_generation_before_revision) {
  Fixture fixture;
  CapacityPrecondition precondition = fixture.precondition();
  precondition.expected_epoch = EpochId::from_value(1);
  precondition.expected_incarnation = IncarnationId::from_value(1);
  precondition.expected_capacity_generation = CapacityGeneration::from_value(9);
  precondition.expected_revision = Revision::from_value(9);
  // Every token is stale; authority is reported first.
  FT_CHECK_ERROR(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)), precondition),
                 ErrorCode::stale_authority);

  CapacityPrecondition generation = fixture.precondition();
  generation.expected_capacity_generation = CapacityGeneration::from_value(9);
  generation.expected_revision = Revision::from_value(9);
  FT_CHECK_ERROR(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)), generation),
                 ErrorCode::stale_generation);
}

FT_TEST(model, evidence_from_another_epoch_is_refused_as_stale_authority) {
  Fixture fixture;
  fsup::EvidenceSpec spec = power(10000, 9000, 1000);
  spec.epoch = 6;
  FT_CHECK_ERROR(fixture.model.declare_evidence(fsup::make_evidence(spec), fixture.precondition()),
                 ErrorCode::stale_authority);
}

FT_TEST(model, an_equal_or_older_evidence_generation_is_refused) {
  Fixture fixture;
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000, 5)),
                                             fixture.precondition()));
  FT_CHECK_ERROR(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000, 5)),
                                                fixture.precondition()),
                 ErrorCode::stale_generation);
  FT_CHECK_ERROR(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000, 4)),
                                                fixture.precondition()),
                 ErrorCode::stale_generation);
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(11000, 10000, 1000, 6)),
                                             fixture.precondition()));
  auto held = fixture.model.evidence(source("power-capacity"),
                                     CapacityDimension::power);
  FT_REQUIRE_OK(held);
  FT_CHECK_EQ(held.value().generation().value(), std::uint64_t{6});
}

FT_TEST(model, idempotent_replay_of_the_same_attempt_is_a_no_op) {
  Fixture fixture;
  const CapacityPrecondition precondition = fixture.precondition();
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)), precondition));
  const Revision after_first = fixture.model.revision();
  // The same attempt token replayed with a precondition that is now stale must
  // still be recognised as a replay and must not change anything.
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)), precondition));
  FT_CHECK_EQ(fixture.model.revision(), after_first);
  FT_CHECK_EQ(fixture.model.all_evidence().size(), std::size_t{1});
}

FT_TEST(model, retiring_unknown_evidence_reports_not_found) {
  Fixture fixture;
  FT_CHECK_ERROR(fixture.model.retire_evidence(source("power-capacity"),
                                               CapacityDimension::power, fixture.precondition()),
                 ErrorCode::not_found);
}

FT_TEST(model, retire_removes_the_record_and_advances_the_revision) {
  Fixture fixture;
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  FT_CHECK_OK(fixture.model.retire_evidence(source("power-capacity"),
                                            CapacityDimension::power, fixture.precondition()));
  FT_CHECK_EQ(fixture.model.all_evidence().size(), std::size_t{0});
  FT_CHECK_EQ(fixture.model.revision().value(), std::uint64_t{2});
  FT_CHECK_ERROR(fixture.model.evidence(source("power-capacity"),
                                        CapacityDimension::power),
                 ErrorCode::not_found);
}

FT_TEST(model, reserves_and_constraints_round_trip) {
  Fixture fixture;
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  fsup::ReserveSpec reserve;
  reserve.id = "protection-1";
  reserve.dimension = CapacityDimension::power;
  reserve.amount = 100;
  FT_CHECK_OK(fixture.model.declare_reserve(fsup::make_reserve(reserve), fixture.precondition()));
  FT_CHECK_EQ(fixture.model.all_reserves().size(), std::size_t{1});

  fsup::ConstraintSpec constraint;
  constraint.id = "floor-1";
  constraint.dimension = CapacityDimension::power;
  constraint.floor_value = 1;
  FT_CHECK_OK(fixture.model.declare_constraint(fsup::make_constraint(constraint), fixture.precondition()));
  FT_CHECK_EQ(fixture.model.all_constraints().size(), std::size_t{1});

  auto id = ReserveId::parse("protection-1");
  FT_REQUIRE_OK(id);
  FT_CHECK_OK(fixture.model.withdraw_reserve(id.value(), fixture.precondition()));
  FT_CHECK_EQ(fixture.model.all_reserves().size(), std::size_t{0});
  FT_CHECK_ERROR(fixture.model.withdraw_reserve(id.value(), fixture.precondition()), ErrorCode::not_found);

  auto constraint_id = ConstraintId::parse("floor-1");
  FT_REQUIRE_OK(constraint_id);
  FT_CHECK_OK(fixture.model.withdraw_constraint(constraint_id.value(), fixture.precondition()));
  FT_CHECK_ERROR(fixture.model.withdraw_constraint(constraint_id.value(), fixture.precondition()),
                 ErrorCode::not_found);
}

FT_TEST(model, a_reserve_declared_under_another_epoch_is_refused) {
  Fixture fixture;
  fsup::ReserveSpec reserve;
  reserve.id = "protection-1";
  reserve.dimension = CapacityDimension::power;
  reserve.amount = 100;
  reserve.epoch = 6;
  FT_CHECK_ERROR(fixture.model.declare_reserve(fsup::make_reserve(reserve), fixture.precondition()),
                 ErrorCode::stale_authority);
}

FT_TEST(model, requirements_replace_by_dimension) {
  Fixture fixture;
  CapacityRequirement requirement;
  requirement.dimension = CapacityDimension::power;
  requirement.required = true;
  FT_CHECK_OK(fixture.model.set_requirement(requirement, fixture.precondition()));
  requirement.required = false;
  FT_CHECK_OK(fixture.model.set_requirement(requirement, fixture.precondition()));
  FT_CHECK_EQ(fixture.model.requirements().size(), std::size_t{1});
  FT_CHECK(!fixture.model.requirements().front().required);
}

FT_TEST(model, advance_epoch_rules) {
  Fixture fixture;
  FT_CHECK_ERROR(fixture.model.advance_epoch(EpochId::from_value(6), IncarnationId::from_value(4),
                                             fixture.precondition()),
                 ErrorCode::invalid_argument);
  FT_CHECK_ERROR(fixture.model.advance_epoch(EpochId::from_value(7), IncarnationId::from_value(3),
                                             fixture.precondition()),
                 ErrorCode::invalid_argument);
  FT_CHECK_ERROR(fixture.model.advance_epoch(EpochId::from_value(7), IncarnationId::from_value(0),
                                             fixture.precondition()),
                 ErrorCode::invalid_argument);

  FT_CHECK_OK(fixture.model.advance_epoch(EpochId::from_value(7), IncarnationId::from_value(4),
                                          fixture.precondition()));
  FT_CHECK_EQ(fixture.model.epoch().value(), std::uint64_t{7});
  FT_CHECK_EQ(fixture.model.incarnation().value(), std::uint64_t{4});

  FT_CHECK_OK(fixture.model.advance_epoch(EpochId::from_value(8), IncarnationId::from_value(1),
                                          fixture.precondition()));
  FT_CHECK_EQ(fixture.model.epoch().value(), std::uint64_t{8});

  // Evidence published under the old epoch can no longer be declared.
  fsup::EvidenceSpec spec = power(10000, 9000, 1000);
  spec.epoch = 7;
  FT_CHECK_ERROR(fixture.model.declare_evidence(fsup::make_evidence(spec), fixture.precondition()),
                 ErrorCode::stale_authority);
}

FT_TEST(model, publish_advances_the_generation_and_records_the_answer) {
  Fixture fixture;
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  auto first = fixture.model.publish(fixture.precondition());
  FT_REQUIRE_OK(first);
  FT_CHECK_EQ(first.value()->generation().value(), std::uint64_t{1});
  FT_CHECK_EQ(first.value()->revision().value(), std::uint64_t{2});
  FT_CHECK_EQ(fixture.model.capacity_generation().value(), std::uint64_t{1});
  FT_CHECK_EQ(fixture.model.published_generation().value(), std::uint64_t{1});
  FT_CHECK(fixture.model.current_snapshot() != nullptr);
  FT_CHECK_EQ(fixture.model.current_snapshot()->digest(), first.value()->digest());

  auto second = fixture.model.publish(fixture.precondition());
  FT_REQUIRE_OK(second);
  FT_CHECK_EQ(second.value()->generation().value(), std::uint64_t{2});
  FT_CHECK(second.value()->digest() != first.value()->digest());
}

FT_TEST(model, publish_with_a_stale_precondition_is_refused) {
  Fixture fixture;
  const CapacityPrecondition applied = fixture.precondition();
  const CapacityPrecondition stale = fixture.precondition();
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)), applied));
  FT_CHECK_ERROR(fixture.model.publish(stale), ErrorCode::stale_generation);
}
FT_TEST(model, query_answers_without_publishing_a_generation) {
  Fixture fixture;
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  auto answer = fixture.model.query(CapacityDimension::power);
  FT_REQUIRE_OK(answer);
  FT_CHECK_EQ(answer.value().outcome, CapacityOutcome::usable);
  FT_CHECK_EQ(answer.value().dimension, CapacityDimension::power);
  FT_CHECK_EQ(answer.value().unit, Unit::milli_watt);
  FT_CHECK_EQ(answer.value().totals.allocatable.magnitude(), std::int64_t{9000});
  FT_CHECK_EQ(fixture.model.capacity_generation().value(), std::uint64_t{0});

  auto unoffered = fixture.model.query(CapacityDimension::cooling);
  FT_REQUIRE_OK(unoffered);
  FT_CHECK_EQ(unoffered.value().outcome, CapacityOutcome::unavailable);
  FT_CHECK(unoffered.value().totals.allocatable.is_unknown());
}

FT_TEST(model, snapshot_validity_is_bounded_by_the_clock) {
  Fixture fixture(500);
  auto snapshot = fixture.model.publish(fixture.precondition());
  FT_REQUIRE_OK(snapshot);
  FT_CHECK_EQ(snapshot.value()->built_at().value(), std::uint64_t{1000});
  FT_CHECK_EQ(snapshot.value()->valid_until().value(), std::uint64_t{1500});
}

FT_TEST(model, state_round_trips_through_the_canonical_encoding) {
  Fixture fixture;
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  fsup::ReserveSpec reserve;
  reserve.id = "protection-1";
  reserve.dimension = CapacityDimension::power;
  reserve.amount = 100;
  FT_CHECK_OK(fixture.model.declare_reserve(fsup::make_reserve(reserve), fixture.precondition()));
  fsup::ConstraintSpec constraint;
  constraint.id = "floor-1";
  constraint.dimension = CapacityDimension::power;
  constraint.floor_value = 1;
  FT_CHECK_OK(fixture.model.declare_constraint(fsup::make_constraint(constraint), fixture.precondition()));
  auto published = fixture.model.publish(fixture.precondition());
  FT_REQUIRE_OK(published);

  auto encoded = fixture.model.encode();
  FT_REQUIRE_OK(encoded);
  auto decoded = FacilityCapacityModel::decode(encoded.value(), fixture.clock);
  FT_REQUIRE_OK(decoded);

  FT_CHECK_EQ(decoded.value().facility().value(), fixture.model.facility().value());
  FT_CHECK_EQ(decoded.value().revision(), fixture.model.revision());
  FT_CHECK_EQ(decoded.value().capacity_generation(), fixture.model.capacity_generation());
  FT_CHECK_EQ(decoded.value().all_evidence().size(), fixture.model.all_evidence().size());
  FT_CHECK_EQ(decoded.value().all_reserves().size(), fixture.model.all_reserves().size());
  FT_CHECK_EQ(decoded.value().all_constraints().size(), fixture.model.all_constraints().size());
  FT_CHECK_OK(decoded.value().validate());

  // Re-encoding produces exactly the same bytes.
  auto reencoded = decoded.value().encode();
  FT_REQUIRE_OK(reencoded);
  FT_CHECK_EQ(reencoded.value(), encoded.value());
}

FT_TEST(model, decoding_refuses_a_tampered_document) {
  Fixture fixture;
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  auto encoded = fixture.model.encode();
  FT_REQUIRE_OK(encoded);
  std::string document = encoded.value();

  std::string truncated = document.substr(0, document.size() - 10);
  FT_CHECK_ERROR(FacilityCapacityModel::decode(truncated, fixture.clock), ErrorCode::corruption);

  std::string without_newline = document.substr(0, document.size() - 1);
  FT_CHECK_ERROR(FacilityCapacityModel::decode(without_newline, fixture.clock), ErrorCode::corruption);

  std::string extra = document;
  extra.append("unexpected=1\n");
  FT_CHECK_ERROR(FacilityCapacityModel::decode(extra, fixture.clock), ErrorCode::corruption);

  std::string wrong_kind = document;
  const std::size_t position = wrong_kind.find("document=model-state");
  FT_REQUIRE(position != std::string::npos);
  wrong_kind.replace(position, 20, "document=model-statf");
  FT_CHECK_ERROR(FacilityCapacityModel::decode(wrong_kind, fixture.clock), ErrorCode::checksum_mismatch);

  FT_CHECK_ERROR(FacilityCapacityModel::decode("", fixture.clock), ErrorCode::corruption);
}

FT_TEST(model, every_mutation_refuses_a_stale_precondition) {
  Fixture fixture;
  const CapacityPrecondition applied = fixture.precondition();
  std::vector<CapacityPrecondition> stale;
  for (int index = 0; index < 8; ++index) {
    stale.push_back(fixture.precondition());
  }
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)), applied));

  fsup::ReserveSpec reserve;
  reserve.id = "protection-1";
  reserve.dimension = CapacityDimension::power;
  reserve.amount = 100;
  FT_CHECK_ERROR(fixture.model.declare_reserve(fsup::make_reserve(reserve), stale[0]),
                 ErrorCode::stale_generation);

  fsup::ConstraintSpec constraint;
  constraint.id = "floor-1";
  constraint.dimension = CapacityDimension::power;
  constraint.floor_value = 1;
  FT_CHECK_ERROR(fixture.model.declare_constraint(fsup::make_constraint(constraint), stale[1]),
                 ErrorCode::stale_generation);

  CapacityRequirement requirement;
  requirement.dimension = CapacityDimension::power;
  FT_CHECK_ERROR(fixture.model.set_requirement(requirement, stale[2]), ErrorCode::stale_generation);

  FT_CHECK_ERROR(fixture.model.retire_evidence(source("power-capacity"), CapacityDimension::power, stale[3]),
                 ErrorCode::stale_generation);

  FT_CHECK_ERROR(fixture.model.advance_epoch(EpochId::from_value(8), IncarnationId::from_value(1), stale[4]),
                 ErrorCode::stale_generation);

  FT_CHECK_ERROR(fixture.model.publish(stale[5]), ErrorCode::stale_generation);
}