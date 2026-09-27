// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Provenance: which source, which revision of that source, under which epoch
// and controller incarnation, produced a piece of evidence.
//
// Provenance is carried on every input. An input without provenance is not
// evidence; it is an assertion, and the library refuses it.

#ifndef DCCP_FACILITY_CAPACITY_PROVENANCE_HPP
#define DCCP_FACILITY_CAPACITY_PROVENANCE_HPP

#include <iosfwd>
#include <string>
#include <string_view>

#include "dccp/facility_capacity/generation.hpp"
#include "dccp/facility_capacity/result.hpp"
#include "dccp/facility_capacity/strong_id.hpp"

namespace dccp::facility_capacity {

struct Provenance {
  /// The evidence source that produced the value.
  CapacitySourceId source;
  /// Revision of that source, e.g. `power-capacity/1.0.0`. Bounded.
  std::string revision;
  /// Instant the source produced the value.
  Tick produced_at{};
  /// Control-plane epoch the source was operating under.
  EpochId epoch{};
  /// Controller incarnation that produced the value.
  IncarnationId incarnation{};
  /// The source's own digest of its evidence, hex encoded. May be empty when
  /// the source does not publish one.
  std::string evidence_digest;

  /// Validates identity, revision length, revision alphabet and digest shape.
  Status validate() const;

  /// Deterministic single-line rendering.
  std::string to_string() const;

  friend bool operator==(const Provenance& lhs, const Provenance& rhs) noexcept {
    return lhs.source == rhs.source && lhs.revision == rhs.revision &&
           lhs.produced_at == rhs.produced_at && lhs.epoch == rhs.epoch &&
           lhs.incarnation == rhs.incarnation && lhs.evidence_digest == rhs.evidence_digest;
  }
};

std::ostream& operator<<(std::ostream& out, const Provenance& provenance);

/// An exact half-open validity window `[start, end)` on the injected timeline.
class ValidityWindow {
 public:
  /// An always-active window starting at tick zero.
  ValidityWindow() = default;

  /// `[start, end)`. Requires `start < end`.
  static Result<ValidityWindow> create(Tick start, Tick end);

  /// `[start, infinity)`.
  static ValidityWindow unbounded_from(Tick start);

  /// True when `instant` is inside the half-open window.
  bool active_at(Tick instant) const noexcept {
    return start_ <= instant && instant < end_;
  }

  Tick start() const noexcept { return start_; }
  Tick end() const noexcept { return end_; }

  bool unbounded() const noexcept { return end_ == max_tick; }

  friend bool operator==(const ValidityWindow& lhs, const ValidityWindow& rhs) noexcept {
    return lhs.start_ == rhs.start_ && lhs.end_ == rhs.end_;
  }

  std::string to_string() const;

 private:
  Tick start_{};
  Tick end_ = max_tick;
};

std::ostream& operator<<(std::ostream& out, const ValidityWindow& window);

/// True when `text` is 64 lowercase hexadecimal characters. Empty is allowed
/// only where a source publishes no digest.
bool is_hex_digest(std::string_view text) noexcept;

}  // namespace dccp::facility_capacity

#endif  // DCCP_FACILITY_CAPACITY_PROVENANCE_HPP
