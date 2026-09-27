// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Suites: identity, generation, provenance.
//
// Identities, counters and provenance are the typed boundary of the product:
// a facility identifier can never be passed where a site identifier is
// expected, a counter that would wrap is reported rather than silently
// returning to zero, and an input without provenance is refused.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace fcap = dccp::facility_capacity;

using fcap::CapacityGeneration;
using fcap::Counter;
using fcap::EpochId;
using fcap::ErrorCode;
using fcap::IncarnationId;
using fcap::Provenance;
using fcap::Tick;
using fcap::ValidityWindow;

// ---------------------------------------------------------------------------
// The families are distinct types: no implicit conversion and no constructing
// one family from another.
// ---------------------------------------------------------------------------

static_assert(!std::is_same_v<fcap::FacilityId, fcap::SiteId>);
static_assert(!std::is_convertible_v<fcap::FacilityId, fcap::SiteId>);
static_assert(!std::is_constructible_v<fcap::SiteId, fcap::FacilityId>);

static_assert(!std::is_convertible_v<fcap::CapacitySourceId, fcap::ReserveId>);
static_assert(!std::is_constructible_v<fcap::ReserveId, fcap::CapacitySourceId>);

static_assert(!std::is_convertible_v<fcap::ReserveId, fcap::SnapshotId>);
static_assert(!std::is_constructible_v<fcap::SnapshotId, fcap::ReserveId>);

static_assert(!std::is_convertible_v<fcap::SnapshotId, fcap::StoreId>);
static_assert(!std::is_constructible_v<fcap::StoreId, fcap::SnapshotId>);

static_assert(!std::is_convertible_v<fcap::FacilityId, fcap::CapacitySourceId>);
static_assert(!std::is_convertible_v<fcap::ReserveId, fcap::CapacitySourceId>);
static_assert(!std::is_convertible_v<fcap::StoreId, fcap::SiteId>);

// No family is constructible or convertible from a bare string.
static_assert(!std::is_constructible_v<fcap::FacilityId, std::string>);
static_assert(!std::is_constructible_v<fcap::SiteId, std::string>);
static_assert(!std::is_constructible_v<fcap::CapacitySourceId, std::string>);
static_assert(!std::is_constructible_v<fcap::ReserveId, std::string>);
static_assert(!std::is_constructible_v<fcap::SnapshotId, std::string>);
static_assert(!std::is_constructible_v<fcap::StoreId, std::string>);
static_assert(!std::is_convertible_v<std::string, fcap::FacilityId>);
static_assert(!std::is_convertible_v<const char*, fcap::FacilityId>);

// Counters are typed the same way.
static_assert(!std::is_convertible_v<fcap::EpochId, fcap::IncarnationId>);
static_assert(!std::is_constructible_v<fcap::IncarnationId, fcap::EpochId>);
static_assert(!std::is_convertible_v<fcap::Revision, fcap::Tick>);

namespace {

template <class Id>
void check_parses(std::string_view text) {
  const auto parsed = Id::parse(text);
  FT_REQUIRE_OK(parsed);
  FT_CHECK(!parsed.value().empty());
  FT_CHECK(!parsed.value().value().empty());
  FT_CHECK_EQ(parsed.value().value(), std::string(text));
  FT_CHECK_EQ(parsed.value().view(), text);
  FT_CHECK_EQ(parsed.value().to_string(), std::string(text));
  FT_CHECK_EQ(ftest::render(parsed.value()), std::string(text));
}

template <class Id>
void check_rejects(std::string_view text) {
  const auto parsed = Id::parse(text);
  FT_CHECK_ERROR(parsed, ErrorCode::invalid_argument);
  FT_REQUIRE(!parsed.has_value());
  FT_CHECK_EQ(parsed.error().constraint(), std::string(fcap::identifier_rejection_reason(text)));
  FT_CHECK(!parsed.error().constraint().empty());
}

template <class Id>
void check_default_is_empty() {
  const Id id;
  FT_CHECK(id.empty());
  FT_CHECK(id.value().empty());
  FT_CHECK(id.view().empty());
  FT_CHECK_EQ(id.to_string(), std::string());
  FT_CHECK(id == Id());
}

template <class Id>
void check_ordering() {
  const auto alpha = Id::parse("alpha");
  const auto beta = Id::parse("beta");
  const auto alpha_again = Id::parse("alpha");
  FT_REQUIRE_OK(alpha);
  FT_REQUIRE_OK(beta);
  FT_REQUIRE_OK(alpha_again);

  FT_CHECK(alpha.value() == alpha_again.value());
  FT_CHECK(alpha.value() != beta.value());
  FT_CHECK(alpha.value() < beta.value());
  FT_CHECK(beta.value() > alpha.value());
  FT_CHECK(alpha.value() <= alpha_again.value());
  FT_CHECK(alpha.value() >= alpha_again.value());

  // A shorter identifier that is a prefix of a longer one sorts first.
  const auto prefix = Id::parse("alph");
  FT_REQUIRE_OK(prefix);
  FT_CHECK(prefix.value() < alpha.value());
}

template <class CounterType>
void check_counter_family() {
  const auto zero = CounterType::parse("0");
  FT_REQUIRE_OK(zero);
  FT_CHECK(zero.value().is_zero());
  FT_CHECK_EQ(zero.value().value(), std::uint64_t{0});

  const auto forty_two = CounterType::parse("42");
  FT_REQUIRE_OK(forty_two);
  FT_CHECK(!forty_two.value().is_zero());
  FT_CHECK_EQ(forty_two.value().value(), std::uint64_t{42});
  FT_CHECK_EQ(forty_two.value().to_string(), "42");
  FT_CHECK_EQ(ftest::render(forty_two.value()), "42");

  const auto top = CounterType::parse("18446744073709551615");
  FT_REQUIRE_OK(top);
  FT_CHECK_EQ(top.value().value(), UINT64_MAX);
  FT_CHECK(!top.value().is_zero());

  const CounterType from_value = CounterType::from_value(9);
  FT_CHECK_EQ(from_value.value(), std::uint64_t{9});
  FT_CHECK(from_value == CounterType::parse("9").value());
}

template <class CounterType>
void check_counter_rejections() {
  FT_CHECK_ERROR(CounterType::parse(""), ErrorCode::invalid_argument);
  FT_CHECK_ERROR(CounterType::parse("+1"), ErrorCode::invalid_argument);
  FT_CHECK_ERROR(CounterType::parse("-1"), ErrorCode::invalid_argument);
  FT_CHECK_ERROR(CounterType::parse("1a"), ErrorCode::invalid_argument);
  FT_CHECK_ERROR(CounterType::parse(" 1"), ErrorCode::invalid_argument);
  FT_CHECK_ERROR(CounterType::parse("1 "), ErrorCode::invalid_argument);
  FT_CHECK_ERROR(CounterType::parse("1.0"), ErrorCode::invalid_argument);

  // UINT64_MAX + 1 is twenty digits, so it passes the text-length guard and is
  // reported by the magnitude check.
  const auto overflow = CounterType::parse("18446744073709551616");
  FT_CHECK_ERROR(overflow, ErrorCode::limit_exceeded);
  FT_REQUIRE(!overflow.has_value());
  FT_CHECK_EQ(overflow.error().constraint(), "value <= 18446744073709551615");

  // More than twenty characters is refused as malformed text before its
  // magnitude is ever examined.
  FT_CHECK_ERROR(CounterType::parse("99999999999999999999999"), ErrorCode::invalid_argument);
  FT_CHECK_ERROR(CounterType::parse("100000000000000000000"), ErrorCode::invalid_argument);
}

/// A syntactically valid 64-character lowercase hexadecimal digest.
std::string lowercase_digest() { return std::string(64, 'a'); }

}  // namespace

// ---------------------------------------------------------------------------
// identity
// ---------------------------------------------------------------------------

FT_TEST(identity, is_valid_identifier_accepts_documented_values) {
  FT_CHECK(fcap::is_valid_identifier("dc-1"));
  FT_CHECK(fcap::is_valid_identifier("a"));
  FT_CHECK(fcap::is_valid_identifier("A.b_c:d-9"));
  FT_CHECK(fcap::is_valid_identifier("0"));
  FT_CHECK(fcap::is_valid_identifier("9z"));
  FT_CHECK(fcap::is_valid_identifier("a.b"));
  FT_CHECK(fcap::is_valid_identifier("a_b"));
  FT_CHECK(fcap::is_valid_identifier("a:b"));
  FT_CHECK(fcap::is_valid_identifier("a-b"));
  FT_CHECK(fcap::is_valid_identifier("Z"));
  FT_CHECK(fcap::is_valid_identifier(std::string(63, 'a')));
  FT_CHECK(fcap::is_valid_identifier(std::string(63, '9')));
}

FT_TEST(identity, is_valid_identifier_rejects_the_empty_string) {
  FT_CHECK(!fcap::is_valid_identifier(""));
  FT_CHECK(!fcap::is_valid_identifier(std::string_view()));
  FT_CHECK_EQ(std::string(fcap::identifier_rejection_reason("")), "identifier is empty");
}

FT_TEST(identity, is_valid_identifier_rejects_a_64_byte_identifier) {
  FT_CHECK(!fcap::is_valid_identifier(std::string(64, 'a')));
  FT_CHECK(!fcap::is_valid_identifier(std::string(96, 'z')));
  FT_CHECK_EQ(std::string(fcap::identifier_rejection_reason(std::string(64, 'a'))),
              "identifier is longer than max_identifier_length");
}

FT_TEST(identity, is_valid_identifier_rejects_a_leading_separator) {
  FT_CHECK(!fcap::is_valid_identifier(".abc"));
  FT_CHECK(!fcap::is_valid_identifier("-abc"));
  FT_CHECK(!fcap::is_valid_identifier("_abc"));
  FT_CHECK(!fcap::is_valid_identifier(":abc"));
  FT_CHECK_EQ(std::string(fcap::identifier_rejection_reason(":abc")),
              "identifier must begin with a letter or a digit");
}

FT_TEST(identity, is_valid_identifier_rejects_characters_outside_the_alphabet) {
  FT_CHECK(!fcap::is_valid_identifier("ab cd"));
  FT_CHECK(!fcap::is_valid_identifier("ab/cd"));
  FT_CHECK(!fcap::is_valid_identifier("ab\\cd"));
  FT_CHECK(!fcap::is_valid_identifier("ab=cd"));
  FT_CHECK(!fcap::is_valid_identifier("ab+cd"));
  FT_CHECK(!fcap::is_valid_identifier("ab\tcd"));
  FT_CHECK(!fcap::is_valid_identifier("ab\ncd"));
  FT_CHECK(!fcap::is_valid_identifier("ab\x7F"));
}

FT_TEST(identity, is_valid_identifier_rejects_an_embedded_nul_byte) {
  std::string text("ab");
  text.push_back('\0');
  text.append("cd");
  FT_CHECK_EQ(text.size(), std::size_t{5});
  FT_CHECK(!fcap::is_valid_identifier(text));
  FT_CHECK_EQ(std::string(fcap::identifier_rejection_reason(text)),
              "identifier contains a character outside the accepted alphabet");
}

FT_TEST(identity, is_valid_identifier_rejects_a_non_ascii_byte) {
  const std::string non_ascii = "\xC3\xA9";
  FT_CHECK_EQ(non_ascii.size(), std::size_t{2});
  FT_CHECK(!fcap::is_valid_identifier(non_ascii));
  FT_CHECK(!fcap::is_valid_identifier(std::string("id-") + non_ascii));
}

FT_TEST(identity, is_valid_identifier_rejects_relative_path_components) {
  FT_CHECK(!fcap::is_valid_identifier("."));
  FT_CHECK(!fcap::is_valid_identifier(".."));
  FT_CHECK(!fcap::is_valid_identifier("./a"));
  FT_CHECK(!fcap::is_valid_identifier("../a"));
}

FT_TEST(identity, rejection_reason_is_non_empty_exactly_when_rejected) {
  const std::vector<std::string> values = {"dc-1",
                                           "a",
                                           "A.b_c:d-9",
                                           std::string(63, 'a'),
                                           "",
                                           std::string(64, 'a'),
                                           ".abc",
                                           "-abc",
                                           "_abc",
                                           ":abc",
                                           ".",
                                           "..",
                                           "ab cd",
                                           "ab/cd",
                                           "ab\\cd",
                                           "\xC3\xA9",
                                           "ab=cd"};

  for (const std::string& value : values) {
    const bool accepted = fcap::is_valid_identifier(value);
    const std::string_view reason = fcap::identifier_rejection_reason(value);
    FT_CHECK_EQ(reason.empty(), accepted);
    if (accepted) {
      FT_CHECK(reason.empty());
    } else {
      FT_CHECK(!reason.empty());
    }
  }
}

FT_TEST(identity, every_family_parses_a_valid_identifier) {
  check_parses<fcap::FacilityId>("facility-a");
  check_parses<fcap::SiteId>("site-1");
  check_parses<fcap::CapacitySourceId>("power-capacity");
  check_parses<fcap::ReserveId>("protection-1");
  check_parses<fcap::ConstraintId>("floor-1");
  check_parses<fcap::SnapshotId>("gen-00000000000000000007");
  check_parses<fcap::StoreId>("store-1");
  check_parses<fcap::ControllerId>("controller-1");
  check_parses<fcap::ServiceClassId>("tier-1");
  check_parses<fcap::OwnerId>("facility-ops");
}

FT_TEST(identity, every_family_rejects_an_invalid_identifier) {
  check_rejects<fcap::FacilityId>("");
  check_rejects<fcap::SiteId>("site 1");
  check_rejects<fcap::CapacitySourceId>(std::string(64, 'a'));
  check_rejects<fcap::ReserveId>("/etc/passwd");
  check_rejects<fcap::ConstraintId>("..");
  check_rejects<fcap::SnapshotId>("\xC3\xA9");
  check_rejects<fcap::StoreId>("store\\1");
  check_rejects<fcap::ControllerId>("-controller");
  check_rejects<fcap::ServiceClassId>("tier=1");
  check_rejects<fcap::OwnerId>(".hidden");
}

FT_TEST(identity, default_constructed_ids_are_empty) {
  check_default_is_empty<fcap::FacilityId>();
  check_default_is_empty<fcap::SiteId>();
  check_default_is_empty<fcap::CapacitySourceId>();
  check_default_is_empty<fcap::ReserveId>();
  check_default_is_empty<fcap::ConstraintId>();
  check_default_is_empty<fcap::SnapshotId>();
  check_default_is_empty<fcap::StoreId>();
  check_default_is_empty<fcap::ControllerId>();
  check_default_is_empty<fcap::ServiceClassId>();
  check_default_is_empty<fcap::OwnerId>();
}

FT_TEST(identity, every_family_orders_by_value) {
  check_ordering<fcap::FacilityId>();
  check_ordering<fcap::SiteId>();
  check_ordering<fcap::CapacitySourceId>();
  check_ordering<fcap::ReserveId>();
  check_ordering<fcap::ConstraintId>();
  check_ordering<fcap::SnapshotId>();
  check_ordering<fcap::StoreId>();
  check_ordering<fcap::ControllerId>();
  check_ordering<fcap::ServiceClassId>();
  check_ordering<fcap::OwnerId>();
}

FT_TEST(identity, support_helpers_agree_with_direct_parsing) {
  const fcap::FacilityId facility = fsup::facility_id("facility-a");
  const fcap::SiteId site = fsup::site_id("site-1");
  const fcap::StoreId store = fsup::store_id("store-1");
  const fcap::ControllerId controller = fsup::controller_id("controller-1");

  FT_CHECK_EQ(facility.value(), "facility-a");
  FT_CHECK_EQ(site.value(), "site-1");
  FT_CHECK_EQ(store.value(), "store-1");
  FT_CHECK_EQ(controller.value(), "controller-1");

  FT_CHECK(facility == fcap::FacilityId::parse("facility-a").value());
  FT_CHECK(site == fcap::SiteId::parse("site-1").value());
  FT_CHECK(store == fcap::StoreId::parse("store-1").value());
  FT_CHECK(controller == fcap::ControllerId::parse("controller-1").value());
}

FT_TEST(identity, rejected_identifiers_carry_the_rejection_reason_as_the_constraint) {
  const auto parsed = fcap::CapacitySourceId::parse(std::string(64, 'a'));
  FT_CHECK_ERROR(parsed, ErrorCode::invalid_argument);
  FT_REQUIRE(!parsed.has_value());
  FT_CHECK_EQ(parsed.error().constraint(), "identifier is longer than max_identifier_length");
  FT_CHECK_EQ(parsed.error().message(), "invalid identifier");
}

// ---------------------------------------------------------------------------
// generation
// ---------------------------------------------------------------------------

FT_TEST(generation, every_counter_family_parses_and_renders) {
  check_counter_family<fcap::CapacityGeneration>();
  check_counter_family<fcap::EvidenceGeneration>();
  check_counter_family<fcap::Revision>();
  check_counter_family<fcap::EpochId>();
  check_counter_family<fcap::IncarnationId>();
  check_counter_family<fcap::AttemptId>();
  check_counter_family<fcap::Tick>();
}

FT_TEST(generation, every_counter_family_rejects_malformed_text) {
  check_counter_rejections<fcap::CapacityGeneration>();
  check_counter_rejections<fcap::EvidenceGeneration>();
  check_counter_rejections<fcap::Revision>();
  check_counter_rejections<fcap::EpochId>();
  check_counter_rejections<fcap::IncarnationId>();
  check_counter_rejections<fcap::AttemptId>();
  check_counter_rejections<fcap::Tick>();
}

FT_TEST(generation, next_at_the_top_of_the_range_fails) {
  const auto top = EpochId::parse("18446744073709551615");
  FT_REQUIRE_OK(top);

  const auto next = top.value().next();
  FT_CHECK_ERROR(next, ErrorCode::limit_exceeded);
  FT_REQUIRE(!next.has_value());
  FT_CHECK(next.error().has_generations());
  FT_CHECK_EQ(next.error().expected_generation(), UINT64_MAX);
  FT_CHECK_EQ(next.error().actual_generation(), UINT64_MAX);
}

FT_TEST(generation, next_below_the_top_advances_by_one) {
  const auto value = EpochId::from_value(UINT64_MAX - 1u);
  const auto next = value.next();
  FT_REQUIRE_OK(next);
  FT_CHECK_EQ(next.value().value(), UINT64_MAX);
}

FT_TEST(generation, unchecked_next_advances_below_the_top) {
  const auto value = IncarnationId::from_value(5);
  const auto next = value.unchecked_next();
  FT_CHECK_EQ(next.value(), std::uint64_t{6});
  FT_CHECK(value < next);
  FT_CHECK_EQ(value.value(), std::uint64_t{5});
}

FT_TEST(generation, counters_compare_and_report_zero) {
  const auto zero = CapacityGeneration::from_value(0);
  const auto three = CapacityGeneration::from_value(3);
  const auto four = CapacityGeneration::from_value(4);

  FT_CHECK(zero.is_zero());
  FT_CHECK(!three.is_zero());
  FT_CHECK(three < four);
  FT_CHECK(four > three);
  FT_CHECK(three <= three);
  FT_CHECK(three >= three);
  FT_CHECK(three == CapacityGeneration::from_value(3));
  FT_CHECK(three != four);
  FT_CHECK(zero < three);
}

FT_TEST(generation, max_tick_is_the_top_of_the_range) {
  static_assert(fcap::max_tick.value() == UINT64_MAX);
  static_assert(fcap::max_tick == Tick::from_value(UINT64_MAX));

  const auto parsed = Tick::parse("18446744073709551615");
  FT_REQUIRE_OK(parsed);
  FT_CHECK_EQ(parsed.value(), fcap::max_tick);
  FT_CHECK(!fcap::max_tick.is_zero());
  FT_CHECK(Tick::from_value(UINT64_MAX - 1u) < fcap::max_tick);
}

// ---------------------------------------------------------------------------
// provenance
// ---------------------------------------------------------------------------

FT_TEST(provenance, validate_accepts_a_well_formed_record) {
  const Provenance value = fsup::provenance("power-capacity", 7, 3, 100);
  FT_REQUIRE_OK(value.validate());
  FT_CHECK_EQ(value.source.value(), "power-capacity");
  FT_CHECK_EQ(value.revision, "test/1.0.0");
  FT_CHECK_EQ(value.epoch.value(), std::uint64_t{7});
  FT_CHECK_EQ(value.incarnation.value(), std::uint64_t{3});
  FT_CHECK_EQ(value.produced_at.value(), std::uint64_t{100});
}

FT_TEST(provenance, validate_rejects_an_empty_source) {
  Provenance value;
  FT_CHECK_ERROR(value.validate(), ErrorCode::invalid_argument);
  FT_REQUIRE(!value.validate().has_value());
  FT_CHECK_EQ(value.validate().error().message(), "provenance has no source identity");
}

FT_TEST(provenance, validate_rejects_an_over_long_revision) {
  Provenance value = fsup::provenance("power-capacity", 7, 3, 100);
  value.revision = std::string(fcap::limits::max_revision_length + 1u, 'a');
  const auto status = value.validate();
  FT_CHECK_ERROR(status, ErrorCode::limit_exceeded);
  FT_REQUIRE(!status.has_value());
  FT_CHECK_EQ(status.error().constraint(), "revision_length <= max_revision_length");
}

FT_TEST(provenance, validate_accepts_a_revision_exactly_at_the_length_bound) {
  Provenance value = fsup::provenance("power-capacity", 7, 3, 100);
  value.revision = std::string(fcap::limits::max_revision_length, 'a');
  FT_CHECK_OK(value.validate());
}

FT_TEST(provenance, validate_rejects_a_revision_containing_equals) {
  Provenance value = fsup::provenance("power-capacity", 7, 3, 100);
  value.revision = "power=1.0.0";
  FT_CHECK_ERROR(value.validate(), ErrorCode::invalid_argument);
}

FT_TEST(provenance, validate_rejects_a_non_printable_revision) {
  Provenance value = fsup::provenance("power-capacity", 7, 3, 100);

  value.revision = "power\t1";
  FT_CHECK_ERROR(value.validate(), ErrorCode::invalid_argument);

  value.revision = "power\n1";
  FT_CHECK_ERROR(value.validate(), ErrorCode::invalid_argument);

  value.revision = std::string("power") + '\0';
  FT_CHECK_ERROR(value.validate(), ErrorCode::invalid_argument);

  value.revision = std::string("power") + static_cast<char>(0x7F);
  FT_CHECK_ERROR(value.validate(), ErrorCode::invalid_argument);
}

FT_TEST(provenance, validate_accepts_an_empty_revision) {
  Provenance value = fsup::provenance("power-capacity", 7, 3, 100);
  value.revision.clear();
  FT_CHECK_OK(value.validate());
}

FT_TEST(provenance, validate_rejects_a_malformed_evidence_digest) {
  Provenance value = fsup::provenance("power-capacity", 7, 3, 100);

  value.evidence_digest = std::string(63, 'a');
  FT_CHECK_ERROR(value.validate(), ErrorCode::invalid_argument);

  value.evidence_digest = std::string(65, 'a');
  FT_CHECK_ERROR(value.validate(), ErrorCode::invalid_argument);

  value.evidence_digest = std::string(64, 'A');
  FT_CHECK_ERROR(value.validate(), ErrorCode::invalid_argument);

  value.evidence_digest = std::string(64, 'z');
  FT_CHECK_ERROR(value.validate(), ErrorCode::invalid_argument);

  value.evidence_digest = std::string(64, ' ');
  FT_CHECK_ERROR(value.validate(), ErrorCode::invalid_argument);

  value.evidence_digest = std::string(63, 'a') + "-";
  FT_CHECK_ERROR(value.validate(), ErrorCode::invalid_argument);
}

FT_TEST(provenance, validate_accepts_a_well_formed_evidence_digest) {
  Provenance value = fsup::provenance("power-capacity", 7, 3, 100);
  value.evidence_digest = lowercase_digest();
  FT_CHECK_OK(value.validate());
}

FT_TEST(provenance, validate_accepts_an_empty_evidence_digest) {
  Provenance value = fsup::provenance("power-capacity", 7, 3, 100);
  FT_CHECK(value.evidence_digest.empty());
  FT_CHECK_OK(value.validate());
}

FT_TEST(provenance, to_string_renders_every_field) {
  const Provenance value = fsup::provenance("power-capacity", 7, 3, 100, "power-capacity/1.0.0");
  FT_CHECK_EQ(value.to_string(), "power-capacity@power-capacity/1.0.0 epoch=7 incarnation=3 produced_at=100");
  FT_CHECK_EQ(ftest::render(value), value.to_string());

  Provenance without_revision = fsup::provenance("power-capacity", 7, 3, 100);
  without_revision.revision.clear();
  FT_CHECK_EQ(without_revision.to_string(), "power-capacity@- epoch=7 incarnation=3 produced_at=100");
}

FT_TEST(provenance, equality_covers_every_field) {
  const Provenance base = fsup::provenance("power-capacity", 7, 3, 100);
  FT_CHECK(base == fsup::provenance("power-capacity", 7, 3, 100));
  FT_CHECK(base != fsup::provenance("cooling-capacity", 7, 3, 100));
  FT_CHECK(base != fsup::provenance("power-capacity", 8, 3, 100));
  FT_CHECK(base != fsup::provenance("power-capacity", 7, 4, 100));
  FT_CHECK(base != fsup::provenance("power-capacity", 7, 3, 101));

  Provenance with_digest = base;
  with_digest.evidence_digest = lowercase_digest();
  FT_CHECK(base != with_digest);
}

FT_TEST(provenance, validity_window_create_rejects_an_empty_window) {
  const auto window = ValidityWindow::create(Tick::from_value(5), Tick::from_value(5));
  FT_CHECK_ERROR(window, ErrorCode::invalid_argument);
  FT_REQUIRE(!window.has_value());
  FT_CHECK_EQ(window.error().constraint(), "start < end");
}

FT_TEST(provenance, validity_window_create_rejects_an_inverted_window) {
  const auto window = ValidityWindow::create(Tick::from_value(6), Tick::from_value(5));
  FT_CHECK_ERROR(window, ErrorCode::invalid_argument);
  FT_REQUIRE(!window.has_value());
  FT_CHECK_EQ(window.error().constraint(), "start < end");
}

FT_TEST(provenance, validity_window_active_at_is_half_open) {
  const auto window = ValidityWindow::create(Tick::from_value(10), Tick::from_value(20));
  FT_REQUIRE_OK(window);

  FT_CHECK(!window.value().active_at(Tick::from_value(9)));
  FT_CHECK(window.value().active_at(Tick::from_value(10)));
  FT_CHECK(window.value().active_at(Tick::from_value(11)));
  FT_CHECK(window.value().active_at(Tick::from_value(19)));
  FT_CHECK(!window.value().active_at(Tick::from_value(20)));
  FT_CHECK(!window.value().active_at(Tick::from_value(21)));

  FT_CHECK_EQ(window.value().start(), Tick::from_value(10));
  FT_CHECK_EQ(window.value().end(), Tick::from_value(20));
  FT_CHECK(!window.value().unbounded());
}

FT_TEST(provenance, validity_window_default_spans_the_whole_timeline) {
  const ValidityWindow window;
  FT_CHECK_EQ(window.start(), Tick::from_value(0));
  FT_CHECK_EQ(window.end(), fcap::max_tick);
  FT_CHECK(window.unbounded());
  FT_CHECK(window.active_at(Tick::from_value(0)));
  FT_CHECK(window.active_at(Tick::from_value(1)));
  FT_CHECK(window.active_at(Tick::from_value(UINT64_MAX - 1u)));
  FT_CHECK(!window.active_at(fcap::max_tick));
}

FT_TEST(provenance, validity_window_unbounded_from_is_unbounded) {
  const ValidityWindow window = ValidityWindow::unbounded_from(Tick::from_value(5));
  FT_CHECK_EQ(window.start(), Tick::from_value(5));
  FT_CHECK_EQ(window.end(), fcap::max_tick);
  FT_CHECK(window.unbounded());
  FT_CHECK(!window.active_at(Tick::from_value(4)));
  FT_CHECK(window.active_at(Tick::from_value(5)));
  FT_CHECK(window.active_at(Tick::from_value(UINT64_MAX - 1u)));
  FT_CHECK(!window.active_at(fcap::max_tick));
}

FT_TEST(provenance, validity_window_to_string_and_equality) {
  const auto window = ValidityWindow::create(Tick::from_value(10), Tick::from_value(20));
  FT_REQUIRE_OK(window);
  FT_CHECK_EQ(window.value().to_string(), "[10,20)");
  FT_CHECK_EQ(ftest::render(window.value()), "[10,20)");

  const auto same = ValidityWindow::create(Tick::from_value(10), Tick::from_value(20));
  FT_REQUIRE_OK(same);
  FT_CHECK(window.value() == same.value());

  const auto other = ValidityWindow::create(Tick::from_value(10), Tick::from_value(21));
  FT_REQUIRE_OK(other);
  FT_CHECK(window.value() != other.value());
  FT_CHECK(window.value() != ValidityWindow::unbounded_from(Tick::from_value(10)));
}

FT_TEST(provenance, is_hex_digest_accepts_exactly_64_lowercase_hex_characters) {
  FT_CHECK(fcap::is_hex_digest("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
  FT_CHECK(fcap::is_hex_digest(lowercase_digest()));
  FT_CHECK(fcap::is_hex_digest(std::string(64, '0')));
  FT_CHECK(fcap::is_hex_digest(std::string(64, 'f')));

  FT_CHECK(!fcap::is_hex_digest(""));
  FT_CHECK(!fcap::is_hex_digest(std::string(63, 'a')));
  FT_CHECK(!fcap::is_hex_digest(std::string(65, 'a')));
  FT_CHECK(!fcap::is_hex_digest(std::string(64, 'A')));
  FT_CHECK(!fcap::is_hex_digest(std::string(64, 'F')));
  FT_CHECK(!fcap::is_hex_digest(std::string(64, 'g')));
  FT_CHECK(!fcap::is_hex_digest(std::string(64, ' ')));
  FT_CHECK(!fcap::is_hex_digest(std::string(64, '0').replace(0, 1, "-")));
}
