// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include "test_framework.hpp"

namespace fsup {
namespace {

std::atomic<std::uint64_t> g_counter{0};

[[noreturn]] void abort_test(const std::string& message) { throw std::runtime_error(message); }

template <class Id>
Id parse_id(const std::string& text) {
  auto parsed = Id::parse(text);
  if (!parsed.has_value()) {
    abort_test("test identifier is invalid: " + text + " (" + parsed.error().to_string() + ")");
  }
  return parsed.value();
}

Measured optional_measured(const std::optional<std::int64_t>& value, Unit unit) {
  if (!value.has_value()) {
    return Measured::unknown();
  }
  auto measured = Measured::known(unit, value.value());
  if (!measured.has_value()) {
    abort_test("test measured value is invalid: " + measured.error().to_string());
  }
  return measured.value();
}

}  // namespace

TempDir::TempDir(const std::string& label) {
  const std::uint64_t counter = g_counter.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  std::string name = "facility-capacity-";
  name.append(label);
  name.push_back('-');
  name.append(std::to_string(static_cast<long long>(stamp)));
  name.push_back('-');
  name.append(std::to_string(static_cast<unsigned long long>(counter)));
  path_ = temp_root() / name;
  std::error_code code;
  std::filesystem::create_directories(path_, code);
  if (code) {
    abort_test("could not create a temporary directory: " + code.message());
  }
}

TempDir::~TempDir() {
  std::error_code code;
  std::filesystem::remove_all(path_, code);
}

std::filesystem::path temp_root() {
  std::error_code code;
  std::filesystem::path root = std::filesystem::temp_directory_path(code);
  if (code) {
    return std::filesystem::path(".");
  }
  return root;
}

Measured known(CapacityDimension dimension, std::int64_t magnitude) {
  return known_unit(dccp::facility_capacity::canonical_unit(dimension), magnitude);
}

Measured known_unit(Unit unit, std::int64_t magnitude) {
  auto measured = Measured::known(unit, magnitude);
  if (!measured.has_value()) {
    abort_test("test measured value is invalid: " + measured.error().to_string());
  }
  return measured.value();
}

Measured unknown() { return Measured::unknown(); }

Provenance provenance(const std::string& source, std::uint64_t epoch, std::uint64_t incarnation,
                      std::uint64_t produced_at, const std::string& revision) {
  Provenance value;
  value.source = parse_id<dccp::facility_capacity::CapacitySourceId>(source);
  value.revision = revision;
  value.produced_at = Tick::from_value(produced_at);
  value.epoch = EpochId::from_value(epoch);
  value.incarnation = IncarnationId::from_value(incarnation);
  return value;
}

SourceEvidence make_evidence(const EvidenceSpec& spec) {
  dccp::facility_capacity::SourceEvidenceFields fields;
  fields.source = parse_id<dccp::facility_capacity::CapacitySourceId>(spec.source);
  fields.dimension = spec.dimension;
  fields.unit = spec.unit;
  fields.generation = dccp::facility_capacity::EvidenceGeneration::from_value(spec.generation);
  fields.observed_at = Tick::from_value(spec.observed_at);
  fields.provenance = provenance(spec.source, spec.epoch, spec.incarnation, spec.observed_at, spec.revision);
  fields.provenance.evidence_digest = spec.source_digest;
  fields.state = spec.state;
  fields.state_reason = spec.state_reason;
  fields.state_detail = spec.state_detail;
  if (spec.installed.has_value()) {
    fields.installed = optional_measured(spec.installed, spec.unit);
  }
  fields.observed = optional_measured(spec.observed, spec.unit);
  fields.usable = optional_measured(spec.usable, spec.unit);
  fields.unavailable = optional_measured(spec.unavailable, spec.unit);
  fields.residual = optional_measured(spec.residual, spec.unit);
  fields.protected_capacity = optional_measured(spec.protected_capacity, spec.unit);
  fields.reserved = optional_measured(spec.reserved, spec.unit);
  fields.derive_residual = spec.derive_residual;

  auto evidence = SourceEvidence::create(fields);
  if (!evidence.has_value()) {
    abort_test("test evidence is invalid: " + evidence.error().to_string());
  }
  return evidence.value();
}

CapacityReserve make_reserve(const ReserveSpec& spec) {
  dccp::facility_capacity::CapacityReserveFields fields;
  fields.id = parse_id<dccp::facility_capacity::ReserveId>(spec.id);
  fields.source = parse_id<dccp::facility_capacity::CapacitySourceId>(spec.source);
  fields.dimension = spec.dimension;
  fields.unit = spec.unit;
  fields.kind = spec.kind;
  fields.amount = optional_measured(spec.amount, spec.unit);
  auto window = dccp::facility_capacity::ValidityWindow::create(Tick::from_value(spec.window_start),
                                                               Tick::from_value(spec.window_end));
  if (!window.has_value()) {
    abort_test("test window is invalid: " + window.error().to_string());
  }
  fields.window = window.value();
  if (!spec.owner.empty()) {
    fields.owner = parse_id<dccp::facility_capacity::OwnerId>(spec.owner);
  }
  if (!spec.service_class.empty()) {
    fields.service_class = parse_id<dccp::facility_capacity::ServiceClassId>(spec.service_class);
  }
  fields.reason = spec.reason;
  fields.detail = spec.detail;
  fields.provenance = provenance(spec.source, spec.epoch, spec.incarnation, spec.produced_at);

  auto reserve = CapacityReserve::create(fields);
  if (!reserve.has_value()) {
    abort_test("test reserve is invalid: " + reserve.error().to_string());
  }
  return reserve.value();
}

ServiceConstraint make_constraint(const ConstraintSpec& spec) {
  dccp::facility_capacity::ServiceConstraintFields fields;
  fields.id = parse_id<dccp::facility_capacity::ConstraintId>(spec.id);
  fields.dimension = spec.dimension;
  fields.unit = spec.unit;
  if (!spec.service_class.empty()) {
    fields.service_class = parse_id<dccp::facility_capacity::ServiceClassId>(spec.service_class);
  }
  fields.floor = optional_measured(spec.floor_value, spec.unit);
  auto window = dccp::facility_capacity::ValidityWindow::create(Tick::from_value(spec.window_start),
                                                               Tick::from_value(spec.window_end));
  if (!window.has_value()) {
    abort_test("test window is invalid: " + window.error().to_string());
  }
  fields.window = window.value();
  if (!spec.owner.empty()) {
    fields.owner = parse_id<dccp::facility_capacity::OwnerId>(spec.owner);
  }
  fields.reason = spec.reason;
  fields.detail = spec.detail;
  fields.provenance = provenance("facility-ops", spec.epoch, spec.incarnation, spec.produced_at);

  auto constraint = ServiceConstraint::create(fields);
  if (!constraint.has_value()) {
    abort_test("test constraint is invalid: " + constraint.error().to_string());
  }
  return constraint.value();
}

dccp::facility_capacity::FacilityId facility_id(const std::string& text) {
  return parse_id<dccp::facility_capacity::FacilityId>(text);
}

dccp::facility_capacity::SiteId site_id(const std::string& text) {
  return parse_id<dccp::facility_capacity::SiteId>(text);
}

dccp::facility_capacity::StoreId store_id(const std::string& text) {
  return parse_id<dccp::facility_capacity::StoreId>(text);
}

dccp::facility_capacity::ControllerId controller_id(const std::string& text) {
  return parse_id<dccp::facility_capacity::ControllerId>(text);
}

void remove_tree(const std::filesystem::path& path) {
  std::error_code code;
  std::filesystem::remove_all(path, code);
}

void write_bytes(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    abort_test("could not open a file for writing");
  }
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  stream.flush();
  if (!stream) {
    abort_test("could not write a file");
  }
}

std::string read_bytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    abort_test("could not open a file for reading");
  }
  std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  return bytes;
}

std::vector<std::string> list_names(const std::filesystem::path& path) {
  std::vector<std::string> names;
  std::error_code code;
  for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(path, code)) {
    names.push_back(entry.path().filename().string());
  }
  if (code) {
    abort_test("could not list a directory: " + code.message());
  }
  std::sort(names.begin(), names.end());
  return names;
}

void copy_file(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code code;
  std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, code);
  if (code) {
    abort_test("could not copy a file: " + code.message());
  }
}

}  // namespace fsup
