// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Derivation of a snapshot's derived content. Private to the library.
//
// Nothing here reads private state: the deriver consumes a
// `CapacitySnapshotInput` and produces exactly the totals, attributions,
// completeness classification and notes that a snapshot carries.

#ifndef DCCP_FACILITY_CAPACITY_SRC_INTERNAL_DERIVE_HPP
#define DCCP_FACILITY_CAPACITY_SRC_INTERNAL_DERIVE_HPP

#include <vector>

#include "dccp/facility_capacity/result.hpp"
#include "dccp/facility_capacity/snapshot.hpp"

namespace dccp::facility_capacity::internal {

struct DerivedSnapshot {
  std::vector<DimensionTotals> totals;
  std::vector<CapacityOutcome> outcomes;
  std::vector<SourceGenerationStamp> sources;
  std::vector<ReserveStamp> reserves;
  std::vector<ConstraintEvaluation> constraints;
  std::vector<DimensionLimit> limits;
  NoteSet notes;
  Completeness completeness;
  LimitingConstraint limiting;
  bool accounting_closed = false;
};

/// Composes every dimension, evaluates every declared service constraint and
/// derives the limiting-constraint attribution and the completeness
/// classification.
///
/// Fails with `invariant_violation` when an exact accounting identity does not
/// hold, which is the only way the composition can report that capacity was
/// created or destroyed rather than accounted for.
Result<DerivedSnapshot> derive_content(const CapacitySnapshotInput& input);

}  // namespace dccp::facility_capacity::internal

#endif  // DCCP_FACILITY_CAPACITY_SRC_INTERNAL_DERIVE_HPP
