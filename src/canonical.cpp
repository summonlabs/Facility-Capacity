// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The public canonical encoding surface.
//
// Encoding is a pure function of state, so equal state always produces equal
// bytes on every platform, in every locale and in every process. Decoding is
// strict: it bounds the input, verifies the self-digest, parses the input
// description, re-derives the answer, re-encodes it and compares the result
// byte for byte with the bytes it was given.

#include "dccp/facility_capacity/canonical.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "dccp/facility_capacity/provenance.hpp"
#include "dccp/facility_capacity/version.hpp"
#include "internal/canonical_io.hpp"
#include "internal/documents.hpp"
#include "internal/text.hpp"

namespace dccp::facility_capacity::canonical {
namespace {

Error corruption(std::string_view message) { return Error::make(ErrorCode::corruption, message); }

std::string render_version() { return std::to_string(canonical_format_version); }

}  // namespace

std::string encode_snapshot(const CapacitySnapshot& snapshot) {
  internal::CanonicalWriter writer;
  writer.field("document", "snapshot");
  writer.field("canonical_format", static_cast<std::uint64_t>(canonical_format_version));
  internal::encode_snapshot_fields(writer, snapshot);
  return writer.finish();
}

Result<std::shared_ptr<const CapacitySnapshot>> decode_snapshot(std::string_view document,
                                                                SnapshotFreshness freshness) {
  Result<internal::CanonicalReader> reader = internal::CanonicalReader::parse(document);
  if (!reader.has_value()) {
    return reader.error();
  }
  const Result<void> document_kind = reader.value().expect("document", "snapshot");
  if (!document_kind.has_value()) {
    return document_kind.error();
  }
  const Result<void> format = reader.value().expect("canonical_format", render_version());
  if (!format.has_value()) {
    return format.error();
  }

  Result<CapacitySnapshotInput> input = internal::decode_snapshot_fields(reader.value());
  if (!input.has_value()) {
    return input.error();
  }
  // Freshness is the reader's stamp on the answer, not part of the answer: it is
  // deliberately absent from the document and therefore from the content
  // digest, so the digest of a recovered answer is the digest that was
  // published. What keeps a recovered answer from being mistaken for a freshly
  // issued one is the stamp itself and the recovery notes.
  input.value().freshness = freshness;

  const Result<std::string_view> derived_digest = reader.value().take("derived_digest");
  if (!derived_digest.has_value()) {
    return derived_digest.error();
  }
  if (!is_hex_digest(derived_digest.value())) {
    return corruption("the snapshot derived digest is not a SHA-256 hex value");
  }
  const Result<void> finished = reader.value().finish();
  if (!finished.has_value()) {
    return finished.error();
  }

  Result<std::shared_ptr<const CapacitySnapshot>> built =
      CapacitySnapshot::build(std::move(input.value()));
  if (!built.has_value()) {
    return built.error();
  }

  // Re-encoding reproduces the stored derived digest as well, because the
  // encoder recomputes it from the freshly derived answer.
  if (encode_snapshot(*built.value()) != document) {
    return corruption("the snapshot document does not reproduce from its own input");
  }

  return built;
}

std::string encode_model_state(const FacilityCapacityModel& model) {
  const Result<std::string> encoded = model.encode();
  if (!encoded.has_value()) {
    return std::string();
  }
  return encoded.value();
}

Result<FacilityCapacityModel> decode_model_state(std::string_view document, Clock& clock) {
  return FacilityCapacityModel::decode(document, clock);
}

Result<void> split_generation_body(std::string_view body, std::string_view& state_document,
                                   std::string_view& snapshot_document) {
  constexpr std::string_view kStateFrame = "@state\n";
  constexpr std::string_view kSnapshotFrame = "\n@snapshot\n";

  if (body.size() < kStateFrame.size() || body.substr(0, kStateFrame.size()) != kStateFrame) {
    return corruption("a generation body does not begin with the state frame");
  }
  const std::size_t marker = body.find(kSnapshotFrame, kStateFrame.size());
  if (marker == std::string_view::npos) {
    return corruption("a generation body has no snapshot frame");
  }
  state_document = body.substr(kStateFrame.size(), marker + 1 - kStateFrame.size());
  snapshot_document = body.substr(marker + kSnapshotFrame.size());
  if (snapshot_document.empty() || state_document.empty()) {
    return corruption("a generation body has an empty section");
  }
  if (snapshot_document.find(kSnapshotFrame) != std::string_view::npos) {
    return corruption("a generation body contains more than one snapshot frame");
  }
  return Status::success();
}

std::string join_generation_body(std::string_view state_document, std::string_view snapshot_document) {
  std::string body;
  body.reserve(state_document.size() + snapshot_document.size() + 32);
  body.append(state_frame);
  body.push_back('\n');
  body.append(state_document);
  body.append(snapshot_frame);
  body.push_back('\n');
  body.append(snapshot_document);
  return body;
}

}  // namespace dccp::facility_capacity::canonical
