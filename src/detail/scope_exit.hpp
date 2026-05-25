#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

namespace memix {

template <typename F>
  requires std::invocable<F>
class scope_exit {
  F fn;
  bool engaged = true;

public:
  template <typename Fp>
    requires(!std::same_as<std::remove_cvref_t<Fp>, scope_exit>
             && std::constructible_from<F, Fp &&>)
  explicit scope_exit(Fp &&fn) : fn(std::forward<Fp>(fn)) {}

  scope_exit(scope_exit &&rhs) : fn(std::move(rhs.fn)), engaged(rhs.engaged) {
    rhs.release();
  }

  scope_exit(const scope_exit &) = delete;
  scope_exit &operator=(scope_exit &&) = delete;
  scope_exit &operator=(const scope_exit &) = delete;

  void release() { engaged = false; }

  ~scope_exit() {
    if (engaged)
      fn();
  }
};

template <typename F>
  requires std::invocable<F>
scope_exit(F) -> scope_exit<F>;

} // namespace memix
