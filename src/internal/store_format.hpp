// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Durable store encodings. Private to the library.
//
// Three encodings live here:
//
//   FORMAT           an immutable canonical document naming the store, its
//                    facility, site and controller, and the instant the store
//                    was created
//   CURRENT          the mutable canonical manifest that is the control-plane
//                    authority: epoch, incarnation, published generation,
//                    revision and the name and digest of the generation file
//                    it names
//   generation file  a fixed 128-byte binary header followed by a canonical
//                    body, with the SHA-256 of the body in the header
//
// The manifests are canonical documents: ASCII `key=value` lines ending in a
// self-`digest=` line, written in a fixed field order and read with a reader
// that consumes every field exactly once. Nothing about either document depends
// on the host's byte order, locale or pointer width.
//
// A file name is treated as hostile input. A name read from disk is accepted
// only when it matches `gen-<digits>.fcs` exactly, so a stored name can never
// carry a separator, `..`, a drive letter or any other character that would let
// it escape the store directory. A generation file has the same property by
// construction: it is located by a generated name, never by a recorded one.
//
// The binary header writes every multi-byte integer one byte at a time in
// little-endian order, and the byte-order marker is the first multi-byte field a
// reader checks, so a byte-swapped or foreign-endian file is refused as
// `incompatible_version` before any other field is believed.

#ifndef DCCP_FACILITY_CAPACITY_SRC_INTERNAL_STORE_FORMAT_HPP
#define DCCP_FACILITY_CAPACITY_SRC_INTERNAL_STORE_FORMAT_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "dccp/facility_capacity/generation.hpp"
#include "dccp/facility_capacity/result.hpp"
#include "dccp/facility_capacity/strong_id.hpp"

namespace dccp::facility_capacity::internal::store_format {

/// The immutable FORMAT manifest.
struct FormatManifest {
  StoreId store_id;
  FacilityId facility;
  SiteId site;
  ControllerId controller;
  Tick created_at{};

  /// Deterministic single-line rendering for operator-facing output.
  std::string to_string() const;
};

/// Encodes the canonical FORMAT document.
///
/// The result ends with the self-`digest=` line, whose value is the SHA-256 of
/// every byte that precedes it, which is exactly what the reader recomputes.
Result<std::string> encode_format(const FormatManifest& manifest);

/// Decodes a canonical FORMAT document.
///
/// Fails with `checksum_mismatch` when the self-digest does not match the bytes,
/// with `limit_exceeded` when the body exceeds `limits::max_manifest_bytes`, with
/// `invalid_argument` when a recorded identifier is malformed and with
/// `corruption` for any missing, extra, duplicated, unknown or malformed field.
Result<FormatManifest> decode_format(std::string_view body);

/// The mutable CURRENT manifest: the control-plane authority of a store.
struct CurrentManifest {
  StoreId store_id;
  FacilityId facility;
  SiteId site;
  ControllerId controller;
  EpochId epoch;
  IncarnationId incarnation;
  CapacityGeneration generation{};
  Revision revision{};
  bool has_published_generation = false;
  Tick published_at{};
  /// `gen-<n>.fcs`, or empty when nothing is published.
  std::string file_name;
  /// 64 lowercase hex characters, or empty.
  std::string snapshot_digest;

  /// Deterministic single-line rendering for operator-facing output.
  std::string to_string() const;
};

/// Encodes the canonical CURRENT document.
///
/// The recorded state is validated first, so a document this encoder produces is
/// always one `decode_current` accepts: a non-empty `file_name` must match
/// `gen-<digits>.fcs` (`path_rejected`), a non-empty `snapshot_digest` must be 64
/// lowercase hex characters (`corruption`), a published generation requires both
/// (`corruption`) and an unpublished store must record generation zero
/// (`corruption`).
///
/// The result ends with the self-`digest=` line, whose value is the SHA-256 of
/// every byte that precedes it, which is exactly what the reader recomputes.
Result<std::string> encode_current(const CurrentManifest& manifest);

/// Decodes a canonical CURRENT document with the same checks `encode_current`
/// applies, plus the document-shape checks described on `decode_format`.
Result<CurrentManifest> decode_current(std::string_view body);

/// The fixed header of a generation file.
struct GenerationHeader {
  std::uint32_t format_version = 0;
  CapacityGeneration generation{};
  EpochId epoch{};
  IncarnationId incarnation{};
  Tick published_at{};
  std::uint32_t body_bytes = 0;
  /// 64 lowercase hex characters.
  std::string body_digest;
};

/// Encodes `body` into a generation file.
///
/// `header.body_digest` must be 64 lowercase hex characters (`invalid_argument`)
/// and must equal the SHA-256 of `body` (`checksum_mismatch`); the digest is
/// recomputed here rather than believed. `header.format_version` and
/// `header.body_bytes` are written from `store_format_version` and
/// `body.size()`, never from the struct fields, so a caller cannot record a
/// version or a length the bytes do not have.
///
/// Fails with `limit_exceeded` when the body exceeds
/// `limits::max_store_body_bytes` or does not fit the 32-bit length field.
Result<std::string> encode_generation_file(const GenerationHeader& header, std::string_view body);

/// Decodes a generation file: `file_bytes` = fixed binary header followed by the
/// canonical body.
///
/// On success `header.body_digest` is the verified lowercase hex SHA-256 of the
/// body and `body` is a view into `file_bytes`. Every failure is reported
/// without materialising anything derived from the declared length:
///
///   `truncated_input`       file shorter than the header, or than the header
///                           plus the declared body
///   `corruption`            magic bytes differ, or a reserved byte is non-zero
///   `incompatible_version`  byte-order marker, format version or header size is
///                           not the one this build writes
///   `limit_exceeded`        the declared body exceeds `limits::max_store_body_bytes`
///   `conflict`              the file is longer than the header describes, so a
///                           region exists that the header does not account for
///   `checksum_mismatch`     the stored digest does not hash the body bytes
Result<void> decode_generation_file(std::string_view file_bytes, GenerationHeader& header,
                                    std::string_view& body);

}  // namespace dccp::facility_capacity::internal::store_format

#endif  // DCCP_FACILITY_CAPACITY_SRC_INTERNAL_STORE_FORMAT_HPP
