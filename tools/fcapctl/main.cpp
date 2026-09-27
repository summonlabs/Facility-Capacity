// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// fcap - the Facility Capacity inspection and administration tool.
//
// Design rules this file follows, because they are the product's rules:
//
//   * Time is injected. The tool installs `SystemClock` explicitly and passes
//     it to every call that needs one; the library never reads a wall clock of
//     its own accord.
//   * Reads open the store with `StoreAccess::reader`, writes with
//     `StoreAccess::writer`. Nothing mutates a store through a reader handle.
//   * A mutating command follows the documented order exactly:
//         create or recover the model
//         -> derive `current_precondition()` from the live state
//         -> assign a fresh non-zero `AttemptId`
//         -> mutate
//         -> publish under a precondition re-derived after the mutation
//         -> commit under the post-publish `current_precondition()`
//     Each of the three state-dependent calls takes its own freshly derived
//     precondition and its own fresh attempt token: a mutation bumps the model
//     revision, and a publication bumps the capacity generation and the
//     revision, so a token captured before either would name a state that no
//     longer exists. The commit names the generation the publication just
//     produced, which is exactly the generation the store is publishing, and
//     the store's fence admits it.
//   * Every failure is one deterministic line on standard error:
//         error: <code>: <message> [constraint=...] [expected_generation=N actual_generation=M]
//     Nothing is written to standard output on a failure path: command output
//     is buffered and emitted only once the command has succeeded.
//   * Exit codes: 0 success, 1 domain failure, 2 usage error.
//   * No output carries a timestamp, an address, a pointer or a locale-formed
//     number. Integer counters are rendered by the library's own canonical
//     `to_string` implementations.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/facility_capacity/capacity.hpp"
#include "script.hpp"

namespace {

namespace fc = dccp::facility_capacity;
using fcapctl::Script;

constexpr int kExitOk = 0;
constexpr int kExitDomainFailure = 1;
constexpr int kExitUsageError = 2;

/// Revision stamped on provenance this tool produces.
constexpr std::string_view kToolRevision = "fcapctl/1.0.0";

/// The source identity a service constraint is declared under. A constraint is
/// a declaration *to* the aggregate model by the operator that owns it, so it
/// is declared under the operator's own source rather than under a capacity
/// source. Evidence and reserves always name a real capacity source.
constexpr std::string_view kOperatorSource = "fcapctl";

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

std::string format_error(const fc::Error& error) {
  std::string line = "error: ";
  line.append(fc::error_code_name(error.code()));
  line.append(": ");
  line.append(error.message());
  if (!error.constraint().empty()) {
    line.append(" [constraint=");
    line.append(error.constraint());
    line.push_back(']');
  }
  if (error.has_generations()) {
    line.append(" [expected_generation=");
    line.append(std::to_string(error.expected_generation()));
    line.append(" actual_generation=");
    line.append(std::to_string(error.actual_generation()));
    line.push_back(']');
  }
  return line;
}

/// Command output, buffered until the command has finished succeeding.
///
/// A command that fails writes nothing to standard output, so a caller that
/// pipes `fcap` into another tool never sees half an answer next to an error.
class Output {
 public:
  void line(std::string_view text) {
    out_.append(text);
    out_.push_back('\n');
  }

  /// Appends an already-rendered block, guaranteeing one final newline.
  void block(std::string_view text) {
    if (text.empty()) {
      return;
    }
    out_.append(text);
    if (text.back() != '\n') {
      out_.push_back('\n');
    }
  }

  /// Records the first failure. Later failures do not overwrite it.
  void fail(const fc::Error& error, int exit_code) {
    if (error_.has_value()) {
      return;
    }
    error_ = error;
    exit_code_ = exit_code;
  }

  /// Lowers the exit code without recording an error: the command ran, and its
  /// own report says the state it inspected is not acceptable.
  void set_exit_code(int exit_code) {
    if (!error_.has_value()) {
      exit_code_ = exit_code;
    }
  }

  bool failed() const noexcept { return error_.has_value(); }

  /// Writes the error line to standard error, or the buffered output to
  /// standard output, and returns the process exit code.
  int emit() const {
    if (error_.has_value()) {
      const std::string text = format_error(error_.value());
      std::fputs(text.c_str(), stderr);
      std::fputc('\n', stderr);
      return exit_code_ == kExitOk ? kExitDomainFailure : exit_code_;
    }
    if (!out_.empty()) {
      std::fwrite(out_.data(), 1, out_.size(), stdout);
    }
    return exit_code_;
  }

 private:
  std::string out_;
  std::optional<fc::Error> error_;
  int exit_code_ = kExitOk;
};

/// Records a usage failure.
void fail_usage(Output& out, const fc::Error& error) { out.fail(error, kExitUsageError); }

/// Records a domain failure.
void fail_domain(Output& out, const fc::Error& error) { out.fail(error, kExitDomainFailure); }

/// True when a library result succeeded; otherwise the failure is reported.
template <class T>
bool check(const fc::Result<T>& result, Output& out) {
  if (result.has_value()) {
    return true;
  }
  fail_domain(out, result.error());
  return false;
}

// ---------------------------------------------------------------------------
// Option reading
//
// Identifiers, dimensions, quantities and units are never guessed here: an
// identifier is parsed by the library, so an invalid one is reported as an
// `invalid_argument` domain failure carrying the library's own explanation.
// ---------------------------------------------------------------------------

std::string dimension_names() {
  const fc::CapacityDimension* dimensions = fc::all_capacity_dimensions();
  std::string names;
  for (std::size_t index = 0; index < fc::capacity_dimension_count; ++index) {
    if (index != 0) {
      names.push_back(',');
    }
    names.append(fc::capacity_dimension_name(dimensions[index]));
  }
  return names;
}

fc::Error unknown_dimension(std::string_view option, const std::string& text) {
  fc::Error error = fc::Error::make(fc::ErrorCode::invalid_argument,
                                    "option --" + std::string(option) + " is not a capacity dimension: '" + text + "'");
  error.with_constraint("expected one of: " + dimension_names());
  return error;
}

bool read_required_text(Script& script, std::string_view name, std::string& target, Output& out) {
  fc::Result<std::string> value = script.required(name);
  if (!value.has_value()) {
    fail_usage(out, value.error());
    return false;
  }
  target = std::move(value.value());
  return true;
}

bool read_required_path(Script& script, std::filesystem::path& target, Output& out) {
  std::string text;
  if (!read_required_text(script, "path", text, out)) {
    return false;
  }
  target = std::filesystem::path(text);
  return true;
}

bool read_required_u64(Script& script, std::string_view name, std::uint64_t& target, Output& out) {
  fc::Result<std::uint64_t> value = script.required_u64(name);
  if (!value.has_value()) {
    fail_usage(out, value.error());
    return false;
  }
  target = value.value();
  return true;
}

/// Rejects a value above `maximum` before it reaches the library, so an
/// out-of-bound request is a usage error naming the bound rather than a
/// counter failure deeper in.
bool read_required_bounded(Script& script, std::string_view name, std::uint64_t maximum, std::string_view what,
                           std::uint64_t& target, Output& out) {
  std::uint64_t value = 0;
  if (!read_required_u64(script, name, value, out)) {
    return false;
  }
  if (value > maximum) {
    fail_usage(out, fc::Error::make(fc::ErrorCode::invalid_argument,
                                    "option --" + std::string(name) + " must not exceed the accepted " +
                                        std::string(what) + " of " + std::to_string(maximum)));
    return false;
  }
  target = value;
  return true;
}

bool read_optional_u64(Script& script, std::string_view name, std::optional<std::uint64_t>& target, Output& out) {
  if (!script.has_option(name)) {
    target = std::nullopt;
    return true;
  }
  fc::Result<std::uint64_t> value = script.option_u64(name);
  if (!value.has_value()) {
    fail_usage(out, value.error());
    return false;
  }
  target = value.value();
  return true;
}

bool read_required_dimension(Script& script, std::string_view name, fc::CapacityDimension& target, Output& out) {
  std::string text;
  if (!read_required_text(script, name, text, out)) {
    return false;
  }
  fc::CapacityDimension dimension = fc::CapacityDimension::space;
  if (!fc::parse_capacity_dimension(text, dimension)) {
    fail_usage(out, unknown_dimension(name, text));
    return false;
  }
  target = dimension;
  return true;
}

bool read_optional_operational_state(Script& script, fc::OperationalState& target, Output& out) {
  if (!script.has_option("state")) {
    target = fc::OperationalState::nominal;
    return true;
  }
  const std::string text = script.option_or("state", std::string());
  fc::OperationalState state = fc::OperationalState::nominal;
  if (!fc::parse_operational_state(text, state)) {
    fc::Error error = fc::Error::make(fc::ErrorCode::invalid_argument,
                                      "option --state is not an operational state: '" + text + "'");
    error.with_constraint("expected one of: nominal,degraded,unavailable,unknown");
    fail_usage(out, error);
    return false;
  }
  target = state;
  return true;
}

bool read_required_reserve_kind(Script& script, fc::ReserveKind& target, Output& out) {
  std::string text;
  if (!read_required_text(script, "kind", text, out)) {
    return false;
  }
  fc::ReserveKind kind = fc::ReserveKind::protection;
  if (!fc::parse_reserve_kind(text, kind)) {
    fc::Error error = fc::Error::make(fc::ErrorCode::invalid_argument,
                                      "option --kind is not a reserve kind: '" + text + "'");
    error.with_constraint("expected one of: protection,operational,service_obligation,contingency");
    fail_usage(out, error);
    return false;
  }
  target = kind;
  return true;
}

/// One quantity option.
///
/// An omitted option means UNMEASURED, and the literal `unknown` means
/// UNMEASURED too: "not measured" is a value this library carries explicitly,
/// and the tool never turns it into a zero. A measurement outside
/// `[0, max_quantity_magnitude]` is rejected here, before the library is
/// called.
bool read_measured(Script& script, std::string_view name, fc::CapacityDimension dimension, fc::Measured& target,
                   Output& out) {
  fc::Result<std::optional<std::string>> text = script.option_text(name);
  if (!text.has_value()) {
    fail_usage(out, text.error());
    return false;
  }
  if (!text.value().has_value()) {
    target = fc::Measured::unknown();
    return true;
  }
  const std::string& literal = text.value().value();
  if (literal == "unknown") {
    target = fc::Measured::unknown();
    return true;
  }
  fc::Result<std::int64_t> magnitude = fcapctl::parse_i64(literal);
  if (!magnitude.has_value()) {
    fail_usage(out, fc::Error::make(fc::ErrorCode::invalid_argument,
                                    "option --" + std::string(name) + " is not a decimal quantity: '" + literal + "'"));
    return false;
  }
  if (magnitude.value() < 0) {
    fail_usage(out, fc::Error::make(fc::ErrorCode::invalid_argument,
                                    "option --" + std::string(name) + " must not be negative: '" + literal + "'"));
    return false;
  }
  if (magnitude.value() > fc::limits::max_quantity_magnitude) {
    fail_usage(out, fc::Error::make(fc::ErrorCode::invalid_argument,
                                    "option --" + std::string(name) +
                                        " must not exceed the accepted maximum quantity magnitude of " +
                                        std::to_string(fc::limits::max_quantity_magnitude)));
    return false;
  }
  fc::Result<fc::Measured> measured = fc::Measured::known(fc::canonical_unit(dimension), magnitude.value());
  if (!measured.has_value()) {
    fail_domain(out, measured.error());
    return false;
  }
  target = measured.value();
  return true;
}

/// The validity window of a reserve or a constraint: `[start, end)`, always
/// active unless the caller narrows it.
bool read_validity_window(Script& script, fc::ValidityWindow& target, Output& out) {
  std::optional<std::uint64_t> start;
  std::optional<std::uint64_t> end;
  if (!read_optional_u64(script, "window-start", start, out)) {
    return false;
  }
  if (!read_optional_u64(script, "window-end", end, out)) {
    return false;
  }
  if (!start.has_value() && !end.has_value()) {
    target = fc::ValidityWindow();
    return true;
  }
  const fc::Tick window_start = fc::Tick::from_value(start.value_or(0));
  const fc::Tick window_end = end.has_value() ? fc::Tick::from_value(end.value()) : fc::max_tick;
  fc::Result<fc::ValidityWindow> window = fc::ValidityWindow::create(window_start, window_end);
  if (!window.has_value()) {
    fail_domain(out, window.error());
    return false;
  }
  target = window.value();
  return true;
}

/// Rejects anything left over once a command has read every option it accepts.
///
/// Called before any file, store or clock operation, so an unknown or malformed
/// option can never do work before it is reported.
bool finish_options(Script& script, Output& out) {
  const fc::Status status = script.finish();
  if (!status.has_value()) {
    fail_usage(out, status.error());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Clock, attempts and the mutating sequence
// ---------------------------------------------------------------------------

std::uint64_t g_attempt_sequence = 0;

/// A fresh, non-zero idempotency token.
///
/// The token is not authority: it lets the library recognise a replayed attempt
/// and apply nothing a second time. It is derived from the injected clock and a
/// process-local sequence so that two calls never share one.
fc::AttemptId next_attempt(fc::Clock& clock) {
  ++g_attempt_sequence;
  std::uint64_t base = 0;
  const fc::Result<fc::Tick> now = clock.now();
  if (now.has_value()) {
    base = now.value().value();
  }
  const std::uint64_t mixed = base + 0x9E3779B97F4A7C15ull * g_attempt_sequence;
  return fc::AttemptId::from_value(mixed == 0 ? 1 : mixed);
}

/// A store handle opened for writing together with the model to mutate.
///
/// The model always comes from the store when a generation has been published,
/// so a mutation is applied to the committed state and never to a locally
/// invented one.
struct Session {
  fc::CapacityStore store;
  fc::FacilityCapacityModel model;
  bool model_was_created;

  Session(fc::CapacityStore store_in, fc::FacilityCapacityModel model_in, bool created)
      : store(std::move(store_in)), model(std::move(model_in)), model_was_created(created) {}
};

/// Recovers the published state of a store.
///
/// `CapacityStore::recover()` is the one supported way to get the model and its
/// snapshot back together: it verifies the container digest, decodes both
/// documents, re-derives everything and checks that the state, the snapshot and
/// the recorded authority all agree about the generation. The tool never reads
/// the store directory itself.
fc::Result<fc::RecoveredState> recover_state(const fc::CapacityStore& store) {
  return store.recover();
}

fc::Result<Session> open_write_session(const std::filesystem::path& path, fc::Clock& clock) {
  fc::StoreOpenOptions options;
  options.access = fc::StoreAccess::writer;
  fc::Result<fc::CapacityStore> store = fc::CapacityStore::open(path, options, clock);
  if (!store.has_value()) {
    return store.error();
  }
  if (store.value().authority().has_published_generation) {
    fc::Result<fc::RecoveredState> recovered = recover_state(store.value());
    if (!recovered.has_value()) {
      return recovered.error();
    }
    return Session(std::move(store.value()), std::move(recovered.value().model), false);
  }
  // A store with no published generation carries no coverage contract, because
  // a contract is durable only inside a published generation. Rebuilding the
  // model from the store's authority is the only honest option left, and the
  // caller is told that the contract is empty.
  const fc::StoreAuthority& authority = store.value().authority();
  fc::ModelConfig config;
  config.facility = authority.facility;
  config.site = authority.site;
  config.epoch = authority.epoch;
  config.incarnation = authority.incarnation;
  fc::Result<fc::FacilityCapacityModel> model = fc::FacilityCapacityModel::create(std::move(config), clock);
  if (!model.has_value()) {
    return model.error();
  }
  return Session(std::move(store.value()), std::move(model.value()), true);
}

/// Publishes the model's current state and commits it to the store.
///
/// The commit is fenced against CURRENT and takes the post-publish precondition:
/// the generation the store is about to publish, which is the state one
/// publication ahead of what it holds.
fc::Status publish_and_commit(fc::CapacityStore& store, fc::FacilityCapacityModel& model, fc::Clock& clock,
                              std::shared_ptr<const fc::CapacitySnapshot>& published_out) {
  fc::CapacityPrecondition publication = model.current_precondition();
  publication.attempt = next_attempt(clock);
  const fc::Result<std::shared_ptr<const fc::CapacitySnapshot>> published = model.publish(publication);
  if (!published.has_value()) {
    return published.error();
  }
  fc::CapacityPrecondition commit = model.current_precondition();
  commit.attempt = next_attempt(clock);
  const fc::Result<fc::StoreAuthority> committed = store.commit(model, commit);
  if (!committed.has_value()) {
    return committed.error();
  }
  published_out = published.value();
  return fc::Status::success();
}

/// Applies one mutation, publishes the result and commits it.
template <class Mutate>
fc::Status mutate_publish_commit(Session& session, fc::Clock& clock, Mutate&& mutate,
                                 std::shared_ptr<const fc::CapacitySnapshot>& published_out) {
  fc::CapacityPrecondition mutation = session.model.current_precondition();
  mutation.attempt = next_attempt(clock);
  const fc::Status mutated = mutate(session.model, mutation);
  if (!mutated.has_value()) {
    return mutated.error();
  }
  return publish_and_commit(session.store, session.model, clock, published_out);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void append_authority(Output& out, const fc::StoreAuthority& authority) {
  out.line("store=" + authority.store_id.value());
  out.line("facility=" + authority.facility.value());
  out.line("site=" + authority.site.value());
  out.line("controller=" + authority.controller.value());
  out.line("epoch=" + authority.epoch.to_string());
  out.line("incarnation=" + authority.incarnation.to_string());
  out.line("published_generation=" + authority.published_generation.to_string());
  out.line("revision=" + authority.revision.to_string());
  out.line("published_file=" + (authority.has_published_generation ? authority.file_name : std::string("-")));
  out.line("snapshot_digest=" + (authority.snapshot_digest.empty() ? std::string("-") : authority.snapshot_digest));
}

/// One short line per dimension of a published answer.
void append_dimension_summary(Output& out, const fc::CapacitySnapshot& snapshot) {
  for (const fc::DimensionTotals& totals : snapshot.dimension_totals()) {
    std::string line = "dimension=";
    line.append(fc::capacity_dimension_name(totals.dimension));
    line.append(" outcome=");
    line.append(fc::capacity_outcome_name(snapshot.outcome(totals.dimension)));
    line.append(" unit=");
    line.append(fc::unit_name(totals.unit));
    line.append(" installed=");
    line.append(totals.installed.to_string());
    line.append(" usable=");
    line.append(totals.usable.to_string());
    line.append(" protected=");
    line.append(totals.protected_capacity.to_string());
    line.append(" reserved=");
    line.append(totals.reserved.to_string());
    line.append(" allocatable=");
    line.append(totals.allocatable.to_string());
    line.append(" sources=");
    line.append(std::to_string(totals.contributing_sources));
    out.line(line);
  }
}

void append_commit_summary(Output& out, const fc::CapacitySnapshot& snapshot) {
  out.line("generation=" + snapshot.generation().to_string());
  out.line("snapshot_id=" + snapshot.id().value());
  out.line("snapshot_digest=" + snapshot.digest());
  append_dimension_summary(out, snapshot);
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

void print_usage(std::FILE* stream) {
  static constexpr std::string_view kUsage =
      "usage: fcap <command> [options]\n"
      "\n"
      "commands:\n"
      "  version\n"
      "  store init         --path DIR --facility ID --site ID --epoch N --incarnation N --controller ID\n"
      "                     [--validity-ticks N] [--require DIM]...\n"
      "  store info         --path DIR\n"
      "  store verify       --path DIR [--deep]\n"
      "  store generations  --path DIR\n"
      "  store recover      --path DIR\n"
      "  store prune        --path DIR --keep N\n"
      "  evidence declare   --path DIR --source S --dimension D --generation N\n"
      "                     [--installed N] [--observed N] [--usable N] [--unavailable N] [--residual N]\n"
      "                     [--protected N] [--reserved N] [--derive-residual]\n"
      "                     [--state nominal|degraded|unavailable|unknown] [--detail TEXT]\n"
      "                     [--epoch N] [--incarnation N] [--observed-at N]\n"
      "  reserve declare    --path DIR --id ID --source S --dimension D\n"
      "                     --kind protection|operational|service_obligation|contingency\n"
      "                     [--amount N] [--window-start N] [--window-end N] [--owner ID]\n"
      "  constraint declare --path DIR --id ID --dimension D [--floor N] [--service-class ID]\n"
      "                     [--window-start N] [--window-end N]\n"
      "  query              --path DIR --dimension D\n"
      "  snapshot show      --path DIR\n"
      "  snapshot explain   --path DIR --dimension D\n"
      "  revalidate         --path DIR\n"
      "\n"
      "dimensions: space rack power cooling operational_reserve facility_service\n"
      "quantities: an exact non-negative integer in the dimension's canonical unit, or `unknown`\n"
      "            for unmeasured; an omitted quantity option means unmeasured\n"
      "exit codes: 0 success, 1 domain failure, 2 usage error\n";
  std::fwrite(kUsage.data(), 1, kUsage.size(), stream);
}

// ---------------------------------------------------------------------------
// Commands: identity
// ---------------------------------------------------------------------------

void run_version(Script& script, Output& out) {
  if (!finish_options(script, out)) {
    return;
  }
  out.line(std::string(fc::product_slug) + " " + std::string(fc::version_string));
}

// ---------------------------------------------------------------------------
// Commands: store
// ---------------------------------------------------------------------------

void run_store_init(Script& script, fc::Clock& clock, Output& out) {
  std::filesystem::path path;
  std::string facility_text;
  std::string site_text;
  std::string controller_text;
  std::uint64_t epoch_value = 0;
  std::uint64_t incarnation_value = 0;
  std::optional<std::uint64_t> validity_ticks;
  std::vector<fc::CapacityRequirement> requirements;

  if (!read_required_path(script, path, out)) {
    return;
  }
  if (!read_required_text(script, "facility", facility_text, out)) {
    return;
  }
  if (!read_required_text(script, "site", site_text, out)) {
    return;
  }
  if (!read_required_u64(script, "epoch", epoch_value, out)) {
    return;
  }
  if (!read_required_u64(script, "incarnation", incarnation_value, out)) {
    return;
  }
  if (!read_required_text(script, "controller", controller_text, out)) {
    return;
  }
  if (!read_optional_u64(script, "validity-ticks", validity_ticks, out)) {
    return;
  }
  for (const std::string& text : script.option_all("require")) {
    fc::CapacityDimension dimension = fc::CapacityDimension::space;
    if (!fc::parse_capacity_dimension(text, dimension)) {
      fail_usage(out, unknown_dimension("require", text));
      return;
    }
    fc::CapacityRequirement requirement;
    requirement.dimension = dimension;
    requirement.required = true;
    requirements.push_back(std::move(requirement));
  }
  if (!finish_options(script, out)) {
    return;
  }

  fc::Result<fc::FacilityId> facility = fc::FacilityId::parse(facility_text);
  if (!check(facility, out)) {
    return;
  }
  fc::Result<fc::SiteId> site = fc::SiteId::parse(site_text);
  if (!check(site, out)) {
    return;
  }
  fc::Result<fc::ControllerId> controller = fc::ControllerId::parse(controller_text);
  if (!check(controller, out)) {
    return;
  }

  fc::StoreCreateOptions options;
  options.facility = facility.value();
  options.site = site.value();
  options.epoch = fc::EpochId::from_value(epoch_value);
  options.incarnation = fc::IncarnationId::from_value(incarnation_value);
  options.controller = controller.value();

  fc::Result<fc::CapacityStore> store = fc::CapacityStore::create(path, options, clock);
  if (!check(store, out)) {
    return;
  }

  // A store records identity and authority and nothing else: the coverage
  // contract and the snapshot validity window belong to the model, and they
  // become durable when the model's first generation is committed. So the
  // store's first generation carries the contract declared here, and every
  // later command recovers it from the store instead of being told it again.
  fc::ModelConfig config;
  config.facility = options.facility;
  config.site = options.site;
  config.epoch = options.epoch;
  config.incarnation = options.incarnation;
  config.requirements = requirements;
  config.snapshot_validity_ticks = fc::Tick::from_value(validity_ticks.value_or(0));
  fc::Result<fc::FacilityCapacityModel> model = fc::FacilityCapacityModel::create(std::move(config), clock);
  if (!check(model, out)) {
    return;
  }

  std::shared_ptr<const fc::CapacitySnapshot> snapshot;
  const fc::Status committed = publish_and_commit(store.value(), model.value(), clock, snapshot);
  if (!check(committed, out)) {
    return;
  }
  append_authority(out, store.value().authority());
}

void run_store_info(Script& script, fc::Clock& clock, Output& out) {
  std::filesystem::path path;
  if (!read_required_path(script, path, out)) {
    return;
  }
  if (!finish_options(script, out)) {
    return;
  }
  fc::StoreOpenOptions options;
  options.access = fc::StoreAccess::reader;
  fc::Result<fc::CapacityStore> store = fc::CapacityStore::open(path, options, clock);
  if (!check(store, out)) {
    return;
  }
  append_authority(out, store.value().authority());
}

void run_store_verify(Script& script, fc::Clock& clock, Output& out) {
  std::filesystem::path path;
  if (!read_required_path(script, path, out)) {
    return;
  }
  const bool deep = script.flag("deep");
  if (!finish_options(script, out)) {
    return;
  }
  fc::StoreOpenOptions options;
  options.access = fc::StoreAccess::reader;
  fc::Result<fc::CapacityStore> store = fc::CapacityStore::open(path, options, clock);
  if (!check(store, out)) {
    return;
  }
  fc::Result<fc::VerifyReport> report = store.value().verify(deep);
  if (!check(report, out)) {
    return;
  }
  out.block(report.value().describe());
  if (!report.value().ok) {
    out.set_exit_code(kExitDomainFailure);
  }
}

void run_store_generations(Script& script, fc::Clock& clock, Output& out) {
  std::filesystem::path path;
  if (!read_required_path(script, path, out)) {
    return;
  }
  if (!finish_options(script, out)) {
    return;
  }
  fc::StoreOpenOptions options;
  options.access = fc::StoreAccess::reader;
  fc::Result<fc::CapacityStore> store = fc::CapacityStore::open(path, options, clock);
  if (!check(store, out)) {
    return;
  }
  fc::Result<std::vector<fc::GenerationRecord>> records = store.value().list_generations();
  if (!check(records, out)) {
    return;
  }
  out.line("generations=" + std::to_string(records.value().size()));
  for (const fc::GenerationRecord& record : records.value()) {
    out.line(record.to_string());
  }
}

void run_store_recover(Script& script, fc::Clock& clock, Output& out) {
  std::filesystem::path path;
  if (!read_required_path(script, path, out)) {
    return;
  }
  if (!finish_options(script, out)) {
    return;
  }
  fc::StoreOpenOptions options;
  options.access = fc::StoreAccess::reader;
  fc::Result<fc::CapacityStore> store = fc::CapacityStore::open(path, options, clock);
  if (!check(store, out)) {
    return;
  }
  fc::Result<fc::RecoveredState> recovered = recover_state(store.value());
  if (!check(recovered, out)) {
    return;
  }
  out.block(recovered.value().snapshot->describe());
  out.line("recovery_notes=" + std::to_string(recovered.value().notes.size()));
  out.block(recovered.value().notes.to_string());
}

void run_store_prune(Script& script, fc::Clock& clock, Output& out) {
  std::filesystem::path path;
  std::uint64_t keep = 0;
  if (!read_required_path(script, path, out)) {
    return;
  }
  if (!read_required_u64(script, "keep", keep, out)) {
    return;
  }
  if (!finish_options(script, out)) {
    return;
  }
  fc::StoreOpenOptions options;
  options.access = fc::StoreAccess::writer;
  fc::Result<fc::CapacityStore> store = fc::CapacityStore::open(path, options, clock);
  if (!check(store, out)) {
    return;
  }
  fc::Result<fc::PruneReport> report = store.value().prune(static_cast<std::size_t>(keep));
  if (!check(report, out)) {
    return;
  }
  out.block(report.value().describe());
}

// ---------------------------------------------------------------------------
// Commands: declarations
// ---------------------------------------------------------------------------

void run_evidence_declare(Script& script, fc::Clock& clock, Output& out) {
  std::filesystem::path path;
  std::string source_text;
  std::string detail;
  fc::CapacityDimension dimension = fc::CapacityDimension::space;
  std::uint64_t generation_value = 0;
  fc::OperationalState state = fc::OperationalState::nominal;
  fc::Measured installed;
  fc::Measured observed;
  fc::Measured usable;
  fc::Measured unavailable;
  fc::Measured residual;
  fc::Measured protected_capacity;
  fc::Measured reserved;
  std::optional<std::uint64_t> observed_at;
  std::optional<std::uint64_t> provenance_epoch;
  std::optional<std::uint64_t> provenance_incarnation;

  if (!read_required_path(script, path, out)) {
    return;
  }
  if (!read_required_text(script, "source", source_text, out)) {
    return;
  }
  if (!read_required_dimension(script, "dimension", dimension, out)) {
    return;
  }
  if (!read_required_bounded(script, "generation", static_cast<std::uint64_t>(fc::limits::max_quantity_magnitude),
                             "maximum generation magnitude", generation_value, out)) {
    return;
  }
  if (!read_measured(script, "installed", dimension, installed, out)) {
    return;
  }
  if (!read_measured(script, "observed", dimension, observed, out)) {
    return;
  }
  if (!read_measured(script, "usable", dimension, usable, out)) {
    return;
  }
  if (!read_measured(script, "unavailable", dimension, unavailable, out)) {
    return;
  }
  if (!read_measured(script, "residual", dimension, residual, out)) {
    return;
  }
  if (!read_measured(script, "protected", dimension, protected_capacity, out)) {
    return;
  }
  if (!read_measured(script, "reserved", dimension, reserved, out)) {
    return;
  }
  const bool derive_residual = script.flag("derive-residual");
  if (!read_optional_operational_state(script, state, out)) {
    return;
  }
  detail = script.option_or("detail", std::string());
  if (!read_optional_u64(script, "observed-at", observed_at, out)) {
    return;
  }
  if (!read_optional_u64(script, "epoch", provenance_epoch, out)) {
    return;
  }
  if (!read_optional_u64(script, "incarnation", provenance_incarnation, out)) {
    return;
  }
  if (!finish_options(script, out)) {
    return;
  }

  fc::Result<fc::CapacitySourceId> source = fc::CapacitySourceId::parse(source_text);
  if (!check(source, out)) {
    return;
  }
  const fc::Result<fc::Tick> now = clock.now();
  if (!check(now, out)) {
    return;
  }
  const fc::Tick observed_instant = observed_at.has_value() ? fc::Tick::from_value(observed_at.value()) : now.value();

  fc::Result<Session> session = open_write_session(path, clock);
  if (!check(session, out)) {
    return;
  }
  if (session.value().model_was_created) {
    out.line("note=the store held no published generation, so the model was created with an empty coverage contract");
  }

  fc::SourceEvidenceFields fields;
  fields.source = source.value();
  fields.dimension = dimension;
  fields.unit = fc::canonical_unit(dimension);
  fields.generation = fc::EvidenceGeneration::from_value(generation_value);
  fields.observed_at = observed_instant;
  fields.provenance.source = source.value();
  fields.provenance.revision = std::string(kToolRevision);
  fields.provenance.produced_at = observed_instant;
  fields.provenance.epoch = fc::EpochId::from_value(provenance_epoch.value_or(session.value().model.epoch().value()));
  fields.provenance.incarnation =
      fc::IncarnationId::from_value(provenance_incarnation.value_or(session.value().model.incarnation().value()));
  fields.state = state;
  fields.state_detail = detail;
  fields.installed = installed;
  fields.observed = observed;
  fields.usable = usable;
  fields.unavailable = unavailable;
  fields.residual = residual;
  fields.protected_capacity = protected_capacity;
  fields.reserved = reserved;
  fields.derive_residual = derive_residual;

  fc::Result<fc::SourceEvidence> evidence = fc::SourceEvidence::create(std::move(fields));
  if (!check(evidence, out)) {
    return;
  }
  const fc::SourceEvidence declared = evidence.value();

  std::shared_ptr<const fc::CapacitySnapshot> snapshot;
  const fc::Status status = mutate_publish_commit(
      session.value(), clock,
      [&declared](fc::FacilityCapacityModel& model, const fc::CapacityPrecondition& precondition) {
        return model.declare_evidence(declared, precondition);
      },
      snapshot);
  if (!check(status, out)) {
    return;
  }
  out.line("evidence=" + declared.to_string());
  append_commit_summary(out, *snapshot);
}

void run_reserve_declare(Script& script, fc::Clock& clock, Output& out) {
  std::filesystem::path path;
  std::string id_text;
  std::string source_text;
  std::string owner_text;
  fc::CapacityDimension dimension = fc::CapacityDimension::space;
  fc::ReserveKind kind = fc::ReserveKind::protection;
  fc::Measured amount;
  fc::ValidityWindow window;

  if (!read_required_path(script, path, out)) {
    return;
  }
  if (!read_required_text(script, "id", id_text, out)) {
    return;
  }
  if (!read_required_text(script, "source", source_text, out)) {
    return;
  }
  if (!read_required_dimension(script, "dimension", dimension, out)) {
    return;
  }
  if (!read_required_reserve_kind(script, kind, out)) {
    return;
  }
  if (!read_measured(script, "amount", dimension, amount, out)) {
    return;
  }
  if (!read_validity_window(script, window, out)) {
    return;
  }
  if (script.has_option("owner") && !read_required_text(script, "owner", owner_text, out)) {
    return;
  }
  if (!finish_options(script, out)) {
    return;
  }

  fc::Result<fc::ReserveId> id = fc::ReserveId::parse(id_text);
  if (!check(id, out)) {
    return;
  }
  fc::Result<fc::CapacitySourceId> source = fc::CapacitySourceId::parse(source_text);
  if (!check(source, out)) {
    return;
  }
  std::optional<fc::OwnerId> owner;
  if (!owner_text.empty()) {
    fc::Result<fc::OwnerId> parsed = fc::OwnerId::parse(owner_text);
    if (!check(parsed, out)) {
      return;
    }
    owner = parsed.value();
  }

  const fc::Result<fc::Tick> now = clock.now();
  if (!check(now, out)) {
    return;
  }
  fc::Result<Session> session = open_write_session(path, clock);
  if (!check(session, out)) {
    return;
  }
  if (session.value().model_was_created) {
    out.line("note=the store held no published generation, so the model was created with an empty coverage contract");
  }

  fc::CapacityReserveFields fields;
  fields.id = id.value();
  fields.source = source.value();
  fields.dimension = dimension;
  fields.unit = fc::canonical_unit(dimension);
  fields.kind = kind;
  fields.amount = amount;
  fields.window = window;
  if (owner.has_value()) {
    fields.owner = owner.value();
  }
  fields.provenance.source = source.value();
  fields.provenance.revision = std::string(kToolRevision);
  fields.provenance.produced_at = now.value();
  fields.provenance.epoch = session.value().model.epoch();
  fields.provenance.incarnation = session.value().model.incarnation();

  fc::Result<fc::CapacityReserve> reserve = fc::CapacityReserve::create(std::move(fields));
  if (!check(reserve, out)) {
    return;
  }
  const fc::CapacityReserve declared = reserve.value();

  std::shared_ptr<const fc::CapacitySnapshot> snapshot;
  const fc::Status status = mutate_publish_commit(
      session.value(), clock,
      [&declared](fc::FacilityCapacityModel& model, const fc::CapacityPrecondition& precondition) {
        return model.declare_reserve(declared, precondition);
      },
      snapshot);
  if (!check(status, out)) {
    return;
  }
  out.line("reserve=" + declared.to_string());
  append_commit_summary(out, *snapshot);
}

void run_constraint_declare(Script& script, fc::Clock& clock, Output& out) {
  std::filesystem::path path;
  std::string id_text;
  std::string service_class_text;
  fc::CapacityDimension dimension = fc::CapacityDimension::space;
  fc::Measured floor_value;
  fc::ValidityWindow window;

  if (!read_required_path(script, path, out)) {
    return;
  }
  if (!read_required_text(script, "id", id_text, out)) {
    return;
  }
  if (!read_required_dimension(script, "dimension", dimension, out)) {
    return;
  }
  if (!read_measured(script, "floor", dimension, floor_value, out)) {
    return;
  }
  if (script.has_option("service-class") && !read_required_text(script, "service-class", service_class_text, out)) {
    return;
  }
  if (!read_validity_window(script, window, out)) {
    return;
  }
  if (!finish_options(script, out)) {
    return;
  }

  fc::Result<fc::ConstraintId> id = fc::ConstraintId::parse(id_text);
  if (!check(id, out)) {
    return;
  }
  std::optional<fc::ServiceClassId> service_class;
  if (!service_class_text.empty()) {
    fc::Result<fc::ServiceClassId> parsed = fc::ServiceClassId::parse(service_class_text);
    if (!check(parsed, out)) {
      return;
    }
    service_class = parsed.value();
  }
  fc::Result<fc::CapacitySourceId> operator_source = fc::CapacitySourceId::parse(kOperatorSource);
  if (!check(operator_source, out)) {
    return;
  }

  const fc::Result<fc::Tick> now = clock.now();
  if (!check(now, out)) {
    return;
  }
  fc::Result<Session> session = open_write_session(path, clock);
  if (!check(session, out)) {
    return;
  }
  if (session.value().model_was_created) {
    out.line("note=the store held no published generation, so the model was created with an empty coverage contract");
  }

  fc::ServiceConstraintFields fields;
  fields.id = id.value();
  fields.dimension = dimension;
  fields.unit = fc::canonical_unit(dimension);
  if (service_class.has_value()) {
    fields.service_class = service_class.value();
  }
  fields.floor = floor_value;
  fields.window = window;
  fields.provenance.source = operator_source.value();
  fields.provenance.revision = std::string(kToolRevision);
  fields.provenance.produced_at = now.value();
  fields.provenance.epoch = session.value().model.epoch();
  fields.provenance.incarnation = session.value().model.incarnation();

  fc::Result<fc::ServiceConstraint> constraint = fc::ServiceConstraint::create(std::move(fields));
  if (!check(constraint, out)) {
    return;
  }
  const fc::ServiceConstraint declared = constraint.value();

  std::shared_ptr<const fc::CapacitySnapshot> snapshot;
  const fc::Status status = mutate_publish_commit(
      session.value(), clock,
      [&declared](fc::FacilityCapacityModel& model, const fc::CapacityPrecondition& precondition) {
        return model.declare_constraint(declared, precondition);
      },
      snapshot);
  if (!check(status, out)) {
    return;
  }
  out.line("constraint=" + declared.to_string());
  append_commit_summary(out, *snapshot);
}

// ---------------------------------------------------------------------------
// Commands: reads
// ---------------------------------------------------------------------------

/// Recovers the published state of a store through a reader handle.
fc::Result<fc::RecoveredState> recover_reader(const std::filesystem::path& path, fc::Clock& clock) {
  fc::StoreOpenOptions options;
  options.access = fc::StoreAccess::reader;
  fc::Result<fc::CapacityStore> store = fc::CapacityStore::open(path, options, clock);
  if (!store.has_value()) {
    return store.error();
  }
  return recover_state(store.value());
}

void run_query(Script& script, fc::Clock& clock, Output& out) {
  std::filesystem::path path;
  fc::CapacityDimension dimension = fc::CapacityDimension::space;
  if (!read_required_path(script, path, out)) {
    return;
  }
  if (!read_required_dimension(script, "dimension", dimension, out)) {
    return;
  }
  if (!finish_options(script, out)) {
    return;
  }
  fc::Result<fc::RecoveredState> recovered = recover_reader(path, clock);
  if (!check(recovered, out)) {
    return;
  }
  fc::Result<fc::CapacityAnswer> answer = recovered.value().model.query(dimension);
  if (!check(answer, out)) {
    return;
  }
  out.block(answer.value().describe());
}

void run_snapshot_show(Script& script, fc::Clock& clock, Output& out) {
  std::filesystem::path path;
  if (!read_required_path(script, path, out)) {
    return;
  }
  if (!finish_options(script, out)) {
    return;
  }
  fc::Result<fc::RecoveredState> recovered = recover_reader(path, clock);
  if (!check(recovered, out)) {
    return;
  }
  out.block(recovered.value().snapshot->describe());
}

void run_snapshot_explain(Script& script, fc::Clock& clock, Output& out) {
  std::filesystem::path path;
  fc::CapacityDimension dimension = fc::CapacityDimension::space;
  if (!read_required_path(script, path, out)) {
    return;
  }
  if (!read_required_dimension(script, "dimension", dimension, out)) {
    return;
  }
  if (!finish_options(script, out)) {
    return;
  }
  fc::Result<fc::RecoveredState> recovered = recover_reader(path, clock);
  if (!check(recovered, out)) {
    return;
  }
  out.block(recovered.value().snapshot->explain(dimension));
}

void run_revalidate(Script& script, fc::Clock& clock, Output& out) {
  std::filesystem::path path;
  if (!read_required_path(script, path, out)) {
    return;
  }
  if (!finish_options(script, out)) {
    return;
  }
  fc::Result<fc::RecoveredState> recovered = recover_reader(path, clock);
  if (!check(recovered, out)) {
    return;
  }
  // A recovered snapshot is stamped `recovered` and is never relied on until
  // the current model has been asked whether it still stands.
  fc::Result<fc::RevalidationReport> report = recovered.value().model.revalidate(*recovered.value().snapshot);
  if (!check(report, out)) {
    return;
  }
  out.block(report.value().describe());
  if (report.value().rejected()) {
    out.set_exit_code(kExitDomainFailure);
  }
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

void dispatch(Script& script, fc::Clock& clock, Output& out) {
  if (script.command_is("version")) {
    run_version(script, out);
    return;
  }
  if (script.command_is("store", "init")) {
    run_store_init(script, clock, out);
    return;
  }
  if (script.command_is("store", "info")) {
    run_store_info(script, clock, out);
    return;
  }
  if (script.command_is("store", "verify")) {
    run_store_verify(script, clock, out);
    return;
  }
  if (script.command_is("store", "generations")) {
    run_store_generations(script, clock, out);
    return;
  }
  if (script.command_is("store", "recover")) {
    run_store_recover(script, clock, out);
    return;
  }
  if (script.command_is("store", "prune")) {
    run_store_prune(script, clock, out);
    return;
  }
  if (script.command_is("evidence", "declare")) {
    run_evidence_declare(script, clock, out);
    return;
  }
  if (script.command_is("reserve", "declare")) {
    run_reserve_declare(script, clock, out);
    return;
  }
  if (script.command_is("constraint", "declare")) {
    run_constraint_declare(script, clock, out);
    return;
  }
  if (script.command_is("query")) {
    run_query(script, clock, out);
    return;
  }
  if (script.command_is("snapshot", "show")) {
    run_snapshot_show(script, clock, out);
    return;
  }
  if (script.command_is("snapshot", "explain")) {
    run_snapshot_explain(script, clock, out);
    return;
  }
  if (script.command_is("revalidate")) {
    run_revalidate(script, clock, out);
    return;
  }
  if (script.command().empty()) {
    fail_usage(out, fc::Error::make(fc::ErrorCode::invalid_argument, "no command given"));
    return;
  }
  fail_usage(out, fc::Error::make(fc::ErrorCode::invalid_argument, "unknown command '" + script.command_text() + "'"));
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> tokens;
  for (int index = 1; index < argc; ++index) {
    tokens.emplace_back(argv[index]);
  }
  if (tokens.empty()) {
    print_usage(stderr);
    return kExitUsageError;
  }
  fc::Result<Script> parsed = Script::parse(tokens);
  if (!parsed.has_value()) {
    const std::string text = format_error(parsed.error());
    std::fputs(text.c_str(), stderr);
    std::fputc('\n', stderr);
    return kExitUsageError;
  }
  Script& script = parsed.value();
  if (script.help_requested()) {
    print_usage(stdout);
    return kExitOk;
  }
  fc::SystemClock clock;
  Output out;
  dispatch(script, clock, out);
  return out.emit();
}
