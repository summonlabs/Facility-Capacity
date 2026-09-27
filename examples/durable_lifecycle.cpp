// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Durable lifecycle: create a store, commit a published answer to it, DESTROY
// the store object and the model, then open the directory again as a fresh
// process would and recover what was committed.
//
// EVERY QUANTITY IN THIS FILE IS SYNTHETIC.
//
// The point of the program is the second half. `recover()` returns a snapshot
// stamped `SnapshotFreshness::recovered`, never `issued`, and a recovered
// snapshot is never silently current: the caller must ask the recovered model
// whether the snapshot still stands before relying on it. This program does
// exactly that and prints the verdict.
//
// Usage: fcap_example_durable_lifecycle <store-directory>
//
// Every `Result` is checked. A failure prints one machine-readable line to
// standard error and returns 1; success returns 0.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "dccp/facility_capacity/capacity.hpp"

namespace {

namespace fc = dccp::facility_capacity;

/// SYNTHETIC control-plane authority for this example.
constexpr std::uint64_t kEpoch = 11;
constexpr std::uint64_t kIncarnation = 4;

/// SYNTHETIC provenance revision of the sources in this example.
constexpr std::string_view kRevision = "example-durable-lifecycle/1.0.0";

/// Deterministic failure line, then the process exit code for that failure.
int fail(const fc::Error& error) {
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
  std::fputs(line.c_str(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

/// True when the call failed; the failure has already been reported.
template <class T>
bool failed(const fc::Result<T>& result) {
  if (result.has_value()) {
    return false;
  }
  (void)fail(result.error());
  return true;
}

void print(std::string_view text) {
  const std::string owned(text);
  std::fputs(owned.c_str(), stdout);
  std::fputc('\n', stdout);
}

void print_block(const std::string& text) {
  std::fputs(text.c_str(), stdout);
  if (!text.empty() && text.back() != '\n') {
    std::fputc('\n', stdout);
  }
}

void print_section(std::string_view title) {
  const std::string owned(title);
  std::fputs("--- ", stdout);
  std::fputs(owned.c_str(), stdout);
  std::fputs(" ---\n", stdout);
}

/// SYNTHETIC inputs for one evidence record. `residual` is derived by the
/// library as `installed - unavailable - usable`, and both roll-ups are stated
/// explicitly so nothing is guessed.
struct SyntheticEvidence {
  std::string source;
  fc::CapacityDimension dimension = fc::CapacityDimension::space;
  std::int64_t installed = 0;
  std::int64_t usable = 0;
  std::int64_t unavailable = 0;
};

fc::Result<fc::SourceEvidence> build_evidence(const SyntheticEvidence& spec, fc::Tick observed_at) {
  const fc::Unit unit = fc::canonical_unit(spec.dimension);
  const fc::Result<fc::CapacitySourceId> source = fc::CapacitySourceId::parse(spec.source);
  if (!source.has_value()) {
    return source.error();
  }
  const fc::Result<fc::Measured> installed = fc::Measured::known(unit, spec.installed);
  if (!installed.has_value()) {
    return installed.error();
  }
  const fc::Result<fc::Measured> usable = fc::Measured::known(unit, spec.usable);
  if (!usable.has_value()) {
    return usable.error();
  }
  const fc::Result<fc::Measured> unavailable = fc::Measured::known(unit, spec.unavailable);
  if (!unavailable.has_value()) {
    return unavailable.error();
  }
  const fc::Result<fc::Measured> zero = fc::Measured::known(unit, 0);
  if (!zero.has_value()) {
    return zero.error();
  }

  fc::SourceEvidenceFields fields;
  fields.source = source.value();
  fields.dimension = spec.dimension;
  fields.unit = unit;
  fields.generation = fc::EvidenceGeneration::from_value(1);
  fields.observed_at = observed_at;
  fields.provenance.source = source.value();
  fields.provenance.revision = std::string(kRevision);
  fields.provenance.produced_at = observed_at;
  fields.provenance.epoch = fc::EpochId::from_value(kEpoch);
  fields.provenance.incarnation = fc::IncarnationId::from_value(kIncarnation);
  fields.state = fc::OperationalState::nominal;
  fields.installed = installed.value();
  fields.usable = usable.value();
  fields.unavailable = unavailable.value();
  fields.derive_residual = true;
  fields.protected_capacity = zero.value();
  fields.reserved = zero.value();
  return fc::SourceEvidence::create(std::move(fields));
}

fc::Status declare(fc::FacilityCapacityModel& model, const fc::SourceEvidence& evidence, std::uint64_t attempt) {
  fc::CapacityPrecondition precondition = model.current_precondition();
  precondition.attempt = fc::AttemptId::from_value(attempt);
  return model.declare_evidence(evidence, precondition);
}

fc::Result<std::shared_ptr<const fc::CapacitySnapshot>> publish(fc::FacilityCapacityModel& model,
                                                                std::uint64_t attempt) {
  fc::CapacityPrecondition precondition = model.current_precondition();
  precondition.attempt = fc::AttemptId::from_value(attempt);
  return model.publish(precondition);
}

void print_authority(const fc::StoreAuthority& authority) {
  print("store=" + authority.store_id.value());
  print("facility=" + authority.facility.value());
  print("site=" + authority.site.value());
  print("controller=" + authority.controller.value());
  print("epoch=" + authority.epoch.to_string());
  print("incarnation=" + authority.incarnation.to_string());
  print("published_generation=" + authority.published_generation.to_string());
  print("revision=" + authority.revision.to_string());
  print("published_file=" + (authority.has_published_generation ? authority.file_name : std::string("-")));
  print("snapshot_digest=" + (authority.snapshot_digest.empty() ? std::string("-") : authority.snapshot_digest));
}

/// SYNTHETIC sources: one power feed and one cooling loop.
std::vector<SyntheticEvidence> synthetic_sources() {
  std::vector<SyntheticEvidence> sources;

  SyntheticEvidence power;
  power.source = "power-feed-a";
  power.dimension = fc::CapacityDimension::power;
  power.installed = 6'000'000;  // SYNTHETIC: 6.0 kW of IT load
  power.usable = 5'400'000;
  power.unavailable = 600'000;
  sources.push_back(power);

  SyntheticEvidence cooling;
  cooling.source = "cooling-loop-a";
  cooling.dimension = fc::CapacityDimension::cooling;
  cooling.installed = 7'000'000;  // SYNTHETIC: 7.0 kW of heat removal
  cooling.usable = 6'500'000;
  cooling.unavailable = 500'000;
  sources.push_back(cooling);

  return sources;
}

/// Runs the first half: creates the store, builds and publishes one answer and
/// commits it.
///
/// The store handle and the model are scoped away before this function
/// returns, so everything the next section sees comes from the directory and
/// from nothing else.
int create_and_commit(const std::filesystem::path& root, fc::Clock& clock) {
  const fc::Result<fc::FacilityId> facility = fc::FacilityId::parse("facility-a");
  if (failed(facility)) {
    return 1;
  }
  const fc::Result<fc::SiteId> site = fc::SiteId::parse("site-1");
  if (failed(site)) {
    return 1;
  }
  const fc::Result<fc::ControllerId> controller = fc::ControllerId::parse("controller-a");
  if (failed(controller)) {
    return 1;
  }

  fc::CapacityRequirement power_requirement;
  power_requirement.dimension = fc::CapacityDimension::power;
  power_requirement.required = true;
  fc::CapacityRequirement cooling_requirement;
  cooling_requirement.dimension = fc::CapacityDimension::cooling;
  cooling_requirement.required = true;

  fc::StoreCreateOptions options;
  options.facility = facility.value();
  options.site = site.value();
  options.epoch = fc::EpochId::from_value(kEpoch);
  options.incarnation = fc::IncarnationId::from_value(kIncarnation);
  options.controller = controller.value();

  fc::Result<fc::CapacityStore> store = fc::CapacityStore::create(root, options, clock);
  if (failed(store)) {
    return 1;
  }

  {
    fc::ModelConfig config;
    config.facility = options.facility;
    config.site = options.site;
    config.epoch = options.epoch;
    config.incarnation = options.incarnation;
    // A store records identity and authority; the coverage contract belongs to
    // the model and becomes durable when this first generation is committed.
    config.requirements = {power_requirement, cooling_requirement};
    fc::Result<fc::FacilityCapacityModel> model = fc::FacilityCapacityModel::create(std::move(config), clock);
    if (failed(model)) {
      return 1;
    }

    const fc::Result<fc::Tick> now = clock.now();
    if (failed(now)) {
      return 1;
    }

    std::uint64_t attempt = 0;
    for (const SyntheticEvidence& spec : synthetic_sources()) {
      const fc::Result<fc::SourceEvidence> evidence = build_evidence(spec, now.value());
      if (failed(evidence)) {
        return 1;
      }
      ++attempt;
      const fc::Status declared = declare(model.value(), evidence.value(), attempt);
      if (failed(declared)) {
        return 1;
      }
    }

    // Publication advances both the model generation and the model revision, so
    // the commit takes the post-publish precondition with a fresh token of its
    // own: the generation it names is the one the commit is publishing.
    ++attempt;
    const fc::Result<std::shared_ptr<const fc::CapacitySnapshot>> published = publish(model.value(), attempt);
    if (failed(published)) {
      return 1;
    }
    fc::CapacityPrecondition commit = model.value().current_precondition();
    commit.attempt = fc::AttemptId::from_value(++attempt);
    const fc::Result<fc::StoreAuthority> committed = store.value().commit(model.value(), commit);
    if (failed(committed)) {
      return 1;
    }
    print_section("authority after commit");
    print_authority(committed.value());
  }  // the model is destroyed here

  return 0;
}  // the store handle is destroyed here

/// Opens the directory again and exercises the recovery path end to end.
int recover_and_inspect(const std::filesystem::path& root, fc::Clock& clock) {
  print_section("reopening the directory");
  fc::StoreOpenOptions options;
  options.access = fc::StoreAccess::writer;
  fc::Result<fc::CapacityStore> store = fc::CapacityStore::open(root, options, clock);
  if (failed(store)) {
    return 1;
  }

  fc::Result<fc::RecoveredState> recovered = store.value().recover();
  if (failed(recovered)) {
    return 1;
  }
  print_block(recovered.value().snapshot->describe());
  print(std::string("snapshot_freshness=") +
        std::string(fc::snapshot_freshness_name(recovered.value().snapshot->freshness())));
  print("recovery_notes=" + std::to_string(recovered.value().notes.size()));
  print_block(recovered.value().notes.to_string());

  print_section("revalidation of the recovered snapshot");
  const fc::Result<fc::RevalidationReport> report = recovered.value().model.revalidate(*recovered.value().snapshot);
  if (failed(report)) {
    return 1;
  }
  print_block(report.value().describe());

  print_section("deep verification of the store");
  const fc::Result<fc::VerifyReport> verified = store.value().verify(true);
  if (failed(verified)) {
    return 1;
  }
  print_block(verified.value().describe());

  print_section("pruning to the two newest generations");
  const fc::Result<fc::PruneReport> pruned = store.value().prune(2);
  if (failed(pruned)) {
    return 1;
  }
  print_block(pruned.value().describe());

  print("note=the recovered snapshot is stamped `recovered`, not `issued`: it must be revalidated against "
        "the current model before it is relied on");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return fail(fc::Error::make(fc::ErrorCode::invalid_argument,
                                "usage: fcap_example_durable_lifecycle <store-directory>"));
  }
  const std::filesystem::path root(argv[1]);

  std::error_code code;
  std::filesystem::create_directories(root, code);
  if (code) {
    return fail(fc::Error::make(fc::ErrorCode::io_failure, "could not create the store directory"));
  }
  if (!std::filesystem::is_directory(root)) {
    return fail(fc::Error::make(fc::ErrorCode::path_rejected, "the store path is not a directory"));
  }
  if (std::filesystem::exists(root / "FORMAT", code)) {
    return fail(fc::Error::make(fc::ErrorCode::already_exists,
                                "the directory already holds a Facility Capacity store; pass a fresh path"));
  }
  if (code) {
    return fail(fc::Error::make(fc::ErrorCode::io_failure, "could not inspect the store directory"));
  }

  // Time is injected and this program is the only thing that advances it, so
  // two runs over the same directory produce the same bytes.
  fc::ManualClock clock(fc::Tick::from_value(1'000));
  const int created = create_and_commit(root, clock);
  if (created != 0) {
    return created;
  }
  return recover_and_inspect(root, clock);
}
