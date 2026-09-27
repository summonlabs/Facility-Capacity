// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The revalidation contract: when a previously published answer may still be
// relied on, and which rejection class is reported.

#include <memory>
#include <stdexcept>
#include <string>

#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace dccp::facility_capacity;

struct Fixture {
  ManualClock clock{Tick::from_value(1000)};
  FacilityCapacityModel model;

  explicit Fixture(std::uint64_t validity_ticks = 0, bool require_power = false)
      : model(make_model(clock, validity_ticks, require_power)) {}

  static FacilityCapacityModel make_model(ManualClock& clock, std::uint64_t validity_ticks,
                                          bool require_power) {
    ModelConfig config;
    config.facility = fsup::facility_id();
    config.site = fsup::site_id();
    config.epoch = EpochId::from_value(7);
    config.incarnation = IncarnationId::from_value(3);
    config.snapshot_validity_ticks = Tick::from_value(validity_ticks);
    if (require_power) {
      CapacityRequirement requirement;
      requirement.dimension = CapacityDimension::power;
      requirement.required = true;
      auto id = CapacitySourceId::parse("power-capacity");
      if (!id.has_value()) {
        throw std::runtime_error("fixture: invalid source identity");
      }
      requirement.required_sources.push_back(id.value());
      config.requirements.push_back(std::move(requirement));
    }
    auto created = FacilityCapacityModel::create(config, clock);
    if (!created.has_value()) {
      throw std::runtime_error("fixture: " + created.error().to_string());
    }
    return std::move(created.value());
  }

  CapacityPrecondition precondition() {
    CapacityPrecondition value = model.current_precondition();
    value.attempt = AttemptId::from_value(++attempts);
    return value;
  }

  std::uint64_t attempts = 0;
};

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

std::shared_ptr<const CapacitySnapshot> publish(Fixture& fixture) {
  auto snapshot = fixture.model.publish(fixture.precondition());
  if (!snapshot.has_value()) {
    throw std::runtime_error("fixture: publish failed: " + snapshot.error().to_string());
  }
  return snapshot.value();
}

}  // namespace

FT_TEST(revalidate, an_untouched_snapshot_is_valid) {
  Fixture fixture;
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  const auto snapshot = publish(fixture);
  auto report = fixture.model.revalidate(*snapshot);
  FT_REQUIRE_OK(report);
  FT_CHECK_EQ(report.value().status, RevalidationStatus::valid);
  FT_CHECK(report.value().is_valid());
  FT_CHECK(!report.value().rejected());
  FT_CHECK(report.value().findings.empty());
}

FT_TEST(revalidate, a_newer_published_generation_supersedes) {
  Fixture fixture;
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  const auto first = publish(fixture);
  const auto second = publish(fixture);
  FT_CHECK(second->generation() > first->generation());

  auto report = fixture.model.revalidate(*first);
  FT_REQUIRE_OK(report);
  FT_CHECK_EQ(report.value().status, RevalidationStatus::superseded);
  FT_CHECK(report.value().findings.contains(ReasonCode::capacity_generation_advanced));
  FT_CHECK_EQ(report.value().snapshot_generation.value(), std::uint64_t{1});
  FT_CHECK_EQ(report.value().current_generation.value(), std::uint64_t{2});

  auto current = fixture.model.revalidate(*second);
  FT_REQUIRE_OK(current);
  FT_CHECK_EQ(current.value().status, RevalidationStatus::valid);
}

FT_TEST(revalidate, a_newer_evidence_generation_makes_the_snapshot_stale) {
  Fixture fixture;
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  const auto snapshot = publish(fixture);
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(11000, 10000, 1000, 2)),
                                             fixture.precondition()));
  auto report = fixture.model.revalidate(*snapshot);
  FT_REQUIRE_OK(report);
  FT_CHECK_EQ(report.value().status, RevalidationStatus::stale);
  const CapacityNote* note = report.value().findings.find(ReasonCode::source_stale);
  FT_REQUIRE(note != nullptr);
  FT_CHECK_EQ(note->source.value(), std::string("power-capacity"));
}

FT_TEST(revalidate, changed_evidence_content_makes_the_snapshot_stale) {
  Fixture fixture;
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000, 4)),
                                             fixture.precondition()));
  const auto snapshot = publish(fixture);
  // A newer generation whose content differs from the one the snapshot used.
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 8000, 2000, 5)),
                                             fixture.precondition()));
  auto report = fixture.model.revalidate(*snapshot);
  FT_REQUIRE_OK(report);
  FT_CHECK_EQ(report.value().status, RevalidationStatus::stale);
  const CapacityNote* note = report.value().findings.find(ReasonCode::source_stale);
  FT_REQUIRE(note != nullptr);
  FT_CHECK_EQ(note->source.value(), std::string("power-capacity"));
}

FT_TEST(revalidate, removed_evidence_makes_the_snapshot_stale) {
  Fixture fixture;
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  const auto snapshot = publish(fixture);
  auto id = CapacitySourceId::parse("power-capacity");
  FT_REQUIRE_OK(id);
  FT_CHECK_OK(fixture.model.retire_evidence(id.value(), CapacityDimension::power, fixture.precondition()));
  auto report = fixture.model.revalidate(*snapshot);
  FT_REQUIRE_OK(report);
  FT_CHECK_EQ(report.value().status, RevalidationStatus::stale);
  FT_CHECK(report.value().findings.contains(ReasonCode::source_superseded));
}

FT_TEST(revalidate, an_epoch_change_makes_the_snapshot_stale) {
  Fixture fixture;
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  const auto snapshot = publish(fixture);
  FT_CHECK_OK(fixture.model.advance_epoch(EpochId::from_value(8), IncarnationId::from_value(1),
                                          fixture.precondition()));
  auto report = fixture.model.revalidate(*snapshot);
  FT_REQUIRE_OK(report);
  FT_CHECK_EQ(report.value().status, RevalidationStatus::stale);
  FT_CHECK(report.value().findings.contains(ReasonCode::source_epoch_mismatch));
}

FT_TEST(revalidate, an_incarnation_change_makes_the_snapshot_stale) {
  Fixture fixture;
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  const auto snapshot = publish(fixture);
  FT_CHECK_OK(fixture.model.advance_epoch(EpochId::from_value(7), IncarnationId::from_value(9),
                                          fixture.precondition()));
  auto report = fixture.model.revalidate(*snapshot);
  FT_REQUIRE_OK(report);
  FT_CHECK_EQ(report.value().status, RevalidationStatus::stale);
  FT_CHECK(report.value().findings.contains(ReasonCode::source_incarnation_mismatch));
}

FT_TEST(revalidate, an_expired_snapshot_is_stale) {
  Fixture fixture(500);
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  const auto snapshot = publish(fixture);
  auto current = fixture.model.revalidate(*snapshot);
  FT_REQUIRE_OK(current);
  FT_CHECK_EQ(current.value().status, RevalidationStatus::valid);

  FT_CHECK_OK(fixture.clock.advance(Tick::from_value(600)));
  auto expired = fixture.model.revalidate(*snapshot);
  FT_REQUIRE_OK(expired);
  FT_CHECK_EQ(expired.value().status, RevalidationStatus::stale);
  FT_CHECK(expired.value().findings.contains(ReasonCode::snapshot_expired));
}

FT_TEST(revalidate, a_missing_required_source_is_incomplete) {
  Fixture fixture(0, true);
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  const auto snapshot = publish(fixture);
  auto id = CapacitySourceId::parse("power-capacity");
  FT_REQUIRE_OK(id);
  FT_CHECK_OK(fixture.model.retire_evidence(id.value(), CapacityDimension::power, fixture.precondition()));
  auto report = fixture.model.revalidate(*snapshot);
  FT_REQUIRE_OK(report);
  FT_CHECK_EQ(report.value().status, RevalidationStatus::incomplete);
  FT_CHECK(report.value().findings.contains(ReasonCode::source_not_reported));
}

FT_TEST(revalidate, a_changed_coverage_contract_is_stale) {
  Fixture fixture;
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  const auto snapshot = publish(fixture);
  CapacityRequirement requirement;
  requirement.dimension = CapacityDimension::power;
  requirement.required = true;
  FT_CHECK_OK(fixture.model.set_requirement(requirement, fixture.precondition()));
  auto report = fixture.model.revalidate(*snapshot);
  FT_REQUIRE_OK(report);
  FT_CHECK_EQ(report.value().status, RevalidationStatus::stale);
  FT_CHECK(report.value().findings.contains(ReasonCode::coverage_contract_changed));
}

FT_TEST(revalidate, incompleteness_outranks_staleness) {
  Fixture fixture(0, true);
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  const auto snapshot = publish(fixture);
  auto id = CapacitySourceId::parse("power-capacity");
  FT_REQUIRE_OK(id);
  // Remove the required source completely and publish a new generation: the
  // snapshot is now both superseded and missing coverage.
  FT_CHECK_OK(fixture.model.retire_evidence(id.value(), CapacityDimension::power, fixture.precondition()));
  (void)publish(fixture);
  auto report = fixture.model.revalidate(*snapshot);
  FT_REQUIRE_OK(report);
  FT_CHECK_EQ(report.value().status, RevalidationStatus::superseded);
  FT_CHECK_EQ(revalidation_status_severity(RevalidationStatus::superseded),
              std::uint8_t{3});
  FT_CHECK(revalidation_status_severity(RevalidationStatus::incomplete) >
            revalidation_status_severity(RevalidationStatus::stale));
  FT_CHECK(revalidation_status_severity(RevalidationStatus::stale) >
            revalidation_status_severity(RevalidationStatus::valid));
}

FT_TEST(revalidate, a_snapshot_for_another_facility_is_a_conflict) {
  Fixture fixture;
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  const auto snapshot = publish(fixture);

  ManualClock other_clock(Tick::from_value(1000));
  ModelConfig config;
  config.facility = fsup::facility_id("facility-other");
  config.site = fsup::site_id();
  config.epoch = EpochId::from_value(7);
  config.incarnation = IncarnationId::from_value(3);
  auto other = FacilityCapacityModel::create(config, other_clock);
  FT_REQUIRE_OK(other);
  FT_CHECK_ERROR(other.value().revalidate(*snapshot), ErrorCode::conflict);
}

FT_TEST(revalidate, every_rejection_class_renders_deterministically) {
  Fixture fixture;
  FT_CHECK_OK(fixture.model.declare_evidence(fsup::make_evidence(power(10000, 9000, 1000)),
                                             fixture.precondition()));
  const auto snapshot = publish(fixture);
  (void)publish(fixture);
  auto report = fixture.model.revalidate(*snapshot);
  FT_REQUIRE_OK(report);
  const std::string first = report.value().describe();
  const std::string second = report.value().describe();
  FT_CHECK_EQ(first, second);
  FT_CHECK(first.find("status=superseded") != std::string::npos);
  FT_CHECK(first.back() == '\n');
  for (const char character : first) {
    FT_CHECK(static_cast<unsigned char>(character) < 0x80u);
  }
}
