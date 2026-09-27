// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// SHA-256, FIPS 180-4.

#include "dccp/facility_capacity/digest.hpp"

#include <cstring>

namespace dccp::facility_capacity::digest {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
}};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned count) noexcept {
  return (value >> count) | (value << (32u - count));
}

}  // namespace

void Sha256Builder::reset() noexcept {
  state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  buffer_.fill(0);
  buffered_ = 0;
  total_bytes_ = 0;
}

void Sha256Builder::compress(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16; ++index) {
    const std::size_t base = index * 4;
    schedule[index] = (static_cast<std::uint32_t>(block[base]) << 24) |
                      (static_cast<std::uint32_t>(block[base + 1]) << 16) |
                      (static_cast<std::uint32_t>(block[base + 2]) << 8) |
                      static_cast<std::uint32_t>(block[base + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotr(schedule[index - 15], 7) ^ rotr(schedule[index - 15], 18) ^
                             (schedule[index - 15] >> 3);
    const std::uint32_t s1 = rotr(schedule[index - 2], 17) ^ rotr(schedule[index - 2], 19) ^
                             (schedule[index - 2] >> 10);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t choice = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + s1 + choice + kRoundConstants[index] + schedule[index];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256Builder::update(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  total_bytes_ += static_cast<std::uint64_t>(size);
  std::size_t offset = 0;
  while (offset < size) {
    const std::size_t room = 64 - buffered_;
    const std::size_t take = (size - offset < room) ? (size - offset) : room;
    std::memcpy(buffer_.data() + buffered_, bytes + offset, take);
    buffered_ += take;
    offset += take;
    if (buffered_ == 64) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
}

Digest Sha256Builder::finalize() const noexcept {
  Sha256Builder copy = *this;
  const std::uint64_t bit_length = copy.total_bytes_ * 8u;
  const std::uint8_t pad = 0x80u;
  copy.update(&pad, 1);
  const std::uint8_t zero = 0x00u;
  while (copy.buffered_ != 56) {
    copy.update(&zero, 1);
  }
  std::uint8_t length_bytes[8];
  for (std::size_t index = 0; index < 8; ++index) {
    length_bytes[index] = static_cast<std::uint8_t>((bit_length >> (56u - 8u * index)) & 0xFFu);
  }
  copy.update(length_bytes, 8);

  Digest result{};
  for (std::size_t index = 0; index < 8; ++index) {
    const std::uint32_t word = copy.state_[index];
    result[index * 4] = static_cast<std::uint8_t>((word >> 24) & 0xFFu);
    result[index * 4 + 1] = static_cast<std::uint8_t>((word >> 16) & 0xFFu);
    result[index * 4 + 2] = static_cast<std::uint8_t>((word >> 8) & 0xFFu);
    result[index * 4 + 3] = static_cast<std::uint8_t>(word & 0xFFu);
  }
  return result;
}

Digest sha256(const void* data, std::size_t size) noexcept {
  Sha256Builder builder;
  builder.update(data, size);
  return builder.finalize();
}

Digest sha256(std::string_view data) noexcept { return sha256(data.data(), data.size()); }

std::string to_hex(const Digest& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (const std::uint8_t byte : value) {
    out.push_back(kHex[(byte >> 4) & 0x0Fu]);
    out.push_back(kHex[byte & 0x0Fu]);
  }
  return out;
}

std::string sha256_hex(std::string_view data) { return to_hex(sha256(data)); }

}  // namespace dccp::facility_capacity::digest
