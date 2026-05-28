#pragma once

#include "detail/error.hpp"

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
CommonResult<std::string> sha256_text(std::string_view text);
bool is_lower_hex_sha256(std::string_view value) noexcept;
bool is_lower_hex_prefix(std::string_view value) noexcept;

} // namespace memix::detail
