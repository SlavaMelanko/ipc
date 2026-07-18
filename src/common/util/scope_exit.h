#ifndef IPC_COMMON_UTIL_SCOPE_EXIT_H_
#define IPC_COMMON_UTIL_SCOPE_EXIT_H_

#include <type_traits>
#include <utility>

namespace ipc::common {

// Runs a callable when it goes out of scope, unless Dismiss() was called.
// Stand-in for std::scope_exit (C++26, not available in this C++23 project).
template <typename F>
class [[nodiscard]] ScopeExit {
 public:
  static_assert(std::is_nothrow_invocable_v<F&>,
                "ScopeExit's action must not throw: it runs from ~ScopeExit, "
                "and an exception escaping a destructor calls std::terminate");

  explicit ScopeExit(F action) : action_(std::move(action)) {}

  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;
  ScopeExit(ScopeExit&&) = delete;
  ScopeExit& operator=(ScopeExit&&) = delete;

  ~ScopeExit() noexcept {
    if (armed_) {
      action_();
    }
  }

  void Dismiss() noexcept { armed_ = false; }

 private:
  F action_;
  bool armed_ = true;
};

template <typename F>
ScopeExit(F) -> ScopeExit<F>;

}  // namespace ipc::common

#endif  // IPC_COMMON_UTIL_SCOPE_EXIT_H_
