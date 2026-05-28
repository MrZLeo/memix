#include "memix/fs.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace std_fs = std::filesystem;

namespace memix::fs {

namespace {

constexpr int kTempDirAttempts = 32;

template <typename Value, typename... Paths>
FsResult<Value> fs_result(Value value, const std::error_code &error,
                          std::string_view operation, const Paths &...paths) {
  if (error) {
    return fs_error(operation, error, paths...);
  }
  return value;
}

template <typename... Paths>
FsVoid fs_void(const std::error_code &error, std::string_view operation,
               const Paths &...paths) {
  if (error) {
    return fs_error(operation, error, paths...);
  }
  return {};
}

} // namespace

FsResult<bool> exists(const path &path) {
  std::error_code error;
  const bool value = std_fs::exists(path, error);
  return fs_result(value, error, "failed to inspect", path);
}

FsResult<bool> is_directory(const path &path) {
  std::error_code error;
  const bool value = std_fs::is_directory(path, error);
  return fs_result(value, error, "failed to inspect directory", path);
}

FsResult<bool> is_regular_file(const path &path) {
  std::error_code error;
  const bool value = std_fs::is_regular_file(path, error);
  return fs_result(value, error, "failed to inspect file", path);
}

FsResult<bool> is_symlink(const path &path) {
  std::error_code error;
  const bool value = std_fs::is_symlink(std_fs::symlink_status(path, error));
  return fs_result(value, error, "failed to inspect symlink", path);
}

FsResult<file_status> status(const path &path) {
  std::error_code error;
  const file_status value = std_fs::status(path, error);
  return fs_result(value, error, "failed to inspect", path);
}

FsResult<file_status> symlink_status(const path &path) {
  std::error_code error;
  const file_status value = std_fs::symlink_status(path, error);
  return fs_result(value, error, "failed to inspect symlink", path);
}

FsResult<std::uintmax_t> file_size(const path &path) {
  std::error_code error;
  const std::uintmax_t value = std_fs::file_size(path, error);
  return fs_result(value, error, "failed to read file size for", path);
}

FsResult<bool> create_directories(const path &path) {
  std::error_code error;
  const bool value = std_fs::create_directories(path, error);
  return fs_result(value, error, "failed to create directory", path);
}

FsResult<bool> create_directory(const path &path) {
  std::error_code error;
  const bool value = std_fs::create_directory(path, error);
  return fs_result(value, error, "failed to create directory", path);
}

FsResult<bool> copy_file(const path &source, const path &destination,
                         copy_options options) {
  std::error_code error;
  const bool value = std_fs::copy_file(source, destination, options, error);
  return fs_result(value, error, "failed to copy file", source, destination);
}

FsVoid create_symlink(const path &target, const path &link) {
  std::error_code error;
  std_fs::create_symlink(target, link, error);
  return fs_void(error, "failed to create symlink", target, link);
}

FsVoid create_hard_link(const path &target, const path &link) {
  std::error_code error;
  std_fs::create_hard_link(target, link, error);
  return fs_void(error, "failed to create hard link", target, link);
}

FsResult<path> read_symlink(const path &path) {
  std::error_code error;
  const auto target = std_fs::read_symlink(path, error);
  return fs_result(target, error, "failed to read symlink", path);
}

FsResult<path> relative(const path &target, const path &base) {
  std::error_code error;
  const auto value = std_fs::relative(target, base, error);
  return fs_result(value, error, "failed to compute relative path", target,
                   base);
}

FsVoid rename(const path &source, const path &destination) {
  std::error_code error;
  std_fs::rename(source, destination, error);
  return fs_void(error, "failed to rename", source, destination);
}

FsResult<bool> remove(const path &path) {
  std::error_code error;
  const bool value = std_fs::remove(path, error);
  return fs_result(value, error, "failed to remove", path);
}

FsResult<std::uintmax_t> remove_all(const path &path) {
  std::error_code error;
  const std::uintmax_t value = std_fs::remove_all(path, error);
  return fs_result(value, error, "failed to remove", path);
}

FsVoid permissions(const path &path, perms permissions, perm_options options) {
  std::error_code error;
  std_fs::permissions(path, permissions, options, error);
  return fs_void(error, "failed to set permissions", path);
}

FsResult<path> temp_directory_path() {
  std::error_code error;
  const path value = std_fs::temp_directory_path(error);
  return fs_result(value, error, "failed to find temporary directory", value);
}

FsResult<std::string> read_text_file(const path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return fs_error(std::format("failed to open `{}`", path.string()));
  }
  std::string output{std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>()};
  if (input.bad()) {
    return fs_error(std::format("failed to read `{}`", path.string()));
  }
  return output;
}

FsResult<std::string> read_text_file(const path &path,
                                     std::string_view subject) {
  if (subject.empty()) {
    return read_text_file(path);
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return fs_error(
        std::format("failed to open {} `{}`", subject, path.string()));
  }
  std::string output{std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>()};
  if (input.bad()) {
    return fs_error(
        std::format("failed to read {} `{}`", subject, path.string()));
  }
  return output;
}

FsVoid write_text_file(const path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    return fs_error(
        std::format("failed to open `{}` for writing", path.string()));
  }
  output << contents;
  if (!output) {
    return fs_error(std::format("failed to write `{}`", path.string()));
  }
  return {};
}

FsResult<path> unique_temp_dir(const path &parent, TempDirOptions options) {
  std::random_device random_device;
  for (int attempt = 0; attempt < kTempDirAttempts; ++attempt) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    path candidate = parent
                   / std::format(".tmp-{}-{}-{}-{}", options.prefix, nanos,
                                 random_device(), attempt);
    const FsResult<bool> candidate_exists = memix::fs::exists(candidate);
    if (!candidate_exists) {
      return std::unexpected(candidate_exists.error());
    }
    if (!*candidate_exists) {
      return candidate;
    }
  }
  return fs_error(std::string(options.failure_message));
}

FsVoid apply_read_only_file_permissions(const path &path) {
  return memix::fs::permissions(path, perms::owner_read, perm_options::replace);
}

FsVoid apply_read_only_directory_permissions(const path &path) {
  return memix::fs::permissions(path, perms::owner_read | perms::owner_exec,
                                perm_options::replace);
}

FsVoid apply_writable_directory_permissions(const path &path) {
  return memix::fs::permissions(
      path, perms::owner_read | perms::owner_write | perms::owner_exec,
      perm_options::replace);
}

} // namespace memix::fs
