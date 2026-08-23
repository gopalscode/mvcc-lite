#include "mvcc/gc.h"

namespace mvcc {

GarbageCollector::~GarbageCollector() { StopBackground(); }

GCStats GarbageCollector::RunOnce() {
  uint64_t boundary = txn_manager_.OldestActiveSnapshot();
  GCStats pass_stats = store_.CollectGarbage(boundary);

  std::lock_guard<std::mutex> lock(stats_mutex_);
  cumulative_stats_.versions_reclaimed += pass_stats.versions_reclaimed;
  cumulative_stats_.keys_removed += pass_stats.keys_removed;
  return pass_stats;
}

void GarbageCollector::StartBackground(std::chrono::milliseconds interval) {
  if (running_.load()) {
    return;  // already running
  }

  {
    std::lock_guard<std::mutex> lock(cv_mutex_);
    stop_requested_ = false;
  }
  running_.store(true);
  background_thread_ = std::thread(&GarbageCollector::BackgroundLoop, this, interval);
}

void GarbageCollector::StopBackground() {
  if (!running_.load()) {
    return;  // not running — includes the case where it was never started
  }

  {
    std::lock_guard<std::mutex> lock(cv_mutex_);
    stop_requested_ = true;
  }
  cv_.notify_all();  // wake the loop immediately instead of waiting out its sleep

  if (background_thread_.joinable()) {
    background_thread_.join();
  }
  running_.store(false);
}

void GarbageCollector::BackgroundLoop(std::chrono::milliseconds interval) {
  while (true) {
    RunOnce();

    std::unique_lock<std::mutex> lock(cv_mutex_);
    // wait_for returns early if notified (StopBackground) or if the
    // interval elapses naturally — either way we re-check stop_requested_
    // rather than trusting the return value, since a spurious wakeup is
    // also possible.
    cv_.wait_for(lock, interval, [this] { return stop_requested_; });
    if (stop_requested_) {
      return;
    }
  }
}

GCStats GarbageCollector::CumulativeStats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return cumulative_stats_;
}

}  // namespace mvcc
