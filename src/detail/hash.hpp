#pragma once

#include "detail/error.hpp"

#include <span>
#include <string>
#include <string_view>

namespace memix::detail {

std::string hex_encode(std::span<const unsigned char> bytes);
CommonResult<std::string> sha256_text(std::string_view text);
bool is_lower_hex_sha256(std::string_view value) noexcept;
bool is_lower_hex_prefix(std::string_view value) noexcept;

} // namespace memix::detail
