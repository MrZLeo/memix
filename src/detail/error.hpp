#pragma once

#include "memix/error.hpp"

#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace memix::detail {

struct CommonError {
  std::string message;
  ErrorKind kind = ErrorKind::Unknown;
};

template <typename Value>
using CommonResult = std::expected<Value, CommonError>;

template <typename TargetError, typename SourceError>
TargetError translate_error(SourceError error) {
  TargetError translated;
  translated.message = std::move(error.message);
  if constexpr (requires(TargetError target, SourceError source) {
                  target.filesystem_error = std::move(source.filesystem_error);
                }) {
    translated.filesystem_error = std::move(error.filesystem_error);
  }
  if constexpr (requires(TargetError target, SourceError source) {
                  target.kind = source.kind;
                }) {
    translated.kind = error.kind;
  }
  return translated;
}

template <typename TargetError>
TargetError translate_error(FsError error) {
  TargetError translated;
  translated.message = error.message;
  if constexpr (requires(TargetError target, FsError filesystem_error) {
                  target.filesystem_error = std::move(filesystem_error);
                }) {
    translated.filesystem_error = std::move(error);
  }
  if constexpr (requires(TargetError target) {
                  target.kind = ErrorKind::Filesystem;
                }) {
    translated.kind = ErrorKind::Filesystem;
  }
  return translated;
}

inline std::unexpected<CommonError> common_error(std::string message) {
  return std::unexpected(CommonError{.message = std::move(message)});
}

inline std::unexpected<CommonError>
common_error(ErrorKind kind, std::string message) {
  return std::unexpected(
      CommonError{.message = std::move(message), .kind = kind});
}

inline CommonError
with_error_context(CommonError error, std::string_view context,
                   ErrorKind kind = ErrorKind::Unknown) {
  error.message = std::format("{}: {}", context, error.message);
  if (kind != ErrorKind::Unknown)
    error.kind = kind;

  return error;
}

} // namespace memix::detail
