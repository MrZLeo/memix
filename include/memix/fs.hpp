#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace memix::fs {

using path = std::filesystem::path;
using perms = std::filesystem::perms;
using perm_options = std::filesystem::perm_options;
using copy_options = std::filesystem::copy_options;
using directory_entry = std::filesystem::directory_entry;
using directory_iterator = std::filesystem::directory_iterator;
using file_status = std::filesystem::file_status;
using file_type = std::filesystem::file_type;
using recursive_directory_iterator =
    std::filesystem::recursive_directory_iterator;

} // namespace memix::fs

namespace memix {

struct FsError {
  std::string message;
  std::error_code code;
  std::string operation;
  std::vector<fs::path> paths;
};

template <typename Value>
using FsResult = std::expected<Value, FsError>;

using FsVoid = std::expected<void, FsError>;

} // namespace memix

namespace memix::fs {

std::unexpected<FsError> fs_error(FsError error);
std::unexpected<FsError> fs_error(std::string message);

template <typename... Paths>
std::string filesystem_error_message(std::string_view operation,
                                     const std::error_code &error,
                                     const Paths &...paths) {
  static_assert(sizeof...(Paths) > 0,
                "filesystem errors must include at least one path");

  std::string message{operation};
  std::string_view separator = " `";
  const auto append_path = [&message, &separator](const path &path) {
    message += std::format("{}{}`", separator, path.string());
    separator = " to `";
  };
  (append_path(paths), ...);
  return std::format("{}: {}", message, error.message());
}

template <typename... Paths>
FsError make_fs_error(std::string_view operation, const std::error_code &error,
                      const Paths &...paths) {
  return FsError{
      .message = filesystem_error_message(operation, error, paths...),
      .code = error,
      .operation = std::string(operation),
      .paths = std::vector<path>{paths...},
  };
}

template <typename... Paths>
std::unexpected<FsError> fs_error(std::string_view operation,
                                  const std::error_code &error,
                                  const Paths &...paths) {
  return fs_error(make_fs_error(operation, error, paths...));
}

FsResult<bool> exists(const path &path);
FsResult<bool> is_directory(const path &path);
FsResult<bool> is_regular_file(const path &path);
FsResult<bool> is_symlink(const path &path);
FsResult<file_status> status(const path &path);
FsResult<file_status> symlink_status(const path &path);
FsResult<std::uintmax_t> file_size(const path &path);
FsResult<bool> create_directories(const path &path);
FsResult<bool> create_directory(const path &path);
FsResult<bool> copy_file(const path &source, const path &destination,
                         copy_options options = copy_options::none);
FsVoid create_symlink(const path &target, const path &link);
FsVoid create_hard_link(const path &target, const path &link);
FsResult<path> read_symlink(const path &path);
FsResult<path> relative(const path &target, const path &base);
FsVoid rename(const path &source, const path &destination);
FsResult<bool> remove(const path &path);
FsResult<std::uintmax_t> remove_all(const path &path);
FsVoid permissions(const path &path, perms permissions,
                   perm_options options = perm_options::replace);
FsResult<path> temp_directory_path();

FsResult<std::string> read_text_file(const path &path);
FsResult<std::string> read_text_file(const path &path,
                                     std::string_view subject);
FsVoid write_text_file(const path &path, std::string_view contents);

struct TempDirOptions {
  std::string_view prefix;
  std::string_view failure_message;
};

FsResult<path> unique_temp_dir(const path &parent, TempDirOptions options);

FsVoid apply_read_only_file_permissions(const path &path);
FsVoid apply_read_only_directory_permissions(const path &path);
FsVoid apply_writable_directory_permissions(const path &path);

} // namespace memix::fs
