// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Facility Capacity - version and format identity.
//
// DCCP Tranche 2: Facility Capacity and Placement.
// Repository 9 of the 72-runtime Data Center Control Plane.

#ifndef DCCP_FACILITY_CAPACITY_VERSION_HPP
#define DCCP_FACILITY_CAPACITY_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace dccp::facility_capacity {

/// Product version.
inline constexpr int version_major = 1;
inline constexpr int version_minor = 0;
inline constexpr int version_patch = 0;
inline constexpr std::string_view version_string = "1.0.0";

/// Name used in notices and in the CLI banner.
inline constexpr std::string_view product_name = "Facility Capacity";
inline constexpr std::string_view product_slug = "facility-capacity";

/// Version of the canonical text encoding used for state and snapshots.
///
/// A reader refuses any other value with `incompatible_version`; it never
/// guesses at a layout it does not know.
inline constexpr std::uint32_t canonical_format_version = 1;

/// Version of the durable store container (binary header plus canonical body).
inline constexpr std::uint32_t store_format_version = 1;

/// Magic at the start of every durable store file.
inline constexpr std::string_view store_magic = "FCAPGEN1";

/// Little-endian byte-order marker written into every durable store file.
///
/// The value is written as four bytes in little-endian order. A reader that
/// observes the byte-swapped value knows the file was produced by a
/// big-endian writer, or that the file was corrupted, and refuses it rather
/// than misinterpreting every multi-byte field.
inline constexpr std::uint32_t store_byte_order_marker = 0x01020304u;

/// Size in bytes of the fixed binary header preceding the canonical body.
inline constexpr std::uint32_t store_header_size = 128;

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_VERSION_HPP
