// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Domain object <-> canonical field mapping. Private to the library.
//
// Every encoder writes a fixed, hand-ordered field sequence and every decoder
// consumes every field exactly once or fails. Neither iterates an unordered
// container.

#ifndef DCCP_FACILITY_CAPACITY_SRC_INTERNAL_CODEC_HPP
#define DCCP_FACILITY_CAPACITY_SRC_INTERNAL_CODEC_HPP

#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_capacity/constraint.hpp"
#include "dccp/facility_capacity/evidence.hpp"
#include "dccp/facility_capacity/model.hpp"
#include "dccp/facility_capacity/reason.hpp"
#include "dccp/facility_capacity/reserve.hpp"
#include "dccp/facility_capacity/result.hpp"
#include "dccp/facility_capacity/snapshot.hpp"
#include "internal/canonical_io.hpp"

namespace dccp::facility_capacity::internal {

/// Writes the fields of `evidence` under `prefix`. The evidence digest field is
/// written only when `include_digest` is true.
void encode_evidence(CanonicalWriter& writer, std::string_view prefix, const SourceEvidence& evidence,
                     bool include_digest);

/// Reads and validates one evidence record, including its digest.
Result<SourceEvidence> decode_evidence(const CanonicalReader& reader, std::string_view prefix);

/// The canonical digest of one evidence record.
std::string evidence_digest(const SourceEvidence& evidence);

void encode_reserve(CanonicalWriter& writer, std::string_view prefix, const CapacityReserve& reserve,
                    bool include_digest);
Result<CapacityReserve> decode_reserve(const CanonicalReader& reader, std::string_view prefix);
std::string reserve_digest(const CapacityReserve& reserve);

void encode_constraint(CanonicalWriter& writer, std::string_view prefix, const ServiceConstraint& constraint,
                       bool include_digest);
Result<ServiceConstraint> decode_constraint(const CanonicalReader& reader, std::string_view prefix);
std::string constraint_digest(const ServiceConstraint& constraint);

void encode_requirement(CanonicalWriter& writer, std::string_view prefix, const CapacityRequirement& requirement);
Result<CapacityRequirement> decode_requirement(const CanonicalReader& reader, std::string_view prefix);

void encode_note(CanonicalWriter& writer, std::string_view prefix, const CapacityNote& note);
Result<CapacityNote> decode_note(const CanonicalReader& reader, std::string_view prefix);

void encode_notes(CanonicalWriter& writer, std::string_view prefix, const NoteSet& notes);
Result<NoteSet> decode_notes(const CanonicalReader& reader, std::string_view prefix);

void encode_precondition(CanonicalWriter& writer, std::string_view prefix, const CapacityPrecondition& precondition);
Result<CapacityPrecondition> decode_precondition(const CanonicalReader& reader, std::string_view prefix);

}  // namespace dccp::facility_capacity::internal

#endif  // DCCP_FACILITY_CAPACITY_SRC_INTERNAL_CODEC_HPP
