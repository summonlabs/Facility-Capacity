// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// A concise builder for snapshot inputs, used by the derivation, model,
// revalidation, property and adversarial suites.

#ifndef FACILITY_CAPACITY_TESTS_TEST_SNAPSHOT_BUILDER_HPP
#define FACILITY_CAPACITY_TESTS_TEST_SNAPSHOT_BUILDER_HPP

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "dccp/facility_capacity/capacity.hpp"
#include "test_support.hpp"

namespace fsup {

using dccp::facility_capacity::CapacityDimension;
using dccp::facility_capacity::CapacityRequirement;
using dccp::facility_capacity::CapacityReserve;
using dccp::facility_capacity::CapacitySnapshot;
using dccp::facility_capacity::CapacitySnapshotInput;
using dccp::facility_capacity::EpochId;
using dccp::facility_capacity::IncarnationId;
using dccp::facility_capacity::Revision;
using dccp::facility_capacity::ServiceConstraint;
using dccp::facility_capacity::SourceEvidence;
using dccp::facility_capacity::Tick;

/// Assembles a snapshot input with deterministic defaults.
class SnapshotBuilder {
 public:
  explicit SnapshotBuilder(std::uint64_t generation = 1, std::uint64_t epoch = 7,
                           std::uint64_t incarnation = 3) {
    input_.facility = facility_id();
    input_.site = site_id();
    input_.generation = dccp::facility_capacity::CapacityGeneration::from_value(generation);
    input_.epoch = EpochId::from_value(epoch);
    input_.incarnation = IncarnationId::from_value(incarnation);
    input_.revision = Revision::from_value(generation);
    input_.built_at = Tick::from_value(1000);
    input_.valid_until = dccp::facility_capacity::max_tick;
  }

  SnapshotBuilder& at(std::uint64_t tick) {
    input_.built_at = Tick::from_value(tick);
    return *this;
  }

  SnapshotBuilder& valid_until(std::uint64_t tick) {
    input_.valid_until = Tick::from_value(tick);
    return *this;
  }

  SnapshotBuilder& facility(const std::string& text) {
    input_.facility = facility_id(text);
    return *this;
  }

  SnapshotBuilder& site(const std::string& text) {
    input_.site = site_id(text);
    return *this;
  }

  SnapshotBuilder& require(CapacityDimension dimension, bool required = true,
                           const std::vector<std::string>& sources = {}) {
    CapacityRequirement requirement;
    requirement.dimension = dimension;
    requirement.required = required;
    for (const std::string& source : sources) {
      auto id = dccp::facility_capacity::CapacitySourceId::parse(source);
      if (!id.has_value()) {
        throw std::runtime_error("builder: invalid source identity");
      }
      requirement.required_sources.push_back(id.value());
    }
    const auto status = requirement.normalize();
    if (!status.has_value()) {
      throw std::runtime_error("builder: " + status.error().to_string());
    }
    input_.requirements.push_back(std::move(requirement));
    return *this;
  }

  SnapshotBuilder& add(const SourceEvidence& evidence) {
    input_.evidence.push_back(evidence);
    return *this;
  }

  SnapshotBuilder& add(const CapacityReserve& reserve) {
    input_.reserves.push_back(reserve);
    return *this;
  }

  SnapshotBuilder& add(const ServiceConstraint& constraint) {
    input_.constraints.push_back(constraint);
    return *this;
  }

  SnapshotBuilder& note(const dccp::facility_capacity::CapacityNote& note) {
    (void)input_.coverage_findings.add(note);
    return *this;
  }

  const CapacitySnapshotInput& input() const noexcept { return input_; }

  dccp::facility_capacity::Result<std::shared_ptr<const CapacitySnapshot>> build() const {
    return CapacitySnapshot::build(input_);
  }

  /// Builds, aborting the test with the library's own error when it fails.
  std::shared_ptr<const CapacitySnapshot> build_ok() const {
    auto snapshot = CapacitySnapshot::build(input_);
    if (!snapshot.has_value()) {
      throw std::runtime_error("builder: snapshot build failed: " + snapshot.error().to_string());
    }
    return snapshot.value();
  }

 private:
  CapacitySnapshotInput input_;
};

}  // namespace fsup

#endif  // FACILITY_CAPACITY_TESTS_TEST_SNAPSHOT_BUILDER_HPP
