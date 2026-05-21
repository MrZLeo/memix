#include "detail/json.hpp"

#include "detail/error.hpp"

#include <format>
#include <string>
#include <string_view>

#include <simdjson.h>

namespace memix::detail {

namespace {

std::string simdjson_error_text(simdjson::error_code error) {
  return simdjson::error_message(error);
}

} // namespace

CommonResult<std::string>
json_from_builder(const simdjson::builder::string_builder &builder,
                  JsonBuilderMessages messages) {
  if (!builder.validate_unicode()) {
    return common_error(std::string(messages.invalid_utf8_message));
  }

  std::string_view output;
  const simdjson::error_code error = builder.view().get(output);
  if (error != simdjson::SUCCESS) {
    return common_error(std::format("{}: {}", messages.build_error_prefix,
                                    simdjson_error_text(error)));
  }
  return std::string(output);
}

} // namespace memix::detail
