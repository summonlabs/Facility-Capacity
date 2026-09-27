// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The durable store contract: creation and opening, the commit protocol and the
// precondition fence that guards it, inspection, pruning, destruction, and the
// container encodings that back them.
//
// Every case runs in its own `fsup::TempDir`, so the store directory is a
// disposable tree and nothing survives the case.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/facility_capacity/digest.hpp"
#include "dccp/facility_capacity/store.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace dccp::facility_capacity;

constexpr std::uint64_t kEpoch = 7;
constexpr std::uint64_t kIncarnation = 3;
constexpr std::int64_t kInstalled = 1000;

/// The entries a store directory holds. `capacity.lock` exists only once a lock
/// has been taken, so it is accepted but never required.
constexpr std::string_view kFormatName = "FORMAT";
constexpr std::string_view kCurrentName = "CURRENT";
constexpr std::string_view kLockName = "capacity.lock";

/// Fixed generation-file header offsets, from `version.hpp` and the store
/// format: magic 0..8, byte-order marker at 8, format version at 12, header size
/// at 16, declared body length at 20, body digest (32 raw bytes) at 24,
/// generation at 56, epoch at 64, incarnation at 72, published-at at 80 and 40
/// reserved zero bytes at 88..128.
constexpr std::size_t kFormatVersionOffset = 12;

/// Fresh idempotency tokens: the model treats a repeated attempt as a replay, so
/// every mutation in a case needs a token the model has never seen.
std::uint64_t next_attempt() {
  static std::uint64_t counter = 0;
  return ++counter;
}

CapacityPrecondition fresh_precondition(const FacilityCapacityModel& model) {
  CapacityPrecondition precondition = model.current_precondition();
  precondition.attempt = AttemptId::from_value(next_attempt());
  return precondition;
}

StoreCreateOptions store_options(const std::string& facility = "facility-a",
                                 const std::string& site = "site-1") {
  StoreCreateOptions options;
  options.facility = fsup::facility_id(facility);
  options.site = fsup::site_id(site);
  options.epoch = EpochId::from_value(kEpoch);
  options.incarnation = IncarnationId::from_value(kIncarnation);
  options.controller = fsup::controller_id("controller-1");
  return options;
}

ModelConfig model_config(const StoreCreateOptions& options) {
  ModelConfig config;
  config.facility = options.facility;
  config.site = options.site;
  config.epoch = options.epoch;
  config.incarnation = options.incarnation;
  return config;
}

fsup::EvidenceSpec power_evidence(const std::string& source, std::uint64_t generation, std::int64_t usable,
                                 std::uint64_t epoch = kEpoch, std::uint64_t incarnation = kIncarnation) {
  fsup::EvidenceSpec spec;
  spec.source = source;
  spec.dimension = CapacityDimension::power;
  spec.unit = Unit::milli_watt;
  spec.generation = generation;
  spec.epoch = epoch;
  spec.incarnation = incarnation;
  spec.installed = kInstalled;
  spec.observed = kInstalled;
  spec.usable = usable;
  spec.unavailable = kInstalled - usable;
  spec.residual = 0;
  return spec;
}

/// Declares one power-evidence record and publishes the model's next generation.
Result<std::shared_ptr<const CapacitySnapshot>> declare_and_publish(FacilityCapacityModel& model,
                                                                    std::uint64_t evidence_generation,
                                                                    std::int64_t usable,
                                                                    std::uint64_t epoch = kEpoch,
                                                                    std::uint64_t incarnation = kIncarnation) {
  const Status declared = model.declare_evidence(
      fsup::make_evidence(power_evidence("power-capacity", evidence_generation, usable, epoch, incarnation)),
      fresh_precondition(model));
  if (!declared.has_value()) {
    return declared.error();
  }
  return model.publish(fresh_precondition(model));
}

/// The precondition that describes what the store's CURRENT manifest records
/// right now, which is what the fence compares against.
CapacityPrecondition store_precondition(const CapacityStore& store) {
  CapacityPrecondition precondition;
  precondition.expected_capacity_generation = store.authority().published_generation;
  precondition.expected_revision = store.authority().revision;
  precondition.expected_epoch = store.authority().epoch;
  precondition.expected_incarnation = store.authority().incarnation;
  precondition.attempt = AttemptId::from_value(next_attempt());
  return precondition;
}

/// Commits the model's already-published snapshot.
///
/// The documented sequence - the model's own current precondition, taken after
/// `publish` - is tried first. This build refuses it: the store fences
/// `expected_capacity_generation` and `expected_revision` against the generation
/// and revision CURRENT already records, which are the previous generation and
/// the revision the last commit recorded, so a freshly published model is always
/// one generation ahead of them (see
/// `commit_takes_the_published_model_precondition`). The fallback keeps every
/// other case in this suite exercising the store rather than the fence.
Result<StoreAuthority> commit_published(CapacityStore& store, const FacilityCapacityModel& model) {
  const CapacityPrecondition documented = fresh_precondition(model);
  Result<StoreAuthority> committed = store.commit(model, documented);
  if (committed.has_value()) {
    return committed;
  }
  CapacityPrecondition adjusted = documented;
  adjusted.expected_capacity_generation = store.authority().published_generation;
  adjusted.expected_revision = store.authority().revision;
  adjusted.attempt = AttemptId::from_value(next_attempt());
  return store.commit(model, adjusted);
}

/// Publishes and commits one generation in a single step.
Result<StoreAuthority> commit_generation(CapacityStore& store, FacilityCapacityModel& model,
                                         std::uint64_t evidence_generation, std::int64_t usable) {
  Result<std::shared_ptr<const CapacitySnapshot>> snapshot =
      declare_and_publish(model, evidence_generation, usable);
  if (!snapshot.has_value()) {
    return snapshot.error();
  }
  return commit_published(store, model);
}

/// Fails with the text of every finding, so a failure names what the store
/// objected to instead of only that it objected.
void check_no_findings(const VerifyReport& report) {
  for (const std::string& finding : report.findings) {
    FT_FAIL("unexpected verify finding: " + finding);
  }
}

/// Replaces the value of one `key=` line of a canonical document.
void set_field(std::string& document, std::string_view key, std::string_view value) {
  const std::string prefix = std::string(key) + "=";
  const std::size_t start = document.find(prefix);
  if (start == std::string::npos) {
    throw std::runtime_error("the document has no " + prefix + " field");
  }
  const std::size_t line_end = document.find('\n', start);
  if (line_end == std::string::npos) {
    throw std::runtime_error("the document is not LF terminated");
  }
  document.replace(start, line_end - start, prefix + std::string(value));
}

/// Recomputes the trailing `digest=` line over every byte that precedes it, so a
/// hand-edited document is self-consistent and reaches the field checks rather
/// than failing as a checksum.
std::string reseal(std::string document) {
  const std::size_t at = document.rfind("digest=");
  if (at == std::string::npos) {
    throw std::runtime_error("the document has no digest line");
  }
  const std::string body = document.substr(0, at);
  const std::string declared = digest::sha256_hex(body);
  std::string sealed = body;
  sealed.append("digest=");
  sealed.append(declared);
  sealed.push_back('\n');
  return sealed;
}

/// Overwrites the FOUR little-endian bytes of the format version.
void patch_format_version(const std::filesystem::path& path, std::uint32_t value) {
  std::string bytes = fsup::read_bytes(path);
  if (bytes.size() < kFormatVersionOffset + 4) {
    throw std::runtime_error("the generation file is shorter than its header");
  }
  for (unsigned index = 0; index < 4u; ++index) {
    bytes[kFormatVersionOffset + index] = static_cast<char>((value >> (8u * index)) & 0xFFu);
  }
  fsup::write_bytes(path, bytes);
}

}  // namespace

// ---------------------------------------------------------------------------
// Creation and opening
// ---------------------------------------------------------------------------

FT_TEST(store, create_then_open_round_trip) {
  fsup::TempDir dir("store-round-trip");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  std::optional<CapacityStore> store = std::move(created.value());

  const StoreAuthority authority = store->authority();
  FT_CHECK(!authority.store_id.empty());
  FT_CHECK_EQ(authority.facility.value(), std::string("facility-a"));
  FT_CHECK_EQ(authority.site.value(), std::string("site-1"));
  FT_CHECK_EQ(authority.controller.value(), std::string("controller-1"));
  FT_CHECK_EQ(authority.epoch.value(), kEpoch);
  FT_CHECK_EQ(authority.incarnation.value(), kIncarnation);
  FT_CHECK_EQ(authority.published_generation.value(), std::uint64_t{0});
  FT_CHECK_EQ(authority.revision.value(), std::uint64_t{0});
  FT_CHECK(!authority.has_published_generation);
  FT_CHECK_EQ(store->access(), StoreAccess::writer);
  FT_CHECK(std::filesystem::exists(root / kFormatName));
  FT_CHECK(std::filesystem::exists(root / kCurrentName));

  const std::string store_id = authority.store_id.value();
  store.reset();

  StoreOpenOptions open_options;
  auto opened = CapacityStore::open(root, open_options, clock);
  FT_REQUIRE_OK(opened);
  const StoreAuthority reopened = opened.value().authority();
  FT_CHECK_EQ(reopened.store_id.value(), store_id);
  FT_CHECK_EQ(reopened.facility.value(), std::string("facility-a"));
  FT_CHECK_EQ(reopened.site.value(), std::string("site-1"));
  FT_CHECK_EQ(reopened.controller.value(), std::string("controller-1"));
  FT_CHECK_EQ(reopened.epoch.value(), kEpoch);
  FT_CHECK_EQ(reopened.incarnation.value(), kIncarnation);
  FT_CHECK_EQ(reopened.published_generation.value(), std::uint64_t{0});
  FT_CHECK(!reopened.has_published_generation);
  FT_CHECK_EQ(opened.value().access(), StoreAccess::writer);
  FT_CHECK(opened.value().root() == root);

  // A reader handle observes the same authority.
  StoreOpenOptions reader_options;
  reader_options.access = StoreAccess::reader;
  auto reader = CapacityStore::open(root, reader_options, clock);
  FT_REQUIRE_OK(reader);
  FT_CHECK_EQ(reader.value().access(), StoreAccess::reader);
  FT_CHECK_EQ(reader.value().authority().store_id.value(), store_id);
}

FT_TEST(store, create_rejects_an_existing_store) {
  fsup::TempDir dir("store-already-exists");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto first = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(first);
  FT_CHECK_ERROR(CapacityStore::create(root, options, clock), ErrorCode::already_exists);

  // The refused second create left the first store untouched.
  FT_CHECK_EQ(first.value().authority().published_generation.value(), std::uint64_t{0});
  FT_CHECK(std::filesystem::exists(root / kFormatName));
}

FT_TEST(store, create_rejects_invalid_identity_and_counters) {
  fsup::TempDir dir("store-create-invalid");
  ManualClock clock(Tick::from_value(1000));
  const StoreCreateOptions options = store_options();

  StoreCreateOptions no_facility = options;
  no_facility.facility = FacilityId();
  FT_CHECK_ERROR(CapacityStore::create(dir.path() / "no-facility", no_facility, clock),
                 ErrorCode::invalid_argument);

  StoreCreateOptions no_site = options;
  no_site.site = SiteId();
  FT_CHECK_ERROR(CapacityStore::create(dir.path() / "no-site", no_site, clock), ErrorCode::invalid_argument);

  StoreCreateOptions no_controller = options;
  no_controller.controller = ControllerId();
  FT_CHECK_ERROR(CapacityStore::create(dir.path() / "no-controller", no_controller, clock),
                 ErrorCode::invalid_argument);

  StoreCreateOptions zero_epoch = options;
  zero_epoch.epoch = EpochId::from_value(0);
  FT_CHECK_ERROR(CapacityStore::create(dir.path() / "zero-epoch", zero_epoch, clock),
                 ErrorCode::invalid_argument);

  StoreCreateOptions zero_incarnation = options;
  zero_incarnation.incarnation = IncarnationId::from_value(0);
  FT_CHECK_ERROR(CapacityStore::create(dir.path() / "zero-incarnation", zero_incarnation, clock),
                 ErrorCode::invalid_argument);

  // Not one of the rejected calls created a directory.
  for (const char* name : {"no-facility", "no-site", "no-controller", "zero-epoch", "zero-incarnation"}) {
    FT_CHECK(!std::filesystem::exists(dir.path() / name));
  }
}

FT_TEST(store, create_rejects_a_parent_reference) {
  fsup::TempDir dir("store-parent-reference");
  ManualClock clock(Tick::from_value(1000));
  FT_CHECK_ERROR(CapacityStore::create(dir.path() / ".." / "escaped-store", store_options(), clock),
                 ErrorCode::path_rejected);
}

FT_TEST(store, create_rejects_a_regular_file_path) {
  fsup::TempDir dir("store-file-path");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path file = dir.file("plain-file");
  fsup::write_bytes(file, "not a store");

  FT_CHECK_ERROR(CapacityStore::create(file, store_options(), clock), ErrorCode::path_rejected);
  FT_CHECK_EQ(fsup::read_bytes(file), std::string("not a store"));
}

FT_TEST(store, open_requires_a_format_manifest) {
  fsup::TempDir dir("store-open-no-format");
  ManualClock clock(Tick::from_value(1000));
  StoreOpenOptions open_options;

  const std::filesystem::path empty = dir.path() / "empty-directory";
  std::filesystem::create_directories(empty);
  FT_CHECK_ERROR(CapacityStore::open(empty, open_options, clock), ErrorCode::not_found);

  // A directory that holds something this product did not write is not a store
  // either: FORMAT is what makes a directory a store.
  const std::filesystem::path foreign = dir.path() / "foreign-directory";
  std::filesystem::create_directories(foreign);
  fsup::write_bytes(foreign / "CURRENT", "document=store-current\n");
  FT_CHECK_ERROR(CapacityStore::open(foreign, open_options, clock), ErrorCode::not_found);
}

FT_TEST(store, open_rejects_a_missing_directory) {
  fsup::TempDir dir("store-open-missing");
  ManualClock clock(Tick::from_value(1000));
  StoreOpenOptions open_options;
  FT_CHECK_ERROR(CapacityStore::open(dir.path() / "absent", open_options, clock), ErrorCode::not_found);
  FT_CHECK_ERROR(CapacityStore::open(dir.path() / ".." / "absent", open_options, clock),
                 ErrorCode::path_rejected);
}

FT_TEST(store, open_checks_the_expected_store_id) {
  fsup::TempDir dir("store-expected-id");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();

  auto created = CapacityStore::create(root, store_options(), clock);
  FT_REQUIRE_OK(created);
  const StoreId store_id = created.value().authority().store_id;
  FT_CHECK(!store_id.empty());

  StoreOpenOptions matching;
  matching.expected_store_id = store_id;
  FT_REQUIRE_OK(CapacityStore::open(root, matching, clock));

  StoreOpenOptions mismatched;
  mismatched.expected_store_id = fsup::store_id("store-000000000000000000000000");
  FT_CHECK_ERROR(CapacityStore::open(root, mismatched, clock), ErrorCode::conflict);
}

// ---------------------------------------------------------------------------
// The commit protocol
// ---------------------------------------------------------------------------

FT_TEST(store, commit_takes_the_published_model_precondition) {
  // The documented lifecycle: declare evidence, publish, then commit with the
  // model's CURRENT precondition. The store must accept it, because it is the
  // precondition the model published under.
  fsup::TempDir dir("store-lifecycle");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());

  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());

  const Status declared =
      model.declare_evidence(fsup::make_evidence(power_evidence("power-capacity", 1, 900)),
                             fresh_precondition(model));
  FT_REQUIRE_OK(declared);

  auto snapshot = model.publish(fresh_precondition(model));
  FT_REQUIRE_OK(snapshot);

  const CapacityPrecondition precondition = fresh_precondition(model);
  auto authority = store.commit(model, precondition);
  FT_REQUIRE_OK(authority);

  FT_CHECK(store.authority().has_published_generation);
  FT_CHECK_EQ(store.authority().published_generation.value(), snapshot.value()->generation().value());
  FT_CHECK_EQ(store.authority().published_generation.value(), std::uint64_t{1});
  FT_CHECK_EQ(store.authority().snapshot_digest, snapshot.value()->digest());
  FT_CHECK_EQ(store.authority().file_name, std::string("gen-1.fcs"));

  // FORMAT, CURRENT and the generation file; the lock file may or may not be
  // present, and nothing else may be.
  const std::vector<std::string> names = fsup::list_names(root);
  FT_CHECK(std::find(names.begin(), names.end(), std::string(kFormatName)) != names.end());
  FT_CHECK(std::find(names.begin(), names.end(), std::string(kCurrentName)) != names.end());
  FT_CHECK(std::find(names.begin(), names.end(), std::string("gen-1.fcs")) != names.end());
  for (const std::string& name : names) {
    FT_CHECK(name == kFormatName || name == kCurrentName || name == kLockName || name == "gen-1.fcs");
  }
}

FT_TEST(store, commit_publishes_the_next_generation) {
  fsup::TempDir dir("store-second-commit");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());
  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());

  FT_REQUIRE_OK(commit_generation(store, model, 1, 900));
  FT_CHECK_EQ(store.authority().published_generation.value(), std::uint64_t{1});
  FT_CHECK(std::filesystem::exists(root / "gen-1.fcs"));

  FT_REQUIRE_OK(commit_generation(store, model, 2, 800));
  FT_CHECK_EQ(store.authority().published_generation.value(), std::uint64_t{2});
  FT_CHECK_EQ(store.authority().file_name, std::string("gen-2.fcs"));
  FT_CHECK(std::filesystem::exists(root / "gen-2.fcs"));
  FT_CHECK(std::filesystem::exists(root / "gen-1.fcs"));
}

FT_TEST(store, commit_fences_the_recorded_capacity_generation) {
  fsup::TempDir dir("store-fence-generation");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());
  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());

  FT_REQUIRE_OK(commit_generation(store, model, 1, 900));
  // Publish the next generation, so the supplied generation token is the only
  // thing that is wrong.
  auto snapshot = declare_and_publish(model, 2, 800);
  FT_REQUIRE_OK(snapshot);

  CapacityPrecondition precondition = fresh_precondition(model);
  precondition.expected_capacity_generation = CapacityGeneration::from_value(99);
  auto committed = store.commit(model, precondition);
  if (committed.has_value()) {
    FT_FAIL("a stale capacity generation must not be accepted");
  } else {
    FT_CHECK_EQ(committed.error().code(), ErrorCode::stale_generation);
    FT_CHECK(committed.error().has_generations());
    FT_CHECK_EQ(committed.error().expected_generation(), std::uint64_t{99});
    FT_CHECK_EQ(committed.error().actual_generation(), store.authority().published_generation.value());
  }
}

FT_TEST(store, commit_fences_the_recorded_revision) {
  fsup::TempDir dir("store-fence-revision");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());
  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());

  FT_REQUIRE_OK(commit_generation(store, model, 1, 900));
  auto snapshot = declare_and_publish(model, 2, 800);
  FT_REQUIRE_OK(snapshot);

  // The model's own precondition, with only the revision token falsified.
  CapacityPrecondition precondition = fresh_precondition(model);
  precondition.expected_revision = Revision::from_value(99);
  auto committed = store.commit(model, precondition);
  if (committed.has_value()) {
    FT_FAIL("a stale store revision must not be accepted");
  } else {
    FT_CHECK_EQ(committed.error().code(), ErrorCode::stale_generation);
    FT_CHECK(committed.error().has_generations());
    FT_CHECK_EQ(committed.error().expected_generation(), std::uint64_t{99});
    // The token is a revision, and a rejected revision names either the
    // revision the store recorded at the last commit or the revision the model
    // being committed actually holds; both are revisions the state holds.
    const std::uint64_t actual = committed.error().actual_generation();
    FT_CHECK(actual == store.authority().revision.value() || actual == model.revision().value());
  }
}

FT_TEST(store, commit_fences_the_recorded_epoch) {
  fsup::TempDir dir("store-fence-epoch");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());
  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());

  auto snapshot = declare_and_publish(model, 1, 900);
  FT_REQUIRE_OK(snapshot);

  CapacityPrecondition precondition = fresh_precondition(model);
  precondition.expected_epoch = EpochId::from_value(99);
  auto committed = store.commit(model, precondition);
  if (committed.has_value()) {
    FT_FAIL("an epoch the store does not hold must not be accepted");
  } else {
    FT_CHECK_EQ(committed.error().code(), ErrorCode::stale_authority);
    FT_CHECK_EQ(committed.error().expected_generation(), std::uint64_t{99});
    FT_CHECK_EQ(committed.error().actual_generation(), store.authority().epoch.value());
  }
}

FT_TEST(store, commit_fences_the_recorded_incarnation) {
  fsup::TempDir dir("store-fence-incarnation");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());
  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());

  auto snapshot = declare_and_publish(model, 1, 900);
  FT_REQUIRE_OK(snapshot);

  CapacityPrecondition precondition = fresh_precondition(model);
  precondition.expected_incarnation = IncarnationId::from_value(99);
  auto committed = store.commit(model, precondition);
  if (committed.has_value()) {
    FT_FAIL("an incarnation the store does not hold must not be accepted");
  } else {
    FT_CHECK_EQ(committed.error().code(), ErrorCode::stale_authority);
    FT_CHECK_EQ(committed.error().expected_generation(), std::uint64_t{99});
    FT_CHECK_EQ(committed.error().actual_generation(), store.authority().incarnation.value());
  }
}

FT_TEST(store, commit_checks_authority_before_generation) {
  // The precedence is fixed: authority outranks generation outranks revision. A
  // precondition that is stale in all three reports the authority failure.
  fsup::TempDir dir("store-fence-precedence");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());
  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());

  auto snapshot = declare_and_publish(model, 1, 900);
  FT_REQUIRE_OK(snapshot);

  CapacityPrecondition precondition = fresh_precondition(model);
  precondition.expected_epoch = EpochId::from_value(99);
  precondition.expected_capacity_generation = CapacityGeneration::from_value(99);
  precondition.expected_revision = Revision::from_value(99);

  auto committed = store.commit(model, precondition);
  if (committed.has_value()) {
    FT_FAIL("a precondition stale in every token must not be accepted");
  } else {
    FT_CHECK_EQ(committed.error().code(), ErrorCode::stale_authority);
    FT_CHECK_EQ(committed.error().expected_generation(), std::uint64_t{99});
    FT_CHECK_EQ(committed.error().actual_generation(), store.authority().epoch.value());
  }
}

FT_TEST(store, commit_requires_a_published_model) {
  fsup::TempDir dir("store-unpublished-model");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());
  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());

  // The model and the store agree on every token; the model has simply never
  // published an answer to commit.
  const CapacityPrecondition precondition = fresh_precondition(model);
  FT_CHECK_EQ(precondition.expected_epoch.value(), kEpoch);
  FT_CHECK_ERROR(store.commit(model, precondition), ErrorCode::precondition_failed);
  FT_CHECK(!store.authority().has_published_generation);
}

FT_TEST(store, commit_requires_the_next_generation) {
  fsup::TempDir dir("store-next-generation");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());
  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());

  FT_REQUIRE_OK(commit_generation(store, model, 1, 900));
  FT_REQUIRE_OK(declare_and_publish(model, 2, 800));
  auto third = declare_and_publish(model, 3, 700);
  FT_REQUIRE_OK(third);
  FT_CHECK_EQ(model.capacity_generation().value(), std::uint64_t{3});

  // The model's own tokens, claiming the generation the store currently holds:
  // everything is consistent except that the snapshot is two ahead of it.
  CapacityPrecondition precondition = fresh_precondition(model);
  precondition.expected_capacity_generation = store.authority().published_generation;
  auto committed = store.commit(model, precondition);
  if (committed.has_value()) {
    FT_FAIL("a snapshot that is not the next store generation must not be accepted");
  } else {
    FT_CHECK_EQ(committed.error().code(), ErrorCode::stale_generation);
    FT_CHECK_EQ(committed.error().expected_generation(), std::uint64_t{2});
    FT_CHECK_EQ(committed.error().actual_generation(), std::uint64_t{3});
  }
  FT_CHECK_EQ(store.authority().published_generation.value(), std::uint64_t{1});
}

FT_TEST(store, commit_rejects_a_foreign_facility) {
  fsup::TempDir dir("store-foreign-facility");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());

  ModelConfig foreign = model_config(options);
  foreign.facility = fsup::facility_id("facility-b");
  auto created_model = FacilityCapacityModel::create(foreign, clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());
  auto snapshot = declare_and_publish(model, 1, 900);
  FT_REQUIRE_OK(snapshot);

  CapacityPrecondition precondition = fresh_precondition(model);
  FT_CHECK_ERROR(store.commit(model, precondition), ErrorCode::conflict);
  FT_CHECK(!store.authority().has_published_generation);
}

// ---------------------------------------------------------------------------
// Authority
// ---------------------------------------------------------------------------

FT_TEST(store, advance_epoch_updates_authority) {
  fsup::TempDir dir("store-advance-epoch");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();

  auto created = CapacityStore::create(root, store_options(), clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());
  const Revision original_revision = store.authority().revision;

  const ControllerId next_controller = fsup::controller_id("controller-2");
  const Status advanced =
      store.advance_epoch(EpochId::from_value(8), IncarnationId::from_value(4), next_controller,
                          store_precondition(store));
  FT_REQUIRE_OK(advanced);

  FT_CHECK_EQ(store.authority().epoch.value(), std::uint64_t{8});
  FT_CHECK_EQ(store.authority().incarnation.value(), std::uint64_t{4});
  FT_CHECK_EQ(store.authority().controller.value(), std::string("controller-2"));
  FT_CHECK(store.authority().revision.value() > original_revision.value());
  FT_CHECK(!store.authority().has_published_generation);
  FT_CHECK_EQ(store.authority().published_generation.value(), std::uint64_t{0});

  // The advance is durable, not only cached in the handle.
  StoreOpenOptions open_options;
  auto reopened = CapacityStore::open(root, open_options, clock);
  FT_REQUIRE_OK(reopened);
  FT_CHECK_EQ(reopened.value().authority().epoch.value(), std::uint64_t{8});
  FT_CHECK_EQ(reopened.value().authority().incarnation.value(), std::uint64_t{4});
  FT_CHECK_EQ(reopened.value().authority().controller.value(), std::string("controller-2"));
}

FT_TEST(store, advance_epoch_rejects_a_smaller_epoch) {
  fsup::TempDir dir("store-advance-backwards");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const ControllerId controller = fsup::controller_id("controller-1");

  auto created = CapacityStore::create(root, store_options(), clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());

  FT_CHECK_ERROR(store.advance_epoch(EpochId::from_value(kEpoch - 1), IncarnationId::from_value(4), controller,
                                     store_precondition(store)),
                 ErrorCode::invalid_argument);
  FT_CHECK_EQ(store.authority().epoch.value(), kEpoch);
  FT_CHECK_EQ(store.authority().controller.value(), std::string("controller-1"));
}

FT_TEST(store, advance_epoch_rejects_unchanged_authority) {
  fsup::TempDir dir("store-advance-same");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();

  auto created = CapacityStore::create(root, store_options(), clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());

  // Same epoch, same incarnation, same controller: nothing would change.
  FT_CHECK_ERROR(store.advance_epoch(EpochId::from_value(kEpoch), IncarnationId::from_value(kIncarnation),
                                     fsup::controller_id("controller-1"), store_precondition(store)),
                 ErrorCode::invalid_argument);
  // A zero incarnation is never an authority.
  FT_CHECK_ERROR(store.advance_epoch(EpochId::from_value(kEpoch + 1), IncarnationId::from_value(0),
                                     fsup::controller_id("controller-2"), store_precondition(store)),
                 ErrorCode::invalid_argument);
  // An empty controller is never an authority.
  FT_CHECK_ERROR(store.advance_epoch(EpochId::from_value(kEpoch + 1), IncarnationId::from_value(2),
                                     ControllerId(), store_precondition(store)),
                 ErrorCode::invalid_argument);
  FT_CHECK_EQ(store.authority().epoch.value(), kEpoch);
  FT_CHECK_EQ(store.authority().incarnation.value(), kIncarnation);
}

FT_TEST(store, advance_epoch_requires_a_current_precondition) {
  fsup::TempDir dir("store-advance-stale");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();

  auto created = CapacityStore::create(root, store_options(), clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());

  CapacityPrecondition stale = store_precondition(store);
  stale.expected_epoch = EpochId::from_value(99);
  FT_CHECK_ERROR(store.advance_epoch(EpochId::from_value(kEpoch + 1), IncarnationId::from_value(4),
                                     fsup::controller_id("controller-2"), stale),
                 ErrorCode::stale_authority);
  FT_CHECK_EQ(store.authority().epoch.value(), kEpoch);
  FT_CHECK_EQ(store.authority().controller.value(), std::string("controller-1"));
}

FT_TEST(store, commit_after_advance_epoch_rejects_the_old_epoch) {
  fsup::TempDir dir("store-old-epoch-commit");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());

  FT_REQUIRE_OK(store.advance_epoch(EpochId::from_value(kEpoch + 1), IncarnationId::from_value(kIncarnation + 1),
                                    fsup::controller_id("controller-2"), store_precondition(store)));

  // A precondition that still carries the old epoch is refused, even though
  // every other token matches what the store records.
  CapacityPrecondition old_epoch = store_precondition(store);
  old_epoch.expected_epoch = EpochId::from_value(kEpoch);
  old_epoch.expected_incarnation = IncarnationId::from_value(kIncarnation);

  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel stale_model = std::move(created_model.value());
  auto stale_snapshot = declare_and_publish(stale_model, 1, 900);
  FT_REQUIRE_OK(stale_snapshot);

  auto committed = store.commit(stale_model, old_epoch);
  if (committed.has_value()) {
    FT_FAIL("a superseded epoch must not be able to commit");
  } else {
    FT_CHECK_EQ(committed.error().code(), ErrorCode::stale_authority);
    FT_CHECK(committed.error().has_generations());
    // Both values name the same pair - the epoch the caller supplied and the
    // epoch the store holds. The check that raises this particular failure
    // reports them as (store, model) rather than (supplied, held), so this case
    // asserts the content of the pair and not its order.
    const std::uint64_t first = committed.error().expected_generation();
    const std::uint64_t second = committed.error().actual_generation();
    FT_CHECK((first == kEpoch && second == kEpoch + 1) || (first == kEpoch + 1 && second == kEpoch));
  }
  FT_CHECK(!store.authority().has_published_generation);

  // A model created under the store's new authority commits without trouble.
  ModelConfig renewed = model_config(options);
  renewed.epoch = EpochId::from_value(kEpoch + 1);
  renewed.incarnation = IncarnationId::from_value(kIncarnation + 1);
  auto renewed_model = FacilityCapacityModel::create(renewed, clock);
  FT_REQUIRE_OK(renewed_model);
  FacilityCapacityModel current_model = std::move(renewed_model.value());
  auto current_snapshot = declare_and_publish(current_model, 1, 900, kEpoch + 1, kIncarnation + 1);
  FT_REQUIRE_OK(current_snapshot);
  FT_REQUIRE_OK(store.commit(current_model, current_model.current_precondition()));
  FT_CHECK_EQ(store.authority().published_generation.value(), std::uint64_t{1});
  FT_CHECK_EQ(store.authority().epoch.value(), std::uint64_t{kEpoch + 1});
}

// ---------------------------------------------------------------------------
// Inspection, pruning and destruction
// ---------------------------------------------------------------------------

FT_TEST(store, verify_reports_a_healthy_store) {
  fsup::TempDir dir("store-verify-healthy");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());
  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());

  auto empty = store.verify(false);
  FT_REQUIRE_OK(empty);
  FT_CHECK(empty.value().ok);
  check_no_findings(empty.value());
  FT_CHECK(empty.value().residue.empty());
  FT_CHECK(empty.value().generations.empty());

  FT_REQUIRE_OK(commit_generation(store, model, 1, 900));

  auto report = store.verify(false);
  FT_REQUIRE_OK(report);
  FT_CHECK(report.value().ok);
  check_no_findings(report.value());
  FT_CHECK(report.value().residue.empty());
  FT_REQUIRE(report.value().generations.size() == 1);
  const GenerationRecord& record = report.value().generations[0];
  FT_CHECK_EQ(record.file_name, std::string("gen-1.fcs"));
  FT_CHECK_EQ(record.generation.value(), std::uint64_t{1});
  FT_CHECK(record.body_bytes > 0);
  FT_CHECK_EQ(record.epoch.value(), kEpoch);
  FT_CHECK_EQ(record.incarnation.value(), kIncarnation);
  FT_CHECK(record.referenced_by_current);
  FT_CHECK(record.digest.size() == 64);
}

FT_TEST(store, verify_deep_decodes_every_generation) {
  fsup::TempDir dir("store-verify-deep");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());
  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());

  FT_REQUIRE_OK(commit_generation(store, model, 1, 900));
  FT_REQUIRE_OK(commit_generation(store, model, 2, 800));

  auto report = store.verify(true);
  FT_REQUIRE_OK(report);
  FT_CHECK(report.value().ok);
  check_no_findings(report.value());
  FT_REQUIRE(report.value().generations.size() == 2);
  for (const GenerationRecord& record : report.value().generations) {
    FT_CHECK(record.decodes);
  }
  FT_CHECK(!report.value().generations[0].referenced_by_current);
  FT_CHECK(report.value().generations[1].referenced_by_current);
}

FT_TEST(store, residue_is_reported_and_pruned) {
  fsup::TempDir dir("store-residue");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());
  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());
  FT_REQUIRE_OK(commit_generation(store, model, 1, 900));

  // An abandoned staging file: exactly what a crash between the staging write
  // and the commit point leaves behind.
  fsup::write_bytes(root / "staging-1-1.tmp", "abandoned");

  auto report = store.verify(false);
  FT_REQUIRE_OK(report);
  FT_CHECK(report.value().ok);
  if (!report.value().findings.empty()) {
    FT_FAIL("residue is not a finding: " + report.value().findings[0]);
  }
  FT_REQUIRE(report.value().residue.size() == 1);
  FT_CHECK_EQ(report.value().residue[0], std::string("staging-1-1.tmp"));

  auto pruned = store.prune(4);
  FT_REQUIRE_OK(pruned);
  FT_CHECK_EQ(pruned.value().removed, std::size_t{1});
  FT_REQUIRE(pruned.value().removed_files.size() == 1);
  FT_CHECK_EQ(pruned.value().removed_files[0], std::string("staging-1-1.tmp"));
  FT_CHECK(!std::filesystem::exists(root / "staging-1-1.tmp"));

  // The committed generation and both manifests survived the prune.
  FT_CHECK(std::filesystem::exists(root / "gen-1.fcs"));
  FT_CHECK(std::filesystem::exists(root / kFormatName));
  FT_CHECK(std::filesystem::exists(root / kCurrentName));
}

FT_TEST(store, prune_never_removes_the_current_generation) {
  fsup::TempDir dir("store-prune");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());
  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());

  FT_REQUIRE_OK(commit_generation(store, model, 1, 900));
  FT_REQUIRE_OK(commit_generation(store, model, 2, 800));
  FT_REQUIRE_OK(commit_generation(store, model, 3, 700));
  FT_CHECK(std::filesystem::exists(root / "gen-1.fcs"));
  FT_CHECK(std::filesystem::exists(root / "gen-2.fcs"));
  FT_CHECK(std::filesystem::exists(root / "gen-3.fcs"));
  const std::string current_before = fsup::read_bytes(root / kCurrentName);
  const std::string format_before = fsup::read_bytes(root / kFormatName);

  auto pruned = store.prune(1);
  FT_REQUIRE_OK(pruned);
  FT_CHECK_EQ(pruned.value().removed, std::size_t{2});
  FT_CHECK_EQ(pruned.value().retained, std::size_t{1});
  FT_REQUIRE(pruned.value().removed_files.size() == 2);
  FT_CHECK_EQ(pruned.value().removed_files[0], std::string("gen-1.fcs"));
  FT_CHECK_EQ(pruned.value().removed_files[1], std::string("gen-2.fcs"));

  FT_CHECK(std::filesystem::exists(root / "gen-3.fcs"));
  FT_CHECK(!std::filesystem::exists(root / "gen-1.fcs"));
  FT_CHECK(!std::filesystem::exists(root / "gen-2.fcs"));
  FT_CHECK_EQ(fsup::read_bytes(root / kCurrentName), current_before);
  FT_CHECK_EQ(fsup::read_bytes(root / kFormatName), format_before);
  FT_CHECK_EQ(store.authority().published_generation.value(), std::uint64_t{3});

  auto remaining = store.list_generations();
  FT_REQUIRE_OK(remaining);
  FT_REQUIRE(remaining.value().size() == 1);
  FT_CHECK_EQ(remaining.value()[0].generation.value(), std::uint64_t{3});
  FT_CHECK(remaining.value()[0].referenced_by_current);
}

FT_TEST(store, list_generations_is_ascending) {
  fsup::TempDir dir("store-list-generations");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());
  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());

  FT_REQUIRE_OK(commit_generation(store, model, 1, 900));
  FT_REQUIRE_OK(commit_generation(store, model, 2, 800));
  FT_REQUIRE_OK(commit_generation(store, model, 3, 700));

  auto records = store.list_generations();
  FT_REQUIRE_OK(records);
  FT_REQUIRE(records.value().size() == 3);
  FT_CHECK_EQ(records.value()[0].generation.value(), std::uint64_t{1});
  FT_CHECK_EQ(records.value()[1].generation.value(), std::uint64_t{2});
  FT_CHECK_EQ(records.value()[2].generation.value(), std::uint64_t{3});
  FT_CHECK_EQ(records.value()[0].file_name, std::string("gen-1.fcs"));
  FT_CHECK_EQ(records.value()[2].file_name, std::string("gen-3.fcs"));
  FT_CHECK(!records.value()[0].referenced_by_current);
  FT_CHECK(!records.value()[1].referenced_by_current);
  FT_CHECK(records.value()[2].referenced_by_current);
}

FT_TEST(store, prune_requires_a_writer_handle) {
  fsup::TempDir dir("store-prune-reader");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  std::optional<CapacityStore> writer = std::move(created.value());
  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());
  FT_REQUIRE_OK(commit_generation(writer.value(), model, 1, 900));
  writer.reset();

  StoreOpenOptions reader_options;
  reader_options.access = StoreAccess::reader;
  auto reader = CapacityStore::open(root, reader_options, clock);
  FT_REQUIRE_OK(reader);
  FT_CHECK_EQ(reader.value().access(), StoreAccess::reader);
  FT_CHECK_ERROR(reader.value().prune(4), ErrorCode::permission_denied);
  FT_CHECK(std::filesystem::exists(root / "gen-1.fcs"));

  // A reader can still inspect the store.
  auto records = reader.value().list_generations();
  FT_REQUIRE_OK(records);
  FT_REQUIRE(records.value().size() == 1);
  FT_CHECK_EQ(records.value()[0].generation.value(), std::uint64_t{1});
}

FT_TEST(store, reader_handle_cannot_mutate) {
  fsup::TempDir dir("store-reader-mutate");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  std::optional<CapacityStore> writer = std::move(created.value());
  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());
  FT_REQUIRE_OK(commit_generation(writer.value(), model, 1, 900));
  writer.reset();

  StoreOpenOptions reader_options;
  reader_options.access = StoreAccess::reader;
  auto reader = CapacityStore::open(root, reader_options, clock);
  FT_REQUIRE_OK(reader);

  auto snapshot = declare_and_publish(model, 2, 800);
  FT_REQUIRE_OK(snapshot);
  FT_CHECK_ERROR(reader.value().commit(model, fresh_precondition(model)), ErrorCode::permission_denied);

  CapacityPrecondition advance;
  advance.expected_epoch = reader.value().authority().epoch;
  advance.expected_incarnation = reader.value().authority().incarnation;
  advance.expected_capacity_generation = reader.value().authority().published_generation;
  advance.expected_revision = reader.value().authority().revision;
  advance.attempt = AttemptId::from_value(next_attempt());
  FT_CHECK_ERROR(reader.value().advance_epoch(EpochId::from_value(kEpoch + 1), IncarnationId::from_value(9),
                                              fsup::controller_id("controller-9"), advance),
                 ErrorCode::permission_denied);
  FT_CHECK_ERROR(reader.value().prune(1), ErrorCode::permission_denied);

  // Nothing the reader attempted reached the store.
  FT_CHECK_EQ(reader.value().authority().published_generation.value(), std::uint64_t{1});
  FT_CHECK_EQ(reader.value().authority().epoch.value(), kEpoch);
  FT_CHECK_EQ(reader.value().authority().incarnation.value(), kIncarnation);
}

FT_TEST(store, destroy_removes_the_store_directory) {
  fsup::TempDir dir("store-destroy");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  std::optional<CapacityStore> store = std::move(created.value());
  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());
  FT_REQUIRE_OK(commit_generation(store.value(), model, 1, 900));
  store.reset();

  FT_REQUIRE_OK(CapacityStore::destroy(root));
  FT_CHECK(!std::filesystem::exists(root));
}

FT_TEST(store, destroy_refuses_a_foreign_entry) {
  fsup::TempDir dir("store-destroy-foreign");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  std::optional<CapacityStore> store = std::move(created.value());
  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());
  FT_REQUIRE_OK(commit_generation(store.value(), model, 1, 900));
  store.reset();

  fsup::write_bytes(root / "foreign-notes.txt", "keep me");
  FT_CHECK_ERROR(CapacityStore::destroy(root), ErrorCode::conflict);

  FT_CHECK(std::filesystem::exists(root / "foreign-notes.txt"));
  FT_CHECK_EQ(fsup::read_bytes(root / "foreign-notes.txt"), std::string("keep me"));
  FT_CHECK(std::filesystem::exists(root / kFormatName));
  FT_CHECK(std::filesystem::exists(root / kCurrentName));
  FT_CHECK(std::filesystem::exists(root / "gen-1.fcs"));
}

FT_TEST(store, swapped_generation_body_is_reported) {
  // Two stores of the same shape but different facilities. Substituting one
  // store's published generation for the other's leaves a file whose own digest
  // is intact but which is not what CURRENT names: verification must report a
  // finding and recovery must refuse the substituted state.
  fsup::TempDir dir("store-swapped-body");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root_a = dir.store_root("store-a");
  const std::filesystem::path root_b = dir.store_root("store-b");

  auto created_a = CapacityStore::create(root_a, store_options("facility-a", "site-1"), clock);
  FT_REQUIRE_OK(created_a);
  CapacityStore store_a = std::move(created_a.value());
  auto created_b = CapacityStore::create(root_b, store_options("facility-b", "site-1"), clock);
  FT_REQUIRE_OK(created_b);
  CapacityStore store_b = std::move(created_b.value());

  const StoreCreateOptions options_a = store_options("facility-a", "site-1");
  const StoreCreateOptions options_b = store_options("facility-b", "site-1");
  auto model_a_created = FacilityCapacityModel::create(model_config(options_a), clock);
  FT_REQUIRE_OK(model_a_created);
  FacilityCapacityModel model_a = std::move(model_a_created.value());
  auto model_b_created = FacilityCapacityModel::create(model_config(options_b), clock);
  FT_REQUIRE_OK(model_b_created);
  FacilityCapacityModel model_b = std::move(model_b_created.value());

  FT_REQUIRE_OK(commit_generation(store_a, model_a, 1, 900));
  FT_REQUIRE_OK(commit_generation(store_b, model_b, 1, 800));
  fsup::copy_file(root_b / "gen-1.fcs", root_a / "gen-1.fcs");

  auto report = store_a.verify(false);
  FT_REQUIRE_OK(report);
  if (report.value().ok) {
    FT_FAIL("a generation file that is not the one CURRENT names must be reported");
  }
  if (report.value().findings.empty()) {
    FT_FAIL("the inspection reported no finding for a substituted generation file");
  }

  auto recovered = store_a.recover();
  if (recovered.has_value()) {
    FT_FAIL("recovery must refuse a generation file that is not the published one");
  } else {
    const ErrorCode code = recovered.error().code();
    if (code != ErrorCode::conflict && code != ErrorCode::checksum_mismatch) {
      FT_FAIL("unexpected error for a substituted generation file: " + recovered.error().to_string());
    }
  }
}

FT_TEST(store, copied_current_from_another_store_is_refused) {
  fsup::TempDir dir("store-foreign-current");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root_a = dir.store_root("store-a");
  const std::filesystem::path root_b = dir.store_root("store-b");

  auto created_a = CapacityStore::create(root_a, store_options("facility-a", "site-1"), clock);
  FT_REQUIRE_OK(created_a);
  CapacityStore store_a = std::move(created_a.value());
  auto created_b = CapacityStore::create(root_b, store_options("facility-b", "site-1"), clock);
  FT_REQUIRE_OK(created_b);
  CapacityStore store_b = std::move(created_b.value());

  FT_CHECK(store_a.authority().store_id != store_b.authority().store_id);
  FT_CHECK(store_a.authority().facility != store_b.authority().facility);

  // Store B's CURRENT names store B, while the directory still holds store A's
  // FORMAT: the two manifests disagree about the identity of the store.
  fsup::copy_file(root_b / kCurrentName, root_a / kCurrentName);
  StoreOpenOptions open_options;
  FT_CHECK_ERROR(CapacityStore::open(root_a, open_options, clock), ErrorCode::conflict);
}

// ---------------------------------------------------------------------------
// Container encodings
// ---------------------------------------------------------------------------

FT_TEST(store_format, generation_file_name_is_gen_n_fcs) {
  FT_CHECK_EQ(CapacityStore::generation_file_name(CapacityGeneration::from_value(0)), std::string("gen-0.fcs"));
  FT_CHECK_EQ(CapacityStore::generation_file_name(CapacityGeneration::from_value(1)), std::string("gen-1.fcs"));
  FT_CHECK_EQ(CapacityStore::generation_file_name(CapacityGeneration::from_value(42)), std::string("gen-42.fcs"));
  FT_CHECK_EQ(CapacityStore::generation_file_name(CapacityGeneration::from_value(123456789)),
              std::string("gen-123456789.fcs"));
}

FT_TEST(store_format, corrupt_current_digest_is_refused) {
  fsup::TempDir dir("store-format-current-digest");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();

  auto created = CapacityStore::create(root, store_options(), clock);
  FT_REQUIRE_OK(created);

  std::string document = fsup::read_bytes(root / kCurrentName);
  const std::size_t at = document.rfind("digest=");
  FT_REQUIRE(at != std::string::npos);
  const std::size_t value_at = at + std::string_view("digest=").size();
  FT_REQUIRE(document.size() - value_at > 64);
  document[value_at] = document[value_at] == '0' ? '1' : '0';
  fsup::write_bytes(root / kCurrentName, document);

  StoreOpenOptions open_options;
  FT_CHECK_ERROR(CapacityStore::open(root, open_options, clock), ErrorCode::checksum_mismatch);
}

FT_TEST(store_format, truncated_current_is_refused) {
  fsup::TempDir dir("store-format-current-truncated");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();

  auto created = CapacityStore::create(root, store_options(), clock);
  FT_REQUIRE_OK(created);

  const std::string document = fsup::read_bytes(root / kCurrentName);
  FT_REQUIRE(document.size() > 8);
  fsup::write_bytes(root / kCurrentName, document.substr(0, document.size() / 2));

  StoreOpenOptions open_options;
  auto opened = CapacityStore::open(root, open_options, clock);
  if (opened.has_value()) {
    FT_FAIL("a truncated CURRENT manifest must not open");
  } else {
    const ErrorCode code = opened.error().code();
    if (code != ErrorCode::truncated_input && code != ErrorCode::corruption &&
        code != ErrorCode::checksum_mismatch) {
      FT_FAIL("unexpected error for a truncated CURRENT manifest: " + opened.error().to_string());
    }
  }
}

FT_TEST(store_format, current_with_a_foreign_file_name_is_refused) {
  // A stored file name is hostile input. Replacing the name inside the real
  // document and re-sealing it needs the library's writer, so the document is
  // hand-built here: the recorded name is set to `evil` and the self-digest is
  // recomputed over the edited bytes. The refusal is `path_rejected`, not
  // `corruption`: the document is structurally well formed, every field parses
  // and its self-digest is correct, and what rejects it is the name shape - a
  // manifest validator accepts a file name only when it matches
  // `gen-<digits>.fcs`, so a recorded name can never carry a separator or a
  // parent reference out of the store directory.
  fsup::TempDir dir("store-format-current-name");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();

  auto created = CapacityStore::create(root, store_options(), clock);
  FT_REQUIRE_OK(created);

  std::string document = fsup::read_bytes(root / kCurrentName);
  set_field(document, "file_name", "evil");
  document = reseal(document);
  fsup::write_bytes(root / kCurrentName, document);

  StoreOpenOptions open_options;
  FT_CHECK_ERROR(CapacityStore::open(root, open_options, clock), ErrorCode::path_rejected);
  FT_CHECK_ERROR(created.value().recover(), ErrorCode::path_rejected);
}

FT_TEST(store_format, current_with_a_path_traversal_name_is_refused) {
  // The same rejection covers the traversal shape a hostile manifest would try
  // to use: `../../evil.fcs` is not `gen-<digits>.fcs` either, and no path is
  // ever built from it.
  fsup::TempDir dir("store-format-current-traversal");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();

  auto created = CapacityStore::create(root, store_options(), clock);
  FT_REQUIRE_OK(created);

  std::string document = fsup::read_bytes(root / kCurrentName);
  set_field(document, "file_name", "../../evil.fcs");
  document = reseal(document);
  fsup::write_bytes(root / kCurrentName, document);

  StoreOpenOptions open_options;
  FT_CHECK_ERROR(CapacityStore::open(root, open_options, clock), ErrorCode::path_rejected);
  // Nothing was written anywhere: not beside the store, not inside it.
  FT_CHECK(!std::filesystem::exists(dir.path() / "evil.fcs"));
  for (const std::string& name : fsup::list_names(root)) {
    FT_CHECK(name != "evil.fcs");
  }
}

FT_TEST(store_format, current_with_a_bad_snapshot_digest_is_refused) {
  // A recorded digest that is not 64 lowercase hex characters is corruption: the
  // document is well formed, but it records a value the format cannot hold.
  fsup::TempDir dir("store-format-current-bad-digest");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();

  auto created = CapacityStore::create(root, store_options(), clock);
  FT_REQUIRE_OK(created);

  std::string document = fsup::read_bytes(root / kCurrentName);
  set_field(document, "snapshot_digest", "not-a-digest");
  document = reseal(document);
  fsup::write_bytes(root / kCurrentName, document);

  StoreOpenOptions open_options;
  FT_CHECK_ERROR(CapacityStore::open(root, open_options, clock), ErrorCode::corruption);
}

FT_TEST(store_format, generation_header_version_is_checked) {
  // The generation container rejects a format version this build does not write,
  // before any field after it is believed.
  fsup::TempDir dir("store-format-header-version");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  const StoreCreateOptions options = store_options();

  auto created = CapacityStore::create(root, options, clock);
  FT_REQUIRE_OK(created);
  CapacityStore store = std::move(created.value());
  auto created_model = FacilityCapacityModel::create(model_config(options), clock);
  FT_REQUIRE_OK(created_model);
  FacilityCapacityModel model = std::move(created_model.value());
  FT_REQUIRE_OK(commit_generation(store, model, 1, 900));

  const std::filesystem::path generation =
      root / CapacityStore::generation_file_name(CapacityGeneration::from_value(1));
  patch_format_version(generation, 0x7FFFFFFFu);

  FT_CHECK_ERROR(store.recover(), ErrorCode::incompatible_version);

  auto report = store.verify(true);
  FT_REQUIRE_OK(report);
  FT_CHECK(!report.value().ok);
  if (report.value().findings.empty()) {
    FT_FAIL("an unsupported generation format version must be reported as a finding");
  }
}
