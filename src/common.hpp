#pragma once

#include "memix/store.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <simdjson.h>

namespace memix::detail {

template <typename Value>
using Result = std::expected<Value, StoreError>;

using VoidResult = std::expected<void, StoreError>;

std::unexpected<StoreError> unexpected_error(std::string message);

template <typename... Paths>
std::string filesystem_error_message(std::string_view operation,
                                     const std::error_code &error,
                                     const Paths &...paths) {
  static_assert(sizeof...(Paths) > 0,
                "filesystem errors must include at least one path");

  std::string message{operation};
  std::string_view separator = " `";
  const auto append_path = [&message,
                            &separator](const std::filesystem::path &path) {
    message += std::format("{}{}`", separator, path.string());
    separator = " to `";
  };
  (append_path(paths), ...);
  return std::format("{}: {}", message, error.message());
}

namespace expected_fs {

Result<bool> exists(const std::filesystem::path &path);
Result<bool> is_directory(const std::filesystem::path &path);
Result<bool> is_regular_file(const std::filesystem::path &path);
Result<bool> is_symlink(const std::filesystem::path &path);
Result<std::uintmax_t> file_size(const std::filesystem::path &path);
Result<bool> create_directories(const std::filesystem::path &path);
Result<bool> create_directory(const std::filesystem::path &path);
Result<bool> copy_file(const std::filesystem::path &source,
                       const std::filesystem::path &destination,
                       std::filesystem::copy_options options);
VoidResult create_symlink(const std::filesystem::path &target,
                          const std::filesystem::path &link);
Result<std::filesystem::path> read_symlink(const std::filesystem::path &path);
Result<std::filesystem::path> relative(const std::filesystem::path &target,
                                       const std::filesystem::path &base);
VoidResult rename(const std::filesystem::path &source,
                  const std::filesystem::path &destination);
Result<bool> remove(const std::filesystem::path &path);
Result<std::uintmax_t> remove_all(const std::filesystem::path &path);
VoidResult permissions(const std::filesystem::path &path,
                       std::filesystem::perms permissions,
                       std::filesystem::perm_options options);

} // namespace expected_fs

Result<std::string> read_text_file(const std::filesystem::path &path);
Result<std::string> read_text_file(const std::filesystem::path &path,
                                   std::string_view subject);
VoidResult write_text_file(const std::filesystem::path &path,
                           std::string_view contents);

std::string hex_encode(std::span<const unsigned char> bytes);
Result<std::string> sha256_text(std::string_view text);
bool is_lower_hex_sha256(std::string_view value) noexcept;
bool is_lower_hex_prefix(std::string_view value) noexcept;

struct JsonBuilderMessages {
  std::string_view invalid_utf8_message;
  std::string_view build_error_prefix;
};

struct TempDirOptions {
  std::string_view prefix;
  std::string_view failure_message;
};

Result<std::string>
json_from_builder(const simdjson::builder::string_builder &builder,
                  JsonBuilderMessages messages);

Result<std::filesystem::path>
unique_temp_dir(const std::filesystem::path &parent, TempDirOptions options);

VoidResult apply_read_only_file_permissions(const std::filesystem::path &path);
VoidResult
apply_read_only_directory_permissions(const std::filesystem::path &path);
VoidResult
apply_writable_directory_permissions(const std::filesystem::path &path);

} // namespace memix::detail
