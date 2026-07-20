#include "consumer/stats_reporter.h"

#include <chrono>
#include <print>

namespace ipc::consumer {

StatsReporter::StatsReporter() : thread_(&StatsReporter::Run, this) {}

StatsReporter::~StatsReporter() {
  {
    std::scoped_lock lock(mutex_);
    stopping_ = true;
  }
  condition_.notify_all();
  thread_.join();
}

void StatsReporter::RecordMessage(std::size_t frameBytes) {
  totalPackets_.fetch_add(1, std::memory_order_relaxed);
  totalBytes_.fetch_add(frameBytes, std::memory_order_relaxed);
  intervalPackets_.fetch_add(1, std::memory_order_relaxed);
  intervalBytes_.fetch_add(frameBytes, std::memory_order_relaxed);
}

void StatsReporter::Run() {
  auto next = std::chrono::steady_clock::now();

  std::unique_lock lock(mutex_);
  while (!stopping_) {
    next += std::chrono::seconds(1);
    if (condition_.wait_until(lock, next, [this] { return stopping_; })) {
      break;
    }

    std::uint64_t packets = intervalPackets_.exchange(0, std::memory_order_relaxed);
    std::uint64_t bytes = intervalBytes_.exchange(0, std::memory_order_relaxed);
    std::uint64_t total = totalPackets_.load(std::memory_order_relaxed);

    std::println("total={} pkts/s={} bytes/s={}", total, packets, bytes);
  }
}

}  // namespace ipc::consumer
