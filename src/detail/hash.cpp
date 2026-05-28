#include "detail/hash.hpp"
#include "detail/error.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>

namespace memix::detail {

namespace {

constexpr std::size_t kSha256HexDigestLength = Sha256::kDigestBytes * 2;
constexpr std::size_t kHashReadBufferBytes = 8192;
constexpr std::size_t kWordBytes = 4;
constexpr std::size_t kLengthOffsetBytes = 56;
constexpr std::size_t kLengthBytes = 8;
constexpr unsigned int kByteBits = 8;
constexpr std::uint32_t kByteMask = 0xFFU;
constexpr std::byte kPaddingStart{0x80U};
constexpr unsigned int kWordByte0Shift = 24;
constexpr unsigned int kWordByte1Shift = 16;
constexpr unsigned int kWordByte2Shift = 8;
constexpr unsigned int kBigSigma0Rotate0 = 2;
constexpr unsigned int kBigSigma0Rotate1 = 13;
constexpr unsigned int kBigSigma0Rotate2 = 22;
constexpr unsigned int kBigSigma1Rotate0 = 6;
constexpr unsigned int kBigSigma1Rotate1 = 11;
constexpr unsigned int kBigSigma1Rotate2 = 25;
constexpr unsigned int kSmallSigma0Rotate0 = 7;
constexpr unsigned int kSmallSigma0Rotate1 = 18;
constexpr unsigned int kSmallSigma0Shift = 3;
constexpr unsigned int kSmallSigma1Rotate0 = 17;
constexpr unsigned int kSmallSigma1Rotate1 = 19;
constexpr unsigned int kSmallSigma1Shift = 10;
constexpr unsigned int kHexShift = 4;
constexpr unsigned int kHexMask = 0x0FU;

constexpr std::array<std::uint32_t, Sha256::kStateWords> kInitialState = {
    0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
    0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U,
};

std::uint32_t byte_value(std::byte byte) noexcept {
  return std::to_integer<std::uint32_t>(byte);
}

std::uint32_t choose(std::uint32_t x, std::uint32_t y,
                     std::uint32_t z) noexcept {
  return (x & y) ^ (~x & z);
}

std::uint32_t majority(std::uint32_t x, std::uint32_t y,
                       std::uint32_t z) noexcept {
  return (x & y) ^ (x & z) ^ (y & z);
}

std::uint32_t big_sigma0(std::uint32_t value) noexcept {
  return std::rotr(value, kBigSigma0Rotate0)
       ^ std::rotr(value, kBigSigma0Rotate1)
       ^ std::rotr(value, kBigSigma0Rotate2);
}

std::uint32_t big_sigma1(std::uint32_t value) noexcept {
  return std::rotr(value, kBigSigma1Rotate0)
       ^ std::rotr(value, kBigSigma1Rotate1)
       ^ std::rotr(value, kBigSigma1Rotate2);
}

std::uint32_t small_sigma0(std::uint32_t value) noexcept {
  return std::rotr(value, kSmallSigma0Rotate0)
       ^ std::rotr(value, kSmallSigma0Rotate1)
       ^ (value >> kSmallSigma0Shift);
}

std::uint32_t small_sigma1(std::uint32_t value) noexcept {
  return std::rotr(value, kSmallSigma1Rotate0)
       ^ std::rotr(value, kSmallSigma1Rotate1)
       ^ (value >> kSmallSigma1Shift);
}

std::uint32_t block_word(std::span<const std::byte> block,
                         std::size_t word_index) noexcept {
  const std::size_t offset = word_index * kWordBytes;
  std::array<std::byte, kWordBytes> bytes{};
  std::ranges::copy(block.subspan(offset, kWordBytes), bytes.begin());
  return (byte_value(bytes[0]) << kWordByte0Shift)
       | (byte_value(bytes[1]) << kWordByte1Shift)
       | (byte_value(bytes[2]) << kWordByte2Shift)
       | byte_value(bytes[3]);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void f_expand(std::uint32_t &a, std::uint32_t &b, std::uint32_t &c,
              std::uint32_t &d, std::uint32_t &e, std::uint32_t &f,
              std::uint32_t &g, std::uint32_t &h, std::uint32_t &w,
              std::uint32_t s1_source, std::uint32_t add_source,
              std::uint32_t s0_source, std::uint32_t k) noexcept {
  h += big_sigma1(e) + choose(e, f, g) + w + k;
  d += h;
  h += big_sigma0(a) + majority(a, b, c);
  w += small_sigma1(s1_source) + add_source + small_sigma0(s0_source);
}
// NOLINTEND(bugprone-easily-swappable-parameters)

} // namespace

void Sha256::init() noexcept {
  state_ = kInitialState;
  buffer_ = {};
  total_bytes_ = 0;
  buffer_size_ = 0;
}

void Sha256::add_uncounted(std::span<const std::byte> bytes) {
  if (buffer_size_ > 0) {
    const std::size_t copied =
        std::min(bytes.size(), kBlockBytes - buffer_size_);
    const std::span destination =
        std::span{buffer_}.subspan(buffer_size_, copied);
    std::ranges::copy(bytes.first(copied), destination.begin());
    buffer_size_ += copied;
    bytes = bytes.subspan(copied);

    if (buffer_size_ < kBlockBytes)
      return;

    process_block(buffer_);
    buffer_size_ = 0;
  }

  while (bytes.size() >= kBlockBytes) {
    process_block(bytes.first(kBlockBytes));
    bytes = bytes.subspan(kBlockBytes);
  }

  std::ranges::copy(bytes, buffer_.begin());
  buffer_size_ = bytes.size();
}

void Sha256::update(std::span<const std::byte> bytes) {
  total_bytes_ += bytes.size();
  add_uncounted(bytes);
}

void Sha256::update(std::string_view text) {
  update(std::as_bytes(std::span{text.data(), text.size()}));
}

void Sha256::pad() {
  const std::uint64_t bit_count = total_bytes_ * kByteBits;

  std::array<std::byte, kBlockBytes> padding{};
  padding.front() = kPaddingStart;
  const std::size_t padding_size =
      buffer_size_ < kLengthOffsetBytes
          ? kLengthOffsetBytes - buffer_size_
          : kBlockBytes + kLengthOffsetBytes - buffer_size_;
  add_uncounted({padding.data(), padding_size});

  const std::array<std::byte, kLengthBytes> length = {
      std::byte{static_cast<unsigned char>((bit_count >> 56U) & kByteMask)},
      std::byte{static_cast<unsigned char>((bit_count >> 48U) & kByteMask)},
      std::byte{static_cast<unsigned char>((bit_count >> 40U) & kByteMask)},
      std::byte{static_cast<unsigned char>((bit_count >> 32U) & kByteMask)},
      std::byte{static_cast<unsigned char>((bit_count >> 24U) & kByteMask)},
      std::byte{static_cast<unsigned char>((bit_count >> 16U) & kByteMask)},
      std::byte{static_cast<unsigned char>((bit_count >> 8U) & kByteMask)},
      std::byte{static_cast<unsigned char>(bit_count & kByteMask)},
  };
  add_uncounted(length);
}

Sha256::Digest Sha256::final() {
  pad();

  Digest digest{};
  for (std::size_t word_index = 0; word_index < state_.size(); ++word_index) {
    const std::uint32_t word = state_[word_index];
    const std::size_t byte_index = word_index * kWordBytes;
    digest[byte_index] =
        static_cast<unsigned char>((word >> kWordByte0Shift) & kByteMask);
    digest[byte_index + 1] =
        static_cast<unsigned char>((word >> kWordByte1Shift) & kByteMask);
    digest[byte_index + 2] =
        static_cast<unsigned char>((word >> kWordByte2Shift) & kByteMask);
    digest[byte_index + 3] = static_cast<unsigned char>(word & kByteMask);
  }
  return digest;
}

Sha256::Digest Sha256::result() const {
  Sha256 copy = *this;
  return copy.final();
}

std::string Sha256::final_hex() {
  const Digest digest = final();
  return hex_encode(digest);
}

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers,readability-function-size)
void Sha256::process_block(std::span<const std::byte> block) {
  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  std::uint32_t w00 = block_word(block, 0);
  std::uint32_t w01 = block_word(block, 1);
  std::uint32_t w02 = block_word(block, 2);
  std::uint32_t w03 = block_word(block, 3);
  std::uint32_t w04 = block_word(block, 4);
  std::uint32_t w05 = block_word(block, 5);
  std::uint32_t w06 = block_word(block, 6);
  std::uint32_t w07 = block_word(block, 7);
  std::uint32_t w08 = block_word(block, 8);
  std::uint32_t w09 = block_word(block, 9);
  std::uint32_t w10 = block_word(block, 10);
  std::uint32_t w11 = block_word(block, 11);
  std::uint32_t w12 = block_word(block, 12);
  std::uint32_t w13 = block_word(block, 13);
  std::uint32_t w14 = block_word(block, 14);
  std::uint32_t w15 = block_word(block, 15);

  f_expand(a, b, c, d, e, f, g, h, w00, w14, w09, w01, 0x428A2F98U);
  f_expand(h, a, b, c, d, e, f, g, w01, w15, w10, w02, 0x71374491U);
  f_expand(g, h, a, b, c, d, e, f, w02, w00, w11, w03, 0xB5C0FBCFU);
  f_expand(f, g, h, a, b, c, d, e, w03, w01, w12, w04, 0xE9B5DBA5U);
  f_expand(e, f, g, h, a, b, c, d, w04, w02, w13, w05, 0x3956C25BU);
  f_expand(d, e, f, g, h, a, b, c, w05, w03, w14, w06, 0x59F111F1U);
  f_expand(c, d, e, f, g, h, a, b, w06, w04, w15, w07, 0x923F82A4U);
  f_expand(b, c, d, e, f, g, h, a, w07, w05, w00, w08, 0xAB1C5ED5U);
  f_expand(a, b, c, d, e, f, g, h, w08, w06, w01, w09, 0xD807AA98U);
  f_expand(h, a, b, c, d, e, f, g, w09, w07, w02, w10, 0x12835B01U);
  f_expand(g, h, a, b, c, d, e, f, w10, w08, w03, w11, 0x243185BEU);
  f_expand(f, g, h, a, b, c, d, e, w11, w09, w04, w12, 0x550C7DC3U);
  f_expand(e, f, g, h, a, b, c, d, w12, w10, w05, w13, 0x72BE5D74U);
  f_expand(d, e, f, g, h, a, b, c, w13, w11, w06, w14, 0x80DEB1FEU);
  f_expand(c, d, e, f, g, h, a, b, w14, w12, w07, w15, 0x9BDC06A7U);
  f_expand(b, c, d, e, f, g, h, a, w15, w13, w08, w00, 0xC19BF174U);

  f_expand(a, b, c, d, e, f, g, h, w00, w14, w09, w01, 0xE49B69C1U);
  f_expand(h, a, b, c, d, e, f, g, w01, w15, w10, w02, 0xEFBE4786U);
  f_expand(g, h, a, b, c, d, e, f, w02, w00, w11, w03, 0x0FC19DC6U);
  f_expand(f, g, h, a, b, c, d, e, w03, w01, w12, w04, 0x240CA1CCU);
  f_expand(e, f, g, h, a, b, c, d, w04, w02, w13, w05, 0x2DE92C6FU);
  f_expand(d, e, f, g, h, a, b, c, w05, w03, w14, w06, 0x4A7484AAU);
  f_expand(c, d, e, f, g, h, a, b, w06, w04, w15, w07, 0x5CB0A9DCU);
  f_expand(b, c, d, e, f, g, h, a, w07, w05, w00, w08, 0x76F988DAU);
  f_expand(a, b, c, d, e, f, g, h, w08, w06, w01, w09, 0x983E5152U);
  f_expand(h, a, b, c, d, e, f, g, w09, w07, w02, w10, 0xA831C66DU);
  f_expand(g, h, a, b, c, d, e, f, w10, w08, w03, w11, 0xB00327C8U);
  f_expand(f, g, h, a, b, c, d, e, w11, w09, w04, w12, 0xBF597FC7U);
  f_expand(e, f, g, h, a, b, c, d, w12, w10, w05, w13, 0xC6E00BF3U);
  f_expand(d, e, f, g, h, a, b, c, w13, w11, w06, w14, 0xD5A79147U);
  f_expand(c, d, e, f, g, h, a, b, w14, w12, w07, w15, 0x06CA6351U);
  f_expand(b, c, d, e, f, g, h, a, w15, w13, w08, w00, 0x14292967U);

  f_expand(a, b, c, d, e, f, g, h, w00, w14, w09, w01, 0x27B70A85U);
  f_expand(h, a, b, c, d, e, f, g, w01, w15, w10, w02, 0x2E1B2138U);
  f_expand(g, h, a, b, c, d, e, f, w02, w00, w11, w03, 0x4D2C6DFCU);
  f_expand(f, g, h, a, b, c, d, e, w03, w01, w12, w04, 0x53380D13U);
  f_expand(e, f, g, h, a, b, c, d, w04, w02, w13, w05, 0x650A7354U);
  f_expand(d, e, f, g, h, a, b, c, w05, w03, w14, w06, 0x766A0ABBU);
  f_expand(c, d, e, f, g, h, a, b, w06, w04, w15, w07, 0x81C2C92EU);
  f_expand(b, c, d, e, f, g, h, a, w07, w05, w00, w08, 0x92722C85U);
  f_expand(a, b, c, d, e, f, g, h, w08, w06, w01, w09, 0xA2BFE8A1U);
  f_expand(h, a, b, c, d, e, f, g, w09, w07, w02, w10, 0xA81A664BU);
  f_expand(g, h, a, b, c, d, e, f, w10, w08, w03, w11, 0xC24B8B70U);
  f_expand(f, g, h, a, b, c, d, e, w11, w09, w04, w12, 0xC76C51A3U);
  f_expand(e, f, g, h, a, b, c, d, w12, w10, w05, w13, 0xD192E819U);
  f_expand(d, e, f, g, h, a, b, c, w13, w11, w06, w14, 0xD6990624U);
  f_expand(c, d, e, f, g, h, a, b, w14, w12, w07, w15, 0xF40E3585U);
  f_expand(b, c, d, e, f, g, h, a, w15, w13, w08, w00, 0x106AA070U);

  f_expand(a, b, c, d, e, f, g, h, w00, w14, w09, w01, 0x19A4C116U);
  f_expand(h, a, b, c, d, e, f, g, w01, w15, w10, w02, 0x1E376C08U);
  f_expand(g, h, a, b, c, d, e, f, w02, w00, w11, w03, 0x2748774CU);
  f_expand(f, g, h, a, b, c, d, e, w03, w01, w12, w04, 0x34B0BCB5U);
  f_expand(e, f, g, h, a, b, c, d, w04, w02, w13, w05, 0x391C0CB3U);
  f_expand(d, e, f, g, h, a, b, c, w05, w03, w14, w06, 0x4ED8AA4AU);
  f_expand(c, d, e, f, g, h, a, b, w06, w04, w15, w07, 0x5B9CCA4FU);
  f_expand(b, c, d, e, f, g, h, a, w07, w05, w00, w08, 0x682E6FF3U);
  f_expand(a, b, c, d, e, f, g, h, w08, w06, w01, w09, 0x748F82EEU);
  f_expand(h, a, b, c, d, e, f, g, w09, w07, w02, w10, 0x78A5636FU);
  f_expand(g, h, a, b, c, d, e, f, w10, w08, w03, w11, 0x84C87814U);
  f_expand(f, g, h, a, b, c, d, e, w11, w09, w04, w12, 0x8CC70208U);
  f_expand(e, f, g, h, a, b, c, d, w12, w10, w05, w13, 0x90BEFFFAU);
  f_expand(d, e, f, g, h, a, b, c, w13, w11, w06, w14, 0xA4506CEBU);
  f_expand(c, d, e, f, g, h, a, b, w14, w12, w07, w15, 0xBEF9A3F7U);
  f_expand(b, c, d, e, f, g, h, a, w15, w13, w08, w00, 0xC67178F2U);

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers,readability-function-size)

std::string hex_encode(std::span<const unsigned char> bytes) {
  constexpr std::array<char, 16> kHex = {
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
  };
  std::string output;
  output.reserve(bytes.size() * 2);
  for (const unsigned char byte : bytes) {
    const auto value = static_cast<unsigned int>(byte);
    output.push_back(kHex[(value >> kHexShift) & kHexMask]);
    output.push_back(kHex[value & kHexMask]);
  }
  return output;
}

CommonResult<std::string> sha256_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return common_error(
        std::format("failed to open `{}` for hashing", path.string()));
  }

  Sha256 sha256;
  std::array<char, kHashReadBufferBytes> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize read_count = input.gcount();
    if (read_count > 0) {
      sha256.update(std::as_bytes(
          std::span{buffer.data(), static_cast<std::size_t>(read_count)}));
    }
  }

  if (input.bad()) {
    return common_error(
        std::format("failed while reading `{}`", path.string()));
  }

  return sha256.final_hex();
}

CommonResult<std::string> sha256_text(std::string_view text) {
  Sha256 sha256;
  sha256.update(text);
  return sha256.final_hex();
}

bool is_lower_hex_sha256(std::string_view value) noexcept {
  if (value.size() != kSha256HexDigestLength) {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    return (character >= '0' && character <= '9')
        || (character >= 'a' && character <= 'f');
  });
}

bool is_lower_hex_prefix(std::string_view value) noexcept {
  if (value.empty() || value.size() > kSha256HexDigestLength) {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    return (character >= '0' && character <= '9')
        || (character >= 'a' && character <= 'f');
  });
}

} // namespace memix::detail
