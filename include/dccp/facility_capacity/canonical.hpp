// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Canonical encoding.
//
// The canonical encoding is line-oriented UTF-8 with LF terminators and ASCII
// content only. Keys are emitted in a fixed order chosen by the encoder, never
// by iteration over a hash container, so the same state always produces the
// same bytes on every platform, in every locale and in every process.
//
// Every document ends with a `digest=` line holding the lowercase hexadecimal
// SHA-256 of every byte before it. A decoder:
//   1. bounds the input before parsing it;
//   2. parses into the structured input description;
//   3. re-derives every derived value;
//   4. re-encodes the re-derived value and compares it byte for byte with the
//      bytes it was given;
//   5. verifies the digest.
// Steps 3 and 4 are what make the check stronger than a checksum: a document
// whose derived fields were edited is refused even if its digest was recomputed
// to match, because the derived fields would not reproduce.

#ifndef DCCP_FACILITY_CAPACITY_CANONICAL_HPP
#define DCCP_FACILITY_CAPACITY_CANONICAL_HPP

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_capacity/model.hpp"
#include "dccp/facility_capacity/result.hpp"
#include "dccp/facility_capacity/snapshot.hpp"

namespace dccp::facility_capacity::canonical {

/// Framing line that introduces the model-state document inside a generation
/// file. Not a `key=value` line, so it cannot collide with a document field.
inline constexpr std::string_view state_frame = "@state";

/// Framing line that introduces the snapshot document inside a generation
/// file.
inline constexpr std::string_view snapshot_frame = "@snapshot";

/// Encodes the complete snapshot document, including its own `digest=` line.
std::string encode_snapshot(const CapacitySnapshot& snapshot);

/// Decodes a snapshot document.
///
/// `freshness` is applied to the result: a document read out of durable storage
/// is decoded as `SnapshotFreshness::recovered` so that it can never claim to
/// be freshly issued.
Result<std::shared_ptr<const CapacitySnapshot>> decode_snapshot(std::string_view document,
                                                                SnapshotFreshness freshness);

/// Encodes the complete model-state document, including its own `digest=` line.
std::string encode_model_state(const FacilityCapacityModel& model);

/// Decodes a model-state document.
Result<FacilityCapacityModel> decode_model_state(std::string_view document, Clock& clock);

/// Splits a generation-file body into its state and snapshot documents.
///
/// The body must consist of exactly `@state`, the state document, `@snapshot`
/// and the snapshot document, in that order, with no trailing content.
Result<void> split_generation_body(std::string_view body, std::string_view& state_document,
                                   std::string_view& snapshot_document);

/// Joins a state document and a snapshot document into a generation-file body.
std::string join_generation_body(std::string_view state_document, std::string_view snapshot_document);

}  // namespace dccp::facility_capacity::canonical

#endif  // DCCP_FACILITY_CAPACITY_CANONICAL_HPP
