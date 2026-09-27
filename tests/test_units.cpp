// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Suites: units, measured, dimension.
//
// Exact integer accounting: a magnitude is either an exact number in one
// explicit unit or a statement that it was never measured. Nothing here
// consults a clock, a locale or a pseudo-random source that is not seeded from
// the run's seed.

#include "test_framework.hpp"
#include "test_rng.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fcap = dccp::facility_capacity;

using fcap::CapacityDimension;
using fcap::ErrorCode;
using fcap::Measured;
using fcap::Quantity;
using fcap::ResultMisuse;
using fcap::Unit;

namespace {

constexpr std::int64_t kMaxMagnitude = fcap::limits::max_quantity_magnitude;

/// Every dimension, in the ordinal order the model publishes.
constexpr CapacityDimension kAllDimensions[fcap::capacity_dimension_count] = {
    CapacityDimension::space,   CapacityDimension::rack,   CapacityDimension::power,
    CapacityDimension::cooling, CapacityDimension::operational_reserve, CapacityDimension::facility_service};

}  // namespace

// ---------------------------------------------------------------------------
// units
// ---------------------------------------------------------------------------

FT_TEST(units, unit_name_and_parse_round_trip) {
  for (std::size_t index = 0; index < fcap::unit_count; ++index) {
    const Unit unit = static_cast<Unit>(index);
    const std::string name(fcap::unit_name(unit));
    FT_CHECK(!name.empty());
    FT_CHECK(name != "unknown_unit");

    Unit parsed = Unit::rack_unit;
    FT_REQUIRE(fcap::parse_unit(name, parsed));
    FT_CHECK_EQ(parsed, unit);
  }
}

FT_TEST(units, unknown_unit_name_fails_to_parse) {
  Unit parsed = Unit::milli_watt;
  FT_CHECK(!fcap::parse_unit("milli_watts", parsed));
  FT_CHECK_EQ(parsed, Unit::milli_watt);
  FT_CHECK(!fcap::parse_unit("", parsed));
  FT_CHECK_EQ(parsed, Unit::milli_watt);
  FT_CHECK(!fcap::parse_unit("MILLI_WATT", parsed));
  FT_CHECK_EQ(parsed, Unit::milli_watt);
  FT_CHECK(!fcap::parse_unit("unknown_unit", parsed));
  FT_CHECK_EQ(parsed, Unit::milli_watt);
}

FT_TEST(units, published_unit_names_and_symbols) {
  FT_CHECK_EQ(std::string(fcap::unit_name(Unit::rack_unit)), "rack_unit");
  FT_CHECK_EQ(std::string(fcap::unit_name(Unit::rack_slot)), "rack_slot");
  FT_CHECK_EQ(std::string(fcap::unit_name(Unit::milli_watt)), "milli_watt");
  FT_CHECK_EQ(std::string(fcap::unit_name(Unit::milli_watt_thermal)), "milli_watt_thermal");
  FT_CHECK_EQ(std::string(fcap::unit_name(Unit::reserve_quantum)), "reserve_quantum");

  FT_CHECK_EQ(std::string(fcap::unit_symbol(Unit::rack_unit)), "U");
  FT_CHECK_EQ(std::string(fcap::unit_symbol(Unit::rack_slot)), "slot");
  FT_CHECK_EQ(std::string(fcap::unit_symbol(Unit::milli_watt)), "mW");
  FT_CHECK_EQ(std::string(fcap::unit_symbol(Unit::milli_watt_thermal)), "mWth");
  FT_CHECK_EQ(std::string(fcap::unit_symbol(Unit::reserve_quantum)), "rq");
}

FT_TEST(units, unnamed_unit_values_report_placeholders) {
  const Unit beyond = static_cast<Unit>(200);
  FT_CHECK_EQ(std::string(fcap::unit_name(beyond)), "unknown_unit");
  FT_CHECK_EQ(std::string(fcap::unit_symbol(beyond)), "?");
}

FT_TEST(units, unit_streams_its_name) {
  FT_CHECK_EQ(ftest::render(Unit::rack_unit), "rack_unit");
  FT_CHECK_EQ(ftest::render(Unit::milli_watt_thermal), "milli_watt_thermal");
}

FT_TEST(units, quantity_make_accepts_the_documented_bounds) {
  FT_REQUIRE_OK(Quantity::make(Unit::milli_watt, 0));
  FT_REQUIRE_OK(Quantity::make(Unit::milli_watt, kMaxMagnitude));
  FT_REQUIRE_OK(Quantity::make(Unit::milli_watt, -kMaxMagnitude));
}

FT_TEST(units, quantity_make_rejects_a_magnitude_beyond_the_bound) {
  FT_CHECK_ERROR(Quantity::make(Unit::milli_watt, kMaxMagnitude + 1), ErrorCode::limit_exceeded);
  FT_CHECK_ERROR(Quantity::make(Unit::milli_watt, -kMaxMagnitude - 1), ErrorCode::limit_exceeded);

  const auto beyond = Quantity::make(Unit::rack_slot, kMaxMagnitude + 1);
  FT_REQUIRE(!beyond.has_value());
  FT_CHECK_EQ(beyond.error().constraint(), "abs(magnitude) <= max_quantity_magnitude");
}

FT_TEST(units, quantity_zero_and_accessors) {
  const Quantity zero = Quantity::zero(Unit::milli_watt_thermal);
  FT_CHECK_EQ(zero.unit(), Unit::milli_watt_thermal);
  FT_CHECK_EQ(zero.magnitude(), 0);
  FT_CHECK(zero.is_zero());
  FT_CHECK(!zero.is_negative());

  FT_REQUIRE_OK(Quantity::make(Unit::rack_slot, -3));
  const auto negative = Quantity::make(Unit::rack_slot, -3);
  FT_REQUIRE_OK(negative);
  FT_CHECK(!negative.value().is_zero());
  FT_CHECK(negative.value().is_negative());
}

FT_TEST(units, quantity_add_is_exact) {
  const auto lhs = Quantity::make(Unit::rack_unit, 40);
  const auto rhs = Quantity::make(Unit::rack_unit, 2);
  FT_REQUIRE_OK(lhs);
  FT_REQUIRE_OK(rhs);

  const auto sum = lhs.value().add(rhs.value());
  FT_REQUIRE_OK(sum);
  FT_CHECK_EQ(sum.value().magnitude(), 42);
  FT_CHECK_EQ(sum.value().unit(), Unit::rack_unit);
}

FT_TEST(units, quantity_add_rejects_different_units) {
  const auto lhs = Quantity::make(Unit::milli_watt, 5);
  const auto rhs = Quantity::make(Unit::rack_unit, 5);
  FT_REQUIRE_OK(lhs);
  FT_REQUIRE_OK(rhs);

  const auto sum = lhs.value().add(rhs.value());
  FT_CHECK_ERROR(sum, ErrorCode::unit_mismatch);
  FT_CHECK_EQ(sum.error().constraint(), "lhs.unit == rhs.unit");
}

// The accepted magnitude bound is far below INT64_MAX, so two quantities that
// each passed `Quantity::make` can never overflow a signed 64-bit sum: the
// observable failure of an out-of-range sum is the range check inside `make`.
FT_TEST(units, quantity_add_reports_a_sum_beyond_the_accepted_range) {
  const auto lhs = Quantity::make(Unit::milli_watt, kMaxMagnitude);
  const auto rhs = Quantity::make(Unit::milli_watt, kMaxMagnitude);
  FT_REQUIRE_OK(lhs);
  FT_REQUIRE_OK(rhs);

  const auto sum = lhs.value().add(rhs.value());
  FT_CHECK_ERROR(sum, ErrorCode::limit_exceeded);
  FT_CHECK_EQ(sum.error().constraint(), "abs(magnitude) <= max_quantity_magnitude");
}

FT_TEST(units, quantity_subtract_is_exact_and_may_be_negative) {
  const auto lhs = Quantity::make(Unit::rack_unit, 5);
  const auto rhs = Quantity::make(Unit::rack_unit, 8);
  FT_REQUIRE_OK(lhs);
  FT_REQUIRE_OK(rhs);

  const auto difference = lhs.value().subtract(rhs.value());
  FT_REQUIRE_OK(difference);
  FT_CHECK_EQ(difference.value().magnitude(), -3);
  FT_CHECK(difference.value().is_negative());
  FT_CHECK_EQ(difference.value().unit(), Unit::rack_unit);
}

FT_TEST(units, quantity_subtract_rejects_different_units) {
  const auto lhs = Quantity::make(Unit::rack_unit, 5);
  const auto rhs = Quantity::make(Unit::milli_watt, 5);
  FT_REQUIRE_OK(lhs);
  FT_REQUIRE_OK(rhs);

  const auto difference = lhs.value().subtract(rhs.value());
  FT_CHECK_ERROR(difference, ErrorCode::unit_mismatch);
  FT_CHECK_EQ(difference.error().constraint(), "lhs.unit == rhs.unit");
}

FT_TEST(units, quantity_scale_is_exact) {
  const auto value = Quantity::make(Unit::milli_watt, 7);
  FT_REQUIRE_OK(value);

  const auto tripled = value.value().scale(3);
  FT_REQUIRE_OK(tripled);
  FT_CHECK_EQ(tripled.value().magnitude(), 21);

  const auto zeroed = value.value().scale(0);
  FT_REQUIRE_OK(zeroed);
  FT_CHECK_EQ(zeroed.value().magnitude(), 0);
  FT_CHECK(zeroed.value().is_zero());

  const auto negated = value.value().scale(-1);
  FT_REQUIRE_OK(negated);
  FT_CHECK_EQ(negated.value().magnitude(), -7);
}

FT_TEST(units, quantity_scale_reports_a_product_beyond_int64) {
  const auto value = Quantity::make(Unit::milli_watt, kMaxMagnitude);
  FT_REQUIRE_OK(value);

  // 10^15 * 100000 = 10^20, which is not representable in a signed 64-bit value.
  const auto overflowed = value.value().scale(100000);
  FT_CHECK_ERROR(overflowed, ErrorCode::limit_exceeded);
  FT_CHECK_EQ(overflowed.error().constraint(), "product within int64");
}

FT_TEST(units, quantity_scale_reports_a_product_beyond_the_accepted_range) {
  const auto value = Quantity::make(Unit::milli_watt, kMaxMagnitude);
  FT_REQUIRE_OK(value);

  // 10^15 * 1000 is representable in int64 but outside the accepted magnitude.
  const auto scaled = value.value().scale(1000);
  FT_CHECK_ERROR(scaled, ErrorCode::limit_exceeded);
  FT_CHECK_EQ(scaled.error().constraint(), "abs(magnitude) <= max_quantity_magnitude");
}

FT_TEST(units, quantity_to_string_and_streaming) {
  const auto value = Quantity::make(Unit::milli_watt, 5);
  FT_REQUIRE_OK(value);
  FT_CHECK_EQ(value.value().to_string(), "5 mW");
  FT_CHECK_EQ(ftest::render(value.value()), "5 mW");

  const auto negative = Quantity::make(Unit::rack_unit, -12);
  FT_REQUIRE_OK(negative);
  FT_CHECK_EQ(negative.value().to_string(), "-12 U");
}

FT_TEST(units, quantity_equality_covers_unit_and_magnitude) {
  const auto lhs = Quantity::make(Unit::milli_watt, 5);
  const auto same = Quantity::make(Unit::milli_watt, 5);
  const auto other_magnitude = Quantity::make(Unit::milli_watt, 6);
  const auto other_unit = Quantity::make(Unit::milli_watt_thermal, 5);
  FT_REQUIRE_OK(lhs);
  FT_REQUIRE_OK(same);
  FT_REQUIRE_OK(other_magnitude);
  FT_REQUIRE_OK(other_unit);

  FT_CHECK(lhs.value() == same.value());
  FT_CHECK(lhs.value() != other_magnitude.value());
  FT_CHECK(lhs.value() != other_unit.value());
}

FT_TEST(units, checked_add_matches_the_method) {
  const auto lhs = Quantity::make(Unit::rack_slot, 11);
  const auto rhs = Quantity::make(Unit::rack_slot, 4);
  FT_REQUIRE_OK(lhs);
  FT_REQUIRE_OK(rhs);

  const auto free_function = fcap::checked_add(lhs.value(), rhs.value());
  const auto method = lhs.value().add(rhs.value());
  FT_REQUIRE_OK(free_function);
  FT_REQUIRE_OK(method);
  FT_CHECK_EQ(free_function.value().magnitude(), 15);
  FT_CHECK(free_function.value() == method.value());
}

FT_TEST(units, checked_subtract_matches_the_method) {
  const auto lhs = Quantity::make(Unit::rack_slot, 11);
  const auto rhs = Quantity::make(Unit::rack_slot, 4);
  FT_REQUIRE_OK(lhs);
  FT_REQUIRE_OK(rhs);

  const auto free_function = fcap::checked_subtract(lhs.value(), rhs.value());
  const auto method = lhs.value().subtract(rhs.value());
  FT_REQUIRE_OK(free_function);
  FT_REQUIRE_OK(method);
  FT_CHECK_EQ(free_function.value().magnitude(), 7);
  FT_CHECK(free_function.value() == method.value());
}

FT_TEST(units, checked_subtract_reports_unit_mismatch) {
  const auto lhs = Quantity::make(Unit::rack_slot, 11);
  const auto rhs = Quantity::make(Unit::reserve_quantum, 4);
  FT_REQUIRE_OK(lhs);
  FT_REQUIRE_OK(rhs);

  FT_CHECK_ERROR(fcap::checked_subtract(lhs.value(), rhs.value()), ErrorCode::unit_mismatch);
}

FT_TEST(units, checked_sum_is_exact) {
  const auto first = Quantity::make(Unit::milli_watt, 10);
  const auto second = Quantity::make(Unit::milli_watt, 32);
  const auto third = Quantity::make(Unit::milli_watt, 8);
  FT_REQUIRE_OK(first);
  FT_REQUIRE_OK(second);
  FT_REQUIRE_OK(third);
  const std::vector<Quantity> quantities = {first.value(), second.value(), third.value()};

  const auto total = fcap::checked_sum(quantities.data(), quantities.size(), Unit::milli_watt);
  FT_REQUIRE_OK(total);
  FT_CHECK_EQ(total.value().magnitude(), 50);
  FT_CHECK_EQ(total.value().unit(), Unit::milli_watt);
}

FT_TEST(units, checked_sum_of_nothing_is_zero_in_the_requested_unit) {
  const std::vector<Quantity> quantities;

  const auto total = fcap::checked_sum(quantities.data(), quantities.size(), Unit::rack_slot);
  FT_REQUIRE_OK(total);
  FT_CHECK_EQ(total.value().magnitude(), 0);
  FT_CHECK_EQ(total.value().unit(), Unit::rack_slot);
  FT_CHECK(total.value().is_zero());
}

FT_TEST(units, checked_sum_reports_a_total_beyond_the_accepted_range) {
  const auto first = Quantity::make(Unit::milli_watt, kMaxMagnitude);
  const auto second = Quantity::make(Unit::milli_watt, kMaxMagnitude);
  FT_REQUIRE_OK(first);
  FT_REQUIRE_OK(second);
  const std::vector<Quantity> quantities = {first.value(), second.value()};

  const auto total = fcap::checked_sum(quantities.data(), quantities.size(), Unit::milli_watt);
  FT_CHECK_ERROR(total, ErrorCode::limit_exceeded);
}

FT_TEST(units, checked_sum_rejects_a_quantity_in_another_unit) {
  const auto first = Quantity::make(Unit::milli_watt, 10);
  const auto second = Quantity::make(Unit::rack_unit, 10);
  FT_REQUIRE_OK(first);
  FT_REQUIRE_OK(second);
  const std::vector<Quantity> quantities = {first.value(), second.value()};

  const auto total = fcap::checked_sum(quantities.data(), quantities.size(), Unit::milli_watt);
  FT_CHECK_ERROR(total, ErrorCode::unit_mismatch);
}

// ---------------------------------------------------------------------------
// measured
// ---------------------------------------------------------------------------

FT_TEST(measured, unknown_is_not_zero) {
  const Measured value = Measured::unknown();
  FT_CHECK(!value.is_known());
  FT_CHECK(value.is_unknown());
  FT_CHECK(!value.is_known_zero());
  FT_CHECK(!value.is_known_positive());
  FT_CHECK_EQ(value.to_string(), "unknown");
  FT_CHECK_EQ(ftest::render(value), "unknown");
}

FT_TEST(measured, magnitude_of_an_unknown_throws_result_misuse) {
  const Measured value = Measured::unknown();
  bool threw = false;
  try {
    const std::int64_t magnitude = value.magnitude();
    static_cast<void>(magnitude);
  } catch (const ResultMisuse&) {
    threw = true;
  }
  FT_CHECK(threw);
}

FT_TEST(measured, known_rejects_a_negative_magnitude) {
  const auto value = Measured::known(Unit::milli_watt, -1);
  FT_CHECK_ERROR(value, ErrorCode::invalid_argument);
  FT_CHECK_EQ(value.error().constraint(), "magnitude >= 0");
}

FT_TEST(measured, known_rejects_a_magnitude_beyond_the_bound) {
  const auto value = Measured::known(Unit::milli_watt, kMaxMagnitude + 1);
  FT_CHECK_ERROR(value, ErrorCode::limit_exceeded);
  FT_CHECK_EQ(value.error().constraint(), "magnitude <= max_quantity_magnitude");
}

FT_TEST(measured, known_accepts_the_documented_bounds) {
  const auto zero = Measured::known(Unit::rack_unit, 0);
  FT_REQUIRE_OK(zero);
  FT_CHECK(zero.value().is_known());
  FT_CHECK(zero.value().is_known_zero());
  FT_CHECK(!zero.value().is_known_positive());

  const auto at_bound = Measured::known(Unit::milli_watt, kMaxMagnitude);
  FT_REQUIRE_OK(at_bound);
  FT_CHECK(at_bound.value().is_known_positive());
  FT_CHECK_EQ(at_bound.value().magnitude(), kMaxMagnitude);
  FT_CHECK_EQ(at_bound.value().unit(), Unit::milli_watt);

  const Measured explicit_zero = Measured::known_zero(Unit::reserve_quantum);
  FT_CHECK(explicit_zero.is_known_zero());
  FT_CHECK_EQ(explicit_zero.unit(), Unit::reserve_quantum);
}

FT_TEST(measured, known_from_a_quantity_applies_the_same_rules) {
  const auto positive = Quantity::make(Unit::milli_watt, 12);
  const auto negative = Quantity::make(Unit::milli_watt, -12);
  const auto beyond = Quantity::make(Unit::milli_watt, kMaxMagnitude);
  FT_REQUIRE_OK(positive);
  FT_REQUIRE_OK(negative);
  FT_REQUIRE_OK(beyond);

  const auto from_positive = Measured::known(positive.value());
  FT_REQUIRE_OK(from_positive);
  FT_CHECK_EQ(from_positive.value().magnitude(), 12);
  FT_CHECK_EQ(from_positive.value().unit(), Unit::milli_watt);

  FT_CHECK_ERROR(Measured::known(negative.value()), ErrorCode::invalid_argument);

  const auto from_bound = Measured::known(beyond.value());
  FT_REQUIRE_OK(from_bound);
  FT_CHECK_EQ(from_bound.value().magnitude(), kMaxMagnitude);
}

FT_TEST(measured, to_quantity_of_an_unknown_fails) {
  FT_CHECK_ERROR(Measured::unknown().to_quantity(), ErrorCode::not_measured);
}

FT_TEST(measured, to_quantity_of_a_known_value_is_exact) {
  const Measured value = fsup::known_unit(Unit::milli_watt_thermal, 33);
  const auto quantity = value.to_quantity();
  FT_REQUIRE_OK(quantity);
  FT_CHECK_EQ(quantity.value().magnitude(), 33);
  FT_CHECK_EQ(quantity.value().unit(), Unit::milli_watt_thermal);
}

FT_TEST(measured, add_of_two_knowns_is_exact) {
  const Measured lhs = fsup::known_unit(Unit::milli_watt, 400);
  const Measured rhs = fsup::known_unit(Unit::milli_watt, 250);

  const Measured sum = lhs.add(rhs);
  FT_CHECK(sum.is_known());
  FT_CHECK_EQ(sum.magnitude(), 650);
  FT_CHECK_EQ(sum.unit(), Unit::milli_watt);
}

FT_TEST(measured, add_with_an_unknown_is_unknown) {
  const Measured known = fsup::known_unit(Unit::milli_watt, 400);
  const Measured unknown = fsup::unknown();

  FT_CHECK(!known.add(unknown).is_known());
  FT_CHECK(!unknown.add(known).is_known());
  FT_CHECK(!unknown.add(unknown).is_known());
  FT_CHECK(!known.add(unknown).is_known_zero());
}

FT_TEST(measured, add_beyond_the_bound_is_unknown_not_wrapped) {
  const Measured lhs = fsup::known_unit(Unit::milli_watt, kMaxMagnitude);
  const Measured rhs = fsup::known_unit(Unit::milli_watt, 1);

  const Measured sum = lhs.add(rhs);
  FT_CHECK(!sum.is_known());
  FT_CHECK(!sum.is_known_zero());
}

// Documents the observed behaviour: `Measured::add` carries the left operand's
// unit through and does not compare the two units. `Quantity::add` does compare
// them and reports `unit_mismatch`. Reported to the maintainers, not fixed here.
FT_TEST(measured, add_does_not_compare_units) {
  // A dimension can never be silently conflated with another: a total function
  // that cannot report the mismatch reports the absence of an answer instead.
  const Measured milli_watts = fsup::known_unit(Unit::milli_watt, 5);
  const Measured rack_units = fsup::known_unit(Unit::rack_unit, 3);
  const Measured sum = milli_watts.add(rack_units);
  FT_CHECK(sum.is_unknown());
  FT_CHECK(!sum.is_known_zero());

  FT_CHECK_ERROR(milli_watts.checked_add(rack_units), ErrorCode::unit_mismatch);
  FT_CHECK_ERROR(milli_watts.subtract(rack_units, "difference >= 0"), ErrorCode::unit_mismatch);

  const auto same_unit = milli_watts.checked_add(fsup::known_unit(Unit::milli_watt, 3));
  FT_REQUIRE_OK(same_unit);
  FT_CHECK_EQ(same_unit.value().magnitude(), std::int64_t{8});
  FT_CHECK_EQ(same_unit.value().unit(), Unit::milli_watt);
}
FT_TEST(measured, subtract_is_exact) {
  const Measured lhs = fsup::known_unit(Unit::milli_watt, 900);
  const Measured rhs = fsup::known_unit(Unit::milli_watt, 400);

  const auto difference = lhs.subtract(rhs, "usable - held >= 0");
  FT_REQUIRE_OK(difference);
  FT_CHECK_EQ(difference.value().magnitude(), 500);
  FT_CHECK_EQ(difference.value().unit(), Unit::milli_watt);
}

FT_TEST(measured, subtract_below_zero_names_the_callers_constraint) {
  const Measured lhs = fsup::known_unit(Unit::milli_watt, 5);
  const Measured rhs = fsup::known_unit(Unit::milli_watt, 8);

  const auto difference = lhs.subtract(rhs, "usable - protected - reserved >= 0");
  FT_CHECK_ERROR(difference, ErrorCode::account_mismatch);
  FT_CHECK_EQ(difference.error().constraint(), "usable - protected - reserved >= 0");
}

FT_TEST(measured, subtract_with_an_unknown_is_unknown) {
  const Measured known = fsup::known_unit(Unit::milli_watt, 5);
  const Measured unknown = fsup::unknown();

  const auto from_unknown = unknown.subtract(known, "unknown - known >= 0");
  FT_REQUIRE_OK(from_unknown);
  FT_CHECK(!from_unknown.value().is_known());

  const auto from_known = known.subtract(unknown, "known - unknown >= 0");
  FT_REQUIRE_OK(from_known);
  FT_CHECK(!from_known.value().is_known());
}

FT_TEST(measured, subtract_to_exactly_zero_is_known_zero) {
  const Measured lhs = fsup::known_unit(Unit::rack_unit, 12);
  const Measured rhs = fsup::known_unit(Unit::rack_unit, 12);

  const auto difference = lhs.subtract(rhs, "identical >= 0");
  FT_REQUIRE_OK(difference);
  FT_CHECK(difference.value().is_known_zero());
  FT_CHECK(!difference.value().is_known_positive());
}

FT_TEST(measured, scale_is_exact) {
  const Measured value = fsup::known_unit(Unit::milli_watt, 21);

  const auto doubled = value.scale(2);
  FT_REQUIRE_OK(doubled);
  FT_CHECK_EQ(doubled.value().magnitude(), 42);

  const auto zeroed = value.scale(0);
  FT_REQUIRE_OK(zeroed);
  FT_CHECK(zeroed.value().is_known_zero());
}

FT_TEST(measured, scale_by_a_negative_factor_fails) {
  const Measured value = fsup::known_unit(Unit::milli_watt, 21);

  const auto scaled = value.scale(-2);
  FT_CHECK_ERROR(scaled, ErrorCode::account_mismatch);
  FT_CHECK_EQ(scaled.error().constraint(), "scaled magnitude >= 0");
}

FT_TEST(measured, scale_of_an_unknown_is_unknown) {
  const auto scaled = Measured::unknown().scale(4);
  FT_REQUIRE_OK(scaled);
  FT_CHECK(!scaled.value().is_known());
}

FT_TEST(measured, scale_reports_a_product_beyond_the_bound) {
  const Measured value = fsup::known_unit(Unit::milli_watt, kMaxMagnitude);

  const auto overflowed = value.scale(100000);
  FT_CHECK_ERROR(overflowed, ErrorCode::limit_exceeded);
  FT_CHECK_EQ(overflowed.error().constraint(), "product within int64");

  const auto out_of_range = value.scale(1000);
  FT_CHECK_ERROR(out_of_range, ErrorCode::limit_exceeded);
  FT_CHECK_EQ(out_of_range.error().constraint(), "magnitude <= max_quantity_magnitude");
}

FT_TEST(measured, or_else_selects_the_fallback_only_when_unknown) {
  const Measured known = fsup::known_unit(Unit::milli_watt, 7);
  const Measured fallback = fsup::known_unit(Unit::milli_watt, 99);
  const Measured unknown = fsup::unknown();

  const Measured kept = known.or_else(fallback);
  FT_CHECK_EQ(kept.magnitude(), 7);

  const Measured replaced = unknown.or_else(fallback);
  FT_CHECK_EQ(replaced.magnitude(), 99);

  const Measured still_unknown = unknown.or_else(Measured::unknown());
  FT_CHECK(!still_unknown.is_known());
}

FT_TEST(measured, equality_distinguishes_unknown_from_known_zero) {
  const Measured first_unknown = Measured::unknown();
  const Measured second_unknown = Measured::unknown();
  const Measured known_zero = Measured::known_zero(Unit::rack_unit);
  const Measured known_positive = fsup::known_unit(Unit::rack_unit, 1);

  FT_CHECK(first_unknown == second_unknown);
  FT_CHECK(first_unknown != known_zero);
  FT_CHECK(known_zero != first_unknown);
  FT_CHECK(known_zero != known_positive);
  FT_CHECK(known_zero == Measured::known_zero(Unit::rack_unit));
  FT_CHECK(known_zero != Measured::known_zero(Unit::milli_watt));
}

FT_TEST(measured, to_string_renders_values) {
  FT_CHECK_EQ(fsup::known_unit(Unit::milli_watt, 250).to_string(), "250 mW");
  FT_CHECK_EQ(fsup::known_unit(Unit::rack_unit, 0).to_string(), "0 U");
  FT_CHECK_EQ(Measured::unknown().to_string(), "unknown");
  FT_CHECK_EQ(ftest::render(fsup::known_unit(Unit::reserve_quantum, 4)), "4 rq");
}

FT_TEST(measured, random_add_is_exact_or_unknown) {
  ftest::Rng rng(ftest::current_seed());
  constexpr int kIterations = 4000;

  for (int iteration = 0; iteration < kIterations; ++iteration) {
    const std::int64_t lhs_magnitude = rng.range(0, kMaxMagnitude);
    const std::int64_t rhs_magnitude = rng.range(0, kMaxMagnitude);
    const Unit unit = static_cast<Unit>(rng.below(fcap::unit_count));

    const auto lhs = Measured::known(unit, lhs_magnitude);
    const auto rhs = Measured::known(unit, rhs_magnitude);
    if (!lhs.has_value() || !rhs.has_value()) {
      ftest::set_case_context("iteration=" + std::to_string(iteration));
      FT_FAIL("a generated magnitude was rejected by Measured::known");
      return;
    }

    const Measured sum = lhs.value().add(rhs.value());

    // The exact sum is representable when it neither overflows int64 nor leaves
    // the accepted magnitude range. Both operands are non-negative, so the
    // representation test is a single comparison against the bound.
    const bool representable = lhs_magnitude <= kMaxMagnitude - rhs_magnitude;
    const bool exact = sum.is_known();
    if (exact != representable) {
      ftest::set_case_context("iteration=" + std::to_string(iteration) + " unit=" +
                              std::to_string(static_cast<unsigned>(unit)) + " lhs=" +
                              std::to_string(lhs_magnitude) + " rhs=" + std::to_string(rhs_magnitude));
      FT_FAIL(std::string("Measured::add known=") + (exact ? "true" : "false") + " but representable=" +
              (representable ? "true" : "false"));
      return;
    }
    if (exact) {
      if (sum.magnitude() != lhs_magnitude + rhs_magnitude) {
        ftest::set_case_context("iteration=" + std::to_string(iteration) + " lhs=" +
                                std::to_string(lhs_magnitude) + " rhs=" + std::to_string(rhs_magnitude));
        FT_FAIL("Measured::add returned a magnitude that is not the exact sum");
        return;
      }
      if (sum.unit() != unit) {
        ftest::set_case_context("iteration=" + std::to_string(iteration));
        FT_FAIL("Measured::add did not carry the operand unit through");
        return;
      }
    }

    const Measured with_unknown = lhs.value().add(Measured::unknown());
    if (with_unknown.is_known()) {
      ftest::set_case_context("iteration=" + std::to_string(iteration) + " lhs=" +
                              std::to_string(lhs_magnitude));
      FT_FAIL("a measured value plus an unmeasured value must stay unmeasured");
      return;
    }
    const Measured unknown_first = Measured::unknown().add(rhs.value());
    if (unknown_first.is_known()) {
      ftest::set_case_context("iteration=" + std::to_string(iteration) + " rhs=" +
                              std::to_string(rhs_magnitude));
      FT_FAIL("an unmeasured value plus a measured value must stay unmeasured");
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// dimension
// ---------------------------------------------------------------------------

FT_TEST(dimension, canonical_unit_per_dimension) {
  FT_CHECK_EQ(fcap::canonical_unit(CapacityDimension::space), Unit::rack_unit);
  FT_CHECK_EQ(fcap::canonical_unit(CapacityDimension::rack), Unit::rack_slot);
  FT_CHECK_EQ(fcap::canonical_unit(CapacityDimension::power), Unit::milli_watt);
  FT_CHECK_EQ(fcap::canonical_unit(CapacityDimension::cooling), Unit::milli_watt_thermal);
  FT_CHECK_EQ(fcap::canonical_unit(CapacityDimension::operational_reserve), Unit::reserve_quantum);
  FT_CHECK_EQ(fcap::canonical_unit(CapacityDimension::facility_service), Unit::milli_watt);
}

FT_TEST(dimension, parse_capacity_dimension_round_trip) {
  for (const CapacityDimension dimension : kAllDimensions) {
    const std::string name(fcap::capacity_dimension_name(dimension));
    FT_CHECK(!name.empty());
    FT_CHECK(name != "unknown_dimension");

    CapacityDimension parsed = CapacityDimension::space;
    FT_REQUIRE(fcap::parse_capacity_dimension(name, parsed));
    FT_CHECK_EQ(parsed, dimension);
  }
}

FT_TEST(dimension, unknown_dimension_name_fails_to_parse) {
  CapacityDimension parsed = CapacityDimension::power;
  FT_CHECK(!fcap::parse_capacity_dimension("powers", parsed));
  FT_CHECK_EQ(parsed, CapacityDimension::power);
  FT_CHECK(!fcap::parse_capacity_dimension("", parsed));
  FT_CHECK_EQ(parsed, CapacityDimension::power);
  FT_CHECK(!fcap::parse_capacity_dimension("unknown_dimension", parsed));
  FT_CHECK_EQ(parsed, CapacityDimension::power);
}

FT_TEST(dimension, published_dimension_names) {
  FT_CHECK_EQ(std::string(fcap::capacity_dimension_name(CapacityDimension::space)), "space");
  FT_CHECK_EQ(std::string(fcap::capacity_dimension_name(CapacityDimension::rack)), "rack");
  FT_CHECK_EQ(std::string(fcap::capacity_dimension_name(CapacityDimension::power)), "power");
  FT_CHECK_EQ(std::string(fcap::capacity_dimension_name(CapacityDimension::cooling)), "cooling");
  FT_CHECK_EQ(std::string(fcap::capacity_dimension_name(CapacityDimension::operational_reserve)),
              "operational_reserve");
  FT_CHECK_EQ(std::string(fcap::capacity_dimension_name(CapacityDimension::facility_service)),
              "facility_service");
  FT_CHECK_EQ(std::string(fcap::capacity_dimension_name(static_cast<CapacityDimension>(200))),
              "unknown_dimension");
}

FT_TEST(dimension, all_capacity_dimensions_is_in_ordinal_order) {
  const CapacityDimension* dimensions = fcap::all_capacity_dimensions();
  FT_REQUIRE(dimensions != nullptr);

  for (std::size_t index = 0; index < fcap::capacity_dimension_count; ++index) {
    FT_CHECK_EQ(dimensions[index], kAllDimensions[index]);
    FT_CHECK_EQ(fcap::dimension_ordinal(dimensions[index]), index);
  }

  // The published order is exactly the ordinal order, so it is strictly
  // increasing and has no duplicate.
  for (std::size_t index = 1; index < fcap::capacity_dimension_count; ++index) {
    FT_CHECK(fcap::dimension_ordinal(dimensions[index - 1]) < fcap::dimension_ordinal(dimensions[index]));
  }
}

FT_TEST(dimension, dimension_streams_its_name) {
  FT_CHECK_EQ(ftest::render(CapacityDimension::operational_reserve), "operational_reserve");
  FT_CHECK_EQ(ftest::render(CapacityDimension::rack), "rack");
}
