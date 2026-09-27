// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Multiprocess suite: real operating-system processes, never threads.
//
// Every case here re-enters this same test executable as `--child <name>`. The
// child's standard output and standard error go to a file inside the case's
// temporary directory, never to a pipe, so a chatty child can never deadlock a
// parent that is waiting for it. Readiness is observed with
// `ChildProcess::wait_for_marker`, a bounded poll with a definite failure
// outcome, and every child has a definite, self-terminating job: a child that
// must hold state open briefly polls for a sentinel file a bounded number of
// times and then exits by itself. No case uses a timeout, and no child is ever
// left waiting for something that may not happen.
//
// What these cases prove that threads cannot: the store lock is an operating
// system lock held against a process, authority decisions are made against
// bytes on disk rather than against in-process state, and the commit protocol
// survives the death of the process that was mid-commit.

#include "test_framework.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "dccp/facility_capacity/capacity.hpp"

#include "child_process.hpp"
#include "test_support.hpp"

namespace {

using dccp::facility_capacity::AttemptId;
using dccp::facility_capacity::CapacityGeneration;
using dccp::facility_capacity::CapacityPrecondition;
using dccp::facility_capacity::CapacityStore;
using dccp::facility_capacity::EpochId;
using dccp::facility_capacity::ErrorCode;
using dccp::facility_capacity::FacilityCapacityModel;
using dccp::facility_capacity::IncarnationId;
using dccp::facility_capacity::ManualClock;
using dccp::facility_capacity::ModelConfig;
using dccp::facility_capacity::PendingCommit;
using dccp::facility_capacity::RecoveredState;
using dccp::facility_capacity::Revision;
using dccp::facility_capacity::Status;
using dccp::facility_capacity::StoreAccess;
using dccp::facility_capacity::StoreAuthority;
using dccp::facility_capacity::StoreCreateOptions;
using dccp::facility_capacity::StoreOpenOptions;
using dccp::facility_capacity::Tick;

/// The instant every clock in this suite starts from. Deterministic and shared
/// by parent and child, so a recovery that depends on the clock is reproducible.
constexpr std::uint64_t kTick = 1000;

/// The control-plane authority a store is created under and the authority every
/// model in this suite is created under.
constexpr std::uint64_t kAuthorityEpoch = 7;
constexpr std::uint64_t kAuthorityIncarnation = 3;

/// The epoch and incarnation a parent moves the store to when it fences an
/// older process out.
constexpr std::uint64_t kNextEpoch = 8;
constexpr std::uint64_t kNextIncarnation = 9;

/// Bound on a child's sentinel poll. Ten seconds of `yield` at the very worst,
/// and normally the sentinel is already there.
constexpr std::size_t kSentinelPollBound = 200000;

/// Bound on a child's commit loop. A child that cannot land its commits gives
/// up and reports the failure rather than running for ever.
constexpr int kCommitAttemptBound = 4096;

// ---------------------------------------------------------------------------
// Child-side helpers.
//
// A child prints one `key=value` line per fact and flushes after every line, so
// the parent can read a fact from the output file the moment it is printed.
// `std::_Exit` is used wherever the case is about a process that dies: it runs
// no destructor and no static destructor, which is exactly a crash.
// ---------------------------------------------------------------------------

void child_line(const char* text) {
  std::fprintf(stdout, "%s\n", text);
  std::fflush(stdout);
}

void child_print(const std::string& text) {
  std::fprintf(stdout, "%s\n", text.c_str());
  std::fflush(stdout);
}

void child_exit(int status) {
  std::fflush(stdout);
  std::fflush(stderr);
  std::_Exit(status);
}

void child_fail(const std::string& message) {
  child_print("error=" + message);
  child_exit(1);
}

bool sentinel_present(const std::string& path) {
  std::error_code code;
  return std::filesystem::exists(std::filesystem::path(path), code);
}

void await_sentinel(const std::string& path) {
  for (std::size_t attempt = 0; attempt < kSentinelPollBound; ++attempt) {
    if (sentinel_present(path)) {
      child_line("released");
      return;
    }
    std::this_thread::yield();
  }
  child_line("sentinel-timeout");
}

std::filesystem::path native(const std::string& text) {
#if defined(_WIN32)
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
#else
  return std::filesystem::path(text);
#endif
}

/// A manual clock reading the same instant in every process.
ManualClock child_clock() { return ManualClock(Tick::from_value(kTick)); }

CapacityStore open_store(const std::string& root, StoreOpenOptions options, ManualClock& clock) {
  auto store = CapacityStore::open(native(root), options, clock);
  if (!store.has_value()) {
    child_fail("open-failed=" + store.error().to_string());
  }
  return std::move(store.value());
}

StoreAuthority authority_of(const CapacityStore& store) {
  const auto refreshed = store.refresh_authority();
  if (!refreshed.has_value()) {
    child_fail("refresh-failed=" + refreshed.error().to_string());
  }
  return store.authority();
}

CapacityPrecondition precondition_of(const StoreAuthority& authority) {
  CapacityPrecondition precondition;
  precondition.expected_capacity_generation = authority.published_generation;
  precondition.expected_revision = authority.revision;
  precondition.expected_epoch = authority.epoch;
  precondition.expected_incarnation = authority.incarnation;
  return precondition;
}

/// A model that describes the same facility and site as the store and carries
/// the store's control-plane authority, with capacity generation zero.
FacilityCapacityModel model_for(const StoreAuthority& authority, ManualClock& clock) {
  ModelConfig config;
  config.facility = authority.facility;
  config.site = authority.site;
  config.epoch = authority.epoch;
  config.incarnation = authority.incarnation;
  auto model = FacilityCapacityModel::create(std::move(config), clock);
  if (!model.has_value()) {
    child_fail("model-create-failed=" + model.error().to_string());
  }
  return std::move(model.value());
}

/// Publishes until the model carries generation `target` and a snapshot. A
/// capacity generation is only reached by publishing, so this is how a fresh
/// process in this suite rejoins the generation sequence the store is on: a
/// store at generation N is joined by a model published up to N, and the commit
/// that follows publishes the model one further, which is the generation the
/// store accepts.
void publish_up_to(FacilityCapacityModel& model, std::uint64_t target) {
  const std::uint64_t steps = target + 1u;
  for (std::uint64_t step = 0; step < steps; ++step) {
    if (model.capacity_generation().value() >= target && model.current_snapshot() != nullptr) {
      return;
    }
    CapacityPrecondition precondition = model.current_precondition();
    precondition.attempt = AttemptId::from_value(step + 1u);
    const auto snapshot = model.publish(precondition);
    if (!snapshot.has_value()) {
      child_fail("publish-failed=" + snapshot.error().to_string());
    }
  }
  if (model.capacity_generation().value() < target || model.current_snapshot() == nullptr) {
    child_fail("publish-target-unreachable");
  }
}

/// Brings a model in line with the store, publishes the generation the store
/// will accept, and commits it. Returns the outcome through `observed`, which
/// is left at `invalid_argument` when the commit landed.
///
/// The revision token is taken from the model *after* it published, because
/// that is the state the commit names: the store fences the token against the
/// model it is being asked to commit, not against its own revision counter.
bool commit_next_generation(CapacityStore& store, ManualClock& clock, ErrorCode& observed) {
  const StoreAuthority authority = authority_of(store);
  FacilityCapacityModel model = model_for(authority, clock);
  publish_up_to(model, authority.published_generation.value() + 1u);
  CapacityPrecondition precondition = model.current_precondition();
  precondition.attempt = AttemptId::from_value(precondition.expected_revision.value() + 1u);
  const auto committed = store.commit(model, precondition);
  if (!committed.has_value()) {
    observed = committed.error().code();
    child_print("commit-detail=" + committed.error().to_string());
    return false;
  }
  observed = ErrorCode::invalid_argument;
  return true;
}

// ---------------------------------------------------------------------------
// Child entry points.
// ---------------------------------------------------------------------------

/// Holds the store's exclusive lifetime lock, reports readiness, and then waits
/// for a sentinel file. args: root, sentinel. Bounded, so it always terminates.
int child_hold_lock(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    child_fail("hold-lock-arguments");
  }
  ManualClock clock = child_clock();
  StoreOpenOptions options;
  options.access = StoreAccess::writer;
  options.hold_writer_lock = true;
  CapacityStore store = open_store(args[0], options, clock);
  child_print("lock=" + std::string(dccp::facility_capacity::store_access_name(store.access())) +
              " lifetime=1 epoch=" + store.authority().epoch.to_string());
  child_line("ready");
  await_sentinel(args[1]);
  return 0;
}

/// Commits one generation and exits normally. args: root.
int child_commit_once(const std::vector<std::string>& args) {
  if (args.empty()) {
    child_fail("commit-once-arguments");
  }
  ManualClock clock = child_clock();
  StoreOpenOptions options;
  options.access = StoreAccess::writer;
  CapacityStore store = open_store(args[0], options, clock);
  ErrorCode observed = ErrorCode::invalid_argument;
  if (!commit_next_generation(store, clock, observed)) {
    child_print(std::string("observed=") + std::string(dccp::facility_capacity::error_code_name(observed)));
    child_exit(2);
  }
  const StoreAuthority authority = authority_of(store);
  child_print("generation=" + authority.published_generation.to_string());
  child_print("revision=" + authority.revision.to_string());
  child_print("observed=none");
  child_line("ready");
  return 0;
}

/// Commits with a precondition the caller chose, so a stale authority or a
/// stale generation can be replayed against the live store.
///
/// args: root, mode, epoch, incarnation, generation.
///   mode `observed` - read the store's authority in this process and commit
///                     with exactly that precondition;
///   mode `fixed`    - commit with the epoch, incarnation and capacity
///                     generation given as the remaining arguments, which is how
///                     a process that captured its tokens before another process
///                     committed is modelled.
///
/// The error code the commit was refused with is printed as `observed=<name>`.
int child_commit_fenced(const std::vector<std::string>& args) {
  if (args.size() < 5) {
    child_fail("commit-fenced-arguments");
  }
  ManualClock clock = child_clock();
  StoreOpenOptions options;
  options.access = StoreAccess::writer;
  CapacityStore store = open_store(args[0], options, clock);

  const StoreAuthority authority = authority_of(store);
  child_print("generation=" + authority.published_generation.to_string());
  child_print("revision=" + authority.revision.to_string());
  child_print("epoch=" + authority.epoch.to_string());
  child_print("incarnation=" + authority.incarnation.to_string());

  const auto parse = [](const std::string& text) { return std::strtoull(text.c_str(), nullptr, 10); };

  FacilityCapacityModel model = model_for(authority, clock);
  publish_up_to(model, authority.published_generation.value() + 1u);
  // The revision token is the model's own, so the fence that refuses this
  // commit is the one this case is about and not the revision.
  CapacityPrecondition precondition = model.current_precondition();
  precondition.attempt = AttemptId::from_value(precondition.expected_revision.value() + 1u);
  if (args[1] == "fixed") {
    precondition.expected_epoch = EpochId::from_value(parse(args[2]));
    precondition.expected_incarnation = IncarnationId::from_value(parse(args[3]));
    precondition.expected_capacity_generation = CapacityGeneration::from_value(parse(args[4]));
  }
  const auto committed = store.commit(model, precondition);
  if (committed.has_value()) {
    child_print("observed=none");
    child_print("generation=" + committed.value().published_generation.to_string());
    child_line("ready");
    return 0;
  }
  child_print(std::string("observed=") +
              std::string(dccp::facility_capacity::error_code_name(committed.error().code())));
  child_line("ready");
  return 0;
}

/// Stages a commit, reports readiness, and dies without ever destroying the
/// pending commit, so the staging file stays behind.
/// args: root.
int child_crash_before_publish(const std::vector<std::string>& args) {
  if (args.empty()) {
    child_fail("crash-before-publish-arguments");
  }
  ManualClock clock = child_clock();
  StoreOpenOptions options;
  options.access = StoreAccess::writer;
  CapacityStore store = open_store(args[0], options, clock);
  const StoreAuthority authority = authority_of(store);
  FacilityCapacityModel model = model_for(authority, clock);
  publish_up_to(model, authority.published_generation.value() + 1u);
  CapacityPrecondition precondition = model.current_precondition();
  precondition.attempt = AttemptId::from_value(precondition.expected_revision.value() + 1u);

  auto pending = store.begin_commit(model, precondition);
  if (!pending.has_value()) {
    child_fail("begin-commit-failed=" + pending.error().to_string());
  }
  // The staging file is named here: the parent asserts on exactly this file.
  child_print("staging=" + pending.value().staging_path().filename().string());
  child_print(std::string("staging-present=") +
              (sentinel_present(pending.value().staging_path().string()) ? "yes" : "no"));
  child_line("ready");
  // Deliberately no publish, no abandon and no destructor: `_Exit` runs
  // neither. The previous generation stays authoritative and the staging file
  // stays on disk.
  child_exit(0);
  // Unreachable: child_exit does not return. Present so the function is well
  // formed even where the debug CRT does not annotate it as noreturn.
  return 0;
}

/// Commits one generation and then dies the instant `commit` returned.
/// args: root.
int child_crash_after_publish(const std::vector<std::string>& args) {
  if (args.empty()) {
    child_fail("crash-after-publish-arguments");
  }
  ManualClock clock = child_clock();
  StoreOpenOptions options;
  options.access = StoreAccess::writer;
  CapacityStore store = open_store(args[0], options, clock);
  ErrorCode observed = ErrorCode::invalid_argument;
  if (!commit_next_generation(store, clock, observed)) {
    child_print(std::string("observed=") + std::string(dccp::facility_capacity::error_code_name(observed)));
    child_exit(2);
  }
  const StoreAuthority authority = authority_of(store);
  child_print("generation=" + authority.published_generation.to_string());
  child_print("revision=" + authority.revision.to_string());
  child_print("observed=none");
  child_line("ready");
  child_exit(0);
  // Unreachable: child_exit does not return. Present so the function is well
  // formed even where the debug CRT does not annotate it as noreturn.
  return 0;
}

/// Tries to commit `count` generations against one store with per-operation
/// locking, so several of these children contend for the same generations.
/// args: root, count.
///
/// Prints `commits=<n>` and the sorted set of error codes it observed as
/// `errors=<a,b,...>`. Nothing outside that set is an acceptable outcome of a
/// lost race.
int child_commit_loop(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    child_fail("commit-loop-arguments");
  }
  const int count = static_cast<int>(std::strtol(args[1].c_str(), nullptr, 10));
  if (count <= 0) {
    child_fail("commit-loop-count");
  }
  ManualClock clock = child_clock();
  StoreOpenOptions options;
  options.access = StoreAccess::writer;
  CapacityStore store = open_store(args[0], options, clock);

  std::set<std::string> observed_codes;
  int succeeded = 0;
  for (int round = 0; round < count; ++round) {
    bool committed = false;
    for (int attempt = 0; attempt < kCommitAttemptBound && !committed; ++attempt) {
      ErrorCode observed = ErrorCode::invalid_argument;
      committed = commit_next_generation(store, clock, observed);
      if (!committed) {
        observed_codes.insert(std::string(dccp::facility_capacity::error_code_name(observed)));
      }
    }
    if (!committed) {
      child_print("commit-loop-exhausted round=" + std::to_string(round));
      break;
    }
    ++succeeded;
  }

  std::string codes;
  for (const std::string& code : observed_codes) {
    if (!codes.empty()) {
      codes.push_back(',');
    }
    codes.append(code);
  }
  child_print("commits=" + std::to_string(succeeded) + " errors=" + (codes.empty() ? "none" : codes));
  child_line("ready");
  return 0;
}

struct ChildRegistration {
  ChildRegistration(const char* name, ftest::ChildFunction function) { ftest::register_child(name, function); }
};

const ChildRegistration g_hold_lock_child("hold-lock", &child_hold_lock);
const ChildRegistration g_commit_once_child("commit-once", &child_commit_once);
const ChildRegistration g_commit_fenced_child("commit-fenced", &child_commit_fenced);
const ChildRegistration g_crash_before_publish_child("crash-before-publish", &child_crash_before_publish);
const ChildRegistration g_crash_after_publish_child("crash-after-publish", &child_crash_after_publish);
const ChildRegistration g_commit_loop_child("commit-loop", &child_commit_loop);

// ---------------------------------------------------------------------------
// Parent-side helpers.
// ---------------------------------------------------------------------------

constexpr int kMarkerAttempts = 400;

/// Starts a child and waits for it to report readiness. Fails the case when the
/// child never reports: `wait_for_marker` has a definite failure outcome, so a
/// child that hangs is reported rather than waited on.
ftest::ChildProcess start_child(const std::string& name, const std::vector<std::string>& arguments,
                                const fsup::TempDir& directory, const std::string& output_name,
                                const std::string& marker = "ready") {
  ftest::ChildProcess child =
      ftest::ChildProcess::start(name, arguments, directory.file(output_name + ".out"));
  if (!ftest::ChildProcess::wait_for_marker(child.output_file(), marker, kMarkerAttempts)) {
    FT_FAIL("the child did not report '" + marker + "': " + child.output());
    child.terminate();
  }
  return child;
}

/// The value of `key=value` on the first line whose key matches, or empty.
///
/// The child writes its log through a text-mode stream, so on Windows a line
/// ends in CR LF while the parent reads the file as bytes: the trailing CR is
/// stripped here. A value that carried one would match no path and no error
/// code, and the case would report a defect that is not there.
std::string child_value(const std::string& output, const std::string& key) {
  std::istringstream stream(output);
  std::string line;
  const std::string prefix = key + "=";
  while (std::getline(stream, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
      line.pop_back();
    }
    if (line.rfind(prefix, 0) == 0) {
      return line.substr(prefix.size());
    }
  }
  return std::string();
}

std::uint64_t child_number(const std::string& output, const std::string& key, std::uint64_t fallback) {
  const std::string value = child_value(output, key);
  if (value.empty()) {
    return fallback;
  }
  return std::strtoull(value.c_str(), nullptr, 10);
}

/// The names in an `errors=a,b,c` line. `none` yields no names.
std::vector<std::string> child_codes(const std::string& output) {
  std::vector<std::string> codes;
  const std::string value = child_value(output, "errors");
  std::string current;
  for (const char character : value) {
    if (character == ',') {
      if (!current.empty()) {
        codes.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(character);
  }
  if (!current.empty()) {
    codes.push_back(current);
  }
  if (codes.size() == 1 && codes.front() == "none") {
    codes.clear();
  }
  return codes;
}

/// Asserts that a child reported a specific stable error code.
void check_child_observed(const std::string& output, ErrorCode expected) {
  const std::string observed = child_value(output, "observed");
  FT_REQUIRE(!observed.empty());
  FT_CHECK_EQ(observed, std::string(dccp::facility_capacity::error_code_name(expected)));
}

/// A store created by the parent, plus the clock that drives it.
///
/// The clock is declared before the store on purpose: a `CapacityStore` holds a
/// pointer to its clock, so the clock must outlive the store, and members are
/// destroyed in reverse declaration order. The store is built in the initialiser
/// list by a lambda that runs after `clock` has been constructed, so the pointer
/// it keeps is to this object's own clock and not to a temporary.
struct ParentStore {
  ManualClock clock{Tick::from_value(kTick)};
  CapacityStore store;

  explicit ParentStore(const fsup::TempDir& directory)
      : store([this, &directory]() -> CapacityStore {
          StoreCreateOptions options;
          options.facility = fsup::facility_id();
          options.site = fsup::site_id();
          options.epoch = EpochId::from_value(kAuthorityEpoch);
          options.incarnation = IncarnationId::from_value(kAuthorityIncarnation);
          options.controller = fsup::controller_id();

          auto created = CapacityStore::create(directory.store_root(), options, clock);
          if (!created.has_value()) {
            FT_FAIL("the test store could not be created: " + created.error().to_string());
            throw ftest::TestAborted{};
          }
          return std::move(created.value());
        }()) {}
};

/// Commits one generation from the parent, so the store holds a published
/// generation before a child touches it.
void parent_commit_once(CapacityStore& store, ManualClock& clock) {
  const StoreAuthority authority = authority_of(store);
  FacilityCapacityModel model = model_for(authority, clock);
  publish_up_to(model, authority.published_generation.value() + 1u);
  CapacityPrecondition precondition = model.current_precondition();
  precondition.attempt = AttemptId::from_value(precondition.expected_revision.value() + 1u);
  const auto committed = store.commit(model, precondition);
  FT_REQUIRE(committed.has_value());
  FT_CHECK_EQ(committed.value().published_generation.value(),
              authority.published_generation.value() + 1u);
}

/// The generation numbers of the store's generation files, in numeric order.
///
/// The names sort as text, not as numbers, so `gen-10.fcs` precedes `gen-2.fcs`
/// in a string sort: the numbers are what the case compares.
std::vector<std::uint64_t> generation_numbers(const std::filesystem::path& root) {
  std::vector<std::uint64_t> numbers;
  for (const std::string& name : fsup::list_names(root)) {
    if (name.rfind("gen-", 0) != 0 || name.size() <= 8 || name.substr(name.size() - 4) != ".fcs") {
      continue;
    }
    numbers.push_back(std::strtoull(name.substr(4, name.size() - 8).c_str(), nullptr, 10));
  }
  std::sort(numbers.begin(), numbers.end());
  return numbers;
}

bool contains(const std::vector<std::string>& values, const std::string& wanted) {
  for (const std::string& value : values) {
    if (value == wanted) {
      return true;
    }
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// The exclusive lock is a real inter-process lock.
// ---------------------------------------------------------------------------

FT_TEST(multiprocess, writer_lock_excludes_another_process) {
  fsup::TempDir directory("mp-lock");
  ParentStore parent(directory);
  const std::filesystem::path sentinel = directory.file("release.sentinel");

  ftest::ChildProcess child =
      start_child("hold-lock", {directory.store_root().string(), sentinel.string()}, directory, "holder");
  const std::string child_output = child.output();
  FT_CHECK(child_output.find("lifetime=1") != std::string::npos);
  FT_CHECK(child.running());

  // While the child holds the lock, this process cannot take it. This is the
  // whole point: the lock is held against a process, not against a thread.
  {
    ManualClock clock(Tick::from_value(kTick));
    StoreOpenOptions options;
    options.access = StoreAccess::writer;
    options.hold_writer_lock = true;
    auto blocked = CapacityStore::open(directory.store_root(), options, clock);
    FT_REQUIRE(!blocked.has_value());
    FT_CHECK_ERROR(blocked, ErrorCode::lock_conflict);
  }

  // Process death relinquishes the lock, because the operating system releases
  // what a terminated process held.
  child.terminate();
  FT_CHECK(!child.running());
  {
    ManualClock clock(Tick::from_value(kTick));
    StoreOpenOptions options;
    options.access = StoreAccess::writer;
    options.hold_writer_lock = true;
    auto reopened = CapacityStore::open(directory.store_root(), options, clock);
    FT_REQUIRE_OK(reopened);
    FT_CHECK_EQ(reopened.value().access(), StoreAccess::writer);
    FT_CHECK_OK(reopened.value().verify(false));
  }
}

FT_TEST(multiprocess, writer_lock_is_released_by_a_clean_exit) {
  fsup::TempDir directory("mp-lock-clean");
  ParentStore parent(directory);
  const std::filesystem::path sentinel = directory.file("release.sentinel");

  ftest::ChildProcess child =
      start_child("hold-lock", {directory.store_root().string(), sentinel.string()}, directory, "holder");
  {
    ManualClock clock(Tick::from_value(kTick));
    StoreOpenOptions options;
    options.access = StoreAccess::writer;
    options.hold_writer_lock = true;
    auto blocked = CapacityStore::open(directory.store_root(), options, clock);
    FT_REQUIRE(!blocked.has_value());
    FT_CHECK_ERROR(blocked, ErrorCode::lock_conflict);
  }

  // The sentinel is the child's own exit condition: it releases the lock by
  // leaving, and the parent then holds it.
  fsup::write_bytes(sentinel, "release");
  const int status = child.wait();
  FT_CHECK_EQ(status, 0);
  FT_CHECK(child.output().find("released") != std::string::npos);

  ManualClock clock(Tick::from_value(kTick));
  StoreOpenOptions options;
  options.access = StoreAccess::writer;
  options.hold_writer_lock = true;
  auto reopened = CapacityStore::open(directory.store_root(), options, clock);
  FT_REQUIRE_OK(reopened);
  FT_CHECK_OK(reopened.value().verify(false));
}

FT_TEST(multiprocess, reader_cannot_open_while_a_writer_holds_the_lock) {
  fsup::TempDir directory("mp-reader");
  ParentStore parent(directory);
  const std::filesystem::path sentinel = directory.file("release.sentinel");

  ftest::ChildProcess child =
      start_child("hold-lock", {directory.store_root().string(), sentinel.string()}, directory, "holder");
  FT_CHECK(child.running());

  {
    ManualClock clock(Tick::from_value(kTick));
    StoreOpenOptions options;
    options.access = StoreAccess::reader;
    auto blocked = CapacityStore::open(directory.store_root(), options, clock);
    FT_REQUIRE(!blocked.has_value());
    FT_CHECK_ERROR(blocked, ErrorCode::lock_conflict);
  }

  child.terminate();
  {
    ManualClock clock(Tick::from_value(kTick));
    StoreOpenOptions options;
    options.access = StoreAccess::reader;
    auto allowed = CapacityStore::open(directory.store_root(), options, clock);
    FT_REQUIRE_OK(allowed);
    FT_CHECK_EQ(allowed.value().access(), StoreAccess::reader);
  }
}

// ---------------------------------------------------------------------------
// Authority is fenced across a process boundary.
// ---------------------------------------------------------------------------

FT_TEST(multiprocess, epoch_advance_fences_an_older_process_out) {
  fsup::TempDir directory("mp-epoch");
  ParentStore parent(directory);

  // Child A commits the first generation. A fresh store has no published
  // generation, so a model at generation zero commits generation one.
  {
    ftest::ChildProcess child = start_child("commit-once", {directory.store_root().string()}, directory, "a");
    FT_CHECK_EQ(child.wait(), 0);
    FT_CHECK_EQ(child_value(child.output(), "observed"), "none");
    FT_CHECK_EQ(child_value(child.output(), "generation"), "1");
  }

  const StoreAuthority after_a = authority_of(parent.store);
  FT_CHECK_EQ(after_a.published_generation, CapacityGeneration::from_value(1));
  FT_CHECK_EQ(after_a.epoch, EpochId::from_value(kAuthorityEpoch));
  FT_CHECK_EQ(after_a.incarnation, IncarnationId::from_value(kAuthorityIncarnation));

  // The parent takes control-plane authority and commits the next generation
  // under the new epoch and incarnation.
  {
    CapacityPrecondition precondition = precondition_of(after_a);
    precondition.attempt = AttemptId::from_value(after_a.revision.value() + 1u);
    FT_REQUIRE_OK(parent.store.advance_epoch(EpochId::from_value(kNextEpoch),
                                             IncarnationId::from_value(kNextIncarnation),
                                             fsup::controller_id("controller-2"), precondition));
  }
  {
    const StoreAuthority advanced = parent.store.authority();
    FT_CHECK_EQ(advanced.epoch, EpochId::from_value(kNextEpoch));
    FT_CHECK_EQ(advanced.incarnation, IncarnationId::from_value(kNextIncarnation));
    parent_commit_once(parent.store, parent.clock);
    FT_CHECK_EQ(parent.store.authority().published_generation, CapacityGeneration::from_value(2));
    FT_CHECK_EQ(parent.store.authority().epoch, EpochId::from_value(kNextEpoch));
  }

  // Child B was written to carry a precondition captured at the old authority:
  // the epoch and incarnation it names are the ones the store had before the
  // parent advanced them. Its commit must be refused, and it must say so.
  {
    ftest::ChildProcess child = start_child(
        "commit-fenced",
        {directory.store_root().string(), "fixed", std::to_string(kAuthorityEpoch),
         std::to_string(kAuthorityIncarnation), std::to_string(after_a.published_generation.value())},
        directory, "b");
    FT_CHECK_EQ(child.wait(), 0);
    check_child_observed(child.output(), ErrorCode::stale_authority);
    // The child read the store's own authority, so the parent can see exactly
    // what the child was fenced against.
    FT_CHECK(child.output().find("epoch=" + std::to_string(kNextEpoch)) != std::string::npos);
    FT_CHECK(child.output().find("incarnation=" + std::to_string(kNextIncarnation)) !=
             std::string::npos);
  }

  // Nothing the fenced child did reached the store.
  const StoreAuthority final_authority = authority_of(parent.store);
  FT_CHECK_EQ(final_authority.published_generation, CapacityGeneration::from_value(2));
  FT_CHECK_EQ(final_authority.epoch, EpochId::from_value(kNextEpoch));
  FT_CHECK_EQ(final_authority.incarnation, IncarnationId::from_value(kNextIncarnation));
  {
    auto recovered = parent.store.recover();
    FT_REQUIRE_OK(recovered);
    FT_CHECK_EQ(recovered.value().authority.published_generation, CapacityGeneration::from_value(2));
    FT_CHECK_EQ(recovered.value().model.epoch(), EpochId::from_value(kNextEpoch));
    FT_CHECK_OK(recovered.value().model.validate());
  }
  {
    auto report = parent.store.verify(true);
    FT_REQUIRE_OK(report);
    FT_CHECK(report.value().ok);
    FT_CHECK_EQ(report.value().residue.size(), 0u);
  }
}

// ---------------------------------------------------------------------------
// Generation fencing across processes.
// ---------------------------------------------------------------------------

FT_TEST(multiprocess, stale_generation_is_refused_across_processes) {
  fsup::TempDir directory("mp-generation");
  ParentStore parent(directory);

  {
    ftest::ChildProcess first = start_child("commit-once", {directory.store_root().string()}, directory, "first");
    FT_CHECK_EQ(first.wait(), 0);
    FT_CHECK_EQ(child_value(first.output(), "observed"), "none");
  }
  FT_CHECK_EQ(parent.store.refresh_authority().has_value(), true);
  const std::uint64_t revision_after_first = parent.store.authority().revision.value();
  FT_CHECK_EQ(parent.store.authority().published_generation, CapacityGeneration::from_value(1));

  {
    ftest::ChildProcess second =
        start_child("commit-once", {directory.store_root().string()}, directory, "second");
    FT_CHECK_EQ(second.wait(), 0);
    FT_CHECK_EQ(child_value(second.output(), "observed"), "none");
  }
  FT_CHECK_EQ(parent.store.refresh_authority().has_value(), true);
  FT_CHECK_EQ(parent.store.authority().published_generation, CapacityGeneration::from_value(2));
  FT_CHECK(parent.store.authority().revision.value() > revision_after_first);

  // The third child captured the generation before the second commit: epoch and
  // incarnation still match, so the fence that refuses it is the generation. It
  // claims the generation the store held then, which is one behind the one it
  // now holds, and its own model is one publication ahead of that.
  {
    ftest::ChildProcess third = start_child(
        "commit-fenced",
        {directory.store_root().string(), "fixed", std::to_string(kAuthorityEpoch),
         std::to_string(kAuthorityIncarnation), "0"},
        directory, "third");
    FT_CHECK_EQ(third.wait(), 0);
    check_child_observed(third.output(), ErrorCode::stale_generation);
  }

  FT_CHECK_EQ(parent.store.authority().published_generation, CapacityGeneration::from_value(2));
  auto recovered = parent.store.recover();
  FT_REQUIRE_OK(recovered);
  FT_CHECK_EQ(recovered.value().snapshot->generation(), CapacityGeneration::from_value(2));
  {
    auto report = parent.store.verify(true);
    FT_REQUIRE_OK(report);
    FT_CHECK(report.value().ok);
  }
}

// ---------------------------------------------------------------------------
// A crash between staging and publish.
// ---------------------------------------------------------------------------

FT_TEST(multiprocess, crash_before_publish_leaves_the_previous_generation) {
  fsup::TempDir directory("mp-crash-stage");
  ParentStore parent(directory);
  parent_commit_once(parent.store, parent.clock);
  const StoreAuthority before = authority_of(parent.store);
  const CapacityGeneration generation_before = before.published_generation;
  const std::string digest_before = before.snapshot_digest;
  FT_REQUIRE(!digest_before.empty());

  std::string staging_name;
  {
    ftest::ChildProcess child =
        start_child("crash-before-publish", {directory.store_root().string()}, directory, "crasher");
    FT_CHECK_EQ(child.wait(), 0);
    staging_name = child_value(child.output(), "staging");
    FT_REQUIRE(!staging_name.empty());
    FT_CHECK_EQ(child_value(child.output(), "staging-present"), "yes");
    FT_CHECK(std::filesystem::exists(directory.store_root() / staging_name));
  }

  // (a) the previous generation is still authoritative, with its own digest.
  {
    auto recovered = parent.store.recover();
    FT_REQUIRE_OK(recovered);
    FT_CHECK_EQ(recovered.value().authority.published_generation, generation_before);
    FT_CHECK_EQ(recovered.value().snapshot->generation(), generation_before);
    FT_CHECK_EQ(recovered.value().snapshot->digest(), digest_before);
    FT_CHECK_EQ(recovered.value().authority.snapshot_digest, digest_before);
    FT_CHECK_OK(recovered.value().model.validate());
  }
  // (b) the leftover staging file is reported as residue, and the store is
  // still sound: residue is not a finding.
  {
    auto report = parent.store.verify(false);
    FT_REQUIRE_OK(report);
    FT_CHECK(report.value().ok);
    FT_CHECK(contains(report.value().residue, staging_name));
    for (const std::string& finding : report.value().findings) {
      FT_FAIL("verify reported a finding after a crash before publish: " + finding);
    }
  }
  // (c) a normal commit afterwards succeeds, and the store still names exactly
  // one published generation.
  {
    parent_commit_once(parent.store, parent.clock);
    const StoreAuthority after = authority_of(parent.store);
    FT_CHECK_EQ(after.published_generation, CapacityGeneration::from_value(generation_before.value() + 1u));
    auto report = parent.store.verify(true);
    FT_REQUIRE_OK(report);
    FT_CHECK(report.value().ok);
    std::size_t referenced = 0;
    for (const auto& record : report.value().generations) {
      if (record.referenced_by_current) {
        ++referenced;
      }
    }
    FT_CHECK_EQ(referenced, 1u);
    FT_CHECK_EQ(after.file_name,
                CapacityStore::generation_file_name(CapacityGeneration::from_value(
                    generation_before.value() + 1u)));
    auto recovered = parent.store.recover();
    FT_REQUIRE_OK(recovered);
    FT_CHECK_EQ(recovered.value().snapshot->generation(), after.published_generation);
    FT_CHECK_OK(recovered.value().model.validate());
  }
}

// ---------------------------------------------------------------------------
// A crash immediately after the publish point is durable.
// ---------------------------------------------------------------------------

FT_TEST(multiprocess, crash_after_publish_is_durable) {
  fsup::TempDir directory("mp-crash-publish");
  ParentStore parent(directory);
  parent_commit_once(parent.store, parent.clock);
  FT_CHECK_EQ(parent.store.authority().published_generation, CapacityGeneration::from_value(1));

  {
    ftest::ChildProcess child =
        start_child("crash-after-publish", {directory.store_root().string()}, directory, "crasher");
    FT_CHECK_EQ(child.wait(), 0);
    FT_CHECK_EQ(child_value(child.output(), "observed"), "none");
    FT_CHECK_EQ(child_value(child.output(), "generation"), "2");
  }

  // The commit point is the replacement of CURRENT, and it has already
  // happened: the generation the child published is the authoritative one.
  auto recovered = parent.store.recover();
  FT_REQUIRE_OK(recovered);
  FT_CHECK_EQ(recovered.value().authority.published_generation, CapacityGeneration::from_value(2));
  FT_CHECK_EQ(recovered.value().snapshot->generation(), CapacityGeneration::from_value(2));
  FT_CHECK_EQ(recovered.value().authority.snapshot_digest, recovered.value().snapshot->digest());
  FT_CHECK(!recovered.value().snapshot->digest().empty());
  FT_CHECK_OK(recovered.value().model.validate());
  // The recovered snapshot is stamped recovered, never silently current.
  FT_CHECK_EQ(recovered.value().snapshot->freshness(),
              dccp::facility_capacity::SnapshotFreshness::recovered);
  {
    auto report = parent.store.verify(true);
    FT_REQUIRE_OK(report);
    FT_CHECK(report.value().ok);
    bool decoded = false;
    for (const auto& record : report.value().generations) {
      if (record.referenced_by_current) {
        FT_CHECK(record.decodes);
        decoded = true;
      }
    }
    FT_CHECK(decoded);
  }
  // And the store is usable afterwards.
  parent_commit_once(parent.store, parent.clock);
  FT_CHECK_EQ(parent.store.authority().published_generation, CapacityGeneration::from_value(3));
}

// ---------------------------------------------------------------------------
// Concurrent writers cannot interleave.
// ---------------------------------------------------------------------------

FT_TEST(multiprocess, concurrent_writers_cannot_interleave) {
  fsup::TempDir directory("mp-concurrent");
  ParentStore parent(directory);

  constexpr std::size_t kChildren = 4;
  constexpr int kCommitsPerChild = 10;

  std::vector<ftest::ChildProcess> children;
  children.reserve(kChildren);
  for (std::size_t index = 0; index < kChildren; ++index) {
    children.push_back(start_child(
        "commit-loop", {directory.store_root().string(), std::to_string(kCommitsPerChild)}, directory,
        "writer-" + std::to_string(index)));
  }
  // Every child is running and past its open before any of them is joined, so
  // the four commit loops genuinely overlap rather than running one after
  // another.
  for (std::size_t index = 0; index < kChildren; ++index) {
    FT_CHECK(children[index].running());
  }

  std::uint64_t total_commits = 0;
  for (std::size_t index = 0; index < kChildren; ++index) {
    const int status = children[index].wait();
    const std::string output = children[index].output();
    FT_CHECK_EQ(status, 0);
    FT_CHECK(output.find("commit-loop-exhausted") == std::string::npos);
    const std::vector<std::string> codes = child_codes(output);
    for (const std::string& code : codes) {
      FT_CHECK(code == "lock_conflict" || code == "stale_generation" || code == "stale_authority");
    }
    const std::uint64_t committed = child_number(output, "commits", 0);
    FT_CHECK_EQ(committed, static_cast<std::uint64_t>(kCommitsPerChild));
    total_commits += committed;
  }

  // Every commit landed exactly once: the store is on the sum of the children's
  // commit counts, and the only refusals any child ever saw were the three that
  // mean "another process got there first".
  FT_CHECK_EQ(parent.store.refresh_authority().has_value(), true);
  FT_CHECK_EQ(parent.store.authority().published_generation.value(), total_commits);
  {
    auto report = parent.store.verify(true);
    FT_REQUIRE_OK(report);
    FT_CHECK(report.value().ok);
    for (const std::string& finding : report.value().findings) {
      FT_FAIL("verify reported a finding after concurrent writers: " + finding);
    }
  }
  {
    auto recovered = parent.store.recover();
    FT_REQUIRE_OK(recovered);
    FT_CHECK_EQ(recovered.value().authority.published_generation.value(), total_commits);
    FT_CHECK_EQ(recovered.value().snapshot->generation().value(), total_commits);
    FT_CHECK_OK(recovered.value().model.validate());
  }
  // Every generation the children committed is present exactly once, with no
  // gap and no duplicate: generations 1..total_commits, which is what "cannot
  // interleave" means for the sequence itself.
  const std::vector<std::uint64_t> numbers = generation_numbers(directory.store_root());
  FT_CHECK_EQ(numbers.size(), total_commits);
  for (std::size_t index = 0; index < numbers.size(); ++index) {
    FT_CHECK_EQ(numbers[index], static_cast<std::uint64_t>(index + 1));
  }
  FT_CHECK_EQ(parent.store.authority().file_name,
              CapacityStore::generation_file_name(CapacityGeneration::from_value(total_commits)));
}
