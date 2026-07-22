#include "consumer/stats_reporter.h"

#include <chrono>
#include <format>
#include <locale>
#include <print>
#include <string>

namespace ipc::consumer {

namespace {

std::string FormatThroughput(std::uint64_t bytesPerSecond) {
  constexpr double kKilo = 1000.0;

  if (bytesPerSecond < static_cast<std::uint64_t>(kKilo)) {
    return std::format("{}B", bytesPerSecond);
  }

  double value = static_cast<double>(bytesPerSecond) / kKilo;
  if (value < kKilo) {
    return std::format("{:.2f}KB", value);
  }

  value /= kKilo;
  if (value < kKilo) {
    return std::format("{:.2f}MB", value);
  }

  value /= kKilo;
  return std::format("{:.2f}GB", value);
}

std::string FormatWithThousandsSeparator(std::uint64_t value) {
  static const std::locale kLocale = [] {
    try {
      return std::locale("");
    } catch (const std::runtime_error&) {
      return std::locale::classic();  // No grouping, same as ungrouped digits.
    }
  }();

  return std::format(kLocale, "{:L}", value);
}

}  // namespace

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
  totalMessages_.fetch_add(1, std::memory_order_relaxed);
  totalBytes_.fetch_add(frameBytes, std::memory_order_relaxed);
  intervalMessages_.fetch_add(1, std::memory_order_relaxed);
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

    std::uint64_t messages = intervalMessages_.exchange(0, std::memory_order_relaxed);
    std::uint64_t bytes = intervalBytes_.exchange(0, std::memory_order_relaxed);
    std::uint64_t total = totalMessages_.load(std::memory_order_relaxed);

    std::println("total={} msgs/s={} throughput={}", FormatWithThousandsSeparator(total),
                 FormatWithThousandsSeparator(messages), FormatThroughput(bytes));
  }
}

}  // namespace ipc::consumer
