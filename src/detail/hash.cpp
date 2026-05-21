#include "detail/hash.hpp"

#include "detail/error.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <openssl/evp.h>

namespace memix::detail {

namespace {

constexpr std::size_t kSha256DigestBytes = 32;
constexpr std::size_t kSha256HexDigestLength = kSha256DigestBytes * 2;
constexpr unsigned int kHexShift = 4;
constexpr unsigned int kHexMask = 0x0FU;

} // namespace

std::string hex_encode(std::span<const unsigned char> bytes) {
  constexpr std::array<char, 16> kHex = {
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
  };
  std::string output;
  output.reserve(bytes.size() * 2);
  for (const unsigned char byte : bytes) {
    const auto value = static_cast<unsigned int>(byte);
    output.push_back(kHex.at((value >> kHexShift) & kHexMask));
    output.push_back(kHex.at(value & kHexMask));
  }
  return output;
}

CommonResult<std::string> sha256_text(std::string_view text) {
  using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  const DigestContext digest_context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!digest_context) {
    return common_error("failed to allocate sha256 context");
  }
  if (EVP_DigestInit_ex(digest_context.get(), EVP_sha256(), nullptr) != 1) {
    return common_error("failed to initialize sha256 context");
  }
  if (!text.empty()
      && EVP_DigestUpdate(digest_context.get(), text.data(), text.size())
             != 1) {
    return common_error("failed to update sha256 digest");
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_length = 0;
  if (EVP_DigestFinal_ex(digest_context.get(), digest.data(), &digest_length)
      != 1) {
    return common_error("failed to finalize sha256 digest");
  }
  if (digest_length != kSha256DigestBytes) {
    return common_error("sha256 digest produced an unexpected length");
  }

  return hex_encode(
      std::span<const unsigned char>{digest.data(), digest_length});
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
