#pragma once

#include "memix/fs.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

namespace memix::detail {

struct FileLockState;

class [[nodiscard]] FileLockGuard {
public:
  using Clock = std::chrono::steady_clock;

  [[nodiscard]] static FsResult<FileLockGuard>
  acquire(const fs::path &path, Clock::time_point deadline);

  FileLockGuard(FileLockGuard &&other) noexcept;
  FileLockGuard &operator=(FileLockGuard &&other) noexcept;

  FileLockGuard(const FileLockGuard &) = delete;
  FileLockGuard &operator=(const FileLockGuard &) = delete;

  ~FileLockGuard();

private:
  FileLockGuard(std::shared_ptr<FileLockState> state,
                std::unique_lock<std::timed_mutex> local_lock,
                std::intptr_t native_handle) noexcept;

  void release() noexcept;

  std::shared_ptr<FileLockState> state_;
  std::unique_lock<std::timed_mutex> local_lock_;
  std::intptr_t native_handle_ = -1;
};

} // namespace memix::detail
