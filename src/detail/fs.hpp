#pragma once

#include "memix/error.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace memix::detail {

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
  const auto append_path = [&message,
                            &separator](const std::filesystem::path &path) {
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
      .paths = std::vector<std::filesystem::path>{paths...},
  };
}

template <typename... Paths>
std::unexpected<FsError> fs_error(std::string_view operation,
                                  const std::error_code &error,
                                  const Paths &...paths) {
  return fs_error(make_fs_error(operation, error, paths...));
}

namespace expected_fs {

FsResult<bool> exists(const std::filesystem::path &path);
FsResult<bool> is_directory(const std::filesystem::path &path);
FsResult<bool> is_regular_file(const std::filesystem::path &path);
FsResult<bool> is_symlink(const std::filesystem::path &path);
FsResult<std::uintmax_t> file_size(const std::filesystem::path &path);
FsResult<bool> create_directories(const std::filesystem::path &path);
FsResult<bool> create_directory(const std::filesystem::path &path);
FsResult<bool> copy_file(const std::filesystem::path &source,
                         const std::filesystem::path &destination,
                         std::filesystem::copy_options options);
FsVoid create_symlink(const std::filesystem::path &target,
                      const std::filesystem::path &link);
FsVoid create_hard_link(const std::filesystem::path &target,
                        const std::filesystem::path &link);
FsResult<std::filesystem::path> read_symlink(const std::filesystem::path &path);
FsResult<std::filesystem::path> relative(const std::filesystem::path &target,
                                         const std::filesystem::path &base);
FsVoid rename(const std::filesystem::path &source,
              const std::filesystem::path &destination);
FsResult<bool> remove(const std::filesystem::path &path);
FsResult<std::uintmax_t> remove_all(const std::filesystem::path &path);
FsVoid permissions(const std::filesystem::path &path,
                   std::filesystem::perms permissions,
                   std::filesystem::perm_options options);

} // namespace expected_fs

FsResult<std::string> read_text_file(const std::filesystem::path &path);
FsResult<std::string> read_text_file(const std::filesystem::path &path,
                                     std::string_view subject);
FsVoid write_text_file(const std::filesystem::path &path,
                       std::string_view contents);

struct TempDirOptions {
  std::string_view prefix;
  std::string_view failure_message;
};

FsResult<std::filesystem::path>
unique_temp_dir(const std::filesystem::path &parent, TempDirOptions options);

class TemporaryDirectoryCleanup {
public:
  explicit TemporaryDirectoryCleanup(std::filesystem::path path)
      : path_(std::move(path)) {}

  TemporaryDirectoryCleanup(const TemporaryDirectoryCleanup &) = delete;
  TemporaryDirectoryCleanup &
  operator=(const TemporaryDirectoryCleanup &) = delete;

  TemporaryDirectoryCleanup(TemporaryDirectoryCleanup &&other) noexcept;
  TemporaryDirectoryCleanup &
  operator=(TemporaryDirectoryCleanup &&other) = delete;

  ~TemporaryDirectoryCleanup();

  [[nodiscard]] FsVoid cleanup();
  void release() noexcept { active_ = false; }

private:
  std::filesystem::path path_;
  bool active_ = true;
};

FsVoid apply_read_only_file_permissions(const std::filesystem::path &path);
FsVoid apply_read_only_directory_permissions(const std::filesystem::path &path);
FsVoid apply_writable_directory_permissions(const std::filesystem::path &path);

} // namespace memix::detail
