#include "detail/file_lock.hpp"

#include "memix/fs.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#ifdef __APPLE__
#include <sys/fcntl.h>
#else
#include <fcntl.h>
#endif
#include <sys/file.h> // NOLINT(misc-include-cleaner)
#include <unistd.h>
#endif

namespace memix::detail {

struct FileLockState {
  std::timed_mutex mutex;
};

namespace {

constexpr auto kRetryDelay = std::chrono::milliseconds(1);
constexpr std::intptr_t kInvalidNativeHandle = -1;
#ifndef _WIN32
constexpr int kLockFileMode = 0666; // NOLINT(readability-magic-numbers)
#endif

using LockRegistry =
    std::unordered_map<std::string, std::weak_ptr<FileLockState>>;

std::mutex &lock_registry_mutex() {
  static std::mutex mutex;
  return mutex;
}

LockRegistry &lock_registry() {
  static LockRegistry registry;
  return registry;
}

FsResult<std::shared_ptr<FileLockState>> lock_state_for(const fs::path &path) {
  FsResult<fs::path> canonical = fs::weakly_canonical(path);
  if (!canonical) {
    return fs::fs_error(canonical.error());
  }
  const std::string key = canonical->generic_string();
  const std::scoped_lock registry_lock(lock_registry_mutex());
  std::weak_ptr<FileLockState> &entry = lock_registry()[key];
  std::shared_ptr<FileLockState> state = entry.lock();
  if (!state) {
    state = std::make_shared<FileLockState>();
    entry = state;
  }
  return state;
}

FsError lock_timeout_error(const fs::path &path) {
  return fs::make_fs_error("timed out acquiring file lock",
                           std::make_error_code(std::errc::timed_out), path);
}

void wait_before_retry(FileLockGuard::Clock::time_point deadline) {
  const FileLockGuard::Clock::time_point now = FileLockGuard::Clock::now();
  if (now < deadline) {
    std::this_thread::sleep_until(std::min(deadline, now + kRetryDelay));
  }
}

#ifdef _WIN32

std::intptr_t native_handle_value(HANDLE handle) noexcept {
  return reinterpret_cast<std::intptr_t>(handle);
}

HANDLE windows_handle(std::intptr_t handle) noexcept {
  return reinterpret_cast<HANDLE>(handle);
}

FsResult<std::intptr_t>
try_acquire_native_lock(const fs::path &path,
                        FileLockGuard::Clock::time_point deadline) {
  while (true) {
    const HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      const DWORD native_error = GetLastError();
      if (native_error != ERROR_SHARING_VIOLATION) {
        return fs::fs_error("failed to open file lock",
                            std::error_code(static_cast<int>(native_error),
                                            std::system_category()),
                            path);
      }
    } else {
      OVERLAPPED range{};
      if (LockFileEx(handle,
                     LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1,
                     0, &range)
          != 0) {
        return native_handle_value(handle);
      }
      const DWORD native_error = GetLastError();
      std::ignore = CloseHandle(handle);
      if (native_error != ERROR_LOCK_VIOLATION
          && native_error != ERROR_SHARING_VIOLATION) {
        return fs::fs_error("failed to acquire file lock",
                            std::error_code(static_cast<int>(native_error),
                                            std::system_category()),
                            path);
      }
    }
    if (FileLockGuard::Clock::now() >= deadline) {
      return fs::fs_error(lock_timeout_error(path));
    }
    wait_before_retry(deadline);
  }
}

void release_native_lock(std::intptr_t native_handle) noexcept {
  const HANDLE handle = windows_handle(native_handle);
  OVERLAPPED range{};
  std::ignore = UnlockFileEx(handle, 0, 1, 0, &range);
  std::ignore = CloseHandle(handle);
}

#else

FsResult<std::intptr_t>
try_acquire_native_lock(const fs::path &path,
                        FileLockGuard::Clock::time_point deadline) {
  while (true) {
    // POSIX open is variadic when O_CREAT supplies the file mode.
    int descriptor =
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
        ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, kLockFileMode);
    while (descriptor < 0 && errno == EINTR) {
      descriptor =
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
          ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, kLockFileMode);
    }
    if (descriptor < 0) {
      const int native_error = errno;
      return fs::fs_error(
          "failed to open file lock",
          std::error_code(native_error, std::generic_category()), path);
    }

    int result =
        ::flock(descriptor, LOCK_EX | LOCK_NB); // NOLINT(misc-include-cleaner)
    while (result != 0 && errno == EINTR) {
      result = ::flock(descriptor,
                       LOCK_EX | LOCK_NB); // NOLINT(misc-include-cleaner)
    }
    if (result == 0) {
      return static_cast<std::intptr_t>(descriptor);
    }

    const int native_error = errno;
    std::ignore = ::close(descriptor);
    if (native_error != EWOULDBLOCK
#if EAGAIN != EWOULDBLOCK
        && native_error != EAGAIN
#endif
    ) {
      return fs::fs_error(
          "failed to acquire file lock",
          std::error_code(native_error, std::generic_category()), path);
    }
    if (FileLockGuard::Clock::now() >= deadline) {
      return fs::fs_error(lock_timeout_error(path));
    }
    wait_before_retry(deadline);
  }
}

void release_native_lock(std::intptr_t native_handle) noexcept {
  const int descriptor = static_cast<int>(native_handle);
  std::ignore = ::flock(descriptor, LOCK_UN); // NOLINT(misc-include-cleaner)
  std::ignore = ::close(descriptor);
}

#endif

} // namespace

FsResult<FileLockGuard>
FileLockGuard::acquire(const fs::path &path, Clock::time_point deadline) {
  FsResult<std::shared_ptr<FileLockState>> state = lock_state_for(path);
  if (!state) {
    return fs::fs_error(state.error());
  }

  std::unique_lock<std::timed_mutex> local_lock((*state)->mutex,
                                                std::defer_lock);
  if (!local_lock.try_lock_until(deadline)) {
    return fs::fs_error(lock_timeout_error(path));
  }

  FsResult<std::intptr_t> native_handle =
      try_acquire_native_lock(path, deadline);
  if (!native_handle) {
    return fs::fs_error(native_handle.error());
  }
  return FileLockGuard(std::move(*state), std::move(local_lock),
                       *native_handle);
}

FileLockGuard::FileLockGuard(std::shared_ptr<FileLockState> state,
                             std::unique_lock<std::timed_mutex> local_lock,
                             std::intptr_t native_handle) noexcept
    : state_(std::move(state)), local_lock_(std::move(local_lock)),
      native_handle_(native_handle) {}

FileLockGuard::FileLockGuard(FileLockGuard &&other) noexcept
    : state_(std::move(other.state_)),
      local_lock_(std::move(other.local_lock_)),
      native_handle_(
          std::exchange(other.native_handle_, kInvalidNativeHandle)) {}

FileLockGuard &FileLockGuard::operator=(FileLockGuard &&other) noexcept {
  if (this != &other) {
    release();
    local_lock_ = std::unique_lock<std::timed_mutex>{};
    state_.reset();
    state_ = std::move(other.state_);
    local_lock_ = std::move(other.local_lock_);
    native_handle_ = std::exchange(other.native_handle_, kInvalidNativeHandle);
  }
  return *this;
}

FileLockGuard::~FileLockGuard() { release(); }

void FileLockGuard::release() noexcept {
  if (native_handle_ != kInvalidNativeHandle) {
    release_native_lock(native_handle_);
    native_handle_ = kInvalidNativeHandle;
  }
  if (local_lock_.owns_lock()) {
    local_lock_.unlock();
  }
}

} // namespace memix::detail
