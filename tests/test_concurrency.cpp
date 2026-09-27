// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Concurrency suite: many threads against one model.
//
// Every case here is deterministic and self-terminating. Threads are released
// together with a `std::latch`, never staggered with a sleep, and no case waits
// for a wall clock: the model is driven by a `ManualClock` whose instant only
// moves when a case moves it.
//
// A worker thread never calls an assertion macro. `ftest::fail` increments a
// plain counter and writes to `std::cout`, which is not safe to do from several
// threads at once, and a failure raised inside a worker would race with every
// other worker's reporting. Every worker records what it observed into its own
// slot, and the case asserts on the joined results: one report, on the main
// thread, with the evidence attached.

#include "test_framework.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "dccp/facility_capacity/capacity.hpp"

#include "test_support.hpp"

namespace {

using dccp::facility_capacity::AttemptId;
using dccp::facility_capacity::CapacityDimension;
using dccp::facility_capacity::CapacityGeneration;
using dccp::facility_capacity::CapacityPrecondition;
using dccp::facility_capacity::CapacitySnapshot;
using dccp::facility_capacity::CapacitySourceId;
using dccp::facility_capacity::EpochId;
using dccp::facility_capacity::Error;
using dccp::facility_capacity::ErrorCode;
using dccp::facility_capacity::FacilityCapacityModel;
using dccp::facility_capacity::IncarnationId;
using dccp::facility_capacity::ManualClock;
using dccp::facility_capacity::ModelConfig;
using dccp::facility_capacity::Revision;
using dccp::facility_capacity::SourceEvidence;
using dccp::facility_capacity::Status;
using dccp::facility_capacity::Tick;

/// The control-plane authority every model in this suite is created under. The
/// multiprocess suite uses the same pair, so a defect that only appears at a
/// non-trivial epoch or incarnation appears in both.
constexpr std::uint64_t kEpoch = 7;
constexpr std::uint64_t kIncarnation = 3;

/// Bound on every retry loop in this file. A retry is only ever spent on a lost
/// race, so reaching the bound is a failure, never a hang: contention between
/// this file's own threads cannot make a case run for ever.
constexpr int kRetryBound = 8192;

std::string source_name(const char* prefix, std::size_t index) {
  std::string name(prefix);
  name.push_back('-');
  const std::string digits = std::to_string(index);
  if (digits.size() < 3) {
    name.append(3 - digits.size(), '0');
  }
  name.append(digits);
  return name;
}

CapacitySourceId source_id(const char* prefix, std::size_t index) {
  const auto parsed = CapacitySourceId::parse(source_name(prefix, index));
  if (!parsed.has_value()) {
    FT_FAIL("the test source identity is invalid: " + parsed.error().to_string());
  }
  return parsed.value();
}

/// A model with no requirements, no evidence and capacity generation zero.
FacilityCapacityModel make_model(ManualClock& clock) {
  ModelConfig config;
  config.facility = fsup::facility_id();
  config.site = fsup::site_id();
  config.epoch = EpochId::from_value(kEpoch);
  config.incarnation = IncarnationId::from_value(kIncarnation);
  auto model = FacilityCapacityModel::create(std::move(config), clock);
  if (!model.has_value()) {
    FT_FAIL("the test model could not be created: " + model.error().to_string());
  }
  return std::move(model.value());
}

/// Evidence for one source and dimension, carrying a chosen evidence generation
/// and a fixed positive installed/usable quantity.
SourceEvidence make_source_evidence(const CapacitySourceId& source, CapacityDimension dimension,
                                    std::uint64_t generation) {
  fsup::EvidenceSpec spec;
  spec.source = source.value();
  spec.dimension = dimension;
  spec.unit = dccp::facility_capacity::canonical_unit(dimension);
  spec.generation = generation;
  spec.observed_at = 100;
  spec.epoch = kEpoch;
  spec.incarnation = kIncarnation;
  spec.installed = 1000;
  spec.observed = 1000;
  spec.usable = 1000;
  spec.unavailable = 0;
  spec.residual = 0;
  spec.protected_capacity = 0;
  spec.reserved = 0;
  return fsup::make_evidence(spec);
}

/// A precondition that matches the model right now, with `attempt` filled in.
CapacityPrecondition fresh_precondition(const FacilityCapacityModel& model, std::uint64_t attempt) {
  CapacityPrecondition precondition = model.current_precondition();
  precondition.attempt = AttemptId::from_value(attempt);
  return precondition;
}

/// Declares `evidence`, re-deriving the precondition after every lost race. Each
/// retry carries a distinct attempt token, so a token that lost a race is never
/// mistaken for an idempotent replay on the next attempt.
Status declare_with_retry(FacilityCapacityModel& model, const SourceEvidence& evidence,
                          std::uint64_t token_base, int& attempts_used) {
  Status last = dccp::facility_capacity::make_status(ErrorCode::invariant_violation,
                                                     "the retry loop never ran");
  for (int attempt = 0; attempt < kRetryBound; ++attempt) {
    const CapacityPrecondition precondition =
        fresh_precondition(model, token_base + static_cast<std::uint64_t>(attempt) + 1u);
    last = model.declare_evidence(evidence, precondition);
    if (last.has_value()) {
      attempts_used = attempt + 1;
      return last;
    }
    if (last.error().code() != ErrorCode::stale_generation &&
        last.error().code() != ErrorCode::stale_authority) {
      attempts_used = attempt + 1;
      return last;
    }
  }
  attempts_used = kRetryBound;
  return last;
}

/// Publishes once, re-deriving the precondition after every lost race.
dccp::facility_capacity::Result<std::shared_ptr<const CapacitySnapshot>> publish_with_retry(
    FacilityCapacityModel& model, std::uint64_t token_base, int& attempts_used) {
  dccp::facility_capacity::Result<std::shared_ptr<const CapacitySnapshot>> last =
      dccp::facility_capacity::make_error<std::shared_ptr<const CapacitySnapshot>>(
          ErrorCode::invariant_violation, "the retry loop never ran");
  for (int attempt = 0; attempt < kRetryBound; ++attempt) {
    const CapacityPrecondition precondition =
        fresh_precondition(model, token_base + static_cast<std::uint64_t>(attempt) + 1u);
    last = model.publish(precondition);
    if (last.has_value()) {
      attempts_used = attempt + 1;
      return last;
    }
    if (last.error().code() != ErrorCode::stale_generation &&
        last.error().code() != ErrorCode::stale_authority) {
      attempts_used = attempt + 1;
      return last;
    }
  }
  attempts_used = kRetryBound;
  return last;
}

/// True for a failure that means "another thread won the race". No other code is
/// an acceptable outcome of a contended mutation.
bool is_race_refusal(const Error& error) {
  return error.code() == ErrorCode::stale_generation || error.code() == ErrorCode::stale_authority;
}

/// True for a failure that means the library broke rather than the caller losing
/// a race.
bool is_defect(const Error& error) {
  return error.code() == ErrorCode::corruption || error.code() == ErrorCode::invariant_violation ||
         error.code() == ErrorCode::checksum_mismatch;
}

}  // namespace

// ---------------------------------------------------------------------------
// One publisher wins; every loser is told the generation moved on.
// ---------------------------------------------------------------------------

FT_TEST(concurrency, publish_same_precondition_has_one_winner) {
  ManualClock clock(Tick::from_value(1000));
  FacilityCapacityModel model = make_model(clock);

  constexpr std::size_t kThreads = 16;
  const CapacityPrecondition precondition = fresh_precondition(model, 0);

  std::vector<std::optional<Error>> failures(kThreads);
  std::vector<bool> winners(kThreads, false);
  std::latch release(kThreads + 1);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (std::size_t index = 0; index < kThreads; ++index) {
    threads.emplace_back([&model, &precondition, &failures, &winners, &release, index]() {
      release.arrive_and_wait();
      const auto snapshot = model.publish(precondition);
      if (snapshot.has_value()) {
        winners[index] = true;
      } else {
        failures[index] = snapshot.error();
      }
    });
  }
  release.arrive_and_wait();
  for (std::thread& thread : threads) {
    thread.join();
  }

  std::size_t winner_count = 0;
  std::size_t stale_count = 0;
  for (std::size_t index = 0; index < kThreads; ++index) {
    if (winners[index]) {
      ++winner_count;
      continue;
    }
    FT_REQUIRE(failures[index].has_value());
    const Error& error = failures[index].value();
    FT_CHECK(!is_defect(error));
    if (error.code() == ErrorCode::stale_generation) {
      ++stale_count;
    }
  }
  FT_CHECK_EQ(winner_count, 1u);
  FT_CHECK_EQ(stale_count, kThreads - 1);
  FT_CHECK_EQ(model.capacity_generation(), CapacityGeneration::from_value(1));
  FT_CHECK_EQ(model.published_generation(), CapacityGeneration::from_value(1));
  FT_CHECK_EQ(model.revision().value(), 1u);
  FT_CHECK(model.current_snapshot() != nullptr);
  FT_CHECK_OK(model.validate());
}

// ---------------------------------------------------------------------------
// Reads stay self-consistent while one thread publishes.
// ---------------------------------------------------------------------------

FT_TEST(concurrency, reads_are_self_consistent_while_publishing) {
  ManualClock clock(Tick::from_value(1000));
  FacilityCapacityModel model = make_model(clock);

  constexpr std::size_t kReaders = 6;
  constexpr std::size_t kReaderRoundBound = 200000;
  constexpr std::uint64_t kPublishRounds = 24;

  const std::vector<CapacityDimension> dimensions = {
      CapacityDimension::space,           CapacityDimension::rack,
      CapacityDimension::power,           CapacityDimension::cooling,
      CapacityDimension::operational_reserve, CapacityDimension::facility_service};

  std::atomic<std::uint64_t> published_count{0};
  std::atomic<bool> publisher_done{false};
  std::atomic<std::size_t> total_reads{0};
  std::vector<std::string> failures(kReaders);
  std::vector<std::uint64_t> non_monotonic(kReaders, 0);
  std::vector<std::uint64_t> final_generation(kReaders, 0);

  std::latch release(kReaders + 2);
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (std::size_t index = 0; index < kReaders; ++index) {
    readers.emplace_back([&, index]() {
      release.arrive_and_wait();
      Revision previous_revision = Revision::from_value(0);
      CapacityGeneration previous_generation = CapacityGeneration::from_value(0);
      // A published snapshot is immutable and content-addressed, so one capacity
      // generation of one store names exactly one snapshot for ever. If two
      // reads could disagree about that, reads would not be self-consistent.
      std::map<std::uint64_t, std::string> digest_by_generation;
      std::map<std::uint64_t, std::string> rendering_by_generation;
      for (std::size_t round = 0; round < kReaderRoundBound; ++round) {
        const Revision revision = model.revision();
        const CapacityGeneration generation = model.capacity_generation();
        const std::shared_ptr<const CapacitySnapshot> snapshot = model.current_snapshot();
        const std::vector<SourceEvidence> evidence = model.all_evidence();
        const auto answer = model.query(dimensions[round % dimensions.size()]);
        total_reads.fetch_add(1, std::memory_order_relaxed);

        if (revision < previous_revision || generation < previous_generation) {
          ++non_monotonic[index];
        }
        previous_revision = revision;
        previous_generation = generation;

        if (!answer.has_value()) {
          if (failures[index].empty()) {
            failures[index] = "query failed: " + answer.error().to_string();
          }
          break;
        }
        // Every part of one answer agrees with every other part of that same
        // answer: it names a snapshot, and it describes the dimension that was
        // asked about.
        const dccp::facility_capacity::CapacityAnswer& value = answer.value();
        if (value.snapshot_digest.empty()) {
          if (failures[index].empty()) {
            failures[index] = "the answer names no snapshot";
          }
          break;
        }
        if (value.totals.dimension != value.dimension ||
            value.dimension != dimensions[round % dimensions.size()]) {
          if (failures[index].empty()) {
            failures[index] = "the answer describes a different dimension than the one asked about";
          }
          break;
        }
        const auto previous_digest = digest_by_generation.find(value.generation.value());
        if (previous_digest == digest_by_generation.end()) {
          digest_by_generation.emplace(value.generation.value(), value.snapshot_digest);
        } else if (previous_digest->second != value.snapshot_digest) {
          if (failures[index].empty()) {
            failures[index] = "one capacity generation named two different snapshots";
          }
          break;
        }
        if (value.totals.installed.is_known() && value.totals.allocatable.is_known() &&
            value.totals.installed.magnitude() < value.totals.allocatable.magnitude()) {
          if (failures[index].empty()) {
            failures[index] = "the answer allocates more than is installed";
          }
          break;
        }
        // The current snapshot, when the model has one, is immutable, and a
        // snapshot is built from the model state by a pure derivation. Two
        // answers that observed the same capacity generation must therefore be
        // the same answer, byte for byte, whichever thread produced them and
        // however many publishes happened in between.
        if (snapshot != nullptr) {
          if (snapshot->digest().empty()) {
            if (failures[index].empty()) {
              failures[index] = "a published snapshot carries no digest";
            }
            break;
          }
          const auto known_digest = digest_by_generation.find(value.generation.value());
          if (known_digest == digest_by_generation.end()) {
            digest_by_generation.emplace(value.generation.value(), value.snapshot_digest);
          } else if (known_digest->second != value.snapshot_digest) {
            if (failures[index].empty()) {
              failures[index] = "one capacity generation named two different snapshots";
            }
            break;
          }
          // The same, checked through the snapshot object rather than through an
          // answer, and the totals of the answer against the totals of the
          // snapshot that generation names.
          if (snapshot->generation() == value.generation) {
            const std::string rendering = snapshot->describe();
            const auto previous_rendering = rendering_by_generation.find(value.generation.value());
            if (previous_rendering == rendering_by_generation.end()) {
              rendering_by_generation.emplace(value.generation.value(), rendering);
            } else if (previous_rendering->second != rendering) {
              if (failures[index].empty()) {
                failures[index] = "one capacity generation rendered two different snapshots";
              }
              break;
            }
            const dccp::facility_capacity::DimensionTotals* totals =
                snapshot->find_totals(value.dimension);
            if (totals != nullptr &&
                (value.totals.installed != totals->installed ||
                 value.totals.usable != totals->usable ||
                 value.totals.allocatable != totals->allocatable ||
                 value.totals.dimension != totals->dimension ||
                 value.totals.unit != totals->unit)) {
              if (failures[index].empty()) {
                failures[index] = "the answer totals are not the snapshot's totals for that dimension";
              }
              break;
            }
          }
        }
        if (evidence.size() > dccp::facility_capacity::limits::max_evidence_sources *
                                  dccp::facility_capacity::capacity_dimension_count) {
          if (failures[index].empty()) {
            failures[index] = "the model handed out more evidence than its own bound";
          }
          break;
        }
        if (publisher_done.load(std::memory_order_acquire)) {
          break;
        }
      }
      // One last read after the publisher has finished: this is the same value
      // for every reader, so the case can assert it, rather than asserting on a
      // value that depends on how the threads happened to be scheduled.
      const std::shared_ptr<const CapacitySnapshot> last = model.current_snapshot();
      final_generation[index] = last == nullptr ? 0u : last->generation().value();
    });
  }

  std::thread publisher([&]() {
    release.arrive_and_wait();
    for (std::uint64_t round = 0; round < kPublishRounds; ++round) {
      int used = 0;
      const auto snapshot = publish_with_retry(model, 100000 + round * 1000, used);
      if (!snapshot.has_value()) {
        break;
      }
      if (snapshot.value()->generation().value() != round + 1 || snapshot.value()->digest().empty()) {
        break;
      }
      published_count.store(snapshot.value()->generation().value(), std::memory_order_relaxed);
    }
    publisher_done.store(true, std::memory_order_release);
  });

  release.arrive_and_wait();
  publisher.join();
  for (std::thread& thread : readers) {
    thread.join();
  }

  FT_CHECK_EQ(published_count.load(std::memory_order_relaxed), kPublishRounds);
  FT_CHECK_EQ(model.capacity_generation().value(), kPublishRounds);
  FT_CHECK_EQ(model.published_generation().value(), kPublishRounds);
  for (std::size_t index = 0; index < kReaders; ++index) {
    if (!failures[index].empty()) {
      FT_FAIL(failures[index]);
    }
    FT_CHECK_EQ(non_monotonic[index], 0u);
    // Every reader, reading after the publisher stopped, sees the same state.
    FT_CHECK_EQ(final_generation[index], kPublishRounds);
  }
  FT_CHECK(total_reads.load(std::memory_order_relaxed) > 0);
  const std::shared_ptr<const CapacitySnapshot> final_snapshot = model.current_snapshot();
  FT_REQUIRE(final_snapshot != nullptr);
  FT_CHECK_EQ(final_snapshot->generation().value(), kPublishRounds);
  FT_CHECK(final_snapshot->input().evidence.empty());
  FT_CHECK_OK(model.validate());
}

// ---------------------------------------------------------------------------
// Distinct sources mutate concurrently; the precondition is re-derived per try.
// ---------------------------------------------------------------------------

FT_TEST(concurrency, concurrent_evidence_declarations_all_land) {
  ManualClock clock(Tick::from_value(1000));
  FacilityCapacityModel model = make_model(clock);

  constexpr std::size_t kThreads = 16;
  constexpr std::uint64_t kTokenBase = 200000;

  std::vector<bool> applied(kThreads, false);
  std::vector<ErrorCode> outcomes(kThreads, ErrorCode::invariant_violation);
  std::vector<int> attempts(kThreads, 0);
  std::latch release(kThreads + 1);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (std::size_t index = 0; index < kThreads; ++index) {
    threads.emplace_back([&, index]() {
      const CapacitySourceId source = source_id("src", index);
      const CapacityDimension dimension =
          index % 2 == 0 ? CapacityDimension::power : CapacityDimension::space;
      const SourceEvidence evidence = make_source_evidence(source, dimension, 1);
      release.arrive_and_wait();
      const Status status =
          declare_with_retry(model, evidence, kTokenBase + index * 1000u, attempts[index]);
      applied[index] = status.has_value();
      if (!status.has_value()) {
        outcomes[index] = status.error().code();
      }
    });
  }
  release.arrive_and_wait();
  for (std::thread& thread : threads) {
    thread.join();
  }

  for (std::size_t index = 0; index < kThreads; ++index) {
    if (!applied[index]) {
      FT_FAIL("a declaration of a distinct source was refused with " +
              std::string(dccp::facility_capacity::error_code_name(outcomes[index])) + " after " +
              std::to_string(attempts[index]) + " attempts");
    }
    FT_CHECK(attempts[index] >= 1);
    FT_CHECK(attempts[index] < kRetryBound);
  }

  // Every declared record is present, exactly once, with the value it carried.
  const std::vector<SourceEvidence> evidence = model.all_evidence();
  FT_CHECK_EQ(evidence.size(), kThreads);
  for (std::size_t index = 0; index < kThreads; ++index) {
    const CapacitySourceId source = source_id("src", index);
    const CapacityDimension dimension =
        index % 2 == 0 ? CapacityDimension::power : CapacityDimension::space;
    const auto held = model.evidence(source, dimension);
    FT_REQUIRE(held.has_value());
    FT_CHECK_EQ(held.value().generation().value(), 1u);
    FT_CHECK_EQ(held.value().installed().magnitude(), 1000);
    FT_CHECK_EQ(held.value().usable().magnitude(), 1000);
    FT_CHECK_EQ(held.value().source(), source);
    FT_CHECK_EQ(held.value().dimension(), dimension);
  }
  FT_CHECK_OK(model.validate());
  // Evidence alone never moves the capacity generation: only a publish does.
  FT_CHECK_EQ(model.capacity_generation(), CapacityGeneration::from_value(0));
  FT_CHECK_EQ(model.revision().value(), kThreads);
  FT_CHECK(model.current_snapshot() == nullptr);
}

// ---------------------------------------------------------------------------
// Immutable snapshots handed between threads.
// ---------------------------------------------------------------------------

FT_TEST(concurrency, published_snapshots_are_immutable_across_threads) {
  ManualClock clock(Tick::from_value(1000));
  FacilityCapacityModel model = make_model(clock);

  constexpr std::size_t kReaders = 4;
  constexpr std::size_t kRounds = 24;
  // A reader spins while the pool is still filling. The spin is bounded, so a
  // publisher that never publishes fails the case instead of hanging it, but it
  // is long enough that the bound is never the reason a reader stops.
  constexpr std::size_t kSpinBound = 20'000'000;

  // The pool that hands snapshots to the readers. These are the very
  // `shared_ptr<const CapacitySnapshot>` values the model published: immutable,
  // shared, and outliving the model's state.
  std::mutex handoff;
  std::vector<std::shared_ptr<const CapacitySnapshot>> published;
  // digest -> the exact text one snapshot rendered as. Every thread that reads
  // the same snapshot must find the same text here.
  std::map<std::string, std::string> rendered;
  std::atomic<std::size_t> reads{0};
  std::atomic<bool> pool_full{false};
  std::vector<std::string> failures(kReaders);
  std::vector<bool> idle(kReaders, false);

  std::latch release(kReaders + 2);
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (std::size_t index = 0; index < kReaders; ++index) {
    readers.emplace_back([&, index]() {
      release.arrive_and_wait();
      std::size_t cursor = 0;
      std::size_t spins = 0;
      // Every reader walks the whole pool as it fills, so the same immutable
      // object is described by several threads while the publisher is still
      // handing out more of them. Once the pool is full each reader makes one
      // final pass over it, which is what lets the case assert that the pool was
      // fully observed rather than sampled.
      while (cursor < kRounds) {
        std::shared_ptr<const CapacitySnapshot> snapshot;
        {
          std::lock_guard<std::mutex> guard(handoff);
          if (cursor < published.size()) {
            snapshot = published[cursor];
            ++cursor;
          }
        }
        if (snapshot == nullptr) {
          if (pool_full.load(std::memory_order_acquire)) {
            break;
          }
          if (++spins > kSpinBound) {
            idle[index] = true;
            return;
          }
          std::this_thread::yield();
          continue;
        }
        // The rendering is derived on every call rather than stored, so
        // identical text is only possible when nothing about the object moves
        // under a concurrent read.
        const std::string first = snapshot->describe();
        const std::string second = snapshot->describe();
        reads.fetch_add(1, std::memory_order_relaxed);
        if (first != second && failures[index].empty()) {
          failures[index] = "describe() is not stable for one immutable snapshot";
        }
        if (first.empty() && failures[index].empty()) {
          failures[index] = "describe() rendered nothing";
        }
        {
          std::lock_guard<std::mutex> guard(handoff);
          const auto existing = rendered.find(snapshot->digest());
          if (existing == rendered.end()) {
            rendered.emplace(snapshot->digest(), first);
          } else if (existing->second != first && failures[index].empty()) {
            failures[index] = "the same snapshot rendered differently to a different thread";
          }
        }
      }
    });
  }

  std::thread publisher([&]() {
    release.arrive_and_wait();
    for (std::size_t round = 0; round < kRounds; ++round) {
      int used = 0;
      const auto snapshot = publish_with_retry(model, 300000 + round * 1000, used);
      if (!snapshot.has_value()) {
        break;
      }
      {
        std::lock_guard<std::mutex> guard(handoff);
        published.push_back(snapshot.value());
      }
    }
    pool_full.store(true, std::memory_order_release);
  });

  release.arrive_and_wait();
  publisher.join();
  for (std::thread& thread : readers) {
    thread.join();
  }

  FT_CHECK_EQ(published.size(), kRounds);
  for (std::size_t index = 0; index < kReaders; ++index) {
    FT_CHECK(!idle[index]);
    if (!failures[index].empty()) {
      FT_FAIL(failures[index]);
    }
  }
  // Readers must have observed the pool they are asserting about: a reader that
  // stopped early would make the stability claim vacuous.
  FT_CHECK(reads.load(std::memory_order_relaxed) >= kReaders);
  FT_CHECK_EQ(rendered.size(), kRounds);
  // Every handed-out snapshot still describes itself identically long after the
  // model that produced it has moved on: the objects outlive the model's state.
  for (std::size_t index = 0; index < published.size(); ++index) {
    FT_CHECK_EQ(published[index]->generation().value(), index + 1);
    FT_CHECK(!published[index]->digest().empty());
    FT_CHECK(published[index]->describe() == published[index]->describe());
    const auto entry = rendered.find(published[index]->digest());
    FT_REQUIRE(entry != rendered.end());
    FT_CHECK(entry->second == published[index]->describe());
  }
  FT_CHECK_OK(model.validate());
}

// ---------------------------------------------------------------------------
// Idempotent replay under concurrency.
// ---------------------------------------------------------------------------

FT_TEST(concurrency, identical_attempt_token_applies_once) {
  ManualClock clock(Tick::from_value(1000));
  FacilityCapacityModel model = make_model(clock);

  constexpr std::size_t kThreads = 2;
  const CapacitySourceId source = source_id("src", 900);
  const SourceEvidence evidence = make_source_evidence(source, CapacityDimension::power, 4);
  const CapacityPrecondition precondition = fresh_precondition(model, 4242);

  std::vector<bool> succeeded(kThreads, false);
  std::vector<ErrorCode> outcomes(kThreads, ErrorCode::invariant_violation);
  std::latch release(kThreads + 1);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (std::size_t index = 0; index < kThreads; ++index) {
    threads.emplace_back([&, index]() {
      release.arrive_and_wait();
      const Status status = model.declare_evidence(evidence, precondition);
      succeeded[index] = status.has_value();
      if (!status.has_value()) {
        outcomes[index] = status.error().code();
      }
    });
  }
  release.arrive_and_wait();
  for (std::thread& thread : threads) {
    thread.join();
  }

  // Both calls report success and the mutation is applied exactly once: one
  // revision bump, one recorded record, one value generation.
  for (std::size_t index = 0; index < kThreads; ++index) {
    if (!succeeded[index]) {
      FT_FAIL("an idempotent replay was refused with " +
              std::string(dccp::facility_capacity::error_code_name(outcomes[index])));
    }
  }
  FT_CHECK_EQ(model.revision().value(), 1u);
  FT_CHECK_EQ(model.all_evidence().size(), 1u);
  const auto held = model.evidence(source, CapacityDimension::power);
  FT_REQUIRE(held.has_value());
  FT_CHECK_EQ(held.value().generation().value(), 4u);
  FT_CHECK_OK(model.validate());
}

// ---------------------------------------------------------------------------
// The documented bound on the idempotency window.
// ---------------------------------------------------------------------------

FT_TEST(concurrency, idempotency_window_is_bounded) {
  // `limits::max_recorded_attempts` is the documented bound: the model keeps the
  // most recent attempt tokens and forgets the rest. A token older than the
  // window is no longer a replay token, and the mutation it carries is applied
  // again rather than being silently reported as already done. This case pins
  // that down: after `max_recorded_attempts + 8` distinct mutations the token
  // used by the very first mutation is forgotten, and re-submitting it mutates
  // the model a second time.
  ManualClock clock(Tick::from_value(1000));
  FacilityCapacityModel model = make_model(clock);

  constexpr std::uint64_t kOldToken = 7001;
  const CapacitySourceId source = source_id("src", 950);
  const CapacityDimension dimension = CapacityDimension::power;

  int attempts_used = 0;
  const Status first =
      declare_with_retry(model, make_source_evidence(source, dimension, 5), kOldToken - 1u, attempts_used);
  FT_REQUIRE_OK(first);
  const std::uint64_t after_first_revision = model.revision().value();
  {
    const auto held = model.evidence(source, dimension);
    FT_REQUIRE(held.has_value());
    FT_CHECK_EQ(held.value().generation().value(), 5u);
  }

  // While the token is still inside the window, re-submitting it is a replay:
  // success, and nothing changes at all.
  {
    CapacityPrecondition replay = model.current_precondition();
    replay.attempt = AttemptId::from_value(kOldToken);
    FT_REQUIRE_OK(model.declare_evidence(make_source_evidence(source, dimension, 3), replay));
    FT_CHECK_EQ(model.revision().value(), after_first_revision);
    const auto held = model.evidence(source, dimension);
    FT_REQUIRE(held.has_value());
    FT_CHECK_EQ(held.value().generation().value(), 5u);
  }

  // `max_recorded_attempts + 8` further distinct mutations, each applied, each
  // carrying a distinct attempt token. Every one of them must actually land:
  // re-declaring the same evidence generation is a replacement, not a no-op,
  // and it must move the revision.
  const std::size_t distinct_mutations = dccp::facility_capacity::limits::max_recorded_attempts + 8u;
  std::uint64_t generation = 6;
  for (std::size_t index = 0; index < distinct_mutations; ++index) {
    const std::uint64_t token = kOldToken + 1u + static_cast<std::uint64_t>(index);
    const Revision before = model.revision();
    int used = 0;
    const Status status =
        declare_with_retry(model, make_source_evidence(source, dimension, generation), token - 1u, used);
    FT_REQUIRE_OK(status);
    FT_CHECK_EQ(model.revision().value(), before.value() + 1u);
    ++generation;
  }
  const std::uint64_t before_probe = model.revision().value();
  FT_CHECK(before_probe > after_first_revision);

  // The window has moved past `kOldToken`, so it is no longer recognised: the
  // mutation is applied again, exactly as a fresh mutation would be. The model
  // is still correct afterwards.
  {
    CapacityPrecondition probe = model.current_precondition();
    probe.attempt = AttemptId::from_value(kOldToken);
    FT_REQUIRE_OK(model.declare_evidence(make_source_evidence(source, dimension, generation), probe));
  }
  FT_CHECK_EQ(model.revision().value(), before_probe + 1u);
  {
    const auto held = model.evidence(source, dimension);
    FT_REQUIRE(held.has_value());
    FT_CHECK_EQ(held.value().generation().value(), generation);
  }
  FT_CHECK_OK(model.validate());
  FT_CHECK_EQ(model.capacity_generation(), CapacityGeneration::from_value(0));
  FT_CHECK(model.current_snapshot() == nullptr);
}

// ---------------------------------------------------------------------------
// A winner and a loser of one publish race.
// ---------------------------------------------------------------------------

FT_TEST(concurrency, publish_race_leaves_no_partial_state) {
  ManualClock clock(Tick::from_value(1000));
  FacilityCapacityModel model = make_model(clock);

  constexpr std::size_t kThreads = 8;
  const CapacityPrecondition precondition = fresh_precondition(model, 0);

  std::vector<bool> winners(kThreads, false);
  std::vector<bool> snapshot_ok(kThreads, false);
  std::vector<std::optional<Error>> failures(kThreads);
  std::latch release(kThreads + 1);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (std::size_t index = 0; index < kThreads; ++index) {
    threads.emplace_back([&, index]() {
      release.arrive_and_wait();
      const auto snapshot = model.publish(precondition);
      if (snapshot.has_value()) {
        winners[index] = true;
        // A winner observes a snapshot that carries the new generation and the
        // digest of the bytes it was built from.
        snapshot_ok[index] = snapshot.value()->generation() == CapacityGeneration::from_value(1) &&
                             !snapshot.value()->digest().empty() &&
                             snapshot.value()->digest() == snapshot.value()->digest();
      } else {
        failures[index] = snapshot.error();
      }
    });
  }
  release.arrive_and_wait();
  for (std::thread& thread : threads) {
    thread.join();
  }

  std::size_t winner_count = 0;
  for (std::size_t index = 0; index < kThreads; ++index) {
    if (winners[index]) {
      ++winner_count;
      FT_CHECK(snapshot_ok[index]);
      continue;
    }
    FT_REQUIRE(failures[index].has_value());
    FT_CHECK(is_race_refusal(failures[index].value()));
  }
  FT_CHECK_EQ(winner_count, 1u);
  FT_CHECK_EQ(model.capacity_generation().value(), 1u);
  FT_CHECK_EQ(model.revision().value(), 1u);
  const std::shared_ptr<const CapacitySnapshot> snapshot = model.current_snapshot();
  FT_REQUIRE(snapshot != nullptr);
  FT_CHECK_EQ(snapshot->generation().value(), 1u);
  FT_CHECK_EQ(model.published_generation().value(), 1u);
  FT_CHECK_OK(model.validate());
}
