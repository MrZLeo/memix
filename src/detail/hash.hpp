#pragma once

#include "detail/error.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace memix::detail {

class Sha256 {
public:
  static constexpr std::size_t kDigestBytes = 32;
  static constexpr std::size_t kBlockBytes = 64;
  static constexpr std::size_t kStateWords = 8;

  using Digest = std::array<unsigned char, kDigestBytes>;

  Sha256() { init(); }

  void init() noexcept;
  void update(std::span<const std::byte> bytes);
  void update(std::string_view text);
  [[nodiscard]] Digest final();
  [[nodiscard]] Digest result() const;
  [[nodiscard]] std::string final_hex();

private:
  void add_uncounted(std::span<const std::byte> bytes);
  void pad();
  void process_block(std::span<const std::byte> block);

  std::array<std::uint32_t, kStateWords> state_{};
  std::array<std::byte, kBlockBytes> buffer_{};
  std::uint64_t total_bytes_ = 0;
  std::size_t buffer_size_ = 0;
};

std::string hex_encode(std::span<const unsigned char> bytes);
CommonResult<std::string> sha256_file(const std::filesystem::path &path);
std::string sha256_text(std::string_view text);

inline constexpr std::size_t kSha256HexDigestLength = Sha256::kDigestBytes * 2;

[[nodiscard]] constexpr bool is_lower_hex_digit(char character) noexcept {
  return (character >= '0' && character <= '9')
      || (character >= 'a' && character <= 'f');
}

[[nodiscard]] constexpr bool
is_lower_hex_sha256(std::string_view value) noexcept {
  if (value.size() != kSha256HexDigestLength) {
    return false;
  }
  return std::ranges::all_of(value, is_lower_hex_digit);
}

[[nodiscard]] constexpr bool
is_lower_hex_prefix(std::string_view value) noexcept {
  if (value.empty() || value.size() > kSha256HexDigestLength) {
    return false;
  }
  return std::ranges::all_of(value, is_lower_hex_digit);
}

} // namespace memix::detail
