#ifndef IPC_CONSUMER_STATS_REPORTER_H_
#define IPC_CONSUMER_STATS_REPORTER_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

namespace ipc::consumer {

// Prints one "total=<n> pkts/s=<n> bytes/s=<n>" line per second on a
// dedicated thread, driven by a steady_clock reference point rather than
// repeated sleep_for(1s) (which drifts -- see AGENTS.md's StatsReporter
// section). Cumulative and interval counters are separate atomic pairs:
// resetting one to compute a rate must never disturb the running total.
class StatsReporter {
 public:
  StatsReporter();
  ~StatsReporter();

  StatsReporter(const StatsReporter&) = delete;
  StatsReporter& operator=(const StatsReporter&) = delete;

  // Called once per successfully received message; frameBytes is the full
  // sizeof(Header) + payloadSize, not payload alone (see AGENTS.md's
  // StatsReporter section on why frame bytes, not payload bytes).
  void RecordMessage(std::size_t frameBytes);

 private:
  void Run();

  std::atomic<std::uint64_t> totalPackets_ = 0;
  std::atomic<std::uint64_t> totalBytes_ = 0;
  std::atomic<std::uint64_t> intervalPackets_ = 0;
  std::atomic<std::uint64_t> intervalBytes_ = 0;

  std::mutex mutex_;
  std::condition_variable condition_;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace ipc::consumer

#endif  // IPC_CONSUMER_STATS_REPORTER_H_
