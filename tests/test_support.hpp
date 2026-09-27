// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Shared test scaffolding: deterministic identifiers, disposable directories
// and short constructors for the domain's immutable values.

#ifndef FACILITY_CAPACITY_TESTS_TEST_SUPPORT_HPP
#define FACILITY_CAPACITY_TESTS_TEST_SUPPORT_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "dccp/facility_capacity/capacity.hpp"

namespace fsup {

using dccp::facility_capacity::CapacityDimension;
using dccp::facility_capacity::CapacityReserve;
using dccp::facility_capacity::IncarnationId;
using dccp::facility_capacity::EpochId;
using dccp::facility_capacity::Measured;
using dccp::facility_capacity::OperationalState;
using dccp::facility_capacity::Provenance;
using dccp::facility_capacity::ReserveKind;
using dccp::facility_capacity::ServiceConstraint;
using dccp::facility_capacity::SourceEvidence;
using dccp::facility_capacity::Tick;
using dccp::facility_capacity::Unit;

/// A directory under the system temporary directory, removed on destruction.
class TempDir {
 public:
  explicit TempDir(const std::string& label);
  ~TempDir();

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;

  const std::filesystem::path& path() const noexcept { return path_; }

  std::filesystem::path file(const std::string& name) const { return path_ / name; }

  /// The store root inside this directory, created on demand.
  std::filesystem::path store_root(const std::string& name = "store") const { return path_ / name; }

 private:
  std::filesystem::path path_;
};

/// The system temporary directory used by the suite.
std::filesystem::path temp_root();

/// A measured value in the canonical unit of `dimension`.
Measured known(CapacityDimension dimension, std::int64_t magnitude);

/// A measured value in an explicit unit.
Measured known_unit(Unit unit, std::int64_t magnitude);

/// The literal unknown value.
Measured unknown();

Provenance provenance(const std::string& source, std::uint64_t epoch, std::uint64_t incarnation,
                      std::uint64_t produced_at, const std::string& revision = "test/1.0.0");

/// A compact description of one evidence record. An absent optional is an
/// unmeasured value; a present optional is an exact measured value.
struct EvidenceSpec {
  std::string source = "power-capacity";
  CapacityDimension dimension = CapacityDimension::power;
  Unit unit = Unit::milli_watt;
  std::uint64_t generation = 1;
  std::uint64_t observed_at = 100;
  std::uint64_t epoch = 7;
  std::uint64_t incarnation = 3;
  std::string revision = "test/1.0.0";
  std::string source_digest;
  OperationalState state = OperationalState::nominal;
  dccp::facility_capacity::ReasonCode state_reason = dccp::facility_capacity::ReasonCode::none;
  std::string state_detail;
  std::optional<std::int64_t> installed;
  std::optional<std::int64_t> observed;
  std::optional<std::int64_t> usable;
  std::optional<std::int64_t> unavailable;
  std::optional<std::int64_t> residual;
  std::optional<std::int64_t> protected_capacity;
  std::optional<std::int64_t> reserved;
  bool derive_residual = false;
};

/// Builds an evidence record, aborting the test when the specification is not
/// valid.
SourceEvidence make_evidence(const EvidenceSpec& spec);

struct ReserveSpec {
  std::string id = "protection-1";
  std::string source = "power-capacity";
  CapacityDimension dimension = CapacityDimension::power;
  Unit unit = Unit::milli_watt;
  ReserveKind kind = ReserveKind::protection;
  std::optional<std::int64_t> amount;
  std::uint64_t window_start = 0;
  std::uint64_t window_end = UINT64_MAX;
  std::string owner = "facility-ops";
  std::string service_class;
  std::uint64_t epoch = 7;
  std::uint64_t incarnation = 3;
  std::uint64_t produced_at = 100;
  dccp::facility_capacity::ReasonCode reason = dccp::facility_capacity::ReasonCode::none;
  std::string detail;
};

CapacityReserve make_reserve(const ReserveSpec& spec);

struct ConstraintSpec {
  std::string id = "floor-1";
  CapacityDimension dimension = CapacityDimension::power;
  Unit unit = Unit::milli_watt;
  std::string service_class = "tier-1";
  std::optional<std::int64_t> floor_value;
  std::uint64_t window_start = 0;
  std::uint64_t window_end = UINT64_MAX;
  std::string owner = "facility-ops";
  std::uint64_t epoch = 7;
  std::uint64_t incarnation = 3;
  std::uint64_t produced_at = 100;
  dccp::facility_capacity::ReasonCode reason = dccp::facility_capacity::ReasonCode::none;
  std::string detail;
};

ServiceConstraint make_constraint(const ConstraintSpec& spec);

dccp::facility_capacity::FacilityId facility_id(const std::string& text = "facility-a");
dccp::facility_capacity::SiteId site_id(const std::string& text = "site-1");
dccp::facility_capacity::StoreId store_id(const std::string& text = "store-1");
dccp::facility_capacity::ControllerId controller_id(const std::string& text = "controller-1");

/// Recursively removes a directory, tolerating a missing path.
void remove_tree(const std::filesystem::path& path);

/// Writes bytes to a file, replacing any existing file.
void write_bytes(const std::filesystem::path& path, const std::string& bytes);

/// Reads a whole file as bytes.
std::string read_bytes(const std::filesystem::path& path);

/// Directory entry names, sorted.
std::vector<std::string> list_names(const std::filesystem::path& path);

/// Copies a file, replacing the destination.
void copy_file(const std::filesystem::path& from, const std::filesystem::path& to);

}  // namespace fsup

#endif  // FACILITY_CAPACITY_TESTS_TEST_SUPPORT_HPP
