// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Durable, integrity-checked, generation-addressed store.
//
// Lock order
// ----------
//   1. the store's advisory file lock (`capacity.lock`, byte zero)
//   2. the store's in-process state mutex, which guards only the cached
//      authority record
//
// The order is never reversed. The state mutex is held only for the few
// instructions that read or write the cached record; no file operation, no
// clock call and no caller callback ever runs while it is held. The file lock
// is taken once per operation, is never upgraded from shared to exclusive, and
// is released before the operation returns to the caller - except across
// `begin_commit`, where the exclusive lock is deliberately held by the pending
// commit until it publishes or is abandoned.

#include "dccp/facility_capacity/store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "dccp/facility_capacity/canonical.hpp"
#include "dccp/facility_capacity/digest.hpp"
#include "dccp/facility_capacity/limits.hpp"
#include "dccp/facility_capacity/provenance.hpp"
#include "dccp/facility_capacity/version.hpp"
#include "internal/canonical_io.hpp"
#include "internal/checked.hpp"
#include "internal/documents.hpp"
#include "internal/file_ops.hpp"
#include "internal/store_format.hpp"
#include "internal/text.hpp"

namespace dccp::facility_capacity {
namespace {

using internal::file_ops::LockMode;

constexpr std::string_view kFormatFileName = "FORMAT";
constexpr std::string_view kCurrentFileName = "CURRENT";
constexpr std::string_view kLockFileName = "capacity.lock";

Error io_error(std::string_view message) { return Error::make(ErrorCode::io_failure, message); }

Error corruption(std::string_view message) { return Error::make(ErrorCode::corruption, message); }

bool is_known_entry(std::string_view name) noexcept {
  return name == kFormatFileName || name == kCurrentFileName || name == kLockFileName ||
         internal::is_generation_file_name(name);
}

bool is_residue_entry(std::string_view name) noexcept {
  return internal::is_staging_file_name(name) ||
         (internal::starts_with(name, "tmp-") && name.size() > 4 &&
          name.substr(name.size() - 4) == ".tmp");
}

}  // namespace

std::string_view store_access_name(StoreAccess access) noexcept {
  switch (access) {
    case StoreAccess::reader:
      return "reader";
    case StoreAccess::writer:
      return "writer";
  }
  return "reader";
}

std::string StoreAuthority::to_string() const {
  std::string out = "store=";
  out.append(store_id.value());
  out.append(" facility=");
  out.append(facility.value());
  out.append(" site=");
  out.append(site.value());
  out.append(" controller=");
  out.append(controller.value());
  out.append(" epoch=");
  out.append(epoch.to_string());
  out.append(" incarnation=");
  out.append(incarnation.to_string());
  out.append(" generation=");
  out.append(published_generation.to_string());
  out.append(" revision=");
  out.append(revision.to_string());
  out.append(has_published_generation ? " published=" : " published=none");
  if (has_published_generation) {
    out.append(file_name);
  }
  return out;
}

std::string GenerationRecord::to_string() const {
  std::string out = file_name;
  out.append(" generation=");
  out.append(generation.to_string());
  out.append(" body_bytes=");
  out.append(std::to_string(body_bytes));
  out.append(" digest=");
  out.append(digest);
  out.append(" epoch=");
  out.append(epoch.to_string());
  out.append(" incarnation=");
  out.append(incarnation.to_string());
  out.append(referenced_by_current ? " current=yes" : " current=no");
  out.append(decodes ? " decodes=yes" : " decodes=no");
  return out;
}

std::string VerifyReport::describe() const {
  std::string out = ok ? "ok=1\n" : "ok=0\n";
  out.append("files_checked=");
  out.append(std::to_string(files_checked));
  out.push_back('\n');
  for (const std::string& finding : findings) {
    out.append("finding ");
    out.append(finding);
    out.push_back('\n');
  }
  for (const std::string& entry : residue) {
    out.append("residue ");
    out.append(entry);
    out.push_back('\n');
  }
  for (const GenerationRecord& record : generations) {
    out.append("generation ");
    out.append(record.to_string());
    out.push_back('\n');
  }
  return out;
}

std::string PruneReport::describe() const {
  std::string out = "removed=";
  out.append(std::to_string(removed));
  out.append(" retained=");
  out.append(std::to_string(retained));
  out.push_back('\n');
  for (const std::string& file : removed_files) {
    out.append("removed_file ");
    out.append(file);
    out.push_back('\n');
  }
  for (const std::string& finding : findings) {
    out.append("finding ");
    out.append(finding);
    out.push_back('\n');
  }
  return out;
}

RecoveredState::RecoveredState(std::shared_ptr<const CapacitySnapshot> snapshot_in,
                               FacilityCapacityModel model_in, StoreAuthority authority_in,
                               NoteSet notes_in)
    : snapshot(std::move(snapshot_in)),
      model(std::move(model_in)),
      authority(std::move(authority_in)),
      notes(std::move(notes_in)) {}

RecoveredState::RecoveredState(RecoveredState&&) noexcept = default;

RecoveredState& RecoveredState::operator=(RecoveredState&&) noexcept = default;

RecoveredState::~RecoveredState() = default;

// ---------------------------------------------------------------------------
// Store implementation
// ---------------------------------------------------------------------------

class CapacityStore::Impl {
 public:
  std::filesystem::path root;
  Clock* clock = nullptr;
  StoreAccess access = StoreAccess::writer;
  bool hold_writer_lock = false;
  bool lifetime_lock_held = false;
  internal::file_ops::FileLock lifetime_lock;
  internal::store_format::CurrentManifest current;
  StoreAuthority authority;
  mutable std::mutex state_mutex;
};

class PendingCommit::Impl {
 public:
  std::shared_ptr<CapacityStore::Impl> store;
  internal::file_ops::FileLock lock;
  internal::store_format::GenerationHeader header;
  std::string new_current_document;
  std::filesystem::path staging_path;
  std::filesystem::path generation_path;
  CapacityGeneration generation{};
  AttemptId attempt{};
  StoreAuthority authority;
  bool published = false;
  bool abandoned = false;
};

namespace {

/// Acquires the lock an operation needs, unless the handle already holds the
/// exclusive lock for its lifetime.
Result<internal::file_ops::FileLock> acquire_operation_lock(const std::filesystem::path& root,
                                                            bool lifetime_lock_held, LockMode mode) {
  if (lifetime_lock_held) {
    return internal::file_ops::FileLock();
  }
  return internal::file_ops::FileLock::acquire(root / kLockFileName, mode);
}

Result<internal::store_format::CurrentManifest> load_current(const std::filesystem::path& root) {
  const std::filesystem::path path = root / kCurrentFileName;
  if (!internal::file_ops::exists(path)) {
    Error error = Error::make(ErrorCode::not_found, "the store has no CURRENT manifest");
    error.with_constraint(std::string(kCurrentFileName));
    return error;
  }
  const Result<std::string> bytes = internal::file_ops::read_file(path, limits::max_manifest_bytes);
  if (!bytes.has_value()) {
    return bytes.error();
  }
  return internal::store_format::decode_current(bytes.value());
}

Result<internal::store_format::FormatManifest> load_format(const std::filesystem::path& root) {
  const std::filesystem::path path = root / kFormatFileName;
  if (!internal::file_ops::exists(path)) {
    Error error = Error::make(ErrorCode::not_found, "the directory does not hold a Facility Capacity store");
    error.with_constraint(std::string(kFormatFileName));
    return error;
  }
  const Result<std::string> bytes = internal::file_ops::read_file(path, limits::max_manifest_bytes);
  if (!bytes.has_value()) {
    return bytes.error();
  }
  return internal::store_format::decode_format(bytes.value());
}

StoreAuthority to_authority(const internal::store_format::CurrentManifest& manifest) {
  StoreAuthority authority;
  authority.store_id = manifest.store_id;
  authority.facility = manifest.facility;
  authority.site = manifest.site;
  authority.controller = manifest.controller;
  authority.epoch = manifest.epoch;
  authority.incarnation = manifest.incarnation;
  authority.published_generation = manifest.generation;
  authority.revision = manifest.revision;
  authority.has_published_generation = manifest.has_published_generation;
  authority.published_at = manifest.published_at;
  authority.snapshot_digest = manifest.snapshot_digest;
  authority.file_name = manifest.file_name;
  return authority;
}

/// The validation precedence of a store mutation: authority before generation
/// before revision, exactly as the model applies it.
///
/// A commit may be fenced with the precondition the caller captured either
/// before or after it published the model's next generation, because both name
/// a state the store can actually be in: the state it holds now, or the state
/// one publication ahead of it. `allow_next` admits the second form and is used
/// only by `begin_commit`; `advance_epoch` is fenced strictly against the state
/// the store currently holds. Anything else is refused, so a caller that is
/// genuinely behind — or ahead by more than one publication — is told so.
Status fence_authority(const internal::store_format::CurrentManifest& current,
                       const CapacityPrecondition& precondition) {
  if (precondition.expected_epoch != current.epoch) {
    Error error = Error::make(ErrorCode::stale_authority, "the control-plane epoch has moved on");
    error.with_generations(precondition.expected_epoch.value(), current.epoch.value());
    return error;
  }
  if (precondition.expected_incarnation != current.incarnation) {
    Error error = Error::make(ErrorCode::stale_authority, "the controller incarnation has been superseded");
    error.with_generations(precondition.expected_incarnation.value(), current.incarnation.value());
    return error;
  }
  return Status::success();
}

/// The whole fence for a commit.
///
/// Authority is checked first, exactly as the model checks it. The capacity
/// generation is then checked against the *snapshot being committed* rather
/// than against the precondition, because the snapshot is what the store is
/// about to publish: it must be exactly one generation ahead of the store, and
/// a precondition that claims anything other than the store's current
/// generation or that one is refused.
///
/// The precondition's revision token is fenced by the caller against the model
/// being committed, not against the store's own revision counter: the store's
/// revision is a property of the store, while the token describes the model.
///
/// The revision token is deliberately NOT compared with CURRENT.revision here,
/// because the model's revision counts model mutations and CURRENT.revision
/// counts store publications, so the two are different counters and comparing
/// them would refuse every legitimate commit made after a non-publishing
/// mutation. The generation continuity check above is the authoritative
/// staleness test for a commit.
Status fence_commit(const internal::store_format::CurrentManifest& current,
                    const CapacityPrecondition& precondition, CapacityGeneration snapshot_generation) {
  const Status authority = fence_authority(current, precondition);
  if (!authority.has_value()) {
    return authority.error();
  }
  const Result<std::uint64_t> next =
      internal::add_u64(current.generation.value(), 1u, "generation + 1 within uint64");
  if (!next.has_value()) {
    return next.error();
  }
  const std::uint64_t claimed = precondition.expected_capacity_generation.value();
  if (claimed != current.generation.value() && claimed != next.value()) {
    Error error = Error::make(ErrorCode::stale_generation, "the capacity generation has moved on");
    error.with_generations(claimed, current.generation.value());
    return error;
  }
  if (snapshot_generation.value() != next.value()) {
    Error error = Error::make(ErrorCode::stale_generation,
                              "the model's published generation is not the next store generation");
    error.with_generations(next.value(), snapshot_generation.value());
    return error;
  }
  return Status::success();
}

/// The whole fence for an authority advance: the precondition must name the
/// state the store is actually in.
Status fence_exact(const internal::store_format::CurrentManifest& current,
                   const CapacityPrecondition& precondition) {
  const Status authority = fence_authority(current, precondition);
  if (!authority.has_value()) {
    return authority.error();
  }
  if (precondition.expected_capacity_generation != current.generation) {
    Error error = Error::make(ErrorCode::stale_generation, "the capacity generation has moved on");
    error.with_generations(precondition.expected_capacity_generation.value(), current.generation.value());
    return error;
  }
  if (precondition.expected_revision != current.revision) {
    Error error = Error::make(ErrorCode::stale_generation, "the store revision has moved on");
    error.with_generations(precondition.expected_revision.value(), current.revision.value());
    return error;
  }
  return Status::success();
}

Result<void> write_current(const std::filesystem::path& root,
                           const internal::store_format::CurrentManifest& manifest) {
  const Result<std::string> encoded = internal::store_format::encode_current(manifest);
  if (!encoded.has_value()) {
    return encoded.error();
  }
  return internal::file_ops::atomic_write_file(root / kCurrentFileName, encoded.value());
}

/// Derives the store identity from the creation inputs.
///
/// The identity is a deterministic function of the facility, site, epoch,
/// incarnation, controller and creation instant, so it is reproducible from the
/// manifest's own fields and carries no machine-specific information. Two store
/// directories created from identical options at the same clock tick therefore
/// share an identity, which is the intended meaning of the value: it names the
/// logical store, not the directory. `StoreOpenOptions::expected_store_id`
/// checks that identity, and it is `FORMAT` plus `CURRENT` that bind a directory
/// to one store.
StoreId derive_store_id(const StoreCreateOptions& options, Tick created_at) {
  std::string material = options.facility.value();
  material.push_back('|');
  material.append(options.site.value());
  material.push_back('|');
  material.append(options.epoch.to_string());
  material.push_back('|');
  material.append(options.incarnation.to_string());
  material.push_back('|');
  material.append(options.controller.value());
  material.push_back('|');
  material.append(created_at.to_string());
  const std::string hex = digest::sha256_hex(material);
  const Result<StoreId> id = StoreId::parse("store-" + hex.substr(0, 24));
  if (!id.has_value()) {
    return StoreId();
  }
  return id.value();
}

}  // namespace

// ---------------------------------------------------------------------------
// PendingCommit
// ---------------------------------------------------------------------------

PendingCommit::PendingCommit(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

PendingCommit::~PendingCommit() {
  if (impl_ != nullptr && !impl_->published && !impl_->abandoned) {
    const Result<bool> removed = internal::file_ops::remove_file(impl_->staging_path);
    (void)removed;
  }
}

PendingCommit::PendingCommit(PendingCommit&&) noexcept = default;

PendingCommit& PendingCommit::operator=(PendingCommit&& other) noexcept {
  if (this != &other) {
    if (impl_ != nullptr && !impl_->published && !impl_->abandoned) {
      const Result<bool> removed = internal::file_ops::remove_file(impl_->staging_path);
      (void)removed;
    }
    impl_ = std::move(other.impl_);
  }
  return *this;
}

CapacityGeneration PendingCommit::generation() const { return impl_->generation; }

AttemptId PendingCommit::attempt() const { return impl_->attempt; }

const std::filesystem::path& PendingCommit::staging_path() const { return impl_->staging_path; }

const std::filesystem::path& PendingCommit::generation_path() const { return impl_->generation_path; }

bool PendingCommit::published() const { return impl_->published; }

Status PendingCommit::publish() {
  if (impl_->published) {
    return Status::success();
  }
  if (impl_->abandoned) {
    return Error::make(ErrorCode::precondition_failed, "the commit was abandoned and cannot be published");
  }

  const Result<void> installed =
      internal::file_ops::rename_replace(impl_->staging_path, impl_->generation_path);
  if (!installed.has_value()) {
    return installed.error();
  }
  const Result<void> directory_flushed = internal::file_ops::flush_directory(impl_->store->root);
  if (!directory_flushed.has_value()) {
    return directory_flushed.error();
  }

  // The commit point.
  const Result<void> pointer =
      internal::file_ops::atomic_write_file(impl_->store->root / kCurrentFileName, impl_->new_current_document);
  if (!pointer.has_value()) {
    return pointer.error();
  }
  const Result<void> pointer_flushed = internal::file_ops::flush_directory(impl_->store->root);
  if (!pointer_flushed.has_value()) {
    return pointer_flushed.error();
  }

  impl_->published = true;
  {
    std::lock_guard<std::mutex> guard(impl_->store->state_mutex);
    impl_->store->authority = impl_->authority;
    impl_->store->current.has_published_generation = true;
    impl_->store->current.generation = impl_->authority.published_generation;
    impl_->store->current.revision = impl_->authority.revision;
    impl_->store->current.published_at = impl_->authority.published_at;
    impl_->store->current.file_name = impl_->authority.file_name;
    impl_->store->current.snapshot_digest = impl_->authority.snapshot_digest;
    impl_->store->current.epoch = impl_->authority.epoch;
    impl_->store->current.incarnation = impl_->authority.incarnation;
  }
  return Status::success();
}

Status PendingCommit::abandon() {
  if (impl_->published) {
    return Error::make(ErrorCode::precondition_failed, "the commit is already published");
  }
  if (impl_->abandoned) {
    return Status::success();
  }
  const Result<bool> removed = internal::file_ops::remove_file(impl_->staging_path);
  if (!removed.has_value()) {
    return removed.error();
  }
  impl_->abandoned = true;
  return Status::success();
}

// ---------------------------------------------------------------------------
// CapacityStore
// ---------------------------------------------------------------------------

CapacityStore::CapacityStore(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

CapacityStore::~CapacityStore() = default;

CapacityStore::CapacityStore(CapacityStore&&) noexcept = default;

CapacityStore& CapacityStore::operator=(CapacityStore&&) noexcept = default;

std::string CapacityStore::generation_file_name(CapacityGeneration generation) {
  std::string name = "gen-";
  name.append(generation.to_string());
  name.append(".fcs");
  return name;
}

bool CapacityStore::is_generation_file_name(std::string_view name) noexcept {
  return internal::is_generation_file_name(name);
}

const std::filesystem::path& CapacityStore::root() const { return impl_->root; }

StoreAccess CapacityStore::access() const { return impl_->access; }

const StoreAuthority& CapacityStore::authority() const { return impl_->authority; }

Result<CapacityStore> CapacityStore::create(const std::filesystem::path& root,
                                            const StoreCreateOptions& options, Clock& clock) {
  if (options.facility.empty()) {
    return Error::make(ErrorCode::invalid_argument, "a store must name a facility");
  }
  if (options.site.empty()) {
    return Error::make(ErrorCode::invalid_argument, "a store must name a site");
  }
  if (options.controller.empty()) {
    return Error::make(ErrorCode::invalid_argument, "a store must name the controller that owns it");
  }
  if (options.epoch.is_zero()) {
    return Error::make(ErrorCode::invalid_argument, "a store must be created inside a non-zero epoch");
  }
  if (options.incarnation.is_zero()) {
    return Error::make(ErrorCode::invalid_argument, "a store must be created with a non-zero incarnation");
  }
  if (root.empty()) {
    return Error::make(ErrorCode::invalid_argument, "a store needs a directory");
  }
  for (const std::filesystem::path& component : root) {
    if (component == "..") {
      return Error::make(ErrorCode::path_rejected, "a store directory path may not contain a parent reference");
    }
  }
  if (internal::file_ops::is_link_like(root)) {
    return Error::make(ErrorCode::path_rejected, "a store directory may not be a symbolic link or reparse point");
  }
  if (internal::file_ops::exists(root) && !internal::file_ops::is_directory(root)) {
    return Error::make(ErrorCode::path_rejected, "the store path names something that is not a directory");
  }
  const std::filesystem::path format_path = root / kFormatFileName;
  if (internal::file_ops::exists(format_path)) {
    Error error = Error::make(ErrorCode::already_exists, "the directory already holds a store");
    error.with_constraint(std::string(kFormatFileName));
    return error;
  }

  const Result<void> created = internal::file_ops::create_directories(root);
  if (!created.has_value()) {
    return created.error();
  }
  const Result<Tick> now = clock.now();
  if (!now.has_value()) {
    return now.error();
  }

  const StoreId store_id = derive_store_id(options, now.value());
  if (store_id.empty()) {
    return Error::make(ErrorCode::invariant_violation, "a store identity could not be derived");
  }

  internal::store_format::FormatManifest format;
  format.store_id = store_id;
  format.facility = options.facility;
  format.site = options.site;
  format.controller = options.controller;
  format.created_at = now.value();
  const Result<std::string> format_document = internal::store_format::encode_format(format);
  if (!format_document.has_value()) {
    return format_document.error();
  }
  const Result<void> format_written =
      internal::file_ops::write_file_flushed(format_path, format_document.value());
  if (!format_written.has_value()) {
    return format_written.error();
  }

  internal::store_format::CurrentManifest current;
  current.store_id = store_id;
  current.facility = options.facility;
  current.site = options.site;
  current.controller = options.controller;
  current.epoch = options.epoch;
  current.incarnation = options.incarnation;
  current.generation = CapacityGeneration::from_value(0);
  current.revision = Revision::from_value(0);
  current.has_published_generation = false;
  current.published_at = now.value();
  const Result<void> current_written = write_current(root, current);
  if (!current_written.has_value()) {
    return current_written.error();
  }

  auto impl = std::make_shared<Impl>();
  impl->root = root;
  impl->clock = &clock;
  impl->access = StoreAccess::writer;
  impl->current = current;
  impl->authority = to_authority(current);
  return CapacityStore(std::move(impl));
}

Result<CapacityStore> CapacityStore::open(const std::filesystem::path& root,
                                          const StoreOpenOptions& options, Clock& clock) {
  if (root.empty()) {
    return Error::make(ErrorCode::invalid_argument, "a store needs a directory");
  }
  for (const std::filesystem::path& component : root) {
    if (component == "..") {
      return Error::make(ErrorCode::path_rejected, "a store directory path may not contain a parent reference");
    }
  }
  if (internal::file_ops::is_link_like(root)) {
    return Error::make(ErrorCode::path_rejected, "a store directory may not be a symbolic link or reparse point");
  }
  if (!internal::file_ops::is_directory(root)) {
    return Error::make(ErrorCode::not_found, "the store directory does not exist");
  }

  auto impl = std::make_shared<Impl>();
  impl->root = root;
  impl->clock = &clock;
  impl->access = options.access;
  impl->hold_writer_lock = options.hold_writer_lock && options.access == StoreAccess::writer;

  const Result<internal::store_format::FormatManifest> format = load_format(root);
  if (!format.has_value()) {
    return format.error();
  }
  if (options.expected_store_id.has_value() && options.expected_store_id.value() != format.value().store_id) {
    Error error = Error::make(ErrorCode::conflict, "the store identity does not match the expected identity");
    error.with_constraint("expected=" + options.expected_store_id.value().value() +
                          " actual=" + format.value().store_id.value());
    return error;
  }

  if (impl->hold_writer_lock) {
    Result<internal::file_ops::FileLock> lock =
        internal::file_ops::FileLock::acquire(root / kLockFileName, LockMode::Exclusive);
    if (!lock.has_value()) {
      return lock.error();
    }
    impl->lifetime_lock = std::move(lock.value());
    impl->lifetime_lock_held = true;
  }

  const LockMode mode = options.access == StoreAccess::writer ? LockMode::Exclusive : LockMode::Shared;
  Result<internal::file_ops::FileLock> guard = acquire_operation_lock(root, impl->lifetime_lock_held, mode);
  if (!guard.has_value()) {
    return guard.error();
  }
  Result<internal::store_format::CurrentManifest> current = load_current(root);
  if (!current.has_value()) {
    return current.error();
  }
  if (current.value().store_id != format.value().store_id) {
    Error error = Error::make(ErrorCode::conflict, "the CURRENT manifest names a different store");
    error.with_constraint("CURRENT.store_id == FORMAT.store_id");
    return error;
  }
  if (current.value().facility != format.value().facility || current.value().site != format.value().site) {
    Error error = Error::make(ErrorCode::conflict, "the CURRENT manifest contradicts the FORMAT manifest");
    error.with_constraint("CURRENT.facility == FORMAT.facility");
    return error;
  }
  impl->current = current.value();
  impl->authority = to_authority(current.value());
  return CapacityStore(std::move(impl));
}

Result<StoreAuthority> CapacityStore::refresh_authority() const {
  Result<internal::file_ops::FileLock> guard =
      acquire_operation_lock(impl_->root, impl_->lifetime_lock_held,
                              impl_->access == StoreAccess::writer ? LockMode::Exclusive : LockMode::Shared);
  if (!guard.has_value()) {
    return guard.error();
  }
  Result<internal::store_format::CurrentManifest> current = load_current(impl_->root);
  if (!current.has_value()) {
    return current.error();
  }
  std::lock_guard<std::mutex> state_guard(impl_->state_mutex);
  impl_->current = current.value();
  impl_->authority = to_authority(current.value());
  return impl_->authority;
}

Status CapacityStore::advance_epoch(EpochId new_epoch, IncarnationId new_incarnation,
                                    const ControllerId& controller,
                                    const CapacityPrecondition& precondition) {
  if (impl_->access != StoreAccess::writer) {
    return Error::make(ErrorCode::permission_denied, "the store was opened for reading");
  }
  if (controller.empty()) {
    return Error::make(ErrorCode::invalid_argument, "a controller identity is required");
  }
  if (new_incarnation.is_zero()) {
    return Error::make(ErrorCode::invalid_argument, "a controller incarnation is never zero");
  }
  Result<internal::file_ops::FileLock> guard = acquire_operation_lock(impl_->root, impl_->lifetime_lock_held, LockMode::Exclusive);
  if (!guard.has_value()) {
    return guard.error();
  }
  Result<internal::store_format::CurrentManifest> current = load_current(impl_->root);
  if (!current.has_value()) {
    return current.error();
  }
  const Status fenced = fence_exact(current.value(), precondition);
  if (!fenced.has_value()) {
    return fenced.error();
  }
  if (new_epoch < current.value().epoch) {
    Error error = Error::make(ErrorCode::invalid_argument, "the control-plane epoch cannot move backwards");
    error.with_generations(current.value().epoch.value(), new_epoch.value());
    return error;
  }
  if (new_epoch == current.value().epoch && new_incarnation == current.value().incarnation &&
      controller == current.value().controller) {
    return Error::make(ErrorCode::invalid_argument,
                       "advancing authority requires a greater epoch, a different incarnation or a different "
                       "controller");
  }
  const Result<Revision> next_revision = current.value().revision.next();
  if (!next_revision.has_value()) {
    return next_revision.error();
  }

  internal::store_format::CurrentManifest updated = current.value();
  updated.epoch = new_epoch;
  updated.incarnation = new_incarnation;
  updated.controller = controller;
  updated.revision = next_revision.value();
  const Result<void> written = write_current(impl_->root, updated);
  if (!written.has_value()) {
    return written.error();
  }
  const Result<void> flushed = internal::file_ops::flush_directory(impl_->root);
  if (!flushed.has_value()) {
    return flushed.error();
  }
  std::lock_guard<std::mutex> state_guard(impl_->state_mutex);
  impl_->current = updated;
  impl_->authority = to_authority(updated);
  return Status::success();
}

Result<RecoveredState> CapacityStore::recover() const {
  Result<internal::file_ops::FileLock> guard =
      acquire_operation_lock(impl_->root, impl_->lifetime_lock_held,
                              impl_->access == StoreAccess::writer ? LockMode::Exclusive : LockMode::Shared);
  if (!guard.has_value()) {
    return guard.error();
  }
  Result<internal::store_format::CurrentManifest> current = load_current(impl_->root);
  if (!current.has_value()) {
    return current.error();
  }
  if (!current.value().has_published_generation) {
    return Error::make(ErrorCode::not_found, "the store holds no published capacity generation");
  }

  const std::filesystem::path path = impl_->root / current.value().file_name;
  const Result<std::string> bytes =
      internal::file_ops::read_file(path, limits::max_store_body_bytes + store_header_size);
  if (!bytes.has_value()) {
    return bytes.error();
  }
  internal::store_format::GenerationHeader header;
  std::string_view body;
  const Result<void> decoded = internal::store_format::decode_generation_file(bytes.value(), header, body);
  if (!decoded.has_value()) {
    return decoded.error();
  }
  if (header.generation != current.value().generation) {
    Error error = Error::make(ErrorCode::conflict, "the generation file does not hold the published generation");
    error.with_generations(current.value().generation.value(), header.generation.value());
    return error;
  }
  // `CURRENT.snapshot_digest` is the content digest of the published capacity
  // answer, which is the snapshot document's own digest. The body digest in the
  // binary header is a different value - it covers the framing and the model
  // state as well - and was already verified while the header was decoded.

  std::string_view state_document;
  std::string_view snapshot_document;
  const Result<void> split = canonical::split_generation_body(body, state_document, snapshot_document);
  if (!split.has_value()) {
    return split.error();
  }

  Result<FacilityCapacityModel> model = FacilityCapacityModel::decode(state_document, *impl_->clock);
  if (!model.has_value()) {
    return model.error();
  }
  Result<std::shared_ptr<const CapacitySnapshot>> snapshot =
      canonical::decode_snapshot(snapshot_document, SnapshotFreshness::recovered);
  if (!snapshot.has_value()) {
    return snapshot.error();
  }

  if (model.value().facility() != current.value().facility || model.value().site() != current.value().site) {
    return Error::make(ErrorCode::conflict, "the recovered state describes a different facility or site");
  }
  if (model.value().capacity_generation() != header.generation) {
    Error error = Error::make(ErrorCode::conflict,
                              "the recovered state and the generation header disagree about the generation");
    error.with_generations(header.generation.value(), model.value().capacity_generation().value());
    return error;
  }
  if (snapshot.value()->digest() != current.value().snapshot_digest) {
    return Error::make(ErrorCode::checksum_mismatch,
                       "the recovered snapshot does not hash to the published digest");
  }
  if (snapshot.value()->generation() != header.generation) {
    Error error = Error::make(ErrorCode::conflict,
                              "the recovered snapshot and the generation header disagree about the generation");
    error.with_generations(header.generation.value(), snapshot.value()->generation().value());
    return error;
  }

  NoteSet notes;
  (void)notes.add(CapacityNote(ReasonCode::recovered_not_revalidated, CapacityDimension::space,
                               CapacitySourceId(),
                               "the state was recovered from durable storage and must be revalidated "
                               "before it is relied on"));
  return RecoveredState(snapshot.value(), std::move(model.value()), to_authority(current.value()),
                        std::move(notes));
}

Result<PendingCommit> CapacityStore::begin_commit(const FacilityCapacityModel& model,
                                                  const CapacityPrecondition& precondition) {
  if (impl_->access != StoreAccess::writer) {
    return Error::make(ErrorCode::permission_denied, "the store was opened for reading");
  }
  Result<internal::file_ops::FileLock> guard = acquire_operation_lock(impl_->root, impl_->lifetime_lock_held, LockMode::Exclusive);
  if (!guard.has_value()) {
    return guard.error();
  }
  Result<internal::store_format::CurrentManifest> current = load_current(impl_->root);
  if (!current.has_value()) {
    return current.error();
  }
  if (model.facility() != current.value().facility || model.site() != current.value().site) {
    return Error::make(ErrorCode::conflict, "the model describes a different facility or site than the store");
  }
  if (model.epoch() < current.value().epoch) {
    Error error = Error::make(ErrorCode::stale_authority, "the model is behind the store's control-plane epoch");
    error.with_generations(model.epoch().value(), current.value().epoch.value());
    return error;
  }
  if (model.epoch() == current.value().epoch && model.incarnation() != current.value().incarnation) {
    Error error = Error::make(ErrorCode::stale_authority,
                              "the model's controller incarnation has been superseded");
    error.with_generations(model.incarnation().value(), current.value().incarnation.value());
    return error;
  }


  const std::shared_ptr<const CapacitySnapshot> snapshot = model.current_snapshot();
  if (snapshot == nullptr) {
    return Error::make(ErrorCode::precondition_failed,
                       "the model has no published snapshot to commit; publish it first");
  }
  if (snapshot->valid_until() < snapshot->built_at()) {
    return Error::make(ErrorCode::invariant_violation, "the snapshot validity window is inverted");
  }

  const Status fenced = fence_commit(current.value(), precondition, snapshot->generation());
  if (!fenced.has_value()) {
    return fenced.error();
  }
  // The revision token is fenced against the model that is being committed, not
  // against the store's own revision counter: the store's revision is a property
  // of the store, while the token the caller holds describes the model. The two
  // agree exactly when the caller captured the precondition from the model it is
  // committing, and any other value is a stale or invented token and is refused.
  //
  // This check runs last so that validation precedence holds: authority is
  // reported before generation, and generation before revision, exactly as the
  // model reports it.
  if (precondition.expected_revision != model.revision()) {
    Error error = Error::make(ErrorCode::stale_generation, "the model revision has moved on");
    error.with_generations(precondition.expected_revision.value(), model.revision().value());
    return error;
  }

  const Result<std::string> state_document = model.encode();
  if (!state_document.has_value()) {
    return state_document.error();
  }
  const std::string body = canonical::join_generation_body(state_document.value(),
                                                           canonical::encode_snapshot(*snapshot));

  internal::store_format::GenerationHeader header;
  header.format_version = store_format_version;
  header.generation = snapshot->generation();
  header.epoch = model.epoch();
  header.incarnation = model.incarnation();
  header.published_at = snapshot->built_at();
  header.body_bytes = static_cast<std::uint32_t>(body.size());
  header.body_digest = digest::sha256_hex(body);

  const Result<std::string> file_bytes = internal::store_format::encode_generation_file(header, body);
  if (!file_bytes.has_value()) {
    return file_bytes.error();
  }

  auto pending = std::make_shared<PendingCommit::Impl>();
  pending->store = impl_;
  if (guard.value().held()) {
    pending->lock = std::move(guard.value());
  }
  pending->header = header;
  pending->generation = snapshot->generation();
  pending->attempt = precondition.attempt;
  pending->generation_path = impl_->root / generation_file_name(snapshot->generation());
  pending->staging_path = impl_->root / ("staging-" + internal::file_ops::process_id_token() + "-" +
                                         internal::file_ops::next_sequence_token() + ".tmp");

  const Result<void> staged = internal::file_ops::write_file_flushed(pending->staging_path, file_bytes.value());
  if (!staged.has_value()) {
    return staged.error();
  }

  // Verify the staged bytes by re-reading and re-deriving them before the
  // commit point. A staging file that does not reproduce is never published.
  const Result<std::string> readback = internal::file_ops::read_file(pending->staging_path,
                                                                     limits::max_store_body_bytes +
                                                                         store_header_size);
  if (!readback.has_value()) {
    (void)internal::file_ops::remove_file(pending->staging_path);
    return readback.error();
  }
  internal::store_format::GenerationHeader verify_header;
  std::string_view verify_body;
  const Result<void> verify_decoded =
      internal::store_format::decode_generation_file(readback.value(), verify_header, verify_body);
  if (!verify_decoded.has_value()) {
    (void)internal::file_ops::remove_file(pending->staging_path);
    return verify_decoded.error();
  }
  if (verify_body != body) {
    (void)internal::file_ops::remove_file(pending->staging_path);
    return corruption("the staged generation body does not reproduce from the bytes on disk");
  }
  std::string_view verify_state;
  std::string_view verify_snapshot;
  const Result<void> verify_split = canonical::split_generation_body(verify_body, verify_state, verify_snapshot);
  if (!verify_split.has_value()) {
    (void)internal::file_ops::remove_file(pending->staging_path);
    return verify_split.error();
  }
  Result<std::shared_ptr<const CapacitySnapshot>> verify_snapshot_value =
      canonical::decode_snapshot(verify_snapshot, snapshot->freshness());
  if (!verify_snapshot_value.has_value()) {
    (void)internal::file_ops::remove_file(pending->staging_path);
    return verify_snapshot_value.error();
  }
  if (verify_snapshot_value.value()->digest() != snapshot->digest()) {
    (void)internal::file_ops::remove_file(pending->staging_path);
    return Error::make(ErrorCode::checksum_mismatch,
                       "the staged snapshot does not hash to the snapshot being committed");
  }

  internal::store_format::CurrentManifest updated = current.value();
  updated.epoch = model.epoch();
  updated.incarnation = model.incarnation();
  updated.generation = snapshot->generation();
  updated.revision = model.revision();
  updated.has_published_generation = true;
  updated.published_at = snapshot->built_at();
  updated.file_name = generation_file_name(snapshot->generation());
  updated.snapshot_digest = snapshot->digest();
  const Result<std::string> current_document = internal::store_format::encode_current(updated);
  if (!current_document.has_value()) {
    (void)internal::file_ops::remove_file(pending->staging_path);
    return current_document.error();
  }
  pending->new_current_document = current_document.value();
  pending->authority = to_authority(updated);

  return PendingCommit(std::move(pending));
}

Result<StoreAuthority> CapacityStore::commit(const FacilityCapacityModel& model,
                                             const CapacityPrecondition& precondition) {
  Result<PendingCommit> pending = begin_commit(model, precondition);
  if (!pending.has_value()) {
    return pending.error();
  }
  const Status published = pending.value().publish();
  if (!published.has_value()) {
    return published.error();
  }
  return impl_->authority;
}

Result<VerifyReport> CapacityStore::verify(bool deep) const {
  VerifyReport report;
  Result<internal::file_ops::FileLock> guard =
      acquire_operation_lock(impl_->root, impl_->lifetime_lock_held,
                              impl_->access == StoreAccess::writer ? LockMode::Exclusive : LockMode::Shared);
  if (!guard.has_value()) {
    return guard.error();
  }

  const Result<internal::store_format::FormatManifest> format = load_format(impl_->root);
  if (!format.has_value()) {
    report.findings.push_back("FORMAT: " + format.error().to_string());
  } else {
    ++report.files_checked;
  }
  Result<internal::store_format::CurrentManifest> current = load_current(impl_->root);
  if (!current.has_value()) {
    report.findings.push_back("CURRENT: " + current.error().to_string());
  } else {
    ++report.files_checked;
    if (format.has_value() && current.value().store_id != format.value().store_id) {
      report.findings.push_back("CURRENT: store identity contradicts FORMAT");
    }
  }

  const Result<std::vector<std::string>> entries = internal::file_ops::list_directory(impl_->root);
  if (!entries.has_value()) {
    return entries.error();
  }

  std::vector<CapacityGeneration> generations;
  for (const std::string& name : entries.value()) {
    if (is_known_entry(name) || internal::file_ops::is_directory(impl_->root / name)) {
      continue;
    }
    if (is_residue_entry(name)) {
      report.residue.push_back(name);
      continue;
    }
    if (!internal::is_generation_file_name(name)) {
      report.findings.push_back(name + ": an entry this product does not create");
    }
  }

  for (const std::string& name : entries.value()) {
    if (!internal::is_generation_file_name(name)) {
      continue;
    }
    const Result<CapacityGeneration> generation = CapacityGeneration::parse(
        std::string_view(name).substr(4, name.size() - 8));
    if (!generation.has_value()) {
      report.findings.push_back(name + ": the generation number is not representable");
      continue;
    }
    generations.push_back(generation.value());
  }
  std::sort(generations.begin(), generations.end());

  for (const CapacityGeneration generation : generations) {
    GenerationRecord record;
    record.generation = generation;
    record.file_name = generation_file_name(generation);
    const std::filesystem::path path = impl_->root / record.file_name;
    const Result<std::string> bytes =
        internal::file_ops::read_file(path, limits::max_store_body_bytes + store_header_size);
    if (!bytes.has_value()) {
      report.findings.push_back(record.file_name + ": " + bytes.error().to_string());
      report.generations.push_back(std::move(record));
      continue;
    }
    ++report.files_checked;
    internal::store_format::GenerationHeader header;
    std::string_view body;
    const Result<void> decoded =
        internal::store_format::decode_generation_file(bytes.value(), header, body);
    if (!decoded.has_value()) {
      report.findings.push_back(record.file_name + ": " + decoded.error().to_string());
      report.generations.push_back(std::move(record));
      continue;
    }
    record.decodes = true;
    record.body_bytes = header.body_bytes;
    record.digest = header.body_digest;
    record.published_at = header.published_at;
    record.epoch = header.epoch;
    record.incarnation = header.incarnation;
    if (header.generation != generation) {
      report.findings.push_back(record.file_name + ": the header names a different generation");
      record.decodes = false;
    }
    if (current.has_value()) {
      record.referenced_by_current = current.value().has_published_generation &&
                                     current.value().file_name == record.file_name;
      if (record.referenced_by_current) {
        if (current.value().generation != generation) {
          report.findings.push_back(record.file_name + ": CURRENT names this file for a different generation");
        }
        if (format.has_value() && current.value().store_id != format.value().store_id) {
          report.findings.push_back(record.file_name + ": the store identity contradicts FORMAT");
        }
      }
    }
    if ((deep || record.referenced_by_current) && record.decodes) {
      std::string_view state_document;
      std::string_view snapshot_document;
      const Result<void> split = canonical::split_generation_body(body, state_document, snapshot_document);
      if (!split.has_value()) {
        report.findings.push_back(record.file_name + ": " + split.error().to_string());
        record.decodes = false;
      } else {
        Result<FacilityCapacityModel> model = FacilityCapacityModel::decode(state_document, *impl_->clock);
        if (!model.has_value()) {
          report.findings.push_back(record.file_name + " state: " + model.error().to_string());
          record.decodes = false;
        }
        Result<std::shared_ptr<const CapacitySnapshot>> snapshot =
            canonical::decode_snapshot(snapshot_document, SnapshotFreshness::recovered);
        if (!snapshot.has_value()) {
          report.findings.push_back(record.file_name + " snapshot: " + snapshot.error().to_string());
          record.decodes = false;
        } else if (record.referenced_by_current && current.has_value()) {
          // CURRENT.snapshot_digest is the content digest of the published
          // capacity answer, which is the snapshot document's own digest.
          record.digest = snapshot.value()->digest();
          if (current.value().snapshot_digest != record.digest) {
            report.findings.push_back(record.file_name + ": CURRENT records a different snapshot digest");
          }
        }
      }
    }
    report.generations.push_back(std::move(record));
  }

  if (current.has_value() && current.value().has_published_generation) {
    const bool present = std::any_of(report.generations.begin(), report.generations.end(),
                                     [&current](const GenerationRecord& record) {
                                       return record.file_name == current.value().file_name;
                                     });
    if (!present) {
      report.findings.push_back("CURRENT names a generation file that is not present");
    }
  }

  std::sort(report.findings.begin(), report.findings.end());
  std::sort(report.residue.begin(), report.residue.end());
  report.ok = report.findings.empty();
  return report;
}

Result<std::vector<GenerationRecord>> CapacityStore::list_generations() const {
  Result<VerifyReport> report = verify(false);
  if (!report.has_value()) {
    return report.error();
  }
  return report.value().generations;
}

Result<PruneReport> CapacityStore::prune(std::size_t keep_newest) {
  if (impl_->access != StoreAccess::writer) {
    return Error::make(ErrorCode::permission_denied, "the store was opened for reading");
  }
  Result<internal::file_ops::FileLock> guard = acquire_operation_lock(impl_->root, impl_->lifetime_lock_held, LockMode::Exclusive);
  if (!guard.has_value()) {
    return guard.error();
  }
  const Result<std::vector<std::string>> entries = internal::file_ops::list_directory(impl_->root);
  if (!entries.has_value()) {
    return entries.error();
  }
  const Result<internal::store_format::CurrentManifest> current = load_current(impl_->root);

  PruneReport report;
  std::vector<std::string> generations;
  for (const std::string& name : entries.value()) {
    if (internal::is_generation_file_name(name)) {
      generations.push_back(name);
    } else if (is_residue_entry(name)) {
      const Result<bool> removed = internal::file_ops::remove_file(impl_->root / name);
      if (!removed.has_value()) {
        return removed.error();
      }
      if (removed.value()) {
        ++report.removed;
        report.removed_files.push_back(name);
      }
    } else if (!is_known_entry(name)) {
      report.findings.push_back(name + ": retained, this product did not create it");
    }
  }

  // Newest first. The generation file CURRENT references is never removed.
  std::sort(generations.begin(), generations.end(), [](const std::string& lhs, const std::string& rhs) {
    const std::size_t lhs_length = lhs.size();
    const std::size_t rhs_length = rhs.size();
    if (lhs_length != rhs_length) {
      return lhs_length > rhs_length;
    }
    return lhs > rhs;
  });

  std::size_t retained = 0;
  for (const std::string& name : generations) {
    const bool referenced = current.has_value() && current.value().has_published_generation &&
                            current.value().file_name == name;
    if (referenced) {
      ++retained;
      continue;
    }
    if (retained < keep_newest) {
      ++retained;
      continue;
    }
    const Result<bool> removed = internal::file_ops::remove_file(impl_->root / name);
    if (!removed.has_value()) {
      return removed.error();
    }
    if (removed.value()) {
      ++report.removed;
      report.removed_files.push_back(name);
    }
  }
  report.retained = retained;
  std::sort(report.removed_files.begin(), report.removed_files.end());
  std::sort(report.findings.begin(), report.findings.end());
  return report;
}

Status CapacityStore::destroy(const std::filesystem::path& root) {
  if (root.empty()) {
    return Error::make(ErrorCode::invalid_argument, "a store needs a directory");
  }
  if (internal::file_ops::is_link_like(root)) {
    return Error::make(ErrorCode::path_rejected, "a store directory may not be a symbolic link or reparse point");
  }
  if (!internal::file_ops::is_directory(root)) {
    return Error::make(ErrorCode::not_found, "the store directory does not exist");
  }
  const Result<internal::store_format::FormatManifest> format = load_format(root);
  if (!format.has_value()) {
    return format.error();
  }
  Result<std::vector<std::string>> entries = internal::file_ops::list_directory(root);
  if (!entries.has_value()) {
    return entries.error();
  }
  for (const std::string& name : entries.value()) {
    if (!is_known_entry(name) && !is_residue_entry(name)) {
      Error error = Error::make(ErrorCode::conflict,
                                "the store directory holds an entry this product did not create");
      error.with_constraint(name);
      return error;
    }
  }
  for (const std::string& name : entries.value()) {
    const std::filesystem::path path = root / name;
    if (internal::file_ops::is_directory(path)) {
      continue;
    }
    const Result<bool> removed = internal::file_ops::remove_file(path);
    if (!removed.has_value()) {
      return removed.error();
    }
  }
  std::error_code code;
  std::filesystem::remove(root, code);
  if (code) {
    return io_error("could not remove the store directory");
  }
  return Status::success();
}

}  // namespace dccp::facility_capacity
