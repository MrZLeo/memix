#include "common.hpp"

#include "memix/store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <openssl/evp.h>
#include <simdjson.h>

namespace fs = std::filesystem;

namespace memix::detail {

namespace {

constexpr std::size_t kSha256DigestBytes = 32;
constexpr std::size_t kSha256HexDigestLength = kSha256DigestBytes * 2;
constexpr int kTempDirAttempts = 32;
constexpr unsigned int kHexShift = 4;
constexpr unsigned int kHexMask = 0x0FU;

template <typename Value, typename... Paths>
Result<Value> fs_result(Value value, const std::error_code &error,
                        std::string_view operation, const Paths &...paths) {
  if (error) {
    return unexpected_error(
        filesystem_error_message(operation, error, paths...));
  }
  return value;
}

template <typename... Paths>
VoidResult fs_void(const std::error_code &error, std::string_view operation,
                   const Paths &...paths) {
  if (error) {
    return unexpected_error(
        filesystem_error_message(operation, error, paths...));
  }
  return {};
}

std::string simdjson_error_text(simdjson::error_code error) {
  return simdjson::error_message(error);
}

} // namespace

std::unexpected<memix::StoreError> unexpected_error(std::string message) {
  return std::unexpected(memix::StoreError{.message = std::move(message)});
}

namespace expected_fs {

Result<bool> exists(const fs::path &path) {
  std::error_code error;
  const bool value = fs::exists(path, error);
  return fs_result(value, error, "failed to inspect", path);
}

Result<bool> is_directory(const fs::path &path) {
  std::error_code error;
  const bool value = fs::is_directory(path, error);
  return fs_result(value, error, "failed to inspect directory", path);
}

Result<bool> is_regular_file(const fs::path &path) {
  std::error_code error;
  const bool value = fs::is_regular_file(path, error);
  return fs_result(value, error, "failed to inspect file", path);
}

Result<bool> is_symlink(const fs::path &path) {
  std::error_code error;
  const bool value = fs::is_symlink(fs::symlink_status(path, error));
  return fs_result(value, error, "failed to inspect symlink", path);
}

Result<std::uintmax_t> file_size(const fs::path &path) {
  std::error_code error;
  const std::uintmax_t value = fs::file_size(path, error);
  return fs_result(value, error, "failed to read file size for", path);
}

Result<bool> create_directories(const fs::path &path) {
  std::error_code error;
  const bool value = fs::create_directories(path, error);
  return fs_result(value, error, "failed to create directory", path);
}

Result<bool> create_directory(const fs::path &path) {
  std::error_code error;
  const bool value = fs::create_directory(path, error);
  return fs_result(value, error, "failed to create directory", path);
}

Result<bool> copy_file(const fs::path &source, const fs::path &destination,
                       fs::copy_options options) {
  std::error_code error;
  const bool value = fs::copy_file(source, destination, options, error);
  return fs_result(value, error, "failed to copy file", source, destination);
}

VoidResult create_symlink(const fs::path &target, const fs::path &link) {
  std::error_code error;
  fs::create_symlink(target, link, error);
  return fs_void(error, "failed to create symlink", target, link);
}

Result<fs::path> read_symlink(const fs::path &path) {
  std::error_code error;
  const fs::path target = fs::read_symlink(path, error);
  return fs_result(target, error, "failed to read symlink", path);
}

Result<fs::path> relative(const fs::path &target, const fs::path &base) {
  std::error_code error;
  const fs::path value = fs::relative(target, base, error);
  return fs_result(value, error, "failed to compute relative path", target,
                   base);
}

VoidResult rename(const fs::path &source, const fs::path &destination) {
  std::error_code error;
  fs::rename(source, destination, error);
  return fs_void(error, "failed to rename", source, destination);
}

Result<bool> remove(const fs::path &path) {
  std::error_code error;
  const bool value = fs::remove(path, error);
  return fs_result(value, error, "failed to remove", path);
}

Result<std::uintmax_t> remove_all(const fs::path &path) {
  std::error_code error;
  const std::uintmax_t value = fs::remove_all(path, error);
  return fs_result(value, error, "failed to remove", path);
}

VoidResult permissions(const fs::path &path, fs::perms permissions,
                       fs::perm_options options) {
  std::error_code error;
  fs::permissions(path, permissions, options, error);
  return fs_void(error, "failed to set permissions", path);
}

} // namespace expected_fs

Result<std::string> read_text_file(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return unexpected_error(std::format("failed to open `{}`", path.string()));
  }
  std::string output{std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>()};
  if (input.bad()) {
    return unexpected_error(std::format("failed to read `{}`", path.string()));
  }
  return output;
}

Result<std::string> read_text_file(const fs::path &path,
                                   std::string_view subject) {
  if (subject.empty()) {
    return read_text_file(path);
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return unexpected_error(
        std::format("failed to open {} `{}`", subject, path.string()));
  }
  std::string output{std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>()};
  if (input.bad()) {
    return unexpected_error(
        std::format("failed to read {} `{}`", subject, path.string()));
  }
  return output;
}

VoidResult write_text_file(const fs::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    return unexpected_error(
        std::format("failed to open `{}` for writing", path.string()));
  }
  output << contents;
  if (!output) {
    return unexpected_error(std::format("failed to write `{}`", path.string()));
  }
  return {};
}

std::string hex_encode(std::span<const unsigned char> bytes) {
  constexpr std::array<char, 16> kHex = {
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
  };
  std::string output;
  output.reserve(bytes.size() * 2);
  for (const unsigned char byte : bytes) {
    const auto value = static_cast<unsigned int>(byte);
    output.push_back(kHex.at((value >> kHexShift) & kHexMask));
    output.push_back(kHex.at(value & kHexMask));
  }
  return output;
}

Result<std::string> sha256_text(std::string_view text) {
  using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  const DigestContext digest_context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!digest_context) {
    return unexpected_error("failed to allocate sha256 context");
  }
  if (EVP_DigestInit_ex(digest_context.get(), EVP_sha256(), nullptr) != 1) {
    return unexpected_error("failed to initialize sha256 context");
  }
  if (!text.empty()
      && EVP_DigestUpdate(digest_context.get(), text.data(), text.size())
             != 1) {
    return unexpected_error("failed to update sha256 digest");
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_length = 0;
  if (EVP_DigestFinal_ex(digest_context.get(), digest.data(), &digest_length)
      != 1) {
    return unexpected_error("failed to finalize sha256 digest");
  }
  if (digest_length != kSha256DigestBytes) {
    return unexpected_error("sha256 digest produced an unexpected length");
  }

  return hex_encode(
      std::span<const unsigned char>{digest.data(), digest_length});
}

bool is_lower_hex_sha256(std::string_view value) noexcept {
  if (value.size() != kSha256HexDigestLength) {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    return (character >= '0' && character <= '9')
        || (character >= 'a' && character <= 'f');
  });
}

bool is_lower_hex_prefix(std::string_view value) noexcept {
  if (value.empty() || value.size() > kSha256HexDigestLength) {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    return (character >= '0' && character <= '9')
        || (character >= 'a' && character <= 'f');
  });
}

Result<std::string>
json_from_builder(const simdjson::builder::string_builder &builder,
                  JsonBuilderMessages messages) {
  if (!builder.validate_unicode()) {
    return unexpected_error(std::string(messages.invalid_utf8_message));
  }

  std::string_view output;
  const simdjson::error_code error = builder.view().get(output);
  if (error != simdjson::SUCCESS) {
    return unexpected_error(std::format("{}: {}", messages.build_error_prefix,
                                        simdjson_error_text(error)));
  }
  return std::string(output);
}

Result<fs::path> unique_temp_dir(const fs::path &parent,
                                 TempDirOptions options) {
  std::random_device random_device;
  for (int attempt = 0; attempt < kTempDirAttempts; ++attempt) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    fs::path path = parent
                  / std::format(".tmp-{}-{}-{}-{}", options.prefix, nanos,
                                random_device(), attempt);
    const Result<bool> exists = expected_fs::exists(path);
    if (!exists) {
      return std::unexpected(exists.error());
    }
    if (!*exists) {
      return path;
    }
  }
  return unexpected_error(std::string(options.failure_message));
}

VoidResult apply_read_only_file_permissions(const fs::path &path) {
  return expected_fs::permissions(path, fs::perms::owner_read,
                                  fs::perm_options::replace);
}

VoidResult apply_read_only_directory_permissions(const fs::path &path) {
  return expected_fs::permissions(path,
                                  fs::perms::owner_read | fs::perms::owner_exec,
                                  fs::perm_options::replace);
}

VoidResult apply_writable_directory_permissions(const fs::path &path) {
  return expected_fs::permissions(
      path,
      fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
      fs::perm_options::replace);
}

} // namespace memix::detail
