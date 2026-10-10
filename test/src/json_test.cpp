#include "detail/json.hpp"

#include <catch2/catch_test_macros.hpp>
#include <simdjson.h>

#include <optional>
#include <string>
#include <string_view>

TEST_CASE("optional JSON fields preserve scalar values and null", "[json]") {
  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"missing">(std::optional<std::string>{});
  builder.append_comma();
  builder.append_key_value<"missing_view">(std::optional<std::string_view>{});
  builder.append_comma();
  builder.append_key_value<"empty">(std::optional<std::string>{""});
  builder.append_comma();
  builder.append_key_value<"text">(
      std::optional<std::string>{"line\n\"quoted\""});
  builder.append_comma();
  builder.append_key_value<"view">(
      std::optional<std::string_view>{"parent-id"});
  builder.append_comma();
  builder.append_key_value<"zero">(std::optional<int>{0});
  builder.end_object();

  const auto json = memix::detail::json_from_builder(builder);
  REQUIRE(json.has_value());
  REQUIRE(
      *json
      == R"({"missing":null,"missing_view":null,"empty":"","text":"line\n\"quoted\"",)"
         R"("view":"parent-id","zero":0})");
}
