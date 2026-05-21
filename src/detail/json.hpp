#pragma once

#include "detail/error.hpp"

#include <string>
#include <string_view>

#include <simdjson.h>

namespace memix::detail {

struct JsonBuilderMessages {
  std::string_view invalid_utf8_message;
  std::string_view build_error_prefix;
};

CommonResult<std::string>
json_from_builder(const simdjson::builder::string_builder &builder,
                  JsonBuilderMessages messages);

} // namespace memix::detail
