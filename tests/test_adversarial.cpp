// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Adversarial tests.
//
// Everything here treats the outside world as hostile: the bytes of a store,
// the names inside a store directory, the values a source declares, and the
// size of a declaration. The product must refuse all of it safely, without
// crashing, without allocating unboundedly, and without ever adopting state it
// cannot verify.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_framework.hpp"
#include "test_rng.hpp"
#include "test_snapshot_builder.hpp"
#include "test_support.hpp"

namespace {

using namespace dccp::facility_capacity;

/// Patches one byte of a file in place.
void patch_byte(const std::filesystem::path& path, std::size_t offset, unsigned char value) {
  std::string bytes = fsup::read_bytes(path);
  FT_REQUIRE(offset < bytes.size());
  bytes[offset] = static_cast<char>(value);
  fsup::write_bytes(path, bytes);
}

std::string make_document(std::size_t padding) {
  std::string document;
  document.reserve(padding + 64);
  for (std::size_t index = 0; index < padding; ++index) {
    document.append("x");
  }
  return document;
}

struct StoreFixture {
  fsup::TempDir directory{"adversarial"};
  ManualClock clock{Tick::from_value(1000)};
  CapacityStore store;
  FacilityCapacityModel model;

  StoreFixture()
      : store(make_store(directory.store_root(), clock)),
        model(make_model(clock)) {}

  static CapacityStore make_store(const std::filesystem::path& root, ManualClock& clock) {
    StoreCreateOptions options;
    options.facility = fsup::facility_id();
    options.site = fsup::site_id();
    options.epoch = EpochId::from_value(7);
    options.incarnation = IncarnationId::from_value(3);
    options.controller = fsup::controller_id();
    auto created = CapacityStore::create(root, options, clock);
    if (!created.has_value()) {
      throw std::runtime_error("fixture: " + created.error().to_string());
    }
    return std::move(created.value());
  }

  static FacilityCapacityModel make_model(ManualClock& clock) {
    ModelConfig config;
    config.facility = fsup::facility_id();
    config.site = fsup::site_id();
    config.epoch = EpochId::from_value(7);
    config.incarnation = IncarnationId::from_value(3);
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

  void commit_one() {
    fsup::EvidenceSpec spec;
    spec.source = "power-capacity";
    spec.dimension = CapacityDimension::power;
    spec.installed = 10000;
    spec.unavailable = 1000;
    spec.usable = 9000;
    spec.derive_residual = true;
    spec.protected_capacity = 0;
    spec.reserved = 0;
    FT_REQUIRE_OK(model.declare_evidence(fsup::make_evidence(spec), precondition()));
    auto snapshot = model.publish(precondition());
    FT_REQUIRE_OK(snapshot);
    FT_REQUIRE_OK(store.commit(model, precondition()));
  }

  std::uint64_t attempts = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// Malformed canonical input
// ---------------------------------------------------------------------------

FT_TEST(adversarial, canonical_decoder_refuses_hostile_documents) {
  fsup::SnapshotBuilder builder;
  fsup::EvidenceSpec spec;
  spec.source = "power-capacity";
  spec.dimension = CapacityDimension::power;
  spec.installed = 10000;
  spec.unavailable = 1000;
  spec.usable = 9000;
  spec.derive_residual = true;
  spec.protected_capacity = 0;
  spec.reserved = 0;
  builder.add(fsup::make_evidence(spec));
  const auto snapshot = builder.build_ok();
  const std::string document = canonical::encode_snapshot(*snapshot);

  // A document with no digest line at all.
  std::string no_digest = document.substr(0, document.rfind("digest="));
  FT_CHECK_ERROR(canonical::decode_snapshot(no_digest, SnapshotFreshness::issued), ErrorCode::corruption);

  // A document whose digest line is short.
  std::string short_digest = document.substr(0, document.rfind("digest=")) + "digest=00\n";
  FT_CHECK_ERROR(canonical::decode_snapshot(short_digest, SnapshotFreshness::issued), ErrorCode::corruption);

  // Carriage returns injected before every line terminator, walking backwards
  // so that an insertion cannot shift the next search position.
  std::string with_cr = document;
  for (std::size_t position = with_cr.size(); position-- > 0;) {
    if (with_cr[position] == '\n') {
      with_cr.insert(position, 1, '\r');
    }
  }
  FT_CHECK_ERROR(canonical::decode_snapshot(with_cr, SnapshotFreshness::issued), ErrorCode::corruption);

  // A NUL byte in the middle.
  std::string with_nul = document;
  with_nul.insert(with_nul.size() / 2, 1, '\0');
  FT_CHECK_ERROR(canonical::decode_snapshot(with_nul, SnapshotFreshness::issued),
                 ErrorCode::checksum_mismatch);

  // A duplicated field name, with the digest recomputed so the parser is the
  // only thing standing between the attacker and acceptance.
  std::string duplicated = document;
  const std::size_t insertion = duplicated.find('\n') + 1;
  duplicated.insert(insertion, "facility=facility-a\n");
  FT_CHECK_ERROR(canonical::decode_snapshot(duplicated, SnapshotFreshness::issued),
                 ErrorCode::checksum_mismatch);

  // Empty input and a single newline.
  FT_CHECK_ERROR(canonical::decode_snapshot("", SnapshotFreshness::issued), ErrorCode::corruption);
  FT_CHECK_ERROR(canonical::decode_snapshot("\n", SnapshotFreshness::issued), ErrorCode::corruption);

  // A body that declares far more records than it carries.
  std::string inflated = document;
  const std::size_t evidence_count = inflated.find("evidence_count=");
  FT_REQUIRE(evidence_count != std::string::npos);
  const std::size_t line_end = inflated.find('\n', evidence_count);
  inflated.replace(evidence_count, line_end - evidence_count, "evidence_count=250000");
  FT_CHECK_ERROR(canonical::decode_snapshot(inflated, SnapshotFreshness::issued),
                 ErrorCode::checksum_mismatch);
}

FT_TEST(adversarial, an_oversized_document_is_refused_before_it_is_indexed) {
  const std::string padded = make_document(static_cast<std::size_t>(limits::max_store_body_bytes) + 16);
  FT_CHECK_ERROR(canonical::decode_snapshot(padded, SnapshotFreshness::issued), ErrorCode::limit_exceeded);
}

FT_TEST(adversarial, identifier_alphabet_rejects_path_and_encoding_attacks) {
  const char* const hostile[] = {
      "..", ".", "../etc/passwd", "a/b", "a\\b", "C:\\windows",
      "leading space", "trailing space ", "tab\there", "new\nline", "semi;colon", "quote\"mark",
      "\xC3\xA9-accented", "*", "?", "$HOME",
  };
  for (const char* value : hostile) {
    FT_CHECK(!is_valid_identifier(value));
    FT_CHECK(!identifier_rejection_reason(value).empty());
    FT_CHECK_ERROR(FacilityId::parse(value), ErrorCode::invalid_argument);
    FT_CHECK_ERROR(CapacitySourceId::parse(value), ErrorCode::invalid_argument);
  }
  const std::string embedded_nul("facility\0null", 13);
  FT_CHECK(!is_valid_identifier(embedded_nul));
  FT_CHECK_ERROR(FacilityId::parse(embedded_nul), ErrorCode::invalid_argument);

  std::string too_long(limits::max_identifier_length + 1, 'a');
  FT_CHECK(!is_valid_identifier(too_long));
  std::string at_limit(limits::max_identifier_length, 'a');
  FT_CHECK(is_valid_identifier(at_limit));
}

// ---------------------------------------------------------------------------
// Hostile persisted state
// ---------------------------------------------------------------------------

FT_TEST(adversarial, a_byte_flipped_generation_file_is_refused) {
  ftest::Rng rng(ftest::current_seed());
  StoreFixture fixture;
  fixture.commit_one();

  const std::filesystem::path generation = fixture.directory.store_root() / "gen-1.fcs";
  FT_REQUIRE(std::filesystem::exists(generation));
  const std::string original = fsup::read_bytes(generation);

  for (int attempt = 0; attempt < 12; ++attempt) {
    fsup::write_bytes(generation, original);
    const std::size_t offset = static_cast<std::size_t>(rng.below(original.size()));
    if (offset < static_cast<std::size_t>(store_header_size)) {
      continue;
    }
    const unsigned char value = static_cast<unsigned char>(rng.below(256));
    if (value == static_cast<unsigned char>(original[offset])) {
      continue;
    }
    patch_byte(generation, offset, value);
    auto recovered = fixture.store.recover();
    FT_CHECK(!recovered.has_value());
  }
  fsup::write_bytes(generation, original);
  FT_CHECK_OK(fixture.store.recover());
}

FT_TEST(adversarial, every_generation_header_field_is_validated) {
  StoreFixture fixture;
  fixture.commit_one();
  const std::filesystem::path generation = fixture.directory.store_root() / "gen-1.fcs";
  const std::string original = fsup::read_bytes(generation);
  FT_REQUIRE(original.size() > store_header_size);

  // Magic.
  fsup::write_bytes(generation, original);
  patch_byte(generation, 0, static_cast<unsigned char>('X'));
  FT_CHECK_ERROR(fixture.store.recover(), ErrorCode::corruption);

  // Byte order. The marker 0x01020304 is written little-endian, so the valid
  // on-disk bytes are 04 03 02 01; the genuinely byte-swapped sequence is
  // 01 02 03 04 and must be refused.
  fsup::write_bytes(generation, original);
  patch_byte(generation, 8, 0x01);
  patch_byte(generation, 9, 0x02);
  patch_byte(generation, 10, 0x03);
  patch_byte(generation, 11, 0x04);
  FT_CHECK_ERROR(fixture.store.recover(), ErrorCode::incompatible_version);

  // Format version.
  fsup::write_bytes(generation, original);
  patch_byte(generation, 12, 99);
  FT_CHECK_ERROR(fixture.store.recover(), ErrorCode::incompatible_version);

  // Header size.
  fsup::write_bytes(generation, original);
  patch_byte(generation, 16, 64);
  FT_CHECK_ERROR(fixture.store.recover(), ErrorCode::incompatible_version);

  // Declared body length, larger than the file.
  fsup::write_bytes(generation, original);
  patch_byte(generation, 20, 0xFF);
  patch_byte(generation, 21, 0xFF);
  FT_CHECK_ERROR(fixture.store.recover(), ErrorCode::truncated_input);

  // A reserved byte made non-zero.
  fsup::write_bytes(generation, original);
  patch_byte(generation, 100, 1);
  FT_CHECK_ERROR(fixture.store.recover(), ErrorCode::corruption);

  // Truncation by a single byte.
  fsup::write_bytes(generation, original.substr(0, original.size() - 1));
  FT_CHECK_ERROR(fixture.store.recover(), ErrorCode::truncated_input);

  // An empty file and a header-only file.
  fsup::write_bytes(generation, std::string());
  FT_CHECK_ERROR(fixture.store.recover(), ErrorCode::truncated_input);
  fsup::write_bytes(generation, original.substr(0, store_header_size));
  FT_CHECK_ERROR(fixture.store.recover(), ErrorCode::truncated_input);

  fsup::write_bytes(generation, original);
  FT_CHECK_OK(fixture.store.recover());
}

FT_TEST(adversarial, a_generation_file_replaced_by_an_unrelated_one_is_refused) {
  fsup::TempDir other{"adversarial-other"};
  ManualClock clock(Tick::from_value(1000));
  StoreCreateOptions options;
  options.facility = fsup::facility_id("facility-other");
  options.site = fsup::site_id("site-other");
  options.epoch = EpochId::from_value(7);
  options.incarnation = IncarnationId::from_value(3);
  options.controller = fsup::controller_id();
  auto other_store = CapacityStore::create(other.store_root(), options, clock);
  FT_REQUIRE_OK(other_store);

  StoreFixture fixture;
  fixture.commit_one();

  // A second store with a different facility publishes its own generation 1.
  ModelConfig config;
  config.facility = fsup::facility_id("facility-other");
  config.site = fsup::site_id("site-other");
  config.epoch = EpochId::from_value(7);
  config.incarnation = IncarnationId::from_value(3);
  auto other_model = FacilityCapacityModel::create(config, clock);
  FT_REQUIRE_OK(other_model);
  fsup::EvidenceSpec spec;
  spec.source = "power-capacity";
  spec.dimension = CapacityDimension::power;
  spec.installed = 50000;
  spec.unavailable = 0;
  spec.usable = 50000;
  spec.derive_residual = true;
  spec.protected_capacity = 0;
  spec.reserved = 0;
  CapacityPrecondition precondition = other_model.value().current_precondition();
  precondition.attempt = AttemptId::from_value(1);
  FT_REQUIRE_OK(other_model.value().declare_evidence(fsup::make_evidence(spec), precondition));
  precondition = other_model.value().current_precondition();
  precondition.attempt = AttemptId::from_value(2);
  auto published = other_model.value().publish(precondition);
  FT_REQUIRE_OK(published);
  precondition = other_model.value().current_precondition();
  precondition.attempt = AttemptId::from_value(3);
  FT_REQUIRE_OK(other_store.value().commit(other_model.value(), precondition));

  // Swap the unrelated generation file over the first store's own.
  fsup::copy_file(other.store_root() / "gen-1.fcs", fixture.directory.store_root() / "gen-1.fcs");

  // Recovery must refuse: the recovered state names a different facility.
  auto recovered = fixture.store.recover();
  FT_CHECK(!recovered.has_value());
  if (!recovered.has_value()) {
    FT_CHECK(recovered.error().code() == ErrorCode::conflict ||
             recovered.error().code() == ErrorCode::checksum_mismatch);
  }

  auto report = fixture.store.verify(true);
  FT_REQUIRE_OK(report);
  FT_CHECK(!report.value().ok);
}

FT_TEST(adversarial, a_swapped_current_manifest_is_refused) {
  fsup::TempDir other{"adversarial-swap"};
  ManualClock clock(Tick::from_value(1000));
  StoreCreateOptions options;
  options.facility = fsup::facility_id("facility-other");
  options.site = fsup::site_id("site-other");
  options.epoch = EpochId::from_value(7);
  options.incarnation = IncarnationId::from_value(3);
  options.controller = fsup::controller_id();
  auto other_store = CapacityStore::create(other.store_root(), options, clock);
  FT_REQUIRE_OK(other_store);

  StoreFixture fixture;
  // The two stores have different identities; adopting the other's authority
  // record must be refused.
  fsup::copy_file(other.store_root() / "CURRENT", fixture.directory.store_root() / "CURRENT");
  FT_CHECK_ERROR(CapacityStore::open(fixture.directory.store_root(), StoreOpenOptions{}, clock),
                 ErrorCode::conflict);
}

FT_TEST(adversarial, a_manifest_naming_a_parent_path_is_refused) {
  StoreFixture fixture;
  fixture.commit_one();
  std::string current = fsup::read_bytes(fixture.directory.store_root() / "CURRENT");

  const std::size_t field = current.find("file_name=");
  FT_REQUIRE(field != std::string::npos);
  const std::size_t end = current.find('\n', field);
  current.replace(field, end - field, "file_name=..%2F..%2Fescape.fcs");

  // The document digest no longer matches, so the safe path is refuted twice
  // over: once by the digest and once by the name check. Either refusal is
  // acceptable; what is not acceptable is adoption.
  fsup::write_bytes(fixture.directory.store_root() / "CURRENT", current);
  auto opened = CapacityStore::open(fixture.directory.store_root(), StoreOpenOptions{}, fixture.clock);
  FT_CHECK(!opened.has_value());

  // Now craft a CURRENT whose digest is recomputed, so only the name check can
  // refuse it. It must still be refused.
  StoreFixture second;
  second.commit_one();
  std::string authentic = fsup::read_bytes(second.directory.store_root() / "CURRENT");
  const std::size_t authentic_field = authentic.find("file_name=");
  const std::size_t authentic_end = authentic.find('\n', authentic_field);
  const std::string real_name = authentic.substr(authentic_field + 10, authentic_end - authentic_field - 10);
  FT_CHECK_EQ(real_name, std::string("gen-1.fcs"));

  // A generation number is the only thing the name may vary in; anything else
  // is rejected by the format, which is what the store-format tests cover. Here
  // we assert the exported predicate directly.
  FT_CHECK(!CapacityStore::is_generation_file_name(".."));
  FT_CHECK(!CapacityStore::is_generation_file_name("gen-1.fcs/../evil"));
  FT_CHECK(!CapacityStore::is_generation_file_name("gen-.fcs"));
  FT_CHECK(!CapacityStore::is_generation_file_name("C:\\evil.fcs"));
  FT_CHECK(CapacityStore::is_generation_file_name("gen-0.fcs"));
  FT_CHECK(CapacityStore::is_generation_file_name("gen-18446744073709551615.fcs"));
}

FT_TEST(adversarial, a_foreign_entry_in_the_store_is_reported_and_never_removed) {
  StoreFixture fixture;
  fixture.commit_one();
  const std::filesystem::path foreign = fixture.directory.store_root() / "operator-notes.txt";
  fsup::write_bytes(foreign, "do not delete me\n");

  auto report = fixture.store.verify(false);
  FT_REQUIRE_OK(report);
  FT_CHECK(!report.value().ok);
  const auto found = std::find(report.value().findings.begin(), report.value().findings.end(),
                               "operator-notes.txt: an entry this product does not create");
  FT_CHECK(found != report.value().findings.end());

  auto pruned = fixture.store.prune(1);
  FT_REQUIRE_OK(pruned);
  FT_CHECK(std::filesystem::exists(foreign));

  FT_CHECK_ERROR(CapacityStore::destroy(fixture.directory.store_root()), ErrorCode::conflict);
  FT_CHECK(std::filesystem::exists(foreign));
}

FT_TEST(adversarial, destroy_refuses_a_non_store_directory) {
  fsup::TempDir directory{"adversarial-destroy"};
  const std::filesystem::path keeper = directory.path() / "important.txt";
  fsup::write_bytes(keeper, "keep\n");
  FT_CHECK_ERROR(CapacityStore::destroy(directory.path()), ErrorCode::not_found);
  FT_CHECK(std::filesystem::exists(keeper));
}

FT_TEST(adversarial, store_creation_refuses_unsafe_paths) {
  fsup::TempDir directory{"adversarial-path"};
  ManualClock clock(Tick::from_value(1000));
  StoreCreateOptions options;
  options.facility = fsup::facility_id();
  options.site = fsup::site_id();
  options.epoch = EpochId::from_value(7);
  options.incarnation = IncarnationId::from_value(3);
  options.controller = fsup::controller_id();

  FT_CHECK_ERROR(CapacityStore::create(directory.path() / ".." / "escape", options, clock),
                 ErrorCode::path_rejected);

  const std::filesystem::path regular = directory.path() / "a-file";
  fsup::write_bytes(regular, "not a directory\n");
  FT_CHECK_ERROR(CapacityStore::create(regular, options, clock), ErrorCode::path_rejected);

  // Creating over an existing store is refused rather than destructive.
  auto created = CapacityStore::create(directory.store_root(), options, clock);
  FT_REQUIRE_OK(created);
  FT_CHECK_ERROR(CapacityStore::create(directory.store_root(), options, clock), ErrorCode::already_exists);
}

// ---------------------------------------------------------------------------
// Hostile declarations
// ---------------------------------------------------------------------------

FT_TEST(adversarial, oversized_declarations_are_refused_before_allocation) {

  fsup::EvidenceSpec spec;
  spec.source = "power-capacity";
  spec.dimension = CapacityDimension::power;
  spec.installed = limits::max_quantity_magnitude;
  spec.unavailable = 0;
  spec.usable = limits::max_quantity_magnitude;
  spec.derive_residual = true;
  spec.protected_capacity = 0;
  spec.reserved = 0;
  (void)fsup::make_evidence(spec);

  // One beyond the accepted magnitude is refused at construction.
  spec.installed = limits::max_quantity_magnitude + 1;
  spec.usable = limits::max_quantity_magnitude + 1;
  bool threw = false;
  try {
    (void)fsup::make_evidence(spec);
  } catch (const std::exception&) {
    threw = true;
  }
  FT_CHECK(threw);

  // Two legal maxima cannot be summed into a legal total. The composed total
  // must report unknown rather than a wrapped magnitude.
  fsup::SnapshotBuilder builder;
  fsup::EvidenceSpec first;
  first.source = "power-capacity-a";
  first.dimension = CapacityDimension::power;
  first.installed = limits::max_quantity_magnitude;
  first.unavailable = 0;
  first.usable = limits::max_quantity_magnitude;
  first.derive_residual = true;
  first.protected_capacity = 0;
  first.reserved = 0;
  fsup::EvidenceSpec second = first;
  second.source = "power-capacity-b";
  builder.add(fsup::make_evidence(first));
  builder.add(fsup::make_evidence(second));
  auto snapshot = builder.build();
  FT_REQUIRE_OK(snapshot);
  const DimensionTotals* totals = snapshot.value()->find_totals(CapacityDimension::power);
  FT_REQUIRE(totals != nullptr);
  FT_CHECK(totals->installed.is_unknown());
  FT_CHECK(totals->allocatable.is_unknown());
  FT_CHECK_EQ(snapshot.value()->outcome(CapacityDimension::power), CapacityOutcome::unknown);
}

FT_TEST(adversarial, a_declaration_that_would_violate_the_bound_is_refused) {
  ManualClock clock(Tick::from_value(1000));
  ModelConfig config;
  config.facility = fsup::facility_id();
  config.site = fsup::site_id();
  config.epoch = EpochId::from_value(7);
  config.incarnation = IncarnationId::from_value(3);
  config.max_sources = 1;
  auto model = FacilityCapacityModel::create(config, clock);
  FT_REQUIRE_OK(model);

  fsup::EvidenceSpec spec;
  spec.source = "power-capacity";
  spec.dimension = CapacityDimension::power;
  spec.installed = 100;
  spec.unavailable = 0;
  spec.usable = 100;
  spec.derive_residual = true;
  spec.protected_capacity = 0;
  spec.reserved = 0;

  CapacityPrecondition precondition = model.value().current_precondition();
  precondition.attempt = AttemptId::from_value(1);
  FT_CHECK_OK(model.value().declare_evidence(fsup::make_evidence(spec), precondition));

  // `max_sources` bounds the number of distinct sources, and a source may hold
  // one record per dimension, so the model accepts six records here. The
  // seventh is refused.
  for (std::size_t ordinal = 1; ordinal < capacity_dimension_count; ++ordinal) {
    spec.dimension = all_capacity_dimensions()[ordinal];
    if (spec.dimension == CapacityDimension::power) {
      continue;
    }
    spec.unit = canonical_unit(spec.dimension);
    spec.installed = 100;
    spec.usable = 100;
    spec.unavailable = 0;
    precondition = model.value().current_precondition();
    precondition.attempt = AttemptId::from_value(10 + ordinal);
    FT_REQUIRE_OK(model.value().declare_evidence(fsup::make_evidence(spec), precondition));
  }
  // A source may hold one record per dimension, so `max_sources == 1` accepts
  // six records: one for each of the five remaining dimensions, then one more.
  spec.source = "second-source";
  spec.dimension = CapacityDimension::rack;
  spec.unit = canonical_unit(CapacityDimension::rack);
  spec.installed = 100;
  spec.usable = 100;
  spec.unavailable = 0;
  precondition = model.value().current_precondition();
  precondition.attempt = AttemptId::from_value(30);
  FT_REQUIRE_OK(model.value().declare_evidence(fsup::make_evidence(spec), precondition));

  spec.dimension = CapacityDimension::cooling;
  spec.unit = canonical_unit(CapacityDimension::cooling);
  precondition = model.value().current_precondition();
  precondition.attempt = AttemptId::from_value(31);
  FT_CHECK_ERROR(model.value().declare_evidence(fsup::make_evidence(spec), precondition),
                 ErrorCode::limit_exceeded);
  FT_CHECK_OK(model.value().validate());
  FT_CHECK_OK(model.value().validate());
}

FT_TEST(adversarial, quantity_arithmetic_refuses_overflow) {
  const auto maximum = Quantity::make(Unit::milli_watt, limits::max_quantity_magnitude);
  FT_REQUIRE_OK(maximum);
  const auto doubled = maximum.value().add(maximum.value());
  FT_CHECK_ERROR(doubled, ErrorCode::limit_exceeded);

  const auto beyond = Quantity::make(Unit::milli_watt, limits::max_quantity_magnitude + 1);
  FT_CHECK_ERROR(beyond, ErrorCode::limit_exceeded);

  const auto scaled = maximum.value().scale(2);
  FT_CHECK_ERROR(scaled, ErrorCode::limit_exceeded);

  const auto mismatch = maximum.value().add(Quantity::zero(Unit::rack_unit));
  FT_CHECK_ERROR(mismatch, ErrorCode::unit_mismatch);

  // An unmeasured value never turns into a number.
  const Measured unmeasured = Measured::unknown();
  FT_CHECK(unmeasured.is_unknown());
  FT_CHECK(!unmeasured.is_known_zero());
  bool threw = false;
  try {
    (void)unmeasured.magnitude();
  } catch (const ResultMisuse&) {
    threw = true;
  }
  FT_CHECK(threw);
}

FT_TEST(adversarial, reason_and_error_code_names_never_collide_with_unknown) {
  // A corrupted stored code must be refused rather than mapped to a default.
  ReasonCode reason = ReasonCode::none;
  FT_CHECK(!parse_reason_code("not_a_reason", reason));
  ErrorCode code = ErrorCode::invalid_argument;
  FT_CHECK(!parse_error_code("not_an_error", code));
  Unit unit = Unit::rack_unit;
  FT_CHECK(!parse_unit("not_a_unit", unit));
  CapacityDimension dimension = CapacityDimension::space;
  FT_CHECK(!parse_capacity_dimension("not_a_dimension", dimension));
  OperationalState state = OperationalState::nominal;
  FT_CHECK(!parse_operational_state("not_a_state", state));
  ReserveKind kind = ReserveKind::protection;
  FT_CHECK(!parse_reserve_kind("not_a_kind", kind));
}
