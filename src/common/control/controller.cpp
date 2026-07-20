#include "common/control/controller.h"

namespace ipc::common {

void Controller::Pause() {
  {
    std::scoped_lock lock(mutex_);
    if (state_ == RunState::kRunning) {
      state_ = RunState::kPaused;
    }
  }
  condition_.notify_all();
}

void Controller::Resume() {
  {
    std::scoped_lock lock(mutex_);
    if (state_ == RunState::kPaused) {
      state_ = RunState::kRunning;
    }
  }
  condition_.notify_all();
}

void Controller::Stop() {
  {
    std::scoped_lock lock(mutex_);
    state_ = RunState::kStopped;
  }
  condition_.notify_all();
}

RunState Controller::State() const {
  std::scoped_lock lock(mutex_);
  return state_;
}

bool Controller::WaitIfPaused() {
  std::unique_lock lock(mutex_);
  condition_.wait(lock, [this] { return state_ != RunState::kPaused; });
  return state_ != RunState::kStopped;
}

}  // namespace ipc::common
