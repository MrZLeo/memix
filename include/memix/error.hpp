#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace memix {

enum class ErrorKind : std::uint8_t {
  Unknown,
  Filesystem,
  InvalidArgument,
  InvalidData,
  NotFound,
  ContextPathNotFound,
  AlreadyExists,
  Conflict,
  Locked,
};

struct FsError {
  std::string message;
  std::error_code code;
  std::string operation;
  std::vector<std::filesystem::path> paths;
};

struct StoreError {
  std::string message;
  std::optional<FsError> filesystem_error;
  ErrorKind kind = ErrorKind::Unknown;
};

struct ViewError {
  std::string message;
  std::optional<FsError> filesystem_error;
  ErrorKind kind = ErrorKind::Unknown;
};

template <typename Value>
using FsResult = std::expected<Value, FsError>;

using FsVoid = std::expected<void, FsError>;

template <typename Value>
using StoreResult = std::expected<Value, StoreError>;

using StoreVoid = std::expected<void, StoreError>;

template <typename Value>
using ViewResult = std::expected<Value, ViewError>;

using ViewVoid = std::expected<void, ViewError>;

} // namespace memix
