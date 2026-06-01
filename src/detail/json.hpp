#pragma once

#include "detail/error.hpp"

#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <simdjson.h>

namespace memix::detail {

class JsonDocument {
public:
  JsonDocument(JsonDocument &&) noexcept = default;
  JsonDocument &operator=(JsonDocument &&) noexcept = default;
  JsonDocument(const JsonDocument &) = delete;
  JsonDocument &operator=(const JsonDocument &) = delete;
  ~JsonDocument();

  [[nodiscard]] simdjson::dom::element root() const noexcept;

  // Precondition: the root value is an object; parse_json_object verifies this.
  [[nodiscard]] simdjson::dom::object root_object() const noexcept;

private:
  friend CommonResult<JsonDocument> parse_json(std::string_view text);

  explicit JsonDocument(std::string_view text);
  [[nodiscard]] simdjson::error_code parse_root();

  simdjson::padded_string input_;
  std::unique_ptr<simdjson::dom::parser> parser_;
  simdjson::dom::element root_;
};

std::string simdjson_error_text(simdjson::error_code error);

CommonResult<JsonDocument> parse_json(std::string_view text);

CommonResult<JsonDocument> parse_json_object(std::string_view text);

CommonResult<std::string>
json_from_builder(const simdjson::builder::string_builder &builder);

template <typename Value>
[[nodiscard]] constexpr std::string_view json_expected_type() noexcept {
  using Type = std::remove_cvref_t<Value>;
  if constexpr (std::is_same_v<Type, simdjson::dom::element>) {
    return "present";
  } else if constexpr (std::is_same_v<Type, std::string_view>) {
    return "a string";
  } else if constexpr (std::is_same_v<Type, std::uint64_t>) {
    return "an unsigned integer";
  } else if constexpr (std::is_same_v<Type, simdjson::dom::array>) {
    return "an array";
  } else if constexpr (std::is_same_v<Type, simdjson::dom::object>) {
    return "an object";
  } else {
    static_assert(std::is_same_v<Type, void>, "unsupported JSON field type");
  }
}

class JsonObjectReader {
public:
  explicit JsonObjectReader(simdjson::dom::object fields) noexcept;

  [[nodiscard]] simdjson::dom::object object() const noexcept;

  [[nodiscard]] CommonResult<simdjson::dom::element>
  require_field(std::string_view key) const;

  [[nodiscard]]
  CommonResult<std::string> require_string(std::string_view key) const;

  [[nodiscard]]
  CommonResult<std::uint64_t> require_uint64(std::string_view key) const;

  [[nodiscard]] CommonResult<simdjson::dom::array>
  require_array(std::string_view key) const;

  [[nodiscard]] CommonResult<simdjson::dom::object>
  require_object(std::string_view key) const;

  [[nodiscard]] CommonResult<std::optional<std::string>>
  optional_string(std::string_view key) const;

  [[nodiscard]] CommonResult<std::optional<simdjson::dom::array>>
  optional_array(std::string_view key) const;

  [[nodiscard]] CommonResult<std::vector<std::string>>
  string_array(std::string_view key) const;

private:
  template <typename Value>
  [[nodiscard]] CommonResult<Value> require_as(std::string_view key) const {
    Value parsed{};
    const simdjson::error_code error = fields_.at_key(key).get(parsed);
    if (error != simdjson::SUCCESS) {
      return common_error(std::format("JSON field `{}` must be {}: {}", key,
                                      json_expected_type<Value>(),
                                      simdjson_error_text(error)));
    }
    return parsed;
  }

  template <typename Value>
  [[nodiscard]] CommonResult<std::optional<Value>>
  optional_as(std::string_view key) const {
    Value parsed{};
    const simdjson::error_code error = fields_.at_key(key).get(parsed);
    if (error == simdjson::NO_SUCH_FIELD) {
      return std::optional<Value>{};
    }
    if (error != simdjson::SUCCESS) {
      return common_error(std::format("JSON field `{}` must be {}: {}", key,
                                      json_expected_type<Value>(),
                                      simdjson_error_text(error)));
    }
    return std::optional<Value>{parsed};
  }

  simdjson::dom::object fields_;
};

template <typename Value>
CommonResult<Value>
json_value_as(simdjson::dom::element value) {
  Value parsed{};
  const simdjson::error_code error = value.get(parsed);
  if (error != simdjson::SUCCESS) {
    return common_error(std::format("JSON value must be {}: {}",
                                    json_expected_type<Value>(),
                                    simdjson_error_text(error)));
  }
  return parsed;
}

CommonResult<simdjson::dom::object>
json_object_from_element(simdjson::dom::element value);

} // namespace memix::detail
