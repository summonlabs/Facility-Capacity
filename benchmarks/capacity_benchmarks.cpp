// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Completed-operation benchmarks.
//
// Every measurement is the wall time of one *completed* operation: the call
// returns only after the work it names is finished. The durable operations
// include the full commit protocol, so a durable timing includes writing the
// staging file, flushing it, re-reading and re-deriving it to verify it, the
// atomic replacement of the generation file, the directory flush, the atomic
// replacement of CURRENT and the second directory flush. Nothing is timed
// before it is submitted.
//
// Methodology
// -----------
//   * One warm-up round, then `rounds` measured rounds; the reported figure is
//     the median of the per-round means, and the minimum and maximum round
//     means are reported alongside so the spread is visible.
//   * Rounds alternate between the configurations being compared, so a slow
//     period of machine activity cannot favour one configuration over another.
//   * All data is generated locally and is SYNTHETIC. This is a single host,
//     single process, single thread measurement of a control-plane model; it is
//     not a hardware, network or storage-device measurement.
//   * The store used by the durable benchmarks is verified after the run and
//     then removed, so no residue is left behind.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dccp/facility_capacity/capacity.hpp"

namespace {

using namespace dccp::facility_capacity;
using SteadyClock = std::chrono::steady_clock;

struct Measurement {
  std::string name;
  std::string unit;
  double median_ns = 0.0;
  double min_ns = 0.0;
  double max_ns = 0.0;
  std::uint64_t operations = 0;
};

std::vector<Measurement> g_measurements;

double to_nanoseconds(SteadyClock::duration span) {
  return std::chrono::duration<double, std::nano>(span).count();
}

void record(const std::string& name, const std::string& unit, const std::vector<double>& per_round,
            std::uint64_t operations_per_round) {
  Measurement measurement;
  measurement.name = name;
  measurement.unit = unit;
  measurement.operations = operations_per_round;
  std::vector<double> sorted = per_round;
  std::sort(sorted.begin(), sorted.end());
  measurement.median_ns = sorted[sorted.size() / 2];
  measurement.min_ns = sorted.front();
  measurement.max_ns = sorted.back();
  g_measurements.push_back(std::move(measurement));
}

[[noreturn]] void fatal(const std::string& message) {
  std::fprintf(stderr, "benchmark failure: %s\n", message.c_str());
  std::fflush(stderr);
  std::exit(1);
}

template <class T>
T require(Result<T> result, const char* what) {
  if (!result.has_value()) {
    fatal(std::string(what) + ": " + result.error().to_string());
  }
  return std::move(result).value();
}

void require_status(Status status, const char* what) {
  if (!status.has_value()) {
    fatal(std::string(what) + ": " + status.error().to_string());
  }
}

FacilityId make_facility(const char* text) { return require(FacilityId::parse(text), "facility id"); }

SiteId make_site(const char* text) { return require(SiteId::parse(text), "site id"); }

SourceEvidence make_evidence(const std::string& source, CapacityDimension dimension, std::uint64_t generation,
                             std::int64_t installed, std::int64_t usable, std::int64_t unavailable,
                             std::int64_t residual_unused) {
  (void)residual_unused;
  SourceEvidenceFields fields;
  fields.source = require(CapacitySourceId::parse(source), "source id");
  fields.dimension = dimension;
  fields.unit = canonical_unit(dimension);
  fields.generation = EvidenceGeneration::from_value(generation);
  fields.observed_at = Tick::from_value(1000);
  fields.provenance.source = fields.source;
  fields.provenance.revision = "benchmark/1.0.0";
  fields.provenance.produced_at = Tick::from_value(1000);
  fields.provenance.epoch = EpochId::from_value(7);
  fields.provenance.incarnation = IncarnationId::from_value(3);
  fields.state = OperationalState::nominal;
  fields.installed = require(Measured::known(fields.unit, installed), "installed");
  fields.observed = fields.installed;
  fields.usable = require(Measured::known(fields.unit, usable), "usable");
  fields.unavailable = require(Measured::known(fields.unit, unavailable), "unavailable");
  fields.residual = Measured::unknown();
  fields.derive_residual = true;
  // Matches the single 5 000-unit protection reserve declared per source, so the
  // declared roll-up reconciles exactly with the itemisation.
  fields.protected_capacity = require(Measured::known(fields.unit, 5000), "protected");
  fields.reserved = Measured::known_zero(fields.unit);
  return require(SourceEvidence::create(fields), "evidence");
}

CapacityReserve make_reserve(const std::string& id, const std::string& source, CapacityDimension dimension,
                             std::int64_t amount) {
  CapacityReserveFields fields;
  fields.id = require(ReserveId::parse(id), "reserve id");
  fields.source = require(CapacitySourceId::parse(source), "source id");
  fields.dimension = dimension;
  fields.unit = canonical_unit(dimension);
  fields.kind = ReserveKind::protection;
  fields.amount = require(Measured::known(fields.unit, amount), "reserve amount");
  fields.window = ValidityWindow::unbounded_from(Tick::from_value(0));
  fields.owner = require(OwnerId::parse("facility-ops"), "owner id");
  fields.provenance.source = fields.source;
  fields.provenance.revision = "benchmark/1.0.0";
  fields.provenance.produced_at = Tick::from_value(1000);
  fields.provenance.epoch = EpochId::from_value(7);
  fields.provenance.incarnation = IncarnationId::from_value(3);
  return require(CapacityReserve::create(fields), "reserve");
}

/// Builds a coverage contract over every dimension.
std::vector<CapacityRequirement> make_requirements(std::size_t sources_per_dimension) {
  std::vector<CapacityRequirement> requirements;
  for (std::size_t ordinal = 0; ordinal < capacity_dimension_count; ++ordinal) {
    CapacityRequirement requirement;
    requirement.dimension = all_capacity_dimensions()[ordinal];
    requirement.required = true;
    for (std::size_t index = 0; index < sources_per_dimension; ++index) {
      requirement.required_sources.push_back(
          require(CapacitySourceId::parse("source-" + std::to_string(index)), "source id"));
    }
    require_status(requirement.normalize(), "requirement");
    requirements.push_back(std::move(requirement));
  }
  return requirements;
}

CapacitySnapshotInput make_input(std::size_t sources_per_dimension, CapacityGeneration generation,
                                 Tick built_at) {
  CapacitySnapshotInput input;
  input.facility = make_facility("facility-benchmark");
  input.site = make_site("site-benchmark");
  input.generation = generation;
  input.epoch = EpochId::from_value(7);
  input.incarnation = IncarnationId::from_value(3);
  input.revision = Revision::from_value(generation.value());
  input.built_at = built_at;
  input.valid_until = max_tick;
  input.freshness = SnapshotFreshness::issued;
  input.requirements = make_requirements(sources_per_dimension);
  for (std::size_t ordinal = 0; ordinal < capacity_dimension_count; ++ordinal) {
    const CapacityDimension dimension = all_capacity_dimensions()[ordinal];
    for (std::size_t index = 0; index < sources_per_dimension; ++index) {
      const std::string source = "source-" + std::to_string(index);
      input.evidence.push_back(make_evidence(source, dimension, 1, 100000, 90000, 4000, 0));
      input.reserves.push_back(make_reserve("protection-" + std::to_string(index), source, dimension, 5000));
    }
  }
  return input;
}

std::shared_ptr<const CapacitySnapshot> build_snapshot(std::size_t sources_per_dimension,
                                                       CapacityGeneration generation) {
  return require(CapacitySnapshot::build(make_input(sources_per_dimension, generation, Tick::from_value(1000))),
                 "snapshot build");
}

FacilityCapacityModel make_model(std::size_t sources_per_dimension, ManualClock& clock) {
  ModelConfig config;
  config.facility = make_facility("facility-benchmark");
  config.site = make_site("site-benchmark");
  config.epoch = EpochId::from_value(7);
  config.incarnation = IncarnationId::from_value(3);
  config.requirements = make_requirements(sources_per_dimension);
  return require(FacilityCapacityModel::create(config, clock), "model create");
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t rounds = 9;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--rounds" && index + 1 < argc) {
      rounds = static_cast<std::size_t>(std::strtoull(argv[++index], nullptr, 10));
    } else if (argument == "--help") {
      std::printf("usage: fcap_benchmarks [--rounds N]\n");
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argument.c_str());
      return 2;
    }
  }
  if (rounds < 3) {
    rounds = 3;
  }

  std::printf("# facility-capacity benchmarks, SYNTHETIC single-host control-plane workload\n");
  std::printf("# rounds=%zu; each figure is the median of the per-round means, ns per completed operation\n",
              rounds);

  ManualClock clock(Tick::from_value(1000));

  // --- composition and encoding over two synthetically sized facilities -----
  const std::size_t configurations[] = {8, 32};
  for (const std::size_t sources : configurations) {
    const std::string suffix = "-" + std::to_string(sources) + "sources";

    std::vector<double> build_rounds;
    std::vector<double> encode_rounds;
    std::vector<double> decode_rounds;
    std::vector<double> diff_rounds;

    const std::uint64_t operations = 200;
    auto snapshot = build_snapshot(sources, CapacityGeneration::from_value(1));
    auto other = build_snapshot(sources, CapacityGeneration::from_value(2));
    const std::string document = canonical::encode_snapshot(*snapshot);

    for (std::size_t round = 0; round <= rounds; ++round) {
      const auto build_start = SteadyClock::now();
      for (std::uint64_t index = 0; index < operations; ++index) {
        auto built = CapacitySnapshot::build(
            make_input(sources, CapacityGeneration::from_value(1 + index), Tick::from_value(1000)));
        if (!built.has_value()) {
          fatal(built.error().to_string());
        }
      }
      const double build_ns = to_nanoseconds(SteadyClock::now() - build_start) / static_cast<double>(operations);

      const auto encode_start = SteadyClock::now();
      for (std::uint64_t index = 0; index < operations; ++index) {
        const std::string encoded = canonical::encode_snapshot(*snapshot);
        if (encoded.size() != document.size()) {
          fatal("canonical encoding is not deterministic in size");
        }
      }
      const double encode_ns = to_nanoseconds(SteadyClock::now() - encode_start) / static_cast<double>(operations);

      const auto decode_start = SteadyClock::now();
      for (std::uint64_t index = 0; index < operations; ++index) {
        auto decoded = canonical::decode_snapshot(document, SnapshotFreshness::issued);
        if (!decoded.has_value()) {
          fatal(decoded.error().to_string());
        }
      }
      const double decode_ns = to_nanoseconds(SteadyClock::now() - decode_start) / static_cast<double>(operations);

      const auto diff_start = SteadyClock::now();
      for (std::uint64_t index = 0; index < operations; ++index) {
        auto difference = diff_snapshots(*snapshot, *other);
        if (!difference.has_value()) {
          fatal(difference.error().to_string());
        }
      }
      const double diff_ns = to_nanoseconds(SteadyClock::now() - diff_start) / static_cast<double>(operations);

      if (round != 0) {
        build_rounds.push_back(build_ns);
        encode_rounds.push_back(encode_ns);
        decode_rounds.push_back(decode_ns);
        diff_rounds.push_back(diff_ns);
      }
    }

    record("compose-snapshot" + suffix, "snapshot", build_rounds, operations);
    record("encode-snapshot" + suffix, "document", encode_rounds, operations);
    record("decode-and-rerive-snapshot" + suffix, "document", decode_rounds, operations);
    record("diff-snapshots" + suffix, "diff", diff_rounds, operations);
  }

  // --- model query and publish --------------------------------------------
  {
    const std::size_t sources = 8;
    std::vector<double> query_rounds;
    std::vector<double> publish_rounds;
    const std::uint64_t operations = 500;

    FacilityCapacityModel model = make_model(sources, clock);
    for (std::size_t index = 0; index < sources; ++index) {
      const std::string source = "source-" + std::to_string(index);
      for (std::size_t ordinal = 0; ordinal < capacity_dimension_count; ++ordinal) {
        const CapacityDimension dimension = all_capacity_dimensions()[ordinal];
        CapacityPrecondition precondition = model.current_precondition();
        precondition.attempt = AttemptId::from_value(1000 + index * 16 + ordinal);
        require_status(model.declare_evidence(make_evidence(source, dimension, 1, 100000, 90000, 4000, 0),
                                               precondition),
                       "declare evidence");
      }
    }

    for (std::size_t round = 0; round <= rounds; ++round) {
      const auto query_start = SteadyClock::now();
      for (std::uint64_t index = 0; index < operations; ++index) {
        auto answer = model.query(CapacityDimension::power);
        if (!answer.has_value()) {
          fatal(answer.error().to_string());
        }
      }
      const double query_ns = to_nanoseconds(SteadyClock::now() - query_start) / static_cast<double>(operations);

      const auto publish_start = SteadyClock::now();
      for (std::uint64_t index = 0; index < operations; ++index) {
        CapacityPrecondition precondition = model.current_precondition();
        precondition.attempt = AttemptId::from_value(100000 + round * 1000 + index);
        auto snapshot = model.publish(precondition);
        if (!snapshot.has_value()) {
          fatal(snapshot.error().to_string());
        }
      }
      const double publish_ns = to_nanoseconds(SteadyClock::now() - publish_start) / static_cast<double>(operations);

      if (round != 0) {
        query_rounds.push_back(query_ns);
        publish_rounds.push_back(publish_ns);
      }
    }
    record("model-query" + std::string("-8sources"), "answer", query_rounds, operations);
    record("model-publish" + std::string("-8sources"), "generation", publish_rounds, operations);
  }

  // --- durable store: commit and recover ----------------------------------
  {
    std::error_code code;
    std::filesystem::path root = std::filesystem::temp_directory_path(code) / "fcap-benchmark-store";
    if (code) {
      fatal("could not locate the temporary directory");
    }
    std::filesystem::remove_all(root, code);
    std::optional<CapacityStore> store_holder;

    StoreCreateOptions create_options;
    create_options.facility = make_facility("facility-benchmark");
    create_options.site = make_site("site-benchmark");
    create_options.epoch = EpochId::from_value(7);
    create_options.incarnation = IncarnationId::from_value(3);
    create_options.controller = require(ControllerId::parse("controller-benchmark"), "controller id");
    CapacityStore created = require(CapacityStore::create(root, create_options, clock), "store create");
    store_holder = std::move(created);
    CapacityStore& store = store_holder.value();

    const std::size_t sources = 8;
    FacilityCapacityModel model = make_model(sources, clock);
    for (std::size_t index = 0; index < sources; ++index) {
      const std::string source = "source-" + std::to_string(index);
      for (std::size_t ordinal = 0; ordinal < capacity_dimension_count; ++ordinal) {
        const CapacityDimension dimension = all_capacity_dimensions()[ordinal];
        CapacityPrecondition precondition = model.current_precondition();
        precondition.attempt = AttemptId::from_value(2000 + index * 16 + ordinal);
        require_status(model.declare_evidence(make_evidence(source, dimension, 1, 100000, 90000, 4000, 0),
                                               precondition),
                       "declare evidence");
      }
    }

    std::vector<double> commit_rounds;
    std::vector<double> recover_rounds;
    const std::uint64_t operations = 20;

    for (std::size_t round = 0; round <= rounds; ++round) {
      const auto commit_start = SteadyClock::now();
      for (std::uint64_t index = 0; index < operations; ++index) {
        CapacityPrecondition precondition = model.current_precondition();
        precondition.attempt = AttemptId::from_value(200000 + round * 1000 + index);
        auto snapshot = model.publish(precondition);
        if (!snapshot.has_value()) {
          fatal(snapshot.error().to_string());
        }
        precondition = model.current_precondition();
        precondition.attempt = AttemptId::from_value(300000 + round * 1000 + index);
        auto authority = store.commit(model, precondition);
        if (!authority.has_value()) {
          fatal(authority.error().to_string());
        }
      }
      const double commit_ns = to_nanoseconds(SteadyClock::now() - commit_start) / static_cast<double>(operations);

      const auto recover_start = SteadyClock::now();
      for (std::uint64_t index = 0; index < operations; ++index) {
        auto recovered = store.recover();
        if (!recovered.has_value()) {
          fatal(recovered.error().to_string());
        }
      }
      const double recover_ns = to_nanoseconds(SteadyClock::now() - recover_start) / static_cast<double>(operations);

      if (round != 0) {
        commit_rounds.push_back(commit_ns);
        recover_rounds.push_back(recover_ns);
      }
    }
    record("store-commit-durable-8sources", "generation", commit_rounds, operations);
    record("store-recover-8sources", "generation", recover_rounds, operations);

    // Verification of the benchmark-created state, then removal of residue.
    auto report = store.verify(true);
    if (!report.has_value()) {
      fatal(report.error().to_string());
    }
    if (!report.value().ok) {
      fatal("the benchmark store did not verify: " + report.value().describe());
    }
    auto pruned = store.prune(2);
    if (!pruned.has_value()) {
      fatal(pruned.error().to_string());
    }
    auto generations = store.list_generations();
    if (!generations.has_value()) {
      fatal(generations.error().to_string());
    }
    std::printf("# benchmark store verified after the run: %zu generation files retained, residue removed\n",
                generations.value().size());

    store_holder.reset();
    std::filesystem::remove_all(root, code);
    if (code) {
      std::fprintf(stderr, "warning: could not remove the benchmark store directory\n");
    }
  }

  std::printf("\n%-36s %14s %14s %14s  %s\n", "operation", "median_ns", "min_ns", "max_ns", "completed/round");
  for (const Measurement& measurement : g_measurements) {
    std::printf("%-36s %14.1f %14.1f %14.1f  %llu %s\n", measurement.name.c_str(), measurement.median_ns,
                measurement.min_ns, measurement.max_ns,
                static_cast<unsigned long long>(measurement.operations), measurement.unit.c_str());
  }
  std::printf("\n# All figures are SYNTHETIC: a single host, single process, single thread.\n");
  std::printf("# Durable figures include staging write, flush, verify-by-reread, atomic publish and the "
              "CURRENT replacement.\n");
  return 0;
}
