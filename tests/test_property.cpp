// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Deterministic property tests.
//
// The central claim this product makes is that capacity is conserved: a change
// to the inputs can never create capacity from nowhere. Each case below builds
// a random facility, computes an independent reference answer with plain
// integer arithmetic over the raw inputs, and compares it with what the library
// published. The reference model does not call the library's composition code
// and does not use its types for the arithmetic, so agreement is evidence
// rather than tautology.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "test_rng.hpp"
#include "test_snapshot_builder.hpp"
#include "test_support.hpp"

namespace {

using namespace dccp::facility_capacity;
using fsup::SnapshotBuilder;

/// An independent recomputation of one dimension's totals from the raw inputs.
struct ReferenceTotals {
  bool offered = false;
  bool complete = true;
  bool any_unknown_usable = false;
  bool any_unknown_protected = false;
  bool any_unknown_reserved = false;
  std::int64_t installed = 0;
  std::int64_t usable = 0;
  std::int64_t unavailable = 0;
  std::int64_t residual = 0;
  std::int64_t protected_total = 0;
  std::int64_t reserved_total = 0;
  std::size_t contributing = 0;
};

/// Recomputes one dimension the long way: plain loops, plain integers, no
/// library accounting helpers.
ReferenceTotals reference(const CapacitySnapshotInput& input, CapacityDimension dimension) {
  ReferenceTotals totals;
  for (const SourceEvidence& record : input.evidence) {
    if (record.dimension() != dimension) {
      continue;
    }
    totals.offered = true;
    if (!record.usable().is_known()) {
      totals.any_unknown_usable = true;
    }
    std::int64_t protection = 0;
    std::int64_t reserved = 0;
    std::size_t items = 0;
    for (const CapacityReserve& reserve : input.reserves) {
      if (reserve.dimension() != dimension || reserve.source() != record.source()) {
        continue;
      }
      if (!reserve.active_at(input.built_at)) {
        continue;
      }
      if (!reserve.amount().is_known()) {
        if (reserve.supplies_protection()) {
          totals.any_unknown_protected = true;
        } else {
          totals.any_unknown_reserved = true;
        }
      } else if (reserve.supplies_protection()) {
        protection += reserve.amount().magnitude();
      } else {
        reserved += reserve.amount().magnitude();
      }
      ++items;
    }
    if (items == 0) {
      if (!record.protected_capacity().is_known()) {
        totals.any_unknown_protected = true;
      } else {
        protection = record.protected_capacity().magnitude();
      }
      if (!record.reserved().is_known()) {
        totals.any_unknown_reserved = true;
      } else {
        reserved = record.reserved().magnitude();
      }
    }
    ++totals.contributing;
    totals.installed += record.installed().magnitude();
    if (record.usable().is_known()) {
      totals.usable += record.usable().magnitude();
    }
    if (record.unavailable().is_known()) {
      totals.unavailable += record.unavailable().magnitude();
    }
    if (record.residual().is_known()) {
      totals.residual += record.residual().magnitude();
    }
    totals.protected_total += protection;
    totals.reserved_total += reserved;
  }
  return totals;
}

std::string label(const CapacityDimension dimension) {
  return std::string(capacity_dimension_name(dimension));
}

}  // namespace

FT_TEST(property, composed_totals_match_an_independent_reference) {
  ftest::Rng rng(ftest::current_seed());
  const CapacityDimension dimensions[] = {CapacityDimension::space, CapacityDimension::power};

  for (int iteration = 0; iteration < 120; ++iteration) {
    SnapshotBuilder builder(static_cast<std::uint64_t>(iteration) + 1);
    builder.at(1000);
    const std::size_t source_count = static_cast<std::size_t>(rng.range(1, 4));
    for (std::size_t index = 0; index < source_count; ++index) {
      const CapacityDimension dimension = dimensions[rng.below(2)];
      const std::int64_t installed = rng.range(1000, 100000);
      const std::int64_t unavailable = rng.range(0, installed / 4);
      const std::int64_t usable = rng.range(0, installed - unavailable);
      const std::int64_t protected_capacity = rng.range(0, usable / 2);
      const std::int64_t reserved = rng.range(0, (usable - protected_capacity) / 2);

      fsup::EvidenceSpec spec;
      spec.source = "source-" + std::to_string(index);
      spec.dimension = dimension;
      spec.unit = canonical_unit(dimension);
      spec.installed = installed;
      spec.unavailable = unavailable;
      spec.usable = usable;
      spec.derive_residual = true;
      spec.protected_capacity = protected_capacity;
      spec.reserved = reserved;
      builder.add(fsup::make_evidence(spec));
    }

    ftest::set_case_context("seed=" + std::to_string(ftest::current_seed()) +
                            " iteration=" + std::to_string(iteration));
    auto snapshot = builder.build();
    FT_REQUIRE_OK(snapshot);

    for (const CapacityDimension dimension : dimensions) {
      const ReferenceTotals expected = reference(builder.input(), dimension);
      const DimensionTotals* actual = snapshot.value()->find_totals(dimension);
      FT_REQUIRE(actual != nullptr);
      if (!expected.offered) {
        FT_CHECK_EQ(actual->contributing_sources, std::size_t{0});
        FT_CHECK(actual->installed.is_unknown());
        continue;
      }
      FT_CHECK_EQ(actual->contributing_sources, expected.contributing);
      FT_CHECK_EQ(actual->installed.magnitude(), expected.installed);
      FT_CHECK_EQ(actual->usable.magnitude(), expected.usable);
      FT_CHECK_EQ(actual->unavailable.magnitude(), expected.unavailable);
      FT_CHECK_EQ(actual->residual.magnitude(), expected.residual);
      FT_CHECK_EQ(actual->protected_capacity.magnitude(), expected.protected_total);
      FT_CHECK_EQ(actual->reserved.magnitude(), expected.reserved_total);
      const std::int64_t expected_allocatable =
          expected.usable - expected.protected_total - expected.reserved_total;
      FT_CHECK_EQ(actual->allocatable.magnitude(), expected_allocatable);

      // Capacity conservation, asserted on the published answer itself.
      FT_CHECK(actual->allocatable.magnitude() <= actual->usable.magnitude());
      FT_CHECK(actual->usable.magnitude() <= actual->installed.magnitude());
      FT_CHECK_EQ(actual->usable.magnitude() + actual->unavailable.magnitude() +
                      actual->residual.magnitude(),
                  actual->installed.magnitude());
      FT_CHECK_EQ(actual->protected_capacity.magnitude() + actual->reserved.magnitude() +
                      actual->allocatable.magnitude(),
                  actual->usable.magnitude());
      FT_CHECK(snapshot.value()->accounting_closed());
    }
  }
}

FT_TEST(property, randomized_source_changes_cannot_create_capacity) {
  ftest::Rng rng(ftest::current_seed());
  const CapacityDimension dimension = CapacityDimension::power;

  for (int iteration = 0; iteration < 80; ++iteration) {
    const std::int64_t installed = rng.range(1000, 50000);
    const std::int64_t unavailable = rng.range(0, installed / 4);
    const std::int64_t usable = rng.range(0, installed - unavailable);

    SnapshotBuilder before(1);
    fsup::EvidenceSpec spec;
    spec.source = "power-capacity";
    spec.dimension = dimension;
    spec.unit = canonical_unit(dimension);
    spec.installed = installed;
    spec.unavailable = unavailable;
    spec.usable = usable;
    spec.derive_residual = true;
    spec.protected_capacity = 0;
    spec.reserved = 0;
    before.add(fsup::make_evidence(spec));
    auto first = before.build();
    FT_REQUIRE_OK(first);

    // A change that only ever reduces capacity: less installed, less usable,
    // more unavailable, more protected, more reserved.
    const std::int64_t new_installed = rng.range(0, installed);
    const std::int64_t new_unavailable = rng.range(std::min(unavailable, new_installed), new_installed);
    const std::int64_t new_usable = rng.range(0, std::min(usable, new_installed - new_unavailable));
    const std::int64_t new_protected = rng.range(0, new_usable);
    const std::int64_t new_reserved = rng.range(0, new_usable - new_protected);

    SnapshotBuilder after(2);
    fsup::EvidenceSpec reduced;
    reduced.source = "power-capacity";
    reduced.dimension = dimension;
    reduced.installed = new_installed;
    reduced.unavailable = new_unavailable;
    reduced.usable = new_usable;
    reduced.derive_residual = true;
    reduced.protected_capacity = new_protected;
    reduced.reserved = new_reserved;
    after.add(fsup::make_evidence(reduced));
    auto second = after.build();
    FT_REQUIRE_OK(second);

    ftest::set_case_context("seed=" + std::to_string(ftest::current_seed()) +
                            " iteration=" + std::to_string(iteration));
    const Measured before_allocatable = first.value()->allocatable(dimension);
    const Measured after_allocatable = second.value()->allocatable(dimension);
    FT_REQUIRE(before_allocatable.is_known());
    FT_REQUIRE(after_allocatable.is_known());
    FT_CHECK(after_allocatable.magnitude() <= before_allocatable.magnitude());
    FT_CHECK(second.value()->find_totals(dimension)->installed.magnitude() <=
             first.value()->find_totals(dimension)->installed.magnitude());
    // No source ever reports more installed capacity than it measured.
    FT_CHECK(second.value()->find_totals(dimension)->allocatable.magnitude() <=
             second.value()->find_totals(dimension)->installed.magnitude());
  }
}

FT_TEST(property, unmeasured_inputs_never_become_zero) {
  ftest::Rng rng(ftest::current_seed());
  const CapacityDimension dimension = CapacityDimension::cooling;

  for (int iteration = 0; iteration < 80; ++iteration) {
    const std::int64_t installed = rng.range(1000, 50000);
    const std::int64_t unavailable = rng.range(0, installed / 4);
    const std::int64_t usable = rng.range(0, installed - unavailable);

    fsup::EvidenceSpec spec;
    spec.source = "cooling-capacity";
    spec.dimension = dimension;
    spec.unit = canonical_unit(dimension);
    spec.installed = installed;
    spec.unavailable = unavailable;
    spec.usable = usable;
    spec.derive_residual = true;
    spec.protected_capacity = 0;
    spec.reserved = 0;

    // Withhold one of the parts of the allocation identity at random.
    const std::uint64_t choice = rng.below(3);
    if (choice == 0) {
      spec.usable = std::nullopt;
      spec.residual = std::nullopt;
      spec.derive_residual = false;
    } else if (choice == 1) {
      spec.protected_capacity = std::nullopt;
    } else {
      spec.reserved = std::nullopt;
    }

    SnapshotBuilder builder(static_cast<std::uint64_t>(iteration) + 1);
    builder.add(fsup::make_evidence(spec));
    auto snapshot = builder.build();
    FT_REQUIRE_OK(snapshot);

    ftest::set_case_context("seed=" + std::to_string(ftest::current_seed()) +
                            " iteration=" + std::to_string(iteration) + " withheld=" +
                            std::to_string(choice));
    const Measured allocatable = snapshot.value()->allocatable(dimension);
    FT_CHECK(allocatable.is_unknown());
    FT_CHECK(!allocatable.is_known_zero());
    FT_CHECK_EQ(snapshot.value()->outcome(dimension), CapacityOutcome::unknown);
    const DimensionTotals* totals = snapshot.value()->find_totals(dimension);
    FT_REQUIRE(totals != nullptr);
    FT_CHECK(!totals->allocation_closed);
    FT_CHECK(totals->installed.is_known());
    FT_CHECK_EQ(totals->installed.magnitude(), installed);
  }
}

FT_TEST(property, encoding_is_order_independent_and_reproducible) {
  ftest::Rng rng(ftest::current_seed());

  for (int iteration = 0; iteration < 40; ++iteration) {
    std::vector<SourceEvidence> records;
    std::vector<CapacityReserve> reserves;
    for (int index = 0; index < 4; ++index) {
      fsup::EvidenceSpec spec;
      spec.source = "source-" + std::to_string(index);
      spec.dimension = CapacityDimension::power;
      spec.installed = rng.range(1000, 20000);
      spec.unavailable = rng.range(0, 500);
      spec.usable = spec.installed.value() - spec.unavailable.value();
      spec.derive_residual = true;
      spec.protected_capacity = 0;
      spec.reserved = 0;
      records.push_back(fsup::make_evidence(spec));
    }
    for (int index = 0; index < 3; ++index) {
      fsup::ReserveSpec spec;
      spec.id = "protection-" + std::to_string(index);
      spec.source = "source-" + std::to_string(index);
      spec.dimension = CapacityDimension::power;
      spec.kind = ReserveKind::operational;
      spec.amount = rng.range(0, 100);
      reserves.push_back(fsup::make_reserve(spec));
    }

    auto build_with = [&](std::vector<SourceEvidence> evidence_order,
                          std::vector<CapacityReserve> reserve_order) {
      SnapshotBuilder builder(5);
      for (const SourceEvidence& record : evidence_order) {
        builder.add(record);
      }
      for (const CapacityReserve& reserve : reserve_order) {
        builder.add(reserve);
      }
      return builder.build();
    };

    auto reference_snapshot = build_with(records, reserves);
    FT_REQUIRE_OK(reference_snapshot);

    for (int shuffle = 0; shuffle < 6; ++shuffle) {
      std::vector<SourceEvidence> shuffled_records = records;
      std::vector<CapacityReserve> shuffled_reserves = reserves;
      for (std::size_t index = shuffled_records.size(); index > 1; --index) {
        std::swap(shuffled_records[index - 1], shuffled_records[static_cast<std::size_t>(rng.below(index))]);
      }
      for (std::size_t index = shuffled_reserves.size(); index > 1; --index) {
        std::swap(shuffled_reserves[index - 1], shuffled_reserves[static_cast<std::size_t>(rng.below(index))]);
      }
      auto shuffled = build_with(shuffled_records, shuffled_reserves);
      FT_REQUIRE_OK(shuffled);
      FT_CHECK_EQ(shuffled.value()->digest(), reference_snapshot.value()->digest());
      FT_CHECK_EQ(shuffled.value()->id().value(), reference_snapshot.value()->id().value());
    }
  }
}

FT_TEST(property, every_dimension_obeys_its_invariants_under_random_mutation) {
  ftest::Rng rng(ftest::current_seed());

  for (int iteration = 0; iteration < 40; ++iteration) {
    SnapshotBuilder builder(static_cast<std::uint64_t>(iteration) + 1);
    for (std::size_t ordinal = 0; ordinal < capacity_dimension_count; ++ordinal) {
      const CapacityDimension dimension = all_capacity_dimensions()[ordinal];
      const std::size_t source_count = static_cast<std::size_t>(rng.range(1, 3));
      for (std::size_t index = 0; index < source_count; ++index) {
        const std::int64_t installed = rng.range(0, 100000);
        const std::int64_t unavailable = rng.range(0, installed);
        const std::int64_t usable = rng.range(0, installed - unavailable);
        const std::int64_t protected_capacity = rng.range(0, usable);
        const std::int64_t reserved = rng.range(0, usable - protected_capacity);
        fsup::EvidenceSpec spec;
        spec.source = "source-" + std::to_string(index);
        spec.dimension = dimension;
        spec.unit = canonical_unit(dimension);
        spec.generation = static_cast<std::uint64_t>(rng.range(1, 5));
        spec.installed = installed;
        spec.unavailable = unavailable;
        spec.usable = usable;
        spec.derive_residual = true;
        spec.protected_capacity = protected_capacity;
        spec.reserved = reserved;
        if (rng.chance(1, 4)) {
          spec.state = OperationalState::degraded;
        }
        builder.add(fsup::make_evidence(spec));
      }
    }

    ftest::set_case_context("seed=" + std::to_string(ftest::current_seed()) +
                            " iteration=" + std::to_string(iteration));
    auto snapshot = builder.build();
    FT_REQUIRE_OK(snapshot);

    for (std::size_t ordinal = 0; ordinal < capacity_dimension_count; ++ordinal) {
      const CapacityDimension dimension = all_capacity_dimensions()[ordinal];
      const DimensionTotals* totals = snapshot.value()->find_totals(dimension);
      FT_REQUIRE(totals != nullptr);
      FT_REQUIRE(totals->installed.is_known());
      FT_REQUIRE(totals->usable.is_known());
      FT_REQUIRE(totals->allocatable.is_known());
      FT_CHECK_EQ(totals->unit, canonical_unit(dimension));
      FT_CHECK(totals->allocatable.magnitude() <= totals->usable.magnitude());
      FT_CHECK(totals->usable.magnitude() <= totals->installed.magnitude());
      FT_CHECK(totals->protected_capacity.magnitude() + totals->reserved.magnitude() <=
               totals->usable.magnitude());
      FT_CHECK_EQ(totals->usable.magnitude() + totals->unavailable.magnitude() +
                      totals->residual.magnitude(),
                  totals->installed.magnitude());
      const CapacityOutcome outcome = snapshot.value()->outcome(dimension);
      FT_CHECK(outcome == CapacityOutcome::usable || outcome == CapacityOutcome::exhausted);
      if (outcome == CapacityOutcome::exhausted) {
        FT_CHECK(totals->allocatable.is_known_zero());
      } else {
        FT_CHECK(totals->allocatable.is_known_positive());
      }
    }
    FT_CHECK(snapshot.value()->accounting_closed());
  }
}
