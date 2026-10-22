#include "memix/fs.hpp"

#include <cerrno>
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
#ifdef __linux__
#include <fcntl.h>
// Linux rename extensions are declared by the C header, not ISO <cstdio>.
#include <stdio.h> // NOLINT(modernize-deprecated-headers)
#elifdef __APPLE__
#include <sys/stdio.h>
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <thread>
#include <tuple>
#include <utility>
#endif

namespace std_fs = std::filesystem;

namespace memix::fs {

namespace {

constexpr int kTempDirAttempts = 32;
#ifdef _WIN32
constexpr int kReplaceFileAttempts = 1000;
constexpr auto kReplaceFileRetryDelay = std::chrono::milliseconds(1);
#endif

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

FsResult<bool> exists(const path &entry_path) {
  std::error_code error;
  const bool value = std_fs::exists(entry_path, error);
  return fs_result(value, error, "failed to inspect", entry_path);
}

FsResult<bool> is_directory(const path &entry_path) {
  std::error_code error;
  const bool value = std_fs::is_directory(entry_path, error);
  return fs_result(value, error, "failed to inspect directory", entry_path);
}

FsResult<bool> is_regular_file(const path &entry_path) {
  std::error_code error;
  const bool value = std_fs::is_regular_file(entry_path, error);
  return fs_result(value, error, "failed to inspect file", entry_path);
}

FsResult<bool> is_symlink(const path &entry_path) {
  std::error_code error;
  const bool value =
      std_fs::is_symlink(std_fs::symlink_status(entry_path, error));
  return fs_result(value, error, "failed to inspect symlink", entry_path);
}

FsResult<file_status> status(const path &entry_path) {
  std::error_code error;
  const file_status value = std_fs::status(entry_path, error);
  return fs_result(value, error, "failed to inspect", entry_path);
}

FsResult<file_status> symlink_status(const path &entry_path) {
  std::error_code error;
  const file_status value = std_fs::symlink_status(entry_path, error);
  return fs_result(value, error, "failed to inspect symlink", entry_path);
}

FsResult<std::uintmax_t> file_size(const path &entry_path) {
  std::error_code error;
  const std::uintmax_t value = std_fs::file_size(entry_path, error);
  return fs_result(value, error, "failed to read file size for", entry_path);
}

FsResult<bool> create_directories(const path &entry_path) {
  std::error_code error;
  const bool value = std_fs::create_directories(entry_path, error);
  return fs_result(value, error, "failed to create directory", entry_path);
}

FsResult<bool> create_directory(const path &entry_path) {
  std::error_code error;
  const bool value = std_fs::create_directory(entry_path, error);
  return fs_result(value, error, "failed to create directory", entry_path);
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

FsVoid create_directory_symlink(const path &target, const path &link) {
  std::error_code error;
  std_fs::create_directory_symlink(target, link, error);
  return fs_void(error, "failed to create directory symlink", target, link);
}

FsVoid create_hard_link(const path &target, const path &link) {
  std::error_code error;
  std_fs::create_hard_link(target, link, error);
  return fs_void(error, "failed to create hard link", target, link);
}

FsResult<path> read_symlink(const path &entry_path) {
  std::error_code error;
  const auto target = std_fs::read_symlink(entry_path, error);
  return fs_result(target, error, "failed to read symlink", entry_path);
}

FsResult<path> relative(const path &target, const path &base) {
  std::error_code error;
  const auto value = std_fs::relative(target, base, error);
  return fs_result(value, error, "failed to compute relative path", target,
                   base);
}

FsResult<path> weakly_canonical(const path &entry_path) {
  std::error_code error;
  const auto value = std_fs::weakly_canonical(entry_path, error);
  return fs_result(value, error, "failed to canonicalize", entry_path);
}

FsVoid rename(const path &source, const path &destination) {
  std::error_code error;
  std_fs::rename(source, destination, error);
  return fs_void(error, "failed to rename", source, destination);
}

FsVoid exchange_paths(const path &left, const path &right) {
#ifdef __linux__
  const int result = ::renameat2(AT_FDCWD, left.c_str(), AT_FDCWD,
                                 right.c_str(), RENAME_EXCHANGE);
#elifdef __APPLE__
  const int result = ::renamex_np(left.c_str(), right.c_str(), RENAME_SWAP);
#else
  return fs_error("atomic path exchange is not supported",
                  std::make_error_code(std::errc::operation_not_supported),
                  left, right);
#endif
#if defined(__linux__) || defined(__APPLE__)
  if (result != 0) {
    return fs_error("failed to exchange paths",
                    std::error_code(errno, std::generic_category()), left,
                    right);
  }
  return {};
#endif
}

FsVoid replace_file(const path &source, const path &destination) {
#ifndef _WIN32
  return memix::fs::rename(source, destination);
#else
  for (int attempt = 0; attempt < kReplaceFileAttempts; ++attempt) {
    if (MoveFileExW(source.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
        != 0) {
      return {};
    }
    const DWORD native_error = GetLastError();
    const bool retryable = native_error == ERROR_ACCESS_DENIED
                        || native_error == ERROR_SHARING_VIOLATION;
    if (!retryable || attempt + 1 == kReplaceFileAttempts) {
      return fs_error("failed to replace file",
                      std::error_code(static_cast<int>(native_error),
                                      std::system_category()),
                      source, destination);
    }
    std::this_thread::sleep_for(kReplaceFileRetryDelay);
  }
  return fs_error("failed to replace file");
#endif
}

FsVoid replace_directory_symlink(const path &source, const path &destination) {
#ifndef _WIN32
  return memix::fs::rename(source, destination);
#else
  std::error_code status_error;
  const file_status destination_status =
      std_fs::symlink_status(destination, status_error);
  if (status_error && status_error != std::errc::no_such_file_or_directory) {
    return fs_void(status_error, "failed to inspect symlink", destination);
  }
  if (status_error || !std_fs::exists(destination_status)) {
    return memix::fs::rename(source, destination);
  }
  if (!std_fs::is_symlink(destination_status)) {
    return fs_error("cannot replace non-symlink destination",
                    std::make_error_code(std::errc::file_exists), destination);
  }

  path parent = destination.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  FsResult<path> backup = unique_temp_dir(
      parent,
      TempDirOptions{
          .prefix = destination.filename().string(),
          .failure_message = "failed to allocate a temporary symlink backup",
      });
  if (!backup) {
    return std::unexpected(backup.error());
  }
  const FsVoid backed_up = memix::fs::rename(destination, *backup);
  if (!backed_up) {
    return std::unexpected(backed_up.error());
  }
  const FsVoid installed = memix::fs::rename(source, destination);
  if (!installed) {
    FsError install_error = installed.error();
    const FsVoid restored = memix::fs::rename(*backup, destination);
    if (!restored) {
      install_error.message = std::format(
          "{}; additionally failed to restore the prior directory symlink: "
          "{}; recover it from `{}`",
          install_error.message, restored.error().message, backup->string());
    }
    return std::unexpected(std::move(install_error));
  }
  std::ignore = memix::fs::remove(*backup);
  return {};
#endif
}

FsResult<bool> remove(const path &entry_path) {
  std::error_code error;
  const bool value = std_fs::remove(entry_path, error);
  return fs_result(value, error, "failed to remove", entry_path);
}

FsResult<std::uintmax_t> remove_all(const path &entry_path) {
  std::error_code error;
  const std::uintmax_t value = std_fs::remove_all(entry_path, error);
  return fs_result(value, error, "failed to remove", entry_path);
}

FsVoid
permissions(const path &entry_path, perms permissions, perm_options options) {
  std::error_code error;
  std_fs::permissions(entry_path, permissions, options, error);
  return fs_void(error, "failed to set permissions", entry_path);
}

FsResult<path> temp_directory_path() {
  std::error_code error;
  const path value = std_fs::temp_directory_path(error);
  return fs_result(value, error, "failed to find temporary directory", value);
}

FsResult<std::string> read_text_file(const path &entry_path) {
  std::ifstream input(entry_path, std::ios::binary);
  if (!input) {
    return fs_error(std::format("failed to open `{}`", entry_path.string()));
  }
  std::string output{std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>()};
  if (input.bad()) {
    return fs_error(std::format("failed to read `{}`", entry_path.string()));
  }
  return output;
}

FsResult<std::string>
read_text_file(const path &entry_path, std::string_view subject) {
  if (subject.empty()) {
    return read_text_file(entry_path);
  }

  std::ifstream input(entry_path, std::ios::binary);
  if (!input) {
    return fs_error(
        std::format("failed to open {} `{}`", subject, entry_path.string()));
  }
  std::string output{std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>()};
  if (input.bad()) {
    return fs_error(
        std::format("failed to read {} `{}`", subject, entry_path.string()));
  }
  return output;
}

FsVoid write_text_file(const path &entry_path, std::string_view contents) {
  std::ofstream output(entry_path, std::ios::binary);
  if (!output) {
    return fs_error(
        std::format("failed to open `{}` for writing", entry_path.string()));
  }
  output << contents;
  if (!output) {
    return fs_error(std::format("failed to write `{}`", entry_path.string()));
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

FsVoid apply_read_only_file_permissions(const path &entry_path) {
  return memix::fs::permissions(entry_path, perms::owner_read,
                                perm_options::replace);
}

FsVoid apply_read_only_directory_permissions(const path &entry_path) {
  return memix::fs::permissions(
      entry_path, perms::owner_read | perms::owner_exec, perm_options::replace);
}

FsVoid apply_writable_directory_permissions(const path &entry_path) {
  return memix::fs::permissions(
      entry_path, perms::owner_read | perms::owner_write | perms::owner_exec,
      perm_options::replace);
}

} // namespace memix::fs
