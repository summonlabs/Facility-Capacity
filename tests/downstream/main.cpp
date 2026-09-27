// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// An independent consumer of the installed Facility Capacity package.
//
// It includes only the installed umbrella header, links only the exported
// namespaced target, and runs a real minimal lifecycle against a store in a
// directory the caller supplies. It is built out of tree against an installed
// prefix; nothing here reaches into the Facility Capacity source tree.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <dccp/facility_capacity/capacity.hpp>

namespace {

namespace fs = std::filesystem;
using namespace dccp::facility_capacity;

int fail(const std::string& message) {
  std::fprintf(stderr, "downstream failure: %s\n", message.c_str());
  return 1;
}

template <class T>
bool ok(const Result<T>& result, const char* what) {
  if (!result.has_value()) {
    std::fprintf(stderr, "downstream failure at %s: %s\n", what, result.error().to_string().c_str());
    return false;
  }
  return true;
}

bool ok(const Status& status, const char* what) {
  if (!status.has_value()) {
    std::fprintf(stderr, "downstream failure at %s: %s\n", what, status.error().to_string().c_str());
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: downstream_consumer <store-directory>\n");
    return 2;
  }
  const fs::path root = fs::path(argv[1]);

  std::printf("facility_capacity version %s\n", std::string(version_string).c_str());
  std::printf("canonical format version %u, store format version %u\n", canonical_format_version,
              store_format_version);

  ManualClock clock(Tick::from_value(1000000));

  StoreCreateOptions create_options;
  create_options.facility = FacilityId::parse("facility-downstream").value();
  create_options.site = SiteId::parse("site-downstream").value();
  create_options.epoch = EpochId::from_value(11);
  create_options.incarnation = IncarnationId::from_value(5);
  create_options.controller = ControllerId::parse("controller-downstream").value();

  auto store = CapacityStore::create(root, create_options, clock);
  if (!ok(store, "CapacityStore::create")) {
    return 1;
  }

  ModelConfig config;
  config.facility = create_options.facility;
  config.site = create_options.site;
  config.epoch = create_options.epoch;
  config.incarnation = create_options.incarnation;
  CapacityRequirement requirement;
  requirement.dimension = CapacityDimension::power;
  requirement.required = true;
  const auto source = CapacitySourceId::parse("power-capacity");
  if (!ok(source, "CapacitySourceId::parse")) {
    return 1;
  }
  requirement.required_sources.push_back(source.value());
  if (!ok(requirement.normalize(), "CapacityRequirement::normalize")) {
    return 1;
  }
  config.requirements.push_back(requirement);

  auto model = FacilityCapacityModel::create(config, clock);
  if (!ok(model, "FacilityCapacityModel::create")) {
    return 1;
  }

  SourceEvidenceFields fields;
  fields.source = source.value();
  fields.dimension = CapacityDimension::power;
  fields.unit = canonical_unit(CapacityDimension::power);
  fields.generation = EvidenceGeneration::from_value(1);
  fields.observed_at = Tick::from_value(900000);
  fields.provenance.source = source.value();
  fields.provenance.revision = "downstream/1.0.0";
  fields.provenance.produced_at = Tick::from_value(900000);
  fields.provenance.epoch = create_options.epoch;
  fields.provenance.incarnation = create_options.incarnation;
  fields.state = OperationalState::nominal;
  fields.installed = Measured::known(fields.unit, 2000000).value();
  fields.observed = fields.installed;
  fields.usable = Measured::known(fields.unit, 1800000).value();
  fields.unavailable = Measured::known(fields.unit, 100000).value();
  fields.derive_residual = true;
  fields.protected_capacity = Measured::known(fields.unit, 200000).value();
  fields.reserved = Measured::known(fields.unit, 100000).value();

  auto evidence = SourceEvidence::create(fields);
  if (!ok(evidence, "SourceEvidence::create")) {
    return 1;
  }

  CapacityPrecondition precondition = model.value().current_precondition();
  precondition.attempt = AttemptId::from_value(1);
  if (!ok(model.value().declare_evidence(evidence.value(), precondition), "declare_evidence")) {
    return 1;
  }

  precondition = model.value().current_precondition();
  precondition.attempt = AttemptId::from_value(2);
  auto snapshot = model.value().publish(precondition);
  if (!ok(snapshot, "publish")) {
    return 1;
  }

  precondition = model.value().current_precondition();
  precondition.attempt = AttemptId::from_value(3);
  auto authority = store.value().commit(model.value(), precondition);
  if (!ok(authority, "commit")) {
    return 1;
  }
  std::printf("committed generation %s\n", authority.value().published_generation.to_string().c_str());
  std::printf("snapshot digest %s\n", authority.value().snapshot_digest.c_str());

  // Reopen from disk and recover the whole authoritative state.
  auto reopened = CapacityStore::open(root, StoreOpenOptions{}, clock);
  if (!ok(reopened, "CapacityStore::open")) {
    return 1;
  }
  auto recovered = reopened.value().recover();
  if (!ok(recovered, "recover")) {
    return 1;
  }
  std::printf("recovered freshness %s\n",
              std::string(snapshot_freshness_name(recovered.value().snapshot->freshness())).c_str());
  if (recovered.value().snapshot->freshness() != SnapshotFreshness::recovered) {
    return fail("a recovered snapshot must not claim to be freshly issued");
  }
  if (recovered.value().snapshot->digest() != authority.value().snapshot_digest) {
    return fail("the recovered snapshot does not match the committed digest");
  }

  auto answer = recovered.value().model.query(CapacityDimension::power);
  if (!ok(answer, "query")) {
    return 1;
  }
  std::printf("outcome %s allocatable %s\n",
              std::string(capacity_outcome_name(answer.value().outcome)).c_str(),
              answer.value().totals.allocatable.to_string().c_str());

  auto report = reopened.value().verify(true);
  if (!ok(report, "verify")) {
    return 1;
  }
  if (!report.value().ok) {
    return fail("the store did not verify: " + report.value().describe());
  }
  std::printf("verify ok, %llu files checked\n",
              static_cast<unsigned long long>(report.value().files_checked));

  auto pruned = reopened.value().prune(1);
  if (!ok(pruned, "prune")) {
    return 1;
  }
  std::printf("pruned %zu file(s)\n", pruned.value().removed);

  if (!ok(CapacityStore::destroy(root), "destroy")) {
    return 1;
  }
  std::printf("downstream consumer completed\n");
  return 0;
}
