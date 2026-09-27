// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Suite: digest.
//
// SHA-256, FIPS 180-4. The published vectors below are asserted byte for byte,
// and the incremental builder is required to produce exactly the same bytes as
// the one-shot function for every split.

#include "test_framework.hpp"
#include "test_rng.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_capacity/digest.hpp"

namespace fdigest = dccp::facility_capacity::digest;

namespace {

using Digest = fdigest::Digest;

/// Asserts every byte of a digest against the expected bytes, and the
/// lowercase hexadecimal rendering against the published string.
void check_digest(const Digest& actual, const std::array<std::uint8_t, fdigest::sha256_bytes>& expected,
                  const char* hex) {
  for (std::size_t index = 0; index < expected.size(); ++index) {
    FT_CHECK_EQ(static_cast<unsigned>(actual[index]), static_cast<unsigned>(expected[index]));
  }
  FT_CHECK_EQ(fdigest::to_hex(actual), std::string(hex));
}

/// The byte sequence of a 64-character lowercase hexadecimal digest.
std::array<std::uint8_t, fdigest::sha256_bytes> bytes_from_hex(const char* hex) {
  std::array<std::uint8_t, fdigest::sha256_bytes> out{};
  for (std::size_t index = 0; index < out.size(); ++index) {
    const char high = hex[index * 2];
    const char low = hex[index * 2 + 1];
    const auto value_of = [](char character) -> unsigned {
      if (character >= '0' && character <= '9') {
        return static_cast<unsigned>(character - '0');
      }
      return static_cast<unsigned>(character - 'a') + 10u;
    };
    out[index] = static_cast<std::uint8_t>((value_of(high) << 4) | value_of(low));
  }
  return out;
}

/// A deterministic byte pattern of the requested length.
std::string pattern(std::size_t size) {
  std::string out;
  out.reserve(size);
  for (std::size_t index = 0; index < size; ++index) {
    out.push_back(static_cast<char>((index * 31u + 7u) & 0xFFu));
  }
  return out;
}

bool is_lowercase_hex(const std::string& text) {
  for (const char character : text) {
    const bool digit = character >= '0' && character <= '9';
    const bool lower = character >= 'a' && character <= 'f';
    if (!digit && !lower) {
      return false;
    }
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Published vectors
// ---------------------------------------------------------------------------

FT_TEST(digest, sha256_of_the_empty_string) {
  constexpr const char* kHex = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  const std::array<std::uint8_t, fdigest::sha256_bytes> expected = {
      0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
      0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};

  const Digest actual = fdigest::sha256("");
  check_digest(actual, expected, kHex);
  FT_CHECK_EQ(fdigest::sha256_hex(""), std::string(kHex));
  FT_CHECK_EQ(fdigest::sha256(""), fdigest::sha256("", 0));
}

FT_TEST(digest, sha256_of_abc) {
  constexpr const char* kHex = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  const std::array<std::uint8_t, fdigest::sha256_bytes> expected = {
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
      0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};

  const Digest actual = fdigest::sha256("abc");
  check_digest(actual, expected, kHex);
  FT_CHECK_EQ(fdigest::sha256_hex("abc"), std::string(kHex));
  FT_CHECK_EQ(actual, bytes_from_hex(kHex));
}

FT_TEST(digest, sha256_of_the_fifty_six_byte_message) {
  constexpr const char* kMessage = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  constexpr const char* kHex = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
  const std::array<std::uint8_t, fdigest::sha256_bytes> expected = {
      0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
      0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1};

  const std::string message(kMessage);
  FT_CHECK_EQ(message.size(), std::size_t{56});
  const Digest actual = fdigest::sha256(message);
  check_digest(actual, expected, kHex);
  FT_CHECK_EQ(fdigest::sha256_hex(message), std::string(kHex));
  FT_CHECK_EQ(actual, bytes_from_hex(kHex));
}

FT_TEST(digest, sha256_of_one_million_a_characters) {
  constexpr const char* kHex = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
  const std::array<std::uint8_t, fdigest::sha256_bytes> expected = {
      0xcd, 0xc7, 0x6e, 0x5c, 0x99, 0x14, 0xfb, 0x92, 0x81, 0xa1, 0xc7, 0xe2, 0x84, 0xd7, 0x3e, 0x67,
      0xf1, 0x80, 0x9a, 0x48, 0xa4, 0x97, 0x20, 0x0e, 0x04, 0x6d, 0x39, 0xcc, 0xc7, 0x11, 0x2c, 0xd0};

  const std::string million_a(1000000u, 'a');
  const Digest actual = fdigest::sha256(million_a);
  check_digest(actual, expected, kHex);
  FT_CHECK_EQ(fdigest::sha256_hex(million_a), std::string(kHex));
  FT_CHECK_EQ(actual, bytes_from_hex(kHex));

  // The same message fed one byte at a time.
  fdigest::Sha256Builder builder;
  for (const char character : million_a) {
    builder.update(&character, 1);
  }
  FT_CHECK_EQ(fdigest::to_hex(builder.finalize()), std::string(kHex));
}

// ---------------------------------------------------------------------------
// Incremental hashing at every block boundary
// ---------------------------------------------------------------------------

FT_TEST(digest, incremental_matches_one_shot_across_block_boundaries) {
  const std::array<std::size_t, 10> sizes = {0, 1, 55, 56, 63, 64, 65, 127, 128, 129};

  for (const std::size_t size : sizes) {
    const std::string data = pattern(size);
    const Digest one_shot = fdigest::sha256(data);

    fdigest::Sha256Builder byte_at_a_time;
    for (const char character : data) {
      byte_at_a_time.update(&character, 1);
    }
    FT_CHECK_EQ(fdigest::to_hex(byte_at_a_time.finalize()), fdigest::to_hex(one_shot));

    fdigest::Sha256Builder whole;
    whole.update(data);
    FT_CHECK_EQ(fdigest::to_hex(whole.finalize()), fdigest::to_hex(one_shot));
  }
}

FT_TEST(digest, one_byte_at_a_time_matches_one_shot_for_many_sizes) {
  for (std::size_t size = 0; size <= 200; ++size) {
    const std::string data = pattern(size);

    fdigest::Sha256Builder builder;
    for (const char character : data) {
      builder.update(&character, 1);
    }
    FT_CHECK_EQ(fdigest::to_hex(builder.finalize()), fdigest::sha256_hex(data));
  }
}

FT_TEST(digest, every_two_way_split_matches_one_shot) {
  const std::string data = pattern(300);
  const std::string expected = fdigest::sha256_hex(data);

  for (std::size_t split = 0; split <= data.size(); ++split) {
    fdigest::Sha256Builder builder;
    builder.update(std::string_view(data).substr(0, split));
    builder.update(std::string_view(data).substr(split));
    FT_CHECK_EQ(fdigest::to_hex(builder.finalize()), expected);
  }
}

FT_TEST(digest, update_of_nothing_is_a_no_op) {
  fdigest::Sha256Builder builder;
  builder.update(nullptr, 0);
  builder.update("abc");
  builder.update(nullptr, 0);
  FT_CHECK_EQ(fdigest::to_hex(builder.finalize()), fdigest::sha256_hex("abc"));
}

FT_TEST(digest, reset_returns_the_builder_to_its_initial_state) {
  fdigest::Sha256Builder builder;
  builder.update("some earlier content that must not survive a reset");
  FT_CHECK_NE(fdigest::to_hex(builder.finalize()), fdigest::sha256_hex("abc"));

  builder.reset();
  builder.update("abc");
  FT_CHECK_EQ(fdigest::to_hex(builder.finalize()), fdigest::sha256_hex("abc"));
}

FT_TEST(digest, finalize_does_not_consume_the_builder) {
  fdigest::Sha256Builder builder;
  builder.update("abc");
  const Digest first = builder.finalize();
  const Digest second = builder.finalize();
  FT_CHECK_EQ(fdigest::to_hex(first), fdigest::to_hex(second));

  builder.update("def");
  const Digest extended = builder.finalize();
  FT_CHECK_EQ(fdigest::to_hex(extended), fdigest::sha256_hex("abcdef"));
}

// ---------------------------------------------------------------------------
// Rendering and pointer overloads
// ---------------------------------------------------------------------------

FT_TEST(digest, to_hex_and_sha256_hex_agree) {
  const std::vector<std::string> messages = {"", "abc", "a", std::string(55, 'x'), std::string(64, 'y'),
                                             std::string(1000, 'z')};

  for (const std::string& message : messages) {
    FT_CHECK_EQ(fdigest::to_hex(fdigest::sha256(message)), fdigest::sha256_hex(message));
  }
}

FT_TEST(digest, hex_is_lowercase_and_sixty_four_characters) {
  const std::vector<std::string> messages = {"", "abc", "Facility Capacity", std::string(257, 'q')};

  for (const std::string& message : messages) {
    const std::string hex = fdigest::sha256_hex(message);
    FT_CHECK_EQ(hex.size(), std::size_t{64});
    FT_CHECK(is_lowercase_hex(hex));

    const Digest raw = fdigest::sha256(message);
    const std::string rendered = fdigest::to_hex(raw);
    FT_CHECK_EQ(rendered, hex);
    FT_CHECK_EQ(rendered.size(), std::size_t{64});
  }
}

FT_TEST(digest, to_hex_renders_each_nibble) {
  Digest value{};
  value[0] = 0x00;
  value[1] = 0x0F;
  value[2] = 0xF0;
  value[31] = 0xFF;

  const std::string hex = fdigest::to_hex(value);
  FT_CHECK_EQ(hex.size(), std::size_t{64});
  FT_CHECK_EQ(hex.substr(0, 6), "000ff0");
  FT_CHECK_EQ(hex.substr(62, 2), "ff");
  FT_CHECK(is_lowercase_hex(hex));
}

FT_TEST(digest, pointer_overload_matches_the_string_view_overload) {
  const std::string text = "the exact same bytes either way";
  FT_CHECK_EQ(fdigest::sha256(text.data(), text.size()), fdigest::sha256(std::string_view(text)));

  const Digest from_literal = fdigest::sha256("abc", 3);
  FT_CHECK_EQ(fdigest::to_hex(from_literal), fdigest::sha256_hex("abc"));

  const std::string with_nul("ab\0cd", 5);
  FT_CHECK_EQ(with_nul.size(), std::size_t{5});
  FT_CHECK_EQ(fdigest::sha256(with_nul.data(), with_nul.size()), fdigest::sha256(std::string_view(with_nul)));
  FT_CHECK(fdigest::sha256(with_nul) != fdigest::sha256(std::string_view("ab")));
}

// ---------------------------------------------------------------------------
// Randomized
// ---------------------------------------------------------------------------

FT_TEST(digest, random_byte_strings_hash_the_same_incrementally) {
  ftest::Rng rng(ftest::current_seed());
  constexpr int kIterations = 400;

  for (int iteration = 0; iteration < kIterations; ++iteration) {
    const std::size_t size = static_cast<std::size_t>(rng.below(700));
    std::string data;
    data.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
      data.push_back(static_cast<char>(rng.below(256)));
    }

    const std::string expected = fdigest::sha256_hex(data);

    // Split the message into random chunks.
    fdigest::Sha256Builder chunked;
    std::size_t offset = 0;
    while (offset < data.size()) {
      const std::size_t remaining = data.size() - offset;
      const std::size_t chunk = 1u + static_cast<std::size_t>(rng.below(remaining));
      chunked.update(data.data() + offset, chunk);
      offset += chunk;
    }
    if (fdigest::to_hex(chunked.finalize()) != expected) {
      ftest::set_case_context("iteration=" + std::to_string(iteration) + " size=" + std::to_string(size));
      FT_FAIL("chunked SHA-256 disagreed with the one-shot digest");
      return;
    }

    // Feed the same message one byte at a time.
    fdigest::Sha256Builder byte_at_a_time;
    for (const char character : data) {
      byte_at_a_time.update(&character, 1);
    }
    if (fdigest::to_hex(byte_at_a_time.finalize()) != expected) {
      ftest::set_case_context("iteration=" + std::to_string(iteration) + " size=" + std::to_string(size));
      FT_FAIL("byte-at-a-time SHA-256 disagreed with the one-shot digest");
      return;
    }

    if (fdigest::sha256(data.data(), data.size()) != fdigest::sha256(std::string_view(data))) {
      ftest::set_case_context("iteration=" + std::to_string(iteration) + " size=" + std::to_string(size));
      FT_FAIL("the pointer and string-view overloads disagreed");
      return;
    }
  }
}

FT_TEST(digest, random_messages_of_the_same_length_usually_differ) {
  ftest::Rng rng(ftest::current_seed());
  constexpr int kIterations = 200;

  for (int iteration = 0; iteration < kIterations; ++iteration) {
    const std::size_t size = 1u + static_cast<std::size_t>(rng.below(64));
    std::string first;
    std::string second;
    first.reserve(size);
    second.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
      first.push_back(static_cast<char>(rng.below(256)));
      second.push_back(static_cast<char>(rng.below(256)));
    }

    const std::string first_hex = fdigest::sha256_hex(first);
    const std::string second_hex = fdigest::sha256_hex(second);
    if (first == second) {
      FT_CHECK_EQ(first_hex, second_hex);
      continue;
    }
    if (first_hex == second_hex) {
      ftest::set_case_context("iteration=" + std::to_string(iteration) + " size=" + std::to_string(size));
      FT_FAIL("two different byte strings produced the same SHA-256 digest");
      return;
    }
  }
}
