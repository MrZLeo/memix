#include "detail/fs.hpp"

#include "memix/error.hpp"

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
#include <utility>

namespace fs = std::filesystem;

namespace memix::detail {

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

std::unexpected<memix::FsError> fs_error(memix::FsError error) {
  return std::unexpected(std::move(error));
}

std::unexpected<memix::FsError> fs_error(std::string message) {
  memix::FsError error;
  error.message = std::move(message);
  return fs_error(std::move(error));
}

namespace expected_fs {

FsResult<bool> exists(const fs::path &path) {
  std::error_code error;
  const bool value = fs::exists(path, error);
  return fs_result(value, error, "failed to inspect", path);
}

FsResult<bool> is_directory(const fs::path &path) {
  std::error_code error;
  const bool value = fs::is_directory(path, error);
  return fs_result(value, error, "failed to inspect directory", path);
}

FsResult<bool> is_regular_file(const fs::path &path) {
  std::error_code error;
  const bool value = fs::is_regular_file(path, error);
  return fs_result(value, error, "failed to inspect file", path);
}

FsResult<bool> is_symlink(const fs::path &path) {
  std::error_code error;
  const bool value = fs::is_symlink(fs::symlink_status(path, error));
  return fs_result(value, error, "failed to inspect symlink", path);
}

FsResult<std::uintmax_t> file_size(const fs::path &path) {
  std::error_code error;
  const std::uintmax_t value = fs::file_size(path, error);
  return fs_result(value, error, "failed to read file size for", path);
}

FsResult<bool> create_directories(const fs::path &path) {
  std::error_code error;
  const bool value = fs::create_directories(path, error);
  return fs_result(value, error, "failed to create directory", path);
}

FsResult<bool> create_directory(const fs::path &path) {
  std::error_code error;
  const bool value = fs::create_directory(path, error);
  return fs_result(value, error, "failed to create directory", path);
}

FsResult<bool> copy_file(const fs::path &source, const fs::path &destination,
                         fs::copy_options options) {
  std::error_code error;
  const bool value = fs::copy_file(source, destination, options, error);
  return fs_result(value, error, "failed to copy file", source, destination);
}

FsVoid create_symlink(const fs::path &target, const fs::path &link) {
  std::error_code error;
  fs::create_symlink(target, link, error);
  return fs_void(error, "failed to create symlink", target, link);
}

FsVoid create_hard_link(const fs::path &target, const fs::path &link) {
  std::error_code error;
  fs::create_hard_link(target, link, error);
  return fs_void(error, "failed to create hard link", target, link);
}

FsResult<fs::path> read_symlink(const fs::path &path) {
  std::error_code error;
  const fs::path target = fs::read_symlink(path, error);
  return fs_result(target, error, "failed to read symlink", path);
}

FsResult<fs::path> relative(const fs::path &target, const fs::path &base) {
  std::error_code error;
  const fs::path value = fs::relative(target, base, error);
  return fs_result(value, error, "failed to compute relative path", target,
                   base);
}

FsVoid rename(const fs::path &source, const fs::path &destination) {
  std::error_code error;
  fs::rename(source, destination, error);
  return fs_void(error, "failed to rename", source, destination);
}

FsResult<bool> remove(const fs::path &path) {
  std::error_code error;
  const bool value = fs::remove(path, error);
  return fs_result(value, error, "failed to remove", path);
}

FsResult<std::uintmax_t> remove_all(const fs::path &path) {
  std::error_code error;
  const std::uintmax_t value = fs::remove_all(path, error);
  return fs_result(value, error, "failed to remove", path);
}

FsVoid permissions(const fs::path &path, fs::perms permissions,
                   fs::perm_options options) {
  std::error_code error;
  fs::permissions(path, permissions, options, error);
  return fs_void(error, "failed to set permissions", path);
}

} // namespace expected_fs

FsResult<std::string> read_text_file(const fs::path &path) {
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

FsResult<std::string> read_text_file(const fs::path &path,
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

FsVoid write_text_file(const fs::path &path, std::string_view contents) {
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

FsResult<fs::path> unique_temp_dir(const fs::path &parent,
                                   TempDirOptions options) {
  std::random_device random_device;
  for (int attempt = 0; attempt < kTempDirAttempts; ++attempt) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    fs::path path = parent
                  / std::format(".tmp-{}-{}-{}-{}", options.prefix, nanos,
                                random_device(), attempt);
    const FsResult<bool> exists = expected_fs::exists(path);
    if (!exists) {
      return std::unexpected(exists.error());
    }
    if (!*exists) {
      return path;
    }
  }
  return fs_error(std::string(options.failure_message));
}

TemporaryDirectoryCleanup::TemporaryDirectoryCleanup(
    TemporaryDirectoryCleanup &&other) noexcept
    : path_(std::move(other.path_)), active_(other.active_) {
  other.release();
}

TemporaryDirectoryCleanup::~TemporaryDirectoryCleanup() {
  if (!active_) {
    return;
  }

  std::error_code remove_error;
  fs::remove_all(path_, remove_error);
}

FsVoid TemporaryDirectoryCleanup::cleanup() {
  if (!active_) {
    return {};
  }

  active_ = false;
  std::error_code remove_error;
  fs::remove_all(path_, remove_error);
  if (remove_error) {
    return fs_error("failed to remove", remove_error, path_);
  }
  return {};
}

FsVoid apply_read_only_file_permissions(const fs::path &path) {
  return expected_fs::permissions(path, fs::perms::owner_read,
                                  fs::perm_options::replace);
}

FsVoid apply_read_only_directory_permissions(const fs::path &path) {
  return expected_fs::permissions(path,
                                  fs::perms::owner_read | fs::perms::owner_exec,
                                  fs::perm_options::replace);
}

FsVoid apply_writable_directory_permissions(const fs::path &path) {
  return expected_fs::permissions(
      path,
      fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
      fs::perm_options::replace);
}

} // namespace memix::detail
