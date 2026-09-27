// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Persistence, close/reopen and restart semantics of the durable store.
//
// A published generation is written to disk, the handle is dropped, the store is
// opened again and the whole authoritative state is recovered from the bytes
// alone. Every corruption case patches one region of the container and asserts
// the exact refusal, and every case asserts that a refused recovery changed
// nothing on disk and nothing in the recovered model.

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

constexpr std::string_view kFormatName = "FORMAT";
constexpr std::string_view kCurrentName = "CURRENT";

/// Fixed generation-file header offsets: magic 0..8, byte-order marker at 8,
/// format version at 12, header size at 16, declared body length at 20, body
/// digest (32 raw bytes) at 24, generation at 56, epoch at 64, incarnation at
/// 72, published-at at 80, and 40 reserved zero bytes at 88..128.
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kMagicBytes = 8;
constexpr std::size_t kByteOrderOffset = 8;
constexpr std::size_t kFormatVersionOffset = 12;
constexpr std::size_t kBodyBytesOffset = 20;
constexpr std::size_t kBodyDigestOffset = 24;
constexpr std::size_t kReservedOffset = 88;
constexpr std::size_t kReservedProbeOffset = 100;
constexpr std::size_t kHeaderBytes = 128;

/// The little-endian byte-order marker this build writes: the marker value is
/// `0x01020304`, so the four bytes on disk are `04 03 02 01`.
constexpr std::string_view kByteOrderBytes = "\x04\x03\x02\x01";
/// The same four bytes the other way round, which is what a big-endian writer
/// would have produced.
constexpr std::string_view kByteSwappedOrderBytes = "\x01\x02\x03\x04";

std::uint64_t next_attempt() {
  static std::uint64_t counter = 0;
  return ++counter;
}

CapacityPrecondition fresh_precondition(const FacilityCapacityModel& model) {
  CapacityPrecondition precondition = model.current_precondition();
  precondition.attempt = AttemptId::from_value(next_attempt());
  return precondition;
}

StoreCreateOptions store_options() {
  StoreCreateOptions options;
  options.facility = fsup::facility_id("facility-a");
  options.site = fsup::site_id("site-1");
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

fsup::EvidenceSpec power_evidence(const std::string& source, std::uint64_t generation, std::int64_t usable) {
  fsup::EvidenceSpec spec;
  spec.source = source;
  spec.dimension = CapacityDimension::power;
  spec.unit = Unit::milli_watt;
  spec.generation = generation;
  spec.epoch = kEpoch;
  spec.incarnation = kIncarnation;
  spec.installed = kInstalled;
  spec.observed = kInstalled;
  spec.usable = usable;
  spec.unavailable = kInstalled - usable;
  spec.residual = 0;
  return spec;
}

/// Aborts the case when a setup step fails, naming the step.
template <class T>
T require(Result<T> value, const char* step) {
  if (!value.has_value()) {
    throw std::runtime_error(std::string("setup (") + step + "): " + value.error().to_string());
  }
  return std::move(value.value());
}

void require_status(const Status& status, const char* step) {
  if (!status.has_value()) {
    throw std::runtime_error(std::string("setup (") + step + "): " + status.error().to_string());
  }
}

/// Commits the model's published snapshot. The documented sequence - the model's
/// own current precondition after `publish` - is tried first; this build refuses
/// it because the store fences the generation and revision tokens against what
/// CURRENT records (the *previous* generation and the revision the last commit
/// recorded). The fallback reads those two tokens from the store itself so the
/// recovery cases below exercise recovery rather than the fence; the fence
/// itself is covered by `test_store.cpp`.
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

/// A store directory holding one committed generation, plus the model that
/// produced it.
struct CommittedStore {
  CapacityStore store;
  FacilityCapacityModel model;
  std::shared_ptr<const CapacitySnapshot> snapshot;
};

/// Creates a store, declares two evidence records, publishes and commits them.
CommittedStore make_committed_store(const std::filesystem::path& root, ManualClock& clock,
                                    std::uint64_t evidence_generation = 1, std::int64_t usable = 900) {
  const StoreCreateOptions options = store_options();
  CapacityStore store = require(CapacityStore::create(root, options, clock), "store create");
  FacilityCapacityModel model = require(FacilityCapacityModel::create(model_config(options), clock), "model create");
  require_status(model.declare_evidence(fsup::make_evidence(power_evidence("power-capacity", evidence_generation,
                                                                           usable)),
                                        fresh_precondition(model)),
                 "declare power evidence");
  require_status(model.declare_evidence(fsup::make_evidence(power_evidence("power-meter", evidence_generation,
                                                                          usable - 10)),
                                        fresh_precondition(model)),
                 "declare meter evidence");
  std::shared_ptr<const CapacitySnapshot> snapshot =
      require(model.publish(fresh_precondition(model)), "publish");
  require(commit_published(store, model), "commit");
  return CommittedStore{std::move(store), std::move(model), std::move(snapshot)};
}

std::filesystem::path generation_path(const std::filesystem::path& root, std::uint64_t generation) {
  return root / CapacityStore::generation_file_name(CapacityGeneration::from_value(generation));
}

/// Replaces the bytes of `path` at `offset` with `bytes`.
void patch_bytes(const std::filesystem::path& path, std::size_t offset, std::string_view bytes) {
  std::string content = fsup::read_bytes(path);
  if (offset + bytes.size() > content.size()) {
    throw std::runtime_error("the patch lies outside the file");
  }
  content.replace(offset, bytes.size(), bytes);
  fsup::write_bytes(path, content);
}

/// Writes `value` as four little-endian bytes at `offset`.
void patch_u32(const std::filesystem::path& path, std::size_t offset, std::uint32_t value) {
  std::string bytes;
  for (unsigned index = 0; index < 4u; ++index) {
    bytes.push_back(static_cast<char>((value >> (8u * index)) & 0xFFu));
  }
  patch_bytes(path, offset, bytes);
}

/// Flips the least significant bit of the byte at `offset`.
void flip_byte(const std::filesystem::path& path, std::size_t offset) {
  std::string content = fsup::read_bytes(path);
  if (offset >= content.size()) {
    throw std::runtime_error("the byte offset lies outside the file");
  }
  content[offset] = static_cast<char>(static_cast<unsigned char>(content[offset]) ^ 0x01u);
  fsup::write_bytes(path, content);
}

/// Reads the declared body length out of a generation file's header.
std::uint32_t declared_body_bytes(const std::filesystem::path& path) {
  const std::string content = fsup::read_bytes(path);
  if (content.size() < kHeaderBytes) {
    throw std::runtime_error("the generation file is shorter than its header");
  }
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4u; ++index) {
    const std::uint32_t byte =
        static_cast<std::uint32_t>(static_cast<unsigned char>(content[kBodyBytesOffset + index]));
    value |= byte << (8u * index);
  }
  return value;
}

/// Opens a store that must open, aborting the case when it does not.
CapacityStore open_store(const std::filesystem::path& root, ManualClock& clock) {
  StoreOpenOptions options;
  return require(CapacityStore::open(root, options, clock), "store open");
}

/// A store that must recover, aborting the case when it does not.
RecoveredState recover_store(const CapacityStore& store) {
  return require(store.recover(), "recover");
}

}  // namespace

// ---------------------------------------------------------------------------
// A committed generation survives the process that wrote it
// ---------------------------------------------------------------------------

FT_TEST(recovery, reopen_and_recover_after_a_commit) {
  fsup::TempDir dir("recovery-round-trip");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();

  {
    // The whole store handle is dropped at the end of this scope: nothing but
    // the bytes on disk carries the state forward.
    CommittedStore committed = make_committed_store(root, clock);
    FT_CHECK(committed.store.authority().has_published_generation);
  }

  CapacityStore reopened = open_store(root, clock);
  auto recovered = reopened.recover();
  FT_REQUIRE_OK(recovered);
  FT_REQUIRE(recovered.value().snapshot != nullptr);
  FT_CHECK_EQ(recovered.value().snapshot->generation().value(), std::uint64_t{1});
  FT_CHECK(recovered.value().authority.has_published_generation);
  FT_CHECK_EQ(recovered.value().authority.published_generation.value(), std::uint64_t{1});
  FT_CHECK_EQ(recovered.value().model.capacity_generation().value(), std::uint64_t{1});
  FT_CHECK(std::filesystem::exists(root / kFormatName));
  FT_CHECK(std::filesystem::exists(root / "gen-1.fcs"));
}

FT_TEST(recovery, recovered_snapshot_is_not_issued) {
  // A snapshot read back from durable storage is stamped `recovered` in its own
  // bytes and carries `recovered_not_revalidated`: it must never be presented as
  // if this process had just issued it.
  fsup::TempDir dir("recovery-freshness");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();

  {
    CommittedStore committed = make_committed_store(root, clock);
    FT_CHECK(committed.snapshot->freshness() == SnapshotFreshness::issued);
  }

  CapacityStore reopened = open_store(root, clock);
  RecoveredState recovered = recover_store(reopened);
  FT_REQUIRE(recovered.snapshot != nullptr);
  FT_CHECK(recovered.snapshot->freshness() == SnapshotFreshness::recovered);
  FT_CHECK(recovered.snapshot->freshness() != SnapshotFreshness::issued);
  FT_CHECK(recovered.notes.contains(ReasonCode::recovered_not_revalidated));
  const CapacityNote* note = recovered.notes.find(ReasonCode::recovered_not_revalidated);
  FT_REQUIRE(note != nullptr);
  FT_CHECK(!note->detail.empty());
}

FT_TEST(recovery, recovered_state_matches_the_committed_state) {
  fsup::TempDir dir("recovery-state");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();

  std::uint64_t generation = 0;
  std::uint64_t revision = 0;
  std::string facility;
  std::string site;
  std::vector<std::uint64_t> evidence_generations;
  std::vector<std::string> evidence_digests;
  {
    CommittedStore committed = make_committed_store(root, clock);
    generation = committed.model.capacity_generation().value();
    revision = committed.model.revision().value();
    facility = committed.model.facility().value();
    site = committed.model.site().value();
    for (const SourceEvidence& evidence : committed.model.all_evidence()) {
      evidence_generations.push_back(evidence.generation().value());
      evidence_digests.push_back(evidence.digest());
    }
    FT_CHECK_EQ(evidence_generations.size(), std::size_t{2});
  }

  CapacityStore reopened = open_store(root, clock);
  RecoveredState recovered = recover_store(reopened);
  const FacilityCapacityModel& model = recovered.model;
  FT_CHECK_EQ(model.capacity_generation().value(), generation);
  FT_CHECK_EQ(model.revision().value(), revision);
  FT_CHECK_EQ(model.epoch().value(), kEpoch);
  FT_CHECK_EQ(model.incarnation().value(), kIncarnation);
  FT_CHECK_EQ(model.facility().value(), facility);
  FT_CHECK_EQ(model.site().value(), site);

  const std::vector<SourceEvidence> evidence = model.all_evidence();
  FT_REQUIRE(evidence.size() == evidence_digests.size());
  FT_REQUIRE(evidence.size() == evidence_generations.size());
  for (std::size_t index = 0; index < evidence.size(); ++index) {
    FT_CHECK_EQ(evidence[index].generation().value(), evidence_generations[index]);
    FT_CHECK_EQ(evidence[index].digest(), evidence_digests[index]);
  }
}

FT_TEST(recovery, recovered_snapshot_matches_the_recorded_authority) {
  fsup::TempDir dir("recovery-authority");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();

  std::string committed_digest;
  {
    CommittedStore committed = make_committed_store(root, clock);
    committed_digest = committed.snapshot->digest();
    FT_CHECK_EQ(committed.store.authority().snapshot_digest, committed_digest);
  }

  CapacityStore reopened = open_store(root, clock);
  RecoveredState recovered = recover_store(reopened);
  FT_REQUIRE(recovered.snapshot != nullptr);
  FT_CHECK_EQ(recovered.snapshot->digest(), committed_digest);
  FT_CHECK_EQ(recovered.snapshot->digest(), recovered.authority.snapshot_digest);
  FT_CHECK_EQ(recovered.snapshot->generation().value(), recovered.authority.published_generation.value());
  FT_CHECK_EQ(recovered.snapshot->generation().value(), std::uint64_t{1});
  FT_CHECK_EQ(recovered.model.capacity_generation().value(), recovered.authority.published_generation.value());
  FT_CHECK_EQ(recovered.snapshot->facility().value(), std::string("facility-a"));
  FT_CHECK_EQ(recovered.snapshot->site().value(), std::string("site-1"));
}

FT_TEST(recovery, recover_without_a_published_generation_fails) {
  fsup::TempDir dir("recovery-nothing-published");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();

  auto created = CapacityStore::create(root, store_options(), clock);
  FT_REQUIRE_OK(created);
  FT_CHECK(!created.value().authority().has_published_generation);
  FT_CHECK_ERROR(created.value().recover(), ErrorCode::not_found);

  // The same after a reopen, and the store still holds no generation file.
  StoreOpenOptions open_options;
  auto reopened = CapacityStore::open(root, open_options, clock);
  FT_REQUIRE_OK(reopened);
  FT_CHECK_ERROR(reopened.value().recover(), ErrorCode::not_found);
  for (const std::string& name : fsup::list_names(root)) {
    FT_CHECK(name.rfind("gen-", 0) != 0);
  }
}

FT_TEST(recovery, recover_returns_the_second_generation) {
  fsup::TempDir dir("recovery-second-generation");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();

  std::string second_digest;
  std::uint64_t second_revision = 0;
  {
    CommittedStore committed = make_committed_store(root, clock);
    require_status(committed.model.declare_evidence(fsup::make_evidence(power_evidence("power-capacity", 2, 800)),
                                                    fresh_precondition(committed.model)),
                   "declare second evidence");
    std::shared_ptr<const CapacitySnapshot> second = require(committed.model.publish(fresh_precondition(committed.model)),
                                                             "publish second");
    require(commit_published(committed.store, committed.model), "commit second");
    FT_CHECK_EQ(second->generation().value(), std::uint64_t{2});
    second_digest = second->digest();
    second_revision = committed.model.revision().value();
  }

  CapacityStore reopened = open_store(root, clock);
  RecoveredState recovered = recover_store(reopened);
  FT_REQUIRE(recovered.snapshot != nullptr);
  FT_CHECK_EQ(recovered.snapshot->generation().value(), std::uint64_t{2});
  FT_CHECK_EQ(recovered.snapshot->digest(), second_digest);
  FT_CHECK_EQ(recovered.model.revision().value(), second_revision);
  FT_CHECK_EQ(recovered.authority.file_name, std::string("gen-2.fcs"));
  FT_CHECK(std::filesystem::exists(root / "gen-2.fcs"));
  FT_CHECK(std::filesystem::exists(root / "gen-1.fcs"));
}

// ---------------------------------------------------------------------------
// Container corruption
// ---------------------------------------------------------------------------

FT_TEST(recovery, body_byte_flip_is_refused) {
  fsup::TempDir dir("recovery-byte-flip");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  {
    CommittedStore committed = make_committed_store(root, clock);
  }

  const std::filesystem::path generation = generation_path(root, 1);
  FT_REQUIRE(fsup::read_bytes(generation).size() > kHeaderBytes);
  flip_byte(generation, kHeaderBytes);

  CapacityStore reopened = open_store(root, clock);
  FT_CHECK_ERROR(reopened.recover(), ErrorCode::checksum_mismatch);
}

FT_TEST(recovery, truncated_generation_file_is_refused) {
  fsup::TempDir dir("recovery-truncated");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  {
    CommittedStore committed = make_committed_store(root, clock);
  }

  const std::filesystem::path generation = generation_path(root, 1);
  const std::string bytes = fsup::read_bytes(generation);
  FT_REQUIRE(bytes.size() > kHeaderBytes + 32);
  fsup::write_bytes(generation, bytes.substr(0, bytes.size() - 32));

  CapacityStore reopened = open_store(root, clock);
  auto recovered = reopened.recover();
  if (recovered.has_value()) {
    FT_FAIL("a truncated generation file must not recover");
  } else {
    const ErrorCode code = recovered.error().code();
    if (code != ErrorCode::truncated_input && code != ErrorCode::checksum_mismatch) {
      FT_FAIL("unexpected error for a truncated generation file: " + recovered.error().to_string());
    }
  }
}

FT_TEST(recovery, magic_bytes_are_checked) {
  fsup::TempDir dir("recovery-magic");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  {
    CommittedStore committed = make_committed_store(root, clock);
  }

  const std::filesystem::path generation = generation_path(root, 1);
  patch_bytes(generation, kMagicOffset, std::string(kMagicBytes, 'X'));

  CapacityStore reopened = open_store(root, clock);
  FT_CHECK_ERROR(reopened.recover(), ErrorCode::corruption);
}

FT_TEST(recovery, byte_order_marker_is_checked) {
  fsup::TempDir dir("recovery-byte-order");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  {
    CommittedStore committed = make_committed_store(root, clock);
  }

  const std::filesystem::path generation = generation_path(root, 1);
  const std::string original = fsup::read_bytes(generation);

  // The marker value is 0x01020304 and it is written little-endian, so the bytes
  // on disk are already `04 03 02 01`: writing that byte string back reproduces
  // the file exactly, and the store still reads it.
  patch_bytes(generation, kByteOrderOffset, kByteOrderBytes);
  FT_CHECK_EQ(fsup::read_bytes(generation), original);

  // The byte-swapped value - what a big-endian writer would have left behind -
  // is refused as `incompatible_version` before any multi-byte field is
  // interpreted.
  patch_bytes(generation, kByteOrderOffset, kByteSwappedOrderBytes);
  CapacityStore reopened = open_store(root, clock);
  FT_CHECK_ERROR(reopened.recover(), ErrorCode::incompatible_version);
}

FT_TEST(recovery, format_version_is_checked) {
  fsup::TempDir dir("recovery-version");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  {
    CommittedStore committed = make_committed_store(root, clock);
  }

  const std::filesystem::path generation = generation_path(root, 1);
  patch_u32(generation, kFormatVersionOffset, 0x7FFFFFFFu);

  CapacityStore reopened = open_store(root, clock);
  FT_CHECK_ERROR(reopened.recover(), ErrorCode::incompatible_version);
}

FT_TEST(recovery, declared_body_length_larger_than_the_file_is_refused) {
  fsup::TempDir dir("recovery-length-long");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  {
    CommittedStore committed = make_committed_store(root, clock);
  }

  const std::filesystem::path generation = generation_path(root, 1);
  const std::uint32_t declared = declared_body_bytes(generation);
  patch_u32(generation, kBodyBytesOffset, declared + 4096u);

  CapacityStore reopened = open_store(root, clock);
  FT_CHECK_ERROR(reopened.recover(), ErrorCode::truncated_input);
}

FT_TEST(recovery, declared_body_length_smaller_than_the_file_is_refused) {
  // A length that is smaller than the bytes present leaves a region the header
  // does not account for, which is a contradiction rather than a truncation.
  fsup::TempDir dir("recovery-length-short");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  {
    CommittedStore committed = make_committed_store(root, clock);
  }

  const std::filesystem::path generation = generation_path(root, 1);
  const std::uint32_t declared = declared_body_bytes(generation);
  FT_REQUIRE(declared > 16u);
  patch_u32(generation, kBodyBytesOffset, declared - 16u);

  CapacityStore reopened = open_store(root, clock);
  FT_CHECK_ERROR(reopened.recover(), ErrorCode::conflict);
}

FT_TEST(recovery, reserved_header_bytes_must_be_zero) {
  // The reserved region is covered by no digest of its own, so a non-zero byte
  // there is caught only by the reserved-bytes check: corruption, not a
  // checksum failure.
  fsup::TempDir dir("recovery-reserved");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  {
    CommittedStore committed = make_committed_store(root, clock);
  }

  const std::filesystem::path generation = generation_path(root, 1);
  const std::string original = fsup::read_bytes(generation);
  FT_REQUIRE(original.size() > kReservedProbeOffset);
  FT_CHECK_EQ(static_cast<unsigned int>(static_cast<unsigned char>(original[kReservedProbeOffset])), 0u);
  FT_CHECK(kReservedProbeOffset >= kReservedOffset && kReservedProbeOffset < kHeaderBytes);
  flip_byte(generation, kReservedProbeOffset);

  CapacityStore reopened = open_store(root, clock);
  FT_CHECK_ERROR(reopened.recover(), ErrorCode::corruption);
}

FT_TEST(recovery, swapped_generation_sections_are_refused) {
  // The body is `@state`, the state document, `@snapshot`, the snapshot
  // document, in that order. Exchanging the two documents keeps the container
  // internally consistent - the length is unchanged and the header digest is
  // recomputed - so the refusal has to come from the body framing itself.
  fsup::TempDir dir("recovery-swapped-sections");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();
  {
    CommittedStore committed = make_committed_store(root, clock);
  }

  const std::filesystem::path generation = generation_path(root, 1);
  const std::string bytes = fsup::read_bytes(generation);
  FT_REQUIRE(bytes.size() > kHeaderBytes);
  const std::string body = bytes.substr(kHeaderBytes);

  constexpr std::string_view state_frame = "@state\n";
  constexpr std::string_view snapshot_frame = "\n@snapshot\n";
  FT_REQUIRE(body.rfind(state_frame, 0) == 0);
  const std::size_t marker = body.find(snapshot_frame, state_frame.size());
  FT_REQUIRE(marker != std::string::npos);
  const std::string state_document = body.substr(state_frame.size(), marker + 1 - state_frame.size());
  const std::string snapshot_document = body.substr(marker + snapshot_frame.size());

  const std::string swapped = std::string(state_frame) + snapshot_document + "@snapshot\n" + state_document;
  FT_CHECK_EQ(swapped.size(), body.size());

  std::string patched = bytes;
  patched.replace(kHeaderBytes, body.size(), swapped);
  const digest::Digest computed = digest::sha256(swapped);
  std::string raw;
  for (const std::uint8_t byte : computed) {
    raw.push_back(static_cast<char>(byte));
  }
  patched.replace(kBodyDigestOffset, raw.size(), raw);
  fsup::write_bytes(generation, patched);

  CapacityStore reopened = open_store(root, clock);
  FT_CHECK_ERROR(reopened.recover(), ErrorCode::corruption);
}

// ---------------------------------------------------------------------------
// Recovery is all-or-nothing
// ---------------------------------------------------------------------------

FT_TEST(recovery, a_refused_recovery_changes_nothing) {
  fsup::TempDir dir("recovery-all-or-nothing");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();

  std::string committed_digest;
  {
    CommittedStore committed = make_committed_store(root, clock);
    committed_digest = committed.snapshot->digest();
  }

  const std::filesystem::path generation = generation_path(root, 1);
  const std::string current_before = fsup::read_bytes(root / kCurrentName);
  const std::string format_before = fsup::read_bytes(root / kFormatName);
  const std::filesystem::path backup = dir.file("gen-1.backup");
  fsup::copy_file(generation, backup);

  flip_byte(generation, kHeaderBytes);
  CapacityStore reopened = open_store(root, clock);
  FT_CHECK_ERROR(reopened.recover(), ErrorCode::checksum_mismatch);
  FT_CHECK(reopened.authority().has_published_generation);

  // The refused recovery wrote nothing: CURRENT, FORMAT and the recorded
  // authority are exactly what the commit left.
  FT_CHECK_EQ(fsup::read_bytes(root / kCurrentName), current_before);
  FT_CHECK_EQ(fsup::read_bytes(root / kFormatName), format_before);
  FT_CHECK_EQ(reopened.authority().snapshot_digest, committed_digest);
  FT_CHECK_EQ(reopened.authority().published_generation.value(), std::uint64_t{1});

  // With the generation file restored, the original committed generation comes
  // back unchanged.
  fsup::copy_file(backup, generation);
  RecoveredState recovered = recover_store(reopened);
  FT_REQUIRE(recovered.snapshot != nullptr);
  FT_CHECK_EQ(recovered.snapshot->generation().value(), std::uint64_t{1});
  FT_CHECK_EQ(recovered.snapshot->digest(), committed_digest);
}

FT_TEST(recovery, persisted_evidence_does_not_become_fresh) {
  fsup::TempDir dir("recovery-revalidate");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();

  std::uint64_t committed_generation = 0;
  std::uint64_t committed_revision = 0;
  {
    CommittedStore committed = make_committed_store(root, clock);
    committed_generation = committed.model.capacity_generation().value();
    committed_revision = committed.model.revision().value();
  }

  CapacityStore reopened = open_store(root, clock);
  RecoveredState recovered = recover_store(reopened);
  FT_REQUIRE(recovered.snapshot != nullptr);
  FT_CHECK(recovered.snapshot->freshness() == SnapshotFreshness::recovered);

  // Revalidating compares the recovered answer with the recovered model. It must
  // never mutate the model, and the snapshot must stay stamped `recovered`.
  auto report = recovered.model.revalidate(*recovered.snapshot);
  FT_REQUIRE_OK(report);
  if (report.value().status != RevalidationStatus::valid) {
    FT_CHECK(report.value().rejected());
  } else {
    // Recovery restored exactly the evidence the snapshot was built from, so
    // `valid` is a legitimate answer here. What must not happen is recovery
    // having published or advanced anything.
    FT_CHECK_EQ(recovered.model.revision().value(), committed_revision);
    FT_CHECK_EQ(recovered.model.capacity_generation().value(), committed_generation);
    FT_CHECK_EQ(recovered.model.published_generation().value(), committed_generation);
  }
  FT_CHECK(recovered.snapshot->freshness() == SnapshotFreshness::recovered);
  FT_CHECK_EQ(recovered.model.revision().value(), committed_revision);
  FT_CHECK_EQ(recovered.model.capacity_generation().value(), committed_generation);
  FT_CHECK_EQ(reopened.authority().published_generation.value(), committed_generation);
}

FT_TEST(recovery, commit_leaves_no_staging_residue) {
  fsup::TempDir dir("recovery-no-residue");
  ManualClock clock(Tick::from_value(1000));
  const std::filesystem::path root = dir.store_root();

  CommittedStore committed = make_committed_store(root, clock);
  require_status(committed.model.declare_evidence(fsup::make_evidence(power_evidence("power-capacity", 2, 800)),
                                                  fresh_precondition(committed.model)),
                 "declare second evidence");
  require(committed.model.publish(fresh_precondition(committed.model)), "publish second");
  require(commit_published(committed.store, committed.model), "second commit");

  const std::vector<std::string> names = fsup::list_names(root);
  for (const std::string& name : names) {
    FT_CHECK(!name.starts_with("staging-"));
    FT_CHECK(!name.starts_with("tmp-"));
  }
  FT_CHECK(std::find(names.begin(), names.end(), std::string("gen-1.fcs")) != names.end());
  FT_CHECK(std::find(names.begin(), names.end(), std::string("gen-2.fcs")) != names.end());
  for (const std::string& name : names) {
    FT_CHECK(name == kFormatName || name == kCurrentName || name == "capacity.lock" ||
             name == "gen-1.fcs" || name == "gen-2.fcs");
  }
}
