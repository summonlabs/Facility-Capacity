// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Deterministic explanations.
//
// Every rendering function in this header is a pure function of its argument:
// no clock, no locale, no address, no iteration order over an unordered
// container, no floating point. Two runs over equal state produce equal bytes,
// which is what makes an explanation usable as evidence and diffable.

#ifndef DCCP_FACILITY_CAPACITY_EXPLAIN_HPP
#define DCCP_FACILITY_CAPACITY_EXPLAIN_HPP

#include <string>

#include "dccp/facility_capacity/diff.hpp"
#include "dccp/facility_capacity/revalidate.hpp"
#include "dccp/facility_capacity/snapshot.hpp"

namespace dccp::facility_capacity::explain {

/// Renders the note set, one note per line, in canonical order.
std::string render_notes(const NoteSet& notes);

/// Renders every dimension's answer with its totals, attribution and reasons.
std::string render_snapshot(const CapacitySnapshot& snapshot);

/// Renders one dimension's answer with the exact inputs that produced it.
std::string render_dimension(const CapacitySnapshot& snapshot, CapacityDimension dimension);

/// Renders a query answer.
std::string render_answer(const CapacityAnswer& answer);

/// Renders a revalidation report.
std::string render_revalidation(const RevalidationReport& report);

/// Renders a diff.
std::string render_diff(const SnapshotDiff& diff);

/// Renders a quantity change as `before -> after (delta)`.
std::string render_quantity_change(const QuantityChange& change);

}  // namespace dccp::facility_capacity::explain

#endif  // DCCP_FACILITY_CAPACITY_EXPLAIN_HPP
