// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// SHA-256, FIPS 180-4.
//
// Implemented here so that the product has no third-party dependency and so
// that the integrity digest is a fixed, auditable function of the bytes.
//
// The digest is an integrity check, not an authenticity mechanism: it detects
// corruption and accidental substitution. It is not a signature and the README
// says so.

#ifndef DCCP_FACILITY_CAPACITY_DIGEST_HPP
#define DCCP_FACILITY_CAPACITY_DIGEST_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace dccp::facility_capacity::digest {

/// Length of a SHA-256 digest in bytes.
inline constexpr std::size_t sha256_bytes = 32;

using Digest = std::array<std::uint8_t, sha256_bytes>;

/// Computes SHA-256 over a byte range.
Digest sha256(const void* data, std::size_t size) noexcept;

/// Computes SHA-256 over a string's bytes.
Digest sha256(std::string_view data) noexcept;

/// Lowercase hexadecimal rendering.
std::string to_hex(const Digest& value);

/// Lowercase hexadecimal SHA-256 of `data`.
std::string sha256_hex(std::string_view data);

/// Incremental SHA-256.
class Sha256Builder {
 public:
  Sha256Builder() noexcept { reset(); }

  void reset() noexcept;
  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view data) noexcept { update(data.data(), data.size()); }
  Digest finalize() const noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
};

}  // namespace dccp::facility_capacity::digest

#endif  // DCCP_FACILITY_CAPACITY_DIGEST_HPP
