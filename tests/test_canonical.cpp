// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The canonical encoding: determinism, round trip, tamper refusal and escaping.

#include <algorithm>
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

fsup::EvidenceSpec cooling(const std::string& source, std::int64_t installed, std::int64_t usable) {
  fsup::EvidenceSpec spec;
  spec.source = source;
  spec.dimension = CapacityDimension::cooling;
  spec.unit = canonical_unit(CapacityDimension::cooling);
  spec.installed = installed;
  spec.usable = usable;
  spec.unavailable = 0;
  spec.derive_residual = true;
  spec.protected_capacity = 0;
  spec.reserved = 0;
  return spec;
}

SnapshotBuilder populated() {
  SnapshotBuilder builder(3);
  builder.at(1000);
  builder.require(CapacityDimension::power, true, {"power-a", "power-b"});
  builder.add(fsup::make_evidence(power("power-a", 10000, 9000, 400)));
  builder.add(fsup::make_evidence(power("power-b", 5000, 4000, 250)));
  builder.add(fsup::make_evidence(cooling("cooling-a", 8000, 7000)));

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
  return builder;
}

}  // namespace

FT_TEST(canonical, encoding_is_independent_of_input_order) {
  const std::shared_ptr<const CapacitySnapshot> reference = populated().build_ok();
  const std::string document = canonical::encode_snapshot(*reference);

  SnapshotBuilder shuffled(3);
  shuffled.at(1000);
  shuffled.require(CapacityDimension::power, true, {"power-b", "power-a"});
  shuffled.add(fsup::make_evidence(cooling("cooling-a", 8000, 7000)));
  shuffled.add(fsup::make_evidence(power("power-b", 5000, 4000, 250)));
  shuffled.add(fsup::make_evidence(power("power-a", 10000, 9000, 400)));
  fsup::ConstraintSpec constraint;
  constraint.id = "floor-1";
  constraint.dimension = CapacityDimension::power;
  constraint.floor_value = 100;
  shuffled.add(fsup::make_constraint(constraint));
  fsup::ReserveSpec reserve;
  reserve.id = "protection-1";
  reserve.source = "power-a";
  reserve.dimension = CapacityDimension::power;
  reserve.kind = ReserveKind::protection;
  reserve.amount = 0;
  shuffled.add(fsup::make_reserve(reserve));

  const std::shared_ptr<const CapacitySnapshot> other = shuffled.build_ok();
  FT_CHECK_EQ(canonical::encode_snapshot(*other), document);
  FT_CHECK_EQ(other->digest(), reference->digest());
  FT_CHECK_EQ(other->id().value(), reference->id().value());
}

FT_TEST(canonical, round_trip_preserves_the_whole_answer) {
  const std::shared_ptr<const CapacitySnapshot> original = populated().build_ok();
  const std::string document = canonical::encode_snapshot(*original);

  auto decoded = canonical::decode_snapshot(document, SnapshotFreshness::issued);
  FT_REQUIRE_OK(decoded);
  const CapacitySnapshot& restored = *decoded.value();
  FT_CHECK_EQ(restored.digest(), original->digest());
  FT_CHECK_EQ(restored.id().value(), original->id().value());
  FT_CHECK_EQ(restored.generation(), original->generation());
  FT_CHECK_EQ(restored.revision(), original->revision());
  FT_CHECK_EQ(restored.freshness(), SnapshotFreshness::issued);
  FT_CHECK_EQ(restored.dimension_totals().size(), original->dimension_totals().size());
  FT_CHECK_EQ(restored.sources().size(), original->sources().size());
  FT_CHECK_EQ(restored.reserves().size(), original->reserves().size());
  FT_CHECK_EQ(restored.constraints().size(), original->constraints().size());
  FT_CHECK_EQ(restored.completeness().classification, original->completeness().classification);
  for (std::size_t ordinal = 0; ordinal < capacity_dimension_count; ++ordinal) {
    const CapacityDimension dimension = all_capacity_dimensions()[ordinal];
    FT_CHECK_EQ(restored.outcome(dimension), original->outcome(dimension));
    FT_CHECK_EQ(restored.allocatable(dimension), original->allocatable(dimension));
  }
  FT_CHECK_EQ(restored.completeness().required_dimensions, original->completeness().required_dimensions);
}

FT_TEST(canonical, recovered_freshness_is_a_stamp_not_a_content_change) {
  const std::shared_ptr<const CapacitySnapshot> original = populated().build_ok();
  const std::string document = canonical::encode_snapshot(*original);

  auto recovered = canonical::decode_snapshot(document, SnapshotFreshness::recovered);
  FT_REQUIRE_OK(recovered);
  FT_CHECK_EQ(recovered.value()->freshness(), SnapshotFreshness::recovered);
  FT_CHECK_EQ(recovered.value()->digest(), original->digest());
  FT_CHECK_EQ(canonical::encode_snapshot(*recovered.value()), document);
}

FT_TEST(canonical, a_recovered_snapshot_carries_no_freshness_note) {
  // The recovery finding belongs to the recovery result, not to the answer's
  // content: if it were part of the content the digest would move and a store
  // could never verify that what it read is what it wrote.
  const std::shared_ptr<const CapacitySnapshot> original = populated().build_ok();
  FT_CHECK(!original->notes().contains(ReasonCode::recovered_not_revalidated));
  auto recovered =
      canonical::decode_snapshot(canonical::encode_snapshot(*original), SnapshotFreshness::recovered);
  FT_REQUIRE_OK(recovered);
  FT_CHECK(!recovered.value()->notes().contains(ReasonCode::recovered_not_revalidated));
}

FT_TEST(canonical, tampering_is_refused) {
  const std::shared_ptr<const CapacitySnapshot> original = populated().build_ok();
  const std::string document = canonical::encode_snapshot(*original);
  FT_REQUIRE(document.size() > 200);

  const auto refused = [&document](const std::string& candidate, const char* what) {
    auto decoded = canonical::decode_snapshot(candidate, SnapshotFreshness::issued);
    if (decoded.has_value()) {
      FT_FAIL(std::string("a tampered document was accepted: ") + what);
    }
    (void)document;
  };

  std::string flipped = document;
  flipped[flipped.size() / 2] = static_cast<char>(flipped[flipped.size() / 2] ^ 0x20);
  refused(flipped, "flipped byte");

  refused(document.substr(0, document.size() - 40), "truncation");
  refused(document + "extra=1\n", "appended field");
  refused(document.substr(0, document.size() - 1), "missing final newline");
  refused("", "empty");
  refused("\n", "lone newline");

  std::string replaced = document;
  const std::size_t digest_at = replaced.rfind("digest=");
  FT_REQUIRE(digest_at != std::string::npos);
  replaced.replace(digest_at + 7, 64, std::string(64, 'f'));
  refused(replaced, "replaced digest");

  std::string wrong_kind = document;
  const std::size_t kind_at = wrong_kind.find("document=snapshot");
  FT_REQUIRE(kind_at != std::string::npos);
  wrong_kind.replace(kind_at, 17, "document=snapshut");
  refused(wrong_kind, "wrong document kind");

  std::string wrong_format = document;
  const std::size_t format_at = wrong_format.find("canonical_format=1");
  FT_REQUIRE(format_at != std::string::npos);
  wrong_format.replace(format_at, 18, "canonical_format=9");
  refused(wrong_format, "wrong canonical format");

  // A duplicated field name, with the digest recomputed so that only the
  // reader's duplicate check stands between it and acceptance.
  std::string duplicated = document.substr(0, document.rfind("digest="));
  duplicated.append("facility=facility-a\n");
  duplicated.append("digest=").append(digest::sha256_hex(duplicated));
  duplicated.push_back('\n');
  refused(duplicated, "duplicated field");
}

FT_TEST(canonical, non_ascii_detail_text_round_trips_through_escaping) {
  SnapshotBuilder builder(4);
  fsup::EvidenceSpec spec = power("power-a", 10000, 9000, 400);
  spec.state = OperationalState::degraded;
  spec.state_reason = ReasonCode::source_degraded;
  spec.state_detail = std::string("key = value\\end");
  builder.add(fsup::make_evidence(spec));

  auto snapshot = builder.build();
  FT_REQUIRE_OK(snapshot);
  const std::string document = canonical::encode_snapshot(*snapshot.value());
  for (const char character : document) {
    FT_CHECK(static_cast<unsigned char>(character) < 0x80u);
  }
  FT_CHECK(document.find('=') != std::string::npos);
  FT_CHECK(document.find("key \\x3d value\\x5cend") != std::string::npos);

  auto decoded = canonical::decode_snapshot(document, SnapshotFreshness::issued);
  FT_REQUIRE_OK(decoded);
  FT_CHECK_EQ(decoded.value()->digest(), snapshot.value()->digest());

  // Detail text is restricted to printable ASCII at creation, so the escaping
  // above is the only thing a value can ever need: a control byte or a byte
  // above 0x7E never reaches the encoder.
  fsup::EvidenceSpec control = power("power-a", 10000, 9000, 400);
  control.state = OperationalState::degraded;
  control.state_detail = std::string("tab\there");
  bool refused_control = false;
  try {
    (void)fsup::make_evidence(control);
  } catch (const std::exception&) {
    refused_control = true;
  }
  FT_CHECK(refused_control);

  fsup::EvidenceSpec non_ascii = power("power-a", 10000, 9000, 400);
  non_ascii.state = OperationalState::degraded;
  non_ascii.state_detail = std::string("\xC3\xA9");
  bool refused_non_ascii = false;
  try {
    (void)fsup::make_evidence(non_ascii);
  } catch (const std::exception&) {
    refused_non_ascii = true;
  }
  FT_CHECK(refused_non_ascii);
}

FT_TEST(canonical, an_oversized_document_is_refused_before_it_is_parsed) {
  std::string huge;
  const std::string line = "filler=0123456789012345678901234567890123456789\n";
  huge.reserve(static_cast<std::size_t>(limits::max_store_body_bytes) + line.size() * 2);
  while (huge.size() <= limits::max_store_body_bytes) {
    huge.append(line);
  }
  FT_CHECK_ERROR(canonical::decode_snapshot(huge, SnapshotFreshness::issued), ErrorCode::limit_exceeded);
}

FT_TEST(canonical, generation_bodies_are_framed_strictly) {
  const std::string state = "document=model-state\ndigest=" + std::string(64, 'a') + "\n";
  const std::string snapshot = "document=snapshot\ndigest=" + std::string(64, 'b') + "\n";
  const std::string body = canonical::join_generation_body(state, snapshot);

  std::string_view parsed_state;
  std::string_view parsed_snapshot;
  FT_CHECK_OK(canonical::split_generation_body(body, parsed_state, parsed_snapshot));
  FT_CHECK_EQ(std::string(parsed_state), state);
  FT_CHECK_EQ(std::string(parsed_snapshot), snapshot);

  std::string_view ignored_state;
  std::string_view ignored_snapshot;
  FT_CHECK_ERROR(canonical::split_generation_body("", ignored_state, ignored_snapshot), ErrorCode::corruption);
  FT_CHECK_ERROR(canonical::split_generation_body("not-a-frame\n", ignored_state, ignored_snapshot),
                 ErrorCode::corruption);
  FT_CHECK_ERROR(canonical::split_generation_body("@state\n" + state, ignored_state, ignored_snapshot),
                 ErrorCode::corruption);
  FT_CHECK_ERROR(canonical::split_generation_body("@snapshot\n" + snapshot, ignored_state, ignored_snapshot),
                 ErrorCode::corruption);
  FT_CHECK_ERROR(canonical::split_generation_body(body + "@snapshot\n" + snapshot, ignored_state, ignored_snapshot),
                 ErrorCode::corruption);
}

FT_TEST(canonical, randomized_snapshots_encode_and_decode_reproducibly) {
  ftest::Rng rng(ftest::current_seed());
  for (int iteration = 0; iteration < 25; ++iteration) {
    SnapshotBuilder builder(static_cast<std::uint64_t>(iteration) + 1);
    builder.at(1000);
    const std::size_t count = static_cast<std::size_t>(rng.range(1, 3));
    for (std::size_t index = 0; index < count; ++index) {
      const CapacityDimension dimension = rng.boolean() ? CapacityDimension::power : CapacityDimension::space;
      const std::int64_t installed = rng.range(0, 50000);
      const std::int64_t unavailable = rng.range(0, installed);
      const std::int64_t usable = rng.range(0, installed - unavailable);
      fsup::EvidenceSpec spec;
      spec.source = "source-" + std::to_string(index);
      spec.dimension = dimension;
      spec.unit = canonical_unit(dimension);
      spec.installed = installed;
      spec.unavailable = unavailable;
      spec.usable = usable;
      spec.derive_residual = true;
      spec.protected_capacity = rng.range(0, usable);
      spec.reserved = 0;
      builder.add(fsup::make_evidence(spec));
    }
    auto snapshot = builder.build();
    FT_REQUIRE_OK(snapshot);
    const std::string document = canonical::encode_snapshot(*snapshot.value());
    auto decoded = canonical::decode_snapshot(document, SnapshotFreshness::issued);
    FT_REQUIRE_OK(decoded);
    FT_CHECK_EQ(decoded.value()->digest(), snapshot.value()->digest());
    FT_CHECK_EQ(canonical::encode_snapshot(*decoded.value()), document);
  }
}
