// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Hard bounds. Every persisted or externally supplied size is checked against
// these before memory is reserved for it, so a malformed or hostile input
// cannot drive an unbounded allocation.

#ifndef DCCP_FACILITY_CAPACITY_LIMITS_HPP
#define DCCP_FACILITY_CAPACITY_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace dccp::facility_capacity::limits {

/// Longest accepted identity, including facility, site, source, reserve and
/// constraint identifiers.
inline constexpr std::size_t max_identifier_length = 63;

/// Longest accepted free-text detail attached to a reason or an error.
inline constexpr std::size_t max_detail_length = 160;

/// Longest accepted error message.
inline constexpr std::size_t max_message_length = 512;

/// Longest accepted provenance revision string.
inline constexpr std::size_t max_revision_length = 96;

/// Upper bound on distinct evidence sources in one model.
inline constexpr std::size_t max_evidence_sources = 256;

/// Upper bound on declared reserves in one model.
inline constexpr std::size_t max_reserves = 4096;

/// Upper bound on declared service constraints in one model.
inline constexpr std::size_t max_service_constraints = 1024;

/// Upper bound on declared capacity requirements in one model.
inline constexpr std::size_t max_requirements = 64;

/// Upper bound on recorded idempotency attempts.
inline constexpr std::size_t max_recorded_attempts = 64;

/// Upper bound on retained published generations in one store.
inline constexpr std::size_t max_retained_generations = 64;

/// Largest canonical body accepted from a durable store file.
inline constexpr std::uint64_t max_store_body_bytes = 64ull * 1024ull * 1024ull;

/// Largest manifest, current-pointer or lock file accepted.
inline constexpr std::uint64_t max_manifest_bytes = 64ull * 1024ull;

/// Largest single canonical line accepted while decoding.
inline constexpr std::size_t max_line_length = 8192;

/// Largest number of lines accepted in one canonical body.
///
/// The bound is deliberately far above any document this product can produce
/// (a full model is a few tens of thousands of lines) and far below the point
/// where the decoder's field index could become a memory-exhaustion vector.
inline constexpr std::size_t max_body_lines = 262144;

/// Largest exact magnitude representable in any quantity.
///
/// The bound leaves a factor of 9000 of headroom below INT64_MAX so that a
/// permille headroom computation and any sum of a full source set can never
/// overflow a signed 64-bit integer.
inline constexpr std::int64_t max_quantity_magnitude = 1'000'000'000'000'000ll;

/// Largest number of entries materialised in one diff.
inline constexpr std::size_t max_diff_entries = 8192;

/// Largest number of lines rendered in one explanation.
inline constexpr std::size_t max_explanation_lines = 8192;

/// Largest number of reasons attached to one note set.
inline constexpr std::size_t max_reasons_per_note = 4096;

}  // namespace dccp::facility_capacity::limits

#endif  // DCCP_FACILITY_CAPACITY_LIMITS_HPP
