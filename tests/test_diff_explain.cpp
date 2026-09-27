// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Structural diffs between published answers, and the deterministic
// explanations built on top of them.

#include <memory>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "test_snapshot_builder.hpp"
#include "test_support.hpp"

namespace {

using namespace dccp::facility_capacity;
using fsup::SnapshotBuilder;

fsup::EvidenceSpec power(const std::string& source, std::int64_t installed, std::int64_t usable,
                         std::int64_t unavailable, std::uint64_t generation = 1) {
  fsup::EvidenceSpec spec;
  spec.source = source;
  spec.dimension = CapacityDimension::power;
  spec.generation = generation;
  spec.installed = installed;
  spec.usable = usable;
  spec.unavailable = unavailable;
  spec.derive_residual = true;
  spec.protected_capacity = 0;
  spec.reserved = 0;
  return spec;
}

void check_ascii_and_newline(const std::string& text, const char* what) {
  for (const char character : text) {
    if (static_cast<unsigned char>(character) >= 0x80u) {
      FT_FAIL(std::string("a rendering is not pure ASCII: ") + what);
    }
  }
  if (!text.empty() && text.back() != '\n') {
    FT_FAIL(std::string("a multi-line rendering does not end with a newline: ") + what);
  }
}

}  // namespace

FT_TEST(diff, a_snapshot_diffed_with_itself_is_empty) {
  SnapshotBuilder builder(1);
  builder.add(fsup::make_evidence(power("power-a", 10000, 9000, 400)));
  const std::shared_ptr<const CapacitySnapshot> snapshot = builder.build_ok();
  auto diff = diff_snapshots(*snapshot, *snapshot);
  FT_REQUIRE_OK(diff);
  FT_CHECK(diff.value().empty());
  FT_CHECK(diff.value().dimensions.empty());
  FT_CHECK(diff.value().sources.empty());
  FT_CHECK(diff.value().constraints.empty());
}

FT_TEST(diff, a_reduction_is_reported_with_an_exact_delta) {
  SnapshotBuilder before_builder(1);
  before_builder.add(fsup::make_evidence(power("power-a", 10000, 9000, 400)));
  SnapshotBuilder after_builder(2);
  after_builder.add(fsup::make_evidence(power("power-a", 10000, 3000, 400)));
  const std::shared_ptr<const CapacitySnapshot> before = before_builder.build_ok();
  const std::shared_ptr<const CapacitySnapshot> after = after_builder.build_ok();

  auto diff = diff_snapshots(*before, *after);
  FT_REQUIRE_OK(diff);
  FT_CHECK(!diff.value().empty());
  FT_CHECK_EQ(diff.value().dimensions.size(), std::size_t{1});
  const DimensionChange& change = diff.value().dimensions.front();
  FT_CHECK_EQ(change.dimension, CapacityDimension::power);
  FT_CHECK_EQ(change.outcome_before, CapacityOutcome::usable);
  FT_CHECK_EQ(change.outcome_after, CapacityOutcome::usable);
  FT_CHECK(change.usable.changed());
  FT_CHECK(change.usable.delta_known);
  FT_CHECK_EQ(change.usable.delta, std::int64_t{-6000});
  FT_CHECK(change.allocatable.changed());
  FT_CHECK_EQ(change.allocatable.delta, std::int64_t{-6000});
  FT_CHECK(change.changed());

  const std::string rendered = diff.value().describe();
  FT_CHECK(rendered.find("power") != std::string::npos);
  check_ascii_and_newline(rendered, "diff describe");
  FT_CHECK_EQ(diff.value().describe(), rendered);
}

FT_TEST(diff, exhaustion_is_reported_as_an_outcome_change) {
  SnapshotBuilder before_builder(1);
  before_builder.add(fsup::make_evidence(power("power-a", 10000, 9000, 400)));
  SnapshotBuilder after_builder(2);
  after_builder.add(fsup::make_evidence(power("power-a", 10000, 0, 10000)));
  const std::shared_ptr<const CapacitySnapshot> before = before_builder.build_ok();
  const std::shared_ptr<const CapacitySnapshot> after = after_builder.build_ok();
  FT_CHECK_EQ(after->outcome(CapacityDimension::power), CapacityOutcome::exhausted);

  auto diff = diff_snapshots(*before, *after);
  FT_REQUIRE_OK(diff);
  FT_CHECK_EQ(diff.value().dimensions.size(), std::size_t{1});
  FT_CHECK_EQ(diff.value().dimensions.front().outcome_before, CapacityOutcome::usable);
  FT_CHECK_EQ(diff.value().dimensions.front().outcome_after, CapacityOutcome::exhausted);
}

FT_TEST(diff, an_unmeasured_side_yields_an_unknown_delta) {
  SnapshotBuilder before_builder(1);
  before_builder.add(fsup::make_evidence(power("power-a", 10000, 9000, 400)));
  SnapshotBuilder after_builder(2);
  fsup::EvidenceSpec spec = power("power-a", 10000, 9000, 400);
  spec.protected_capacity = std::nullopt;
  after_builder.add(fsup::make_evidence(spec));
  const std::shared_ptr<const CapacitySnapshot> before = before_builder.build_ok();
  const std::shared_ptr<const CapacitySnapshot> after = after_builder.build_ok();

  auto diff = diff_snapshots(*before, *after);
  FT_REQUIRE_OK(diff);
  FT_REQUIRE(!diff.value().dimensions.empty());
  const DimensionChange& change = diff.value().dimensions.front();
  FT_CHECK(change.allocatable.changed());
  FT_CHECK(change.allocatable.before_known);
  FT_CHECK(!change.allocatable.after_known);
  FT_CHECK(!change.allocatable.delta_known);
}

FT_TEST(diff, added_and_removed_sources_are_reported) {
  SnapshotBuilder before_builder(1);
  before_builder.add(fsup::make_evidence(power("power-a", 10000, 9000, 400)));
  SnapshotBuilder after_builder(2);
  after_builder.add(fsup::make_evidence(power("power-a", 10000, 9000, 400, 5)));
  after_builder.add(fsup::make_evidence(power("power-b", 5000, 4000, 100)));
  const std::shared_ptr<const CapacitySnapshot> before = before_builder.build_ok();
  const std::shared_ptr<const CapacitySnapshot> after = after_builder.build_ok();

  auto diff = diff_snapshots(*before, *after);
  FT_REQUIRE_OK(diff);
  FT_CHECK_EQ(diff.value().sources.size(), std::size_t{2});
  bool saw_added = false;
  bool saw_bumped = false;
  for (const SourceChange& change : diff.value().sources) {
    if (change.source.value() == "power-b") {
      FT_CHECK(!change.present_before);
      FT_CHECK(change.present_after);
      saw_added = true;
    } else {
      FT_CHECK(change.present_before);
      FT_CHECK(change.present_after);
      FT_CHECK_EQ(change.generation_before.value(), std::uint64_t{1});
      FT_CHECK_EQ(change.generation_after.value(), std::uint64_t{5});
      saw_bumped = true;
    }
  }
  FT_CHECK(saw_added);
  FT_CHECK(saw_bumped);

  auto reversed = diff_snapshots(*after, *before);
  FT_REQUIRE_OK(reversed);
  for (const SourceChange& change : reversed.value().sources) {
    if (change.source.value() == "power-b") {
      FT_CHECK(change.present_before);
      FT_CHECK(!change.present_after);
    }
  }
}

FT_TEST(diff, a_constraint_change_is_reported) {
  SnapshotBuilder before_builder(1);
  before_builder.add(fsup::make_evidence(power("power-a", 10000, 9000, 400)));
  fsup::ConstraintSpec low;
  low.id = "floor-1";
  low.dimension = CapacityDimension::power;
  low.floor_value = 100;
  before_builder.add(fsup::make_constraint(low));

  SnapshotBuilder after_builder(2);
  after_builder.add(fsup::make_evidence(power("power-a", 10000, 9000, 400)));
  fsup::ConstraintSpec high = low;
  high.floor_value = 99000;
  after_builder.add(fsup::make_constraint(high));

  const std::shared_ptr<const CapacitySnapshot> before = before_builder.build_ok();
  const std::shared_ptr<const CapacitySnapshot> after = after_builder.build_ok();
  auto diff = diff_snapshots(*before, *after);
  FT_REQUIRE_OK(diff);
  FT_CHECK_EQ(diff.value().constraints.size(), std::size_t{1});
  FT_CHECK_EQ(diff.value().constraints.front().status_before, ConstraintStatus::satisfied);
  FT_CHECK_EQ(diff.value().constraints.front().status_after, ConstraintStatus::violated);
}

FT_TEST(diff, snapshots_for_different_facilities_cannot_be_diffed) {
  SnapshotBuilder first(1);
  first.facility("facility-a");
  first.add(fsup::make_evidence(power("power-a", 10000, 9000, 400)));
  SnapshotBuilder second(2);
  second.facility("facility-b");
  second.add(fsup::make_evidence(power("power-a", 10000, 9000, 400)));
  auto diff = diff_snapshots(*first.build_ok(), *second.build_ok());
  FT_CHECK_ERROR(diff, ErrorCode::conflict);
}

FT_TEST(explain, every_rendering_is_pure_and_ascii) {
  SnapshotBuilder builder(1);
  builder.add(fsup::make_evidence(power("power-a", 10000, 9000, 400)));
  fsup::ReserveSpec reserve;
  reserve.id = "protection-1";
  reserve.source = "power-a";
  reserve.dimension = CapacityDimension::power;
  reserve.kind = ReserveKind::protection;
  reserve.amount = 0;
  builder.add(fsup::make_reserve(reserve));
  fsup::ConstraintSpec constraint;
  constraint.id = "floor-1";
  constraint.dimension = CapacityDimension::power;
  constraint.floor_value = 100;
  builder.add(fsup::make_constraint(constraint));
  const std::shared_ptr<const CapacitySnapshot> snapshot = builder.build_ok();

  const std::string describe = snapshot->describe();
  FT_CHECK_EQ(snapshot->describe(), describe);
  check_ascii_and_newline(describe, "snapshot describe");
  FT_CHECK(describe.find("facility=facility-a") != std::string::npos);
  FT_CHECK(describe.find("generation=1") != std::string::npos);
  FT_CHECK(describe.find("completeness=") != std::string::npos);
  FT_CHECK(describe.find(snapshot->digest()) != std::string::npos);
  for (std::size_t ordinal = 0; ordinal < capacity_dimension_count; ++ordinal) {
    FT_CHECK(describe.find(std::string("dimension ") +
                           std::string(capacity_dimension_name(all_capacity_dimensions()[ordinal]))) !=
             std::string::npos);
  }

  const std::string explain = snapshot->explain(CapacityDimension::power);
  FT_CHECK_EQ(snapshot->explain(CapacityDimension::power), explain);
  check_ascii_and_newline(explain, "snapshot explain");
  FT_CHECK_EQ(explain, explain::render_dimension(*snapshot, CapacityDimension::power));
  FT_CHECK_EQ(describe, explain::render_snapshot(*snapshot));
  FT_CHECK_EQ(explain::render_notes(snapshot->notes()), snapshot->notes().to_string());

  ManualClock clock(Tick::from_value(1000));
  ModelConfig config;
  config.facility = fsup::facility_id();
  config.site = fsup::site_id();
  config.epoch = EpochId::from_value(7);
  config.incarnation = IncarnationId::from_value(3);
  auto model = FacilityCapacityModel::create(config, clock);
  FT_REQUIRE_OK(model);
  CapacityPrecondition precondition = model.value().current_precondition();
  precondition.attempt = AttemptId::from_value(1);
  FT_REQUIRE_OK(model.value().declare_evidence(
      fsup::make_evidence(power("power-a", 10000, 9000, 400)), precondition));
  auto answer = model.value().query(CapacityDimension::power);
  FT_REQUIRE_OK(answer);
  const std::string answer_text = answer.value().describe();
  FT_CHECK_EQ(answer.value().describe(), answer_text);
  check_ascii_and_newline(answer_text, "answer describe");
  FT_CHECK_EQ(answer_text, explain::render_answer(answer.value()));

  auto report = model.value().revalidate(*snapshot);
  FT_REQUIRE_OK(report);
  const std::string report_text = report.value().describe();
  FT_CHECK_EQ(report.value().describe(), report_text);
  check_ascii_and_newline(report_text, "revalidation describe");
  FT_CHECK(report_text.find("status=") != std::string::npos);
  FT_CHECK_EQ(report_text, explain::render_revalidation(report.value()));
}

FT_TEST(explain, every_part_renders_purely) {
  SnapshotBuilder builder(1);
  builder.add(fsup::make_evidence(power("power-a", 10000, 9000, 400)));
  const std::shared_ptr<const CapacitySnapshot> snapshot = builder.build_ok();

  for (const DimensionTotals& totals : snapshot->dimension_totals()) {
    FT_CHECK_EQ(totals.to_string(), totals.to_string());
    FT_CHECK(!totals.to_string().empty());
  }
  for (const SourceGenerationStamp& stamp : snapshot->sources()) {
    FT_CHECK_EQ(stamp.to_string(), stamp.to_string());
  }
  for (const DimensionLimit& limit : snapshot->limits()) {
    FT_CHECK_EQ(limit.to_string(), limit.to_string());
  }
  for (const ConstraintEvaluation& evaluation : snapshot->constraints()) {
    FT_CHECK_EQ(evaluation.to_string(), evaluation.to_string());
  }
  FT_CHECK_EQ(snapshot->limiting().to_string(), snapshot->limiting().to_string());

  const QuantityChange change;
  FT_CHECK_EQ(explain::render_quantity_change(change), change.to_string());
  FT_CHECK(!change.changed());
  FT_CHECK(!change.delta_known);
  FT_CHECK_EQ(change.delta, std::int64_t{0});
}
