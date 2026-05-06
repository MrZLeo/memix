#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

#include <magic_enum/magic_enum.hpp>

namespace memix::detail {
namespace enum_text_backend {

// Keep backend-specific enum discovery behind this namespace. The current
// implementation uses magic_enum; a std::meta backend can replace it here.
template <typename E>
concept Enum = std::is_enum_v<E>;

constexpr char ascii_lower(char character) noexcept {
  if (character >= 'A' && character <= 'Z') {
    return static_cast<char>(character - 'A' + 'a');
  }
  return character;
}

template <std::size_t Capacity>
class Token {
public:
  constexpr Token() = default;

  explicit constexpr Token(std::string_view name) noexcept
      : size_(name.size()) {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index, cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    for (std::size_t index = 0; index < name.size(); ++index) {
      text_[index] = ascii_lower(name[index]);
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index, cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  }

  [[nodiscard]] constexpr std::string_view view() const noexcept {
    return {text_.data(), size_};
  }

private:
  std::array<char, Capacity + 1> text_{};
  std::size_t size_ = 0;
};

template <Enum E>
consteval std::size_t max_token_size() {
  std::size_t max_size = 0;
  constexpr auto values = magic_enum::enum_values<E>();
  for (const E value : values) {
    const std::string_view name = magic_enum::enum_name(value);
    max_size = std::max(max_size, name.size());
  }
  return max_size;
}

template <Enum E>
consteval auto make_token_table() {
  constexpr auto values = magic_enum::enum_values<E>();
  constexpr std::size_t token_capacity = max_token_size<E>();
  std::array<Token<token_capacity>, values.size()> tokens{};

  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index, cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  for (std::size_t index = 0; index < values.size(); ++index) {
    tokens[index] =
        Token<token_capacity>{magic_enum::enum_name(values[index])};
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index, cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

  return tokens;
}

template <Enum E>
struct EnumTable {
  static constexpr auto values = magic_enum::enum_values<E>();
  static constexpr auto tokens = make_token_table<E>();
};

} // namespace enum_text_backend

template <enum_text_backend::Enum E>
[[nodiscard]] std::string_view enum_token(E value) noexcept {
  constexpr auto &values = enum_text_backend::EnumTable<E>::values;
  constexpr auto &tokens = enum_text_backend::EnumTable<E>::tokens;

  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index, cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (values[index] == value) {
      return tokens[index].view();
    }
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index, cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

  return {};
}

template <enum_text_backend::Enum E>
[[nodiscard]] std::optional<E>
parse_enum_token(std::string_view value) noexcept {
  constexpr auto &values = enum_text_backend::EnumTable<E>::values;
  constexpr auto &tokens = enum_text_backend::EnumTable<E>::tokens;

  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index, cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (tokens[index].view() == value) {
      return values[index];
    }
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index, cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

  return std::nullopt;
}

template <enum_text_backend::Enum E>
[[nodiscard]] std::vector<E> enum_values() {
  constexpr auto &values = enum_text_backend::EnumTable<E>::values;
  return {values.begin(), values.end()};
}

} // namespace memix::detail
