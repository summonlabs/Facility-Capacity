// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Durable, integrity-checked, generation-addressed storage.
//
// Layout of a store directory:
//
//   FORMAT            immutable manifest: store identity, facility identity,
//                     format version, byte-order marker, creation instant
//   CURRENT           mutable manifest: control-plane authority (epoch,
//                     incarnation, controller), published generation, revision
//                     and the name and digest of the generation file it names
//   capacity.lock     advisory lock file; byte range zero carries the store
//                     lock, shared for readers and exclusive for writers
//   gen-<n>.fcs       a published generation: fixed binary header plus a
//                     canonical body holding the whole authoritative state
//                     (model state document and snapshot document)
//   staging-*.tmp     a commit that has not been published yet
//
// Commit protocol, in order:
//   plan -> validate -> reserve generation and attempt -> write staging ->
//   flush staging -> verify staging by re-reading and re-deriving -> publish
//   the generation file by atomic replacement -> flush the directory -> update
//   CURRENT by atomic replacement -> flush the directory -> retire staging.
//
// The commit point is the replacement of CURRENT. Before it, a crash leaves the
// previous generation authoritative and at most an unreferenced generation file
// or a staging file behind. After it, the new generation is authoritative. There
// is no window in which a reader can observe a mixture of the two, because
// CURRENT is replaced atomically and the generation file it names was already
// complete, flushed and verified.

#ifndef DCCP_FACILITY_CAPACITY_STORE_HPP
#define DCCP_FACILITY_CAPACITY_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dccp/facility_capacity/clock.hpp"
#include "dccp/facility_capacity/constraint.hpp"
#include "dccp/facility_capacity/model.hpp"
#include "dccp/facility_capacity/result.hpp"
#include "dccp/facility_capacity/snapshot.hpp"
#include "dccp/facility_capacity/strong_id.hpp"

namespace dccp::facility_capacity {

/// How a process holds the store.
enum class StoreAccess : std::uint8_t {
  /// Shared lock: many readers may hold it, no writer may.
  reader = 0,
  /// Exclusive lock: one writer at a time.
  writer = 1,
};

std::string_view store_access_name(StoreAccess access) noexcept;

/// Options for creating a store directory.
///
/// A store records identity and authority and nothing else. The coverage
/// contract and the snapshot validity window belong to the model, and they
/// become durable when the model's first generation is committed, because the
/// whole model state is what a generation carries.
struct StoreCreateOptions {
  FacilityId facility;
  SiteId site;
  EpochId epoch;
  IncarnationId incarnation;
  ControllerId controller;
};

/// Options for opening an existing store directory.
struct StoreOpenOptions {
  StoreAccess access = StoreAccess::writer;
  /// When true the exclusive lock is held for the lifetime of the handle,
  /// which is what a long-running controller wants. When false, each mutating
  /// operation takes the exclusive lock for its own duration and each read
  /// takes the shared lock. Neither mode ever upgrades a held lock.
  bool hold_writer_lock = false;
  /// When set, opening a store whose recorded store identity differs fails
  /// with `conflict` instead of silently adopting it.
  std::optional<StoreId> expected_store_id;
};

/// The durable authority of a store.
struct StoreAuthority {
  StoreId store_id;
  FacilityId facility;
  SiteId site;
  ControllerId controller;
  EpochId epoch;
  IncarnationId incarnation;
  CapacityGeneration published_generation{};
  Revision revision{};
  bool has_published_generation = false;
  Tick published_at{};
  /// Content digest of the published capacity answer. This is the digest that
  /// `CapacitySnapshot::digest()` reports, and it is the value a reader checks
  /// after recovery, so an answer and its authority are bound to one another.
  std::string snapshot_digest;
  std::string file_name;

  std::string to_string() const;
};

/// One file in the store.
struct GenerationRecord {
  CapacityGeneration generation{};
  std::string file_name;
  std::uint64_t body_bytes = 0;
  /// Integrity digest of the file body as recorded in its binary header. For
  /// the generation CURRENT references, `verify` replaces this with the content
  /// digest of the published snapshot, which is the value CURRENT records.
  std::string digest;
  Tick published_at{};
  EpochId epoch{};
  IncarnationId incarnation{};
  bool referenced_by_current = false;
  bool decodes = false;

  std::string to_string() const;
};

/// Result of a store inspection. Inspection is at least as strict as the normal
/// open path: it performs every check open performs and more.
struct VerifyReport {
  bool ok = false;
  std::uint64_t files_checked = 0;
  std::vector<GenerationRecord> generations;
  /// Stable machine-readable findings, sorted.
  std::vector<std::string> findings;
  /// Files that are not part of the committed state: unreferenced generation
  /// files and abandoned staging files.
  std::vector<std::string> residue;

  std::string describe() const;
};

struct PruneReport {
  std::size_t removed = 0;
  std::size_t retained = 0;
  std::vector<std::string> removed_files;
  std::vector<std::string> findings;

  std::string describe() const;
};

/// The whole authoritative state recovered from a store.
struct RecoveredState {
  /// The published snapshot, always stamped `SnapshotFreshness::recovered`.
  std::shared_ptr<const CapacitySnapshot> snapshot;
  /// The model state that accompanied it. One whole state, never a mixture.
  FacilityCapacityModel model;
  StoreAuthority authority;
  /// Findings raised while recovering, including
  /// `recovered_not_revalidated`.
  NoteSet notes;

  RecoveredState(std::shared_ptr<const CapacitySnapshot> snapshot_in, FacilityCapacityModel model_in,
                 StoreAuthority authority_in, NoteSet notes_in);
  RecoveredState(RecoveredState&&) noexcept;
  RecoveredState& operator=(RecoveredState&&) noexcept;
  RecoveredState(const RecoveredState&) = delete;
  RecoveredState& operator=(const RecoveredState&) = delete;
  ~RecoveredState();
};

/// A commit that has been staged, flushed and verified but not yet published.
///
/// The store's commit point is `publish()`. Destroying an unpublished commit
/// abandons it, removing the staging file. A process that dies between
/// `begin_commit` and `publish` therefore leaves the previously published
/// generation authoritative and at most one staging file behind, which
/// `CapacityStore::verify` reports as residue and `prune` removes.
class PendingCommit {
 public:
  PendingCommit() = delete;
  ~PendingCommit();

  PendingCommit(PendingCommit&&) noexcept;
  PendingCommit& operator=(PendingCommit&&) noexcept;
  PendingCommit(const PendingCommit&) = delete;
  PendingCommit& operator=(const PendingCommit&) = delete;

  /// The generation this commit will publish.
  CapacityGeneration generation() const;

  /// The attempt token this commit reserved.
  AttemptId attempt() const;

  /// The staging file, valid until publish or abandon.
  const std::filesystem::path& staging_path() const;

  /// The generation file this commit will install.
  const std::filesystem::path& generation_path() const;

  bool published() const;

  /// Publishes: atomically installs the generation file and then atomically
  /// replaces CURRENT, flushing the directory after each step.
  Status publish();

  /// Removes the staging file. Idempotent.
  Status abandon();

 private:
  friend class CapacityStore;
  class Impl;
  explicit PendingCommit(std::shared_ptr<Impl> impl) noexcept;
  std::shared_ptr<Impl> impl_;
};

/// A durable store of published capacity generations.
class CapacityStore {
 public:
  CapacityStore() = delete;
  ~CapacityStore();

  CapacityStore(CapacityStore&&) noexcept;
  CapacityStore& operator=(CapacityStore&&) noexcept;
  CapacityStore(const CapacityStore&) = delete;
  CapacityStore& operator=(const CapacityStore&) = delete;

  /// Creates a new store.
  ///
  /// Fails with `already_exists` when the directory already holds a store, and
  /// with `path_rejected` when the path names a symbolic link, a non-directory
  /// or a directory outside an accepted shape.
  static Result<CapacityStore> create(const std::filesystem::path& root, const StoreCreateOptions& options,
                                      Clock& clock);

  /// Opens an existing store.
  static Result<CapacityStore> open(const std::filesystem::path& root, const StoreOpenOptions& options,
                                    Clock& clock);

  const std::filesystem::path& root() const;
  StoreAccess access() const;

  /// The authority as recorded by CURRENT.
  ///
  /// The reference stays valid for the lifetime of the handle and is refreshed
  /// by `open`, `create`, `refresh_authority`, `advance_epoch` and `commit`.
  /// It is not synchronised against a concurrent commit from another thread:
  /// the store's concurrency guarantee is per operation, and a caller that
  /// shares one handle between threads should call `refresh_authority` itself.
  const StoreAuthority& authority() const;

  /// Re-reads CURRENT under the shared lock and returns it.
  Result<StoreAuthority> refresh_authority() const;

  /// Advances the control-plane epoch and controller incarnation.
  ///
  /// The new epoch must be strictly greater than the recorded one. After this
  /// succeeds, any process that still holds the previous epoch is refused with
  /// `stale_authority` when it next tries to commit, even though it holds no
  /// lock while it waits. This is the mechanism that makes authority survive a
  /// process boundary.
  Status advance_epoch(EpochId new_epoch, IncarnationId new_incarnation, const ControllerId& controller,
                       const CapacityPrecondition& precondition);

  /// Reads, verifies and decodes the published generation.
  ///
  /// Fails with `not_found` when no generation has been published.
  Result<RecoveredState> recover() const;

  /// Stages, flushes and verifies a commit without publishing it.
  ///
  /// The model must already have published the generation this commit writes,
  /// and that generation must be exactly one ahead of the store's. The
  /// precondition may be the one captured before the model published or the one
  /// captured after; both name a state the store can be in, and anything else
  /// is refused with `stale_generation` or `stale_authority`.
  Result<PendingCommit> begin_commit(const FacilityCapacityModel& model, const CapacityPrecondition& precondition);

  /// Stages, publishes and returns the new authority in one call.
  ///
  /// See `begin_commit` for the accepted precondition. The returned authority is
  /// the state that is durable when this call returns.
  Result<StoreAuthority> commit(const FacilityCapacityModel& model, const CapacityPrecondition& precondition);

  /// Inspects the store. When `deep` is true every generation file present is
  /// decoded, not only the one CURRENT names.
  Result<VerifyReport> verify(bool deep) const;

  Result<std::vector<GenerationRecord>> list_generations() const;

  /// Retains the `keep_newest` most recent generation files, never including
  /// the one CURRENT names, and removes abandoned staging files.
  Result<PruneReport> prune(std::size_t keep_newest);

  /// Removes a store directory created by this product.
  ///
  /// Refuses with `conflict` when the directory contains an entry that this
  /// product does not create, so it can never be used to delete unrelated
  /// files.
  static Status destroy(const std::filesystem::path& root);

  /// The file name a generation is stored under, for a given generation.
  static std::string generation_file_name(CapacityGeneration generation);

  /// True when `name` is exactly a generation file name: `gen-<digits>.fcs`.
  ///
  /// Exposed so that an operator tool, and this product's own tests, can apply
  /// the same path-safety predicate the store applies when it reads a file name
  /// out of `CURRENT`. A name that fails this test is never opened.
  static bool is_generation_file_name(std::string_view name) noexcept;

 private:
  friend class PendingCommit;
  class Impl;
  explicit CapacityStore(std::shared_ptr<Impl> impl) noexcept;
  std::shared_ptr<Impl> impl_;
};

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_STORE_HPP
