#pragma once

#include "memix/fs.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

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
using StoreResult = std::expected<Value, StoreError>;

using StoreVoid = std::expected<void, StoreError>;

template <typename Value>
using ViewResult = std::expected<Value, ViewError>;

using ViewVoid = std::expected<void, ViewError>;

} // namespace memix
