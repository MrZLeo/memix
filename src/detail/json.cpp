#include "detail/json.hpp"

#include "detail/error.hpp"

#include <cassert>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <simdjson.h>

namespace memix::detail {

std::string simdjson_error_text(simdjson::error_code error) {
  return simdjson::error_message(error);
}

JsonDocument::JsonDocument(std::string_view text)
    : input_(text), parser_(std::make_unique<simdjson::dom::parser>()) {}

JsonDocument::~JsonDocument() = default;

simdjson::error_code JsonDocument::parse_root() {
  return parser_->parse(input_).get(root_);
}

simdjson::dom::element JsonDocument::root() const noexcept { return root_; }

simdjson::dom::object JsonDocument::root_object() const noexcept {
  simdjson::dom::object object;
  [[maybe_unused]] const simdjson::error_code error = root_.get(object);
  // NOLINTNEXTLINE(cert-dcl03-c, hicpp-static-assert, misc-static-assert):
  // runtime error check, not a constant condition
  assert(error == simdjson::SUCCESS);
  return object;
}

CommonResult<JsonDocument> parse_json(std::string_view text) {
  JsonDocument parsed(text);
  const simdjson::error_code error = parsed.parse_root();
  if (error != simdjson::SUCCESS) {
    return common_error(
        std::format("failed to parse JSON: {}", simdjson_error_text(error)));
  }
  return parsed;
}

CommonResult<JsonDocument> parse_json_object(std::string_view text) {
  auto parsed = parse_json(text);
  if (!parsed) {
    return parsed;
  }

  simdjson::dom::object object;
  const simdjson::error_code error = parsed->root().get(object);
  if (error != simdjson::SUCCESS) {
    return common_error(std::format("JSON root must be an object: {}",
                                    simdjson_error_text(error)));
  }
  return parsed;
}

CommonResult<std::string>
json_from_builder(const simdjson::builder::string_builder &builder) {
  if (!builder.validate_unicode())
    return common_error("generated JSON contains invalid UTF-8");

  std::string_view output;
  const simdjson::error_code error = builder.view().get(output);
  if (error != simdjson::SUCCESS) {
    return common_error(
        std::format("failed to build JSON: {}", simdjson_error_text(error)));
  }
  return std::string(output);
}

JsonObjectReader::JsonObjectReader(simdjson::dom::object fields) noexcept
    : fields_(fields) {}

simdjson::dom::object JsonObjectReader::object() const noexcept {
  return fields_;
}

CommonResult<simdjson::dom::element>
JsonObjectReader::require_field(std::string_view key) const {
  return require_as<simdjson::dom::element>(key);
}

CommonResult<std::string>
JsonObjectReader::require_string(std::string_view key) const {
  return require_as<std::string_view>(key).transform(
      [](std::string_view s) { return std::string(s); });
}

CommonResult<std::uint64_t>
JsonObjectReader::require_uint64(std::string_view key) const {
  return require_as<std::uint64_t>(key);
}

CommonResult<simdjson::dom::array>
JsonObjectReader::require_array(std::string_view key) const {
  return require_as<simdjson::dom::array>(key);
}

CommonResult<simdjson::dom::object>
JsonObjectReader::require_object(std::string_view key) const {
  return require_as<simdjson::dom::object>(key);
}

CommonResult<std::optional<std::string>>
JsonObjectReader::optional_string(std::string_view key) const {
  return optional_as<std::string_view>(key).transform(
      [](std::optional<std::string_view> view) {
        return view.transform(
            [](std::string_view s) { return std::string(s); });
      });
}

CommonResult<std::optional<simdjson::dom::array>>
JsonObjectReader::optional_array(std::string_view key) const {
  return optional_as<simdjson::dom::array>(key);
}

CommonResult<std::vector<std::string>>
JsonObjectReader::string_array(std::string_view key) const {
  return require_array(key).and_then(
      [=](simdjson::dom::array values)
          -> CommonResult<std::vector<std::string>> {
        std::vector<std::string> output;
        for (const auto value_element : values) {
          auto value = json_value_as<std::string_view>(value_element);
          if (!value) {
            return std::unexpected(
                with_error_context(std::move(value.error()),
                                   std::format("JSON field `{}` values", key)));
          }
          output.emplace_back(*value);
        }
        return output;
      });
}

CommonResult<simdjson::dom::object>
json_object_from_element(simdjson::dom::element value) {
  return json_value_as<simdjson::dom::object>(value);
}

} // namespace memix::detail
