#include "common/control/signal_handler.h"

#include <csignal>

namespace ipc::common {

namespace {

// SIGUSR1 is not a shutdown signal -- it exists only to make the blocked
// sigwait() in Run() return during ~SignalHandler(), so the thread can be
// joined instead of leaked on normal process exit.
sigset_t WaitSet() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGUSR1);

  return set;
}

}  // namespace

SignalHandler::SignalHandler(Controller& controller) : controller_(controller) {
  sigset_t set = WaitSet();
  pthread_sigmask(SIG_BLOCK, &set, nullptr);

  thread_ = std::thread(&SignalHandler::Run, this);
}

SignalHandler::~SignalHandler() {
  pthread_kill(thread_.native_handle(), SIGUSR1);
  thread_.join();
}

void SignalHandler::Run() {
  sigset_t set = WaitSet();

  int signalNumber = 0;
  sigwait(&set, &signalNumber);

  if (signalNumber == SIGINT || signalNumber == SIGTERM) {
    controller_.Stop();
  }
}

}  // namespace ipc::common
