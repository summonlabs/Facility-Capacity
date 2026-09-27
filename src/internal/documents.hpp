// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Canonical document field mapping for whole snapshots and whole model states.
// Private to the library.
//
// The document carries both the *input* description (evidence, reserves,
// constraints, requirements) and every *derived* value the snapshot publishes.
// A decoder parses the input, re-derives the answer, re-encodes it and compares
// the result byte for byte with the bytes it was given. A document whose
// derived fields were edited is therefore refused even when its digest was
// recomputed to match.

#ifndef DCCP_FACILITY_CAPACITY_SRC_INTERNAL_DOCUMENTS_HPP
#define DCCP_FACILITY_CAPACITY_SRC_INTERNAL_DOCUMENTS_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "dccp/facility_capacity/constraint.hpp"
#include "dccp/facility_capacity/evidence.hpp"
#include "dccp/facility_capacity/generation.hpp"
#include "dccp/facility_capacity/limits.hpp"
#include "dccp/facility_capacity/reserve.hpp"
#include "dccp/facility_capacity/result.hpp"
#include "dccp/facility_capacity/snapshot.hpp"
#include "dccp/facility_capacity/strong_id.hpp"
#include "internal/canonical_io.hpp"

namespace dccp::facility_capacity::internal {

/// The complete persisted state of one aggregate model.
struct ModelState {
  FacilityId facility;
  SiteId site;
  EpochId epoch;
  IncarnationId incarnation;
  CapacityGeneration capacity_generation{};
  Revision revision{};
  Tick snapshot_validity_ticks{};
  std::size_t max_sources = limits::max_evidence_sources;
  std::size_t max_reserves = limits::max_reserves;
  std::size_t max_constraints = limits::max_service_constraints;
  std::vector<CapacityRequirement> requirements;
  std::vector<SourceEvidence> evidence;
  std::vector<CapacityReserve> reserves;
  std::vector<ServiceConstraint> constraints;
  /// Applied idempotency attempts, oldest first, bounded.
  std::vector<AttemptId> attempts;
  std::string published_snapshot_id;
  std::string published_snapshot_digest;

  std::string to_string() const;
};

/// Writes the derived and input fields of a snapshot. The terminating digest
/// line is not written here.
void encode_snapshot_fields(CanonicalWriter& writer, const CapacitySnapshot& snapshot);

/// Reads a snapshot document back into its input description and checks every
/// stored derived value against the value the derivation produces.
Result<CapacitySnapshotInput> decode_snapshot_fields(const CanonicalReader& reader);

void encode_model_state_fields(CanonicalWriter& writer, const ModelState& state);
Result<ModelState> decode_model_state_fields(const CanonicalReader& reader);

}  // namespace dccp::facility_capacity::internal

#endif  // DCCP_FACILITY_CAPACITY_SRC_INTERNAL_DOCUMENTS_HPP
