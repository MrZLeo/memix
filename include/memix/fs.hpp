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

using std::filesystem::copy_options;
using std::filesystem::directory_entry;
using std::filesystem::directory_iterator;
using std::filesystem::file_status;
using std::filesystem::file_type;
using std::filesystem::path;
using std::filesystem::perm_options;
using std::filesystem::perms;
using std::filesystem::recursive_directory_iterator;

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

inline std::unexpected<memix::FsError> fs_error(memix::FsError error) {
  return std::unexpected(std::move(error));
}

inline std::unexpected<memix::FsError> fs_error(std::string message) {
  memix::FsError error;
  error.message = std::move(message);
  return fs_error(std::move(error));
}

template <typename... Paths>
std::string
filesystem_error_message(std::string_view operation,
                         const std::error_code &error, const Paths &...paths) {
  static_assert(sizeof...(Paths) > 0,
                "filesystem errors must include at least one path");

  std::string message{operation};
  std::string_view separator = " `";
  const auto append_path = [&message, &separator](const path &entry_path) {
    message += std::format("{}{}`", separator, entry_path.string());
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
std::unexpected<FsError>
fs_error(std::string_view operation, const std::error_code &error,
         const Paths &...paths) {
  return fs_error(make_fs_error(operation, error, paths...));
}

FsResult<bool> exists(const path &entry_path);
FsResult<bool> is_directory(const path &entry_path);
FsResult<bool> is_regular_file(const path &entry_path);
FsResult<bool> is_symlink(const path &entry_path);
FsResult<file_status> status(const path &entry_path);
FsResult<file_status> symlink_status(const path &entry_path);
FsResult<std::uintmax_t> file_size(const path &entry_path);
FsResult<bool> create_directories(const path &entry_path);
FsResult<bool> create_directory(const path &entry_path);
FsResult<bool> copy_file(const path &source, const path &destination,
                         copy_options options = copy_options::none);
FsVoid create_symlink(const path &target, const path &link);
FsVoid create_directory_symlink(const path &target, const path &link);
FsVoid create_hard_link(const path &target, const path &link);
FsResult<path> read_symlink(const path &entry_path);
FsResult<path> relative(const path &target, const path &base);
FsResult<path> weakly_canonical(const path &entry_path);
FsVoid rename(const path &source, const path &destination);
// Atomically exchanges two existing paths, including nonempty directories.
// Unsupported platforms/filesystems return an error; there is no two-rename
// fallback and neither path is removed on failure.
FsVoid exchange_paths(const path &left, const path &right);
// Atomically installs `source` at `destination`, replacing an existing file.
// Windows retries transient sharing violations caused by concurrent readers.
FsVoid replace_file(const path &source, const path &destination);
// Installs a prepared directory symlink at `destination`. POSIX uses one
// atomic rename; Windows uses a recoverable handoff because it cannot replace
// an existing directory symlink with rename.
FsVoid replace_directory_symlink(const path &source, const path &destination);
FsResult<bool> remove(const path &entry_path);
FsResult<std::uintmax_t> remove_all(const path &entry_path);
FsVoid permissions(const path &entry_path, perms permissions,
                   perm_options options = perm_options::replace);
FsResult<path> temp_directory_path();

FsResult<std::string> read_text_file(const path &entry_path);
FsResult<std::string>
read_text_file(const path &entry_path, std::string_view subject);
FsVoid write_text_file(const path &entry_path, std::string_view contents);

struct TempDirOptions {
  std::string_view prefix;
  std::string_view failure_message;
};

FsResult<path> unique_temp_dir(const path &parent, TempDirOptions options);

FsVoid apply_read_only_file_permissions(const path &entry_path);
FsVoid apply_read_only_directory_permissions(const path &entry_path);
FsVoid apply_writable_directory_permissions(const path &entry_path);

} // namespace memix::fs
