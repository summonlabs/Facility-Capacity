// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "internal/store_format.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "dccp/facility_capacity/digest.hpp"
#include "dccp/facility_capacity/limits.hpp"
#include "dccp/facility_capacity/provenance.hpp"
#include "dccp/facility_capacity/version.hpp"
#include "internal/canonical_io.hpp"
#include "internal/text.hpp"

namespace dccp::facility_capacity::internal::store_format {
namespace {

constexpr std::string_view kDocumentFormat = "store-format";
constexpr std::string_view kDocumentCurrent = "store-current";
constexpr std::string_view kByteOrderLittle = "little";

// The fixed generation-file header, byte for byte. Every constant below is
// stated twice: once as the offset the format table gives and once as the
// assertion that ties it to the neighbouring field, so a change to any field
// width fails the build instead of silently moving every later field.
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kMagicBytes = 8;
constexpr std::size_t kByteOrderOffset = 8;
constexpr std::size_t kFormatVersionOffset = 12;
constexpr std::size_t kHeaderSizeOffset = 16;
constexpr std::size_t kBodyBytesOffset = 20;
constexpr std::size_t kBodyDigestOffset = 24;
constexpr std::size_t kGenerationOffset = 56;
constexpr std::size_t kEpochOffset = 64;
constexpr std::size_t kIncarnationOffset = 72;
constexpr std::size_t kPublishedAtOffset = 80;
constexpr std::size_t kReservedOffset = 88;
constexpr std::size_t kReservedBytes = 40;
constexpr std::size_t kHeaderBytes = static_cast<std::size_t>(store_header_size);

constexpr std::size_t kMaxManifestBytes = static_cast<std::size_t>(limits::max_manifest_bytes);
constexpr std::uint64_t kMaxBodyBytes = limits::max_store_body_bytes;

// `digest=` + 64 hex characters + LF.

static_assert(store_magic.size() == kMagicBytes, "the store magic fills the first eight header bytes");
static_assert(kByteOrderOffset == kMagicOffset + kMagicBytes, "the byte-order marker follows the magic");
static_assert(kFormatVersionOffset == kByteOrderOffset + 4, "the format version follows the byte-order marker");
static_assert(kHeaderSizeOffset == kFormatVersionOffset + 4, "the header size follows the format version");
static_assert(kBodyBytesOffset == kHeaderSizeOffset + 4, "the body length follows the header size");
static_assert(kBodyDigestOffset == kBodyBytesOffset + 4, "the body digest follows the body length");
static_assert(kGenerationOffset == kBodyDigestOffset + digest::sha256_bytes,
              "the generation follows the body digest");
static_assert(kEpochOffset == kGenerationOffset + 8, "the epoch follows the generation");
static_assert(kIncarnationOffset == kEpochOffset + 8, "the incarnation follows the epoch");
static_assert(kPublishedAtOffset == kIncarnationOffset + 8, "the published-at tick follows the incarnation");
static_assert(kReservedOffset == kPublishedAtOffset + 8, "the reserved region follows the published-at tick");
static_assert(kReservedOffset + kReservedBytes == kHeaderBytes, "the reserved region ends the header");

Error corruption(std::string_view message) { return Error::make(ErrorCode::corruption, message); }

Error path_rejected(std::string_view message, std::string_view constraint) {
  Error error = Error::make(ErrorCode::path_rejected, message);
  error.with_constraint(std::string(constraint));
  return error;
}

/// The header fields every canonical manifest begins with.
std::string format_version_text() { return render_u64(static_cast<std::uint64_t>(store_format_version)); }

/// Bounds the body, then parses and digest-verifies it. The bound is applied
/// before the parser sees a byte, so an oversized body never reaches the field
/// index.
Result<CanonicalReader> parse_manifest(std::string_view body) {
  if (body.size() > kMaxManifestBytes) {
    Error error = Error::make(ErrorCode::limit_exceeded, "manifest exceeds the manifest byte bound");
    error.with_constraint("body_bytes <= max_manifest_bytes");
    return error;
  }
  return CanonicalReader::parse(body);
}

/// Consumes the three fixed leading fields of a manifest.
Status expect_manifest_header(CanonicalReader& reader, std::string_view document) {
  const Status document_ok = reader.expect("document", document);
  if (!document_ok.has_value()) {
    return document_ok.error();
  }
  const std::string version = format_version_text();
  const Status version_ok = reader.expect("store_format_version", version);
  if (!version_ok.has_value()) {
    return version_ok.error();
  }
  return reader.expect("byte_order", kByteOrderLittle);
}

template <class Id>
Result<Id> take_id(CanonicalReader& reader, std::string_view key) {
  const Result<std::string_view> raw = reader.take(key);
  if (!raw.has_value()) {
    return raw.error();
  }
  return Id::parse(raw.value());
}

Status validate_current(const CurrentManifest& manifest) {
  if (!manifest.file_name.empty() && !is_generation_file_name(manifest.file_name)) {
    return path_rejected("store file name is not an accepted generation file name",
                         "file_name matches gen-<digits>.fcs");
  }
  if (!manifest.snapshot_digest.empty() && !is_hex_digest(manifest.snapshot_digest)) {
    return corruption("snapshot digest is not a lowercase SHA-256 hex value");
  }
  if (manifest.has_published_generation) {
    if (manifest.file_name.empty() || manifest.snapshot_digest.empty()) {
      Error error = Error::make(ErrorCode::corruption, "a published generation must name a file and a digest");
      error.with_constraint("published generation requires a file name and a digest");
      return error;
    }
    return Status::success();
  }
  if (!manifest.generation.is_zero()) {
    return corruption("an unpublished store records a non-zero generation");
  }
  return Status::success();
}

/// Appends `value` one byte at a time in little-endian order. Native byte order
/// and native integer layout never take part.
void append_le_u32(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>(value & 0xFFu));
  out.push_back(static_cast<char>((value >> 8u) & 0xFFu));
  out.push_back(static_cast<char>((value >> 16u) & 0xFFu));
  out.push_back(static_cast<char>((value >> 24u) & 0xFFu));
}

void append_le_u64(std::string& out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64u; shift += 8u) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

void append_digest(std::string& out, const digest::Digest& value) {
  for (const std::uint8_t byte : value) {
    out.push_back(static_cast<char>(byte));
  }
}

std::uint32_t read_le_u32(std::string_view bytes, std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4u; ++index) {
    const std::uint32_t byte = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + index]));
    value |= byte << (8u * index);
  }
  return value;
}

std::uint64_t read_le_u64(std::string_view bytes, std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8u; ++index) {
    const std::uint64_t byte = static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + index]));
    value |= byte << (8u * index);
  }
  return value;
}

bool all_zero(std::string_view bytes, std::size_t offset, std::size_t count) noexcept {
  for (std::size_t index = 0; index < count; ++index) {
    if (bytes[offset + index] != '\0') {
      return false;
    }
  }
  return true;
}

}  // namespace

std::string FormatManifest::to_string() const {
  std::string out = "store_id=";
  out.append(store_id.value());
  out.append(" facility=");
  out.append(facility.value());
  out.append(" site=");
  out.append(site.value());
  out.append(" controller=");
  out.append(controller.value());
  out.append(" created_at=");
  out.append(created_at.to_string());
  return out;
}

Result<std::string> encode_format(const FormatManifest& manifest) {
  CanonicalWriter writer;
  writer.field("document", kDocumentFormat);
  writer.field("store_format_version", static_cast<std::uint64_t>(store_format_version));
  writer.field("byte_order", kByteOrderLittle);
  writer.field("store_id", manifest.store_id.value());
  writer.field("facility", manifest.facility.value());
  writer.field("site", manifest.site.value());
  writer.field("controller", manifest.controller.value());
  writer.field("created_at", manifest.created_at.value());
  return writer.finish();
}

Result<FormatManifest> decode_format(std::string_view body) {
  Result<CanonicalReader> parsed = parse_manifest(body);
  if (!parsed.has_value()) {
    return parsed.error();
  }
  CanonicalReader& reader = parsed.value();
  const Status header_ok = expect_manifest_header(reader, kDocumentFormat);
  if (!header_ok.has_value()) {
    return header_ok.error();
  }

  FormatManifest manifest;
  const Result<StoreId> store_id = take_id<StoreId>(reader, "store_id");
  if (!store_id.has_value()) {
    return store_id.error();
  }
  manifest.store_id = store_id.value();
  const Result<FacilityId> facility = take_id<FacilityId>(reader, "facility");
  if (!facility.has_value()) {
    return facility.error();
  }
  manifest.facility = facility.value();
  const Result<SiteId> site = take_id<SiteId>(reader, "site");
  if (!site.has_value()) {
    return site.error();
  }
  manifest.site = site.value();
  const Result<ControllerId> controller = take_id<ControllerId>(reader, "controller");
  if (!controller.has_value()) {
    return controller.error();
  }
  manifest.controller = controller.value();
  const Result<std::uint64_t> created_at = reader.take_u64("created_at");
  if (!created_at.has_value()) {
    return created_at.error();
  }
  manifest.created_at = Tick::from_value(created_at.value());

  const Status finished = reader.finish();
  if (!finished.has_value()) {
    return finished.error();
  }
  return manifest;
}

std::string CurrentManifest::to_string() const {
  std::string out = "store_id=";
  out.append(store_id.value());
  out.append(" facility=");
  out.append(facility.value());
  out.append(" site=");
  out.append(site.value());
  out.append(" controller=");
  out.append(controller.value());
  out.append(" epoch=");
  out.append(epoch.to_string());
  out.append(" incarnation=");
  out.append(incarnation.to_string());
  out.append(" generation=");
  out.append(generation.to_string());
  out.append(" revision=");
  out.append(revision.to_string());
  out.append(" has_published_generation=");
  out.append(render_bool(has_published_generation));
  out.append(" published_at=");
  out.append(published_at.to_string());
  out.append(" file_name=");
  out.append(file_name.empty() ? std::string_view("-") : std::string_view(file_name));
  out.append(" snapshot_digest=");
  out.append(snapshot_digest.empty() ? std::string_view("-") : std::string_view(snapshot_digest));
  return out;
}

Result<std::string> encode_current(const CurrentManifest& manifest) {
  const Status valid = validate_current(manifest);
  if (!valid.has_value()) {
    return valid.error();
  }

  CanonicalWriter writer;
  writer.field("document", kDocumentCurrent);
  writer.field("store_format_version", static_cast<std::uint64_t>(store_format_version));
  writer.field("byte_order", kByteOrderLittle);
  writer.field("store_id", manifest.store_id.value());
  writer.field("facility", manifest.facility.value());
  writer.field("site", manifest.site.value());
  writer.field("controller", manifest.controller.value());
  writer.field("epoch", manifest.epoch.value());
  writer.field("incarnation", manifest.incarnation.value());
  writer.field("generation", manifest.generation.value());
  writer.field("revision", manifest.revision.value());
  writer.field("has_published_generation", manifest.has_published_generation);
  writer.field("published_at", manifest.published_at.value());
  writer.field("file_name", manifest.file_name);
  writer.field("snapshot_digest", manifest.snapshot_digest);
  return writer.finish();
}

Result<CurrentManifest> decode_current(std::string_view body) {
  Result<CanonicalReader> parsed = parse_manifest(body);
  if (!parsed.has_value()) {
    return parsed.error();
  }
  CanonicalReader& reader = parsed.value();
  const Status header_ok = expect_manifest_header(reader, kDocumentCurrent);
  if (!header_ok.has_value()) {
    return header_ok.error();
  }

  CurrentManifest manifest;
  const Result<StoreId> store_id = take_id<StoreId>(reader, "store_id");
  if (!store_id.has_value()) {
    return store_id.error();
  }
  manifest.store_id = store_id.value();
  const Result<FacilityId> facility = take_id<FacilityId>(reader, "facility");
  if (!facility.has_value()) {
    return facility.error();
  }
  manifest.facility = facility.value();
  const Result<SiteId> site = take_id<SiteId>(reader, "site");
  if (!site.has_value()) {
    return site.error();
  }
  manifest.site = site.value();
  const Result<ControllerId> controller = take_id<ControllerId>(reader, "controller");
  if (!controller.has_value()) {
    return controller.error();
  }
  manifest.controller = controller.value();
  const Result<std::uint64_t> epoch = reader.take_u64("epoch");
  if (!epoch.has_value()) {
    return epoch.error();
  }
  manifest.epoch = EpochId::from_value(epoch.value());
  const Result<std::uint64_t> incarnation = reader.take_u64("incarnation");
  if (!incarnation.has_value()) {
    return incarnation.error();
  }
  manifest.incarnation = IncarnationId::from_value(incarnation.value());
  const Result<std::uint64_t> generation = reader.take_u64("generation");
  if (!generation.has_value()) {
    return generation.error();
  }
  manifest.generation = CapacityGeneration::from_value(generation.value());
  const Result<std::uint64_t> revision = reader.take_u64("revision");
  if (!revision.has_value()) {
    return revision.error();
  }
  manifest.revision = Revision::from_value(revision.value());
  const Result<bool> has_published = reader.take_bool("has_published_generation");
  if (!has_published.has_value()) {
    return has_published.error();
  }
  manifest.has_published_generation = has_published.value();
  const Result<std::uint64_t> published_at = reader.take_u64("published_at");
  if (!published_at.has_value()) {
    return published_at.error();
  }
  manifest.published_at = Tick::from_value(published_at.value());
  const Result<std::string> file_name = reader.take_string("file_name");
  if (!file_name.has_value()) {
    return file_name.error();
  }
  manifest.file_name = file_name.value();
  const Result<std::string> snapshot_digest = reader.take_string("snapshot_digest");
  if (!snapshot_digest.has_value()) {
    return snapshot_digest.error();
  }
  manifest.snapshot_digest = snapshot_digest.value();

  const Status finished = reader.finish();
  if (!finished.has_value()) {
    return finished.error();
  }
  const Status valid = validate_current(manifest);
  if (!valid.has_value()) {
    return valid.error();
  }
  return manifest;
}

Result<std::string> encode_generation_file(const GenerationHeader& header, std::string_view body) {
  if (body.size() > static_cast<std::size_t>(kMaxBodyBytes)) {
    Error error = Error::make(ErrorCode::limit_exceeded, "generation body exceeds the store body bound");
    error.with_constraint("body_bytes <= max_store_body_bytes");
    return error;
  }
  if (body.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    Error error = Error::make(ErrorCode::limit_exceeded, "generation body does not fit the 32-bit length field");
    error.with_constraint("body_bytes <= 4294967295");
    return error;
  }
  if (!is_hex_digest(header.body_digest)) {
    return Error::make(ErrorCode::invalid_argument, "body digest is not 64 lowercase hex characters");
  }
  const digest::Digest computed = digest::sha256(body);
  if (digest::to_hex(computed) != header.body_digest) {
    Error error = Error::make(ErrorCode::checksum_mismatch, "body digest does not hash the generation body");
    error.with_constraint("body_digest == sha256(body)");
    return error;
  }

  std::string out;
  out.reserve(kHeaderBytes + body.size());
  out.append(store_magic);
  append_le_u32(out, store_byte_order_marker);
  append_le_u32(out, store_format_version);
  append_le_u32(out, store_header_size);
  append_le_u32(out, static_cast<std::uint32_t>(body.size()));
  append_digest(out, computed);
  append_le_u64(out, header.generation.value());
  append_le_u64(out, header.epoch.value());
  append_le_u64(out, header.incarnation.value());
  append_le_u64(out, header.published_at.value());
  out.append(kReservedBytes, '\0');
  out.append(body);
  return out;
}

Result<void> decode_generation_file(std::string_view file_bytes, GenerationHeader& header,
                                    std::string_view& body) {
  if (file_bytes.size() < kHeaderBytes) {
    Error error = Error::make(ErrorCode::truncated_input, "generation file is shorter than its fixed header");
    error.with_constraint("file_bytes >= store_header_size");
    return error;
  }
  if (file_bytes.substr(kMagicOffset, kMagicBytes) != store_magic) {
    return corruption("generation file does not begin with the store magic");
  }

  const std::uint32_t byte_order = read_le_u32(file_bytes, kByteOrderOffset);
  if (byte_order != store_byte_order_marker) {
    Error error = Error::make(ErrorCode::incompatible_version, "generation file byte order is not little-endian");
    error.with_constraint("byte_order == little");
    return error;
  }
  const std::uint32_t format_version = read_le_u32(file_bytes, kFormatVersionOffset);
  if (format_version != store_format_version) {
    Error error = Error::make(ErrorCode::incompatible_version, "generation file format version is not supported");
    error.with_constraint("store_format_version == " + format_version_text());
    return error;
  }
  const std::uint32_t header_bytes = read_le_u32(file_bytes, kHeaderSizeOffset);
  if (header_bytes != store_header_size) {
    Error error = Error::make(ErrorCode::incompatible_version, "generation file header size is not supported");
    error.with_constraint("header_bytes == " + render_u64(static_cast<std::uint64_t>(store_header_size)));
    return error;
  }

  const std::uint32_t declared_body = read_le_u32(file_bytes, kBodyBytesOffset);
  if (static_cast<std::uint64_t>(declared_body) > kMaxBodyBytes) {
    Error error = Error::make(ErrorCode::limit_exceeded, "generation body exceeds the store body bound");
    error.with_constraint("body_bytes <= max_store_body_bytes");
    return error;
  }
  const std::size_t body_bytes = static_cast<std::size_t>(declared_body);
  const std::size_t declared_total = kHeaderBytes + body_bytes;
  if (file_bytes.size() < declared_total) {
    Error error = Error::make(ErrorCode::truncated_input, "generation file is shorter than its header declares");
    error.with_constraint("file_bytes >= store_header_size + body_bytes");
    return error;
  }
  if (file_bytes.size() > declared_total) {
    Error error = Error::make(ErrorCode::conflict, "generation file is longer than its header declares");
    error.with_constraint("file_bytes == store_header_size + body_bytes");
    return error;
  }

  digest::Digest stored{};
  for (std::size_t index = 0; index < digest::sha256_bytes; ++index) {
    stored[index] = static_cast<std::uint8_t>(static_cast<unsigned char>(file_bytes[kBodyDigestOffset + index]));
  }
  const std::string_view body_view = file_bytes.substr(kHeaderBytes, body_bytes);
  const digest::Digest computed = digest::sha256(body_view);
  if (computed != stored) {
    Error error = Error::make(ErrorCode::checksum_mismatch, "generation body does not hash to the recorded digest");
    error.with_constraint("sha256(body) == header body digest");
    return error;
  }
  if (!all_zero(file_bytes, kReservedOffset, kReservedBytes)) {
    return corruption("generation file reserved header bytes are not zero");
  }

  header.format_version = format_version;
  header.generation = CapacityGeneration::from_value(read_le_u64(file_bytes, kGenerationOffset));
  header.epoch = EpochId::from_value(read_le_u64(file_bytes, kEpochOffset));
  header.incarnation = IncarnationId::from_value(read_le_u64(file_bytes, kIncarnationOffset));
  header.published_at = Tick::from_value(read_le_u64(file_bytes, kPublishedAtOffset));
  header.body_bytes = declared_body;
  header.body_digest = digest::to_hex(computed);
  body = body_view;
  return Status::success();
}

}  // namespace dccp::facility_capacity::internal::store_format
