#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "mvcc/transaction.h"
#include "mvcc/version_store.h"

namespace mvcc {

// GarbageCollector ties VersionStore and TransactionManager together to
// answer the one question GC actually needs answered: "what's the oldest
// version any live transaction could still ask for?" — and then acts on
// it.
//
// CORRECTNESS ARGUMENT (this is the part worth being able to explain,
// not just the part worth having tests for):
//
// RunOnce() computes `boundary = txn_manager_.OldestActiveSnapshot()` and
// then calls `store_.CollectGarbage(boundary)`. Between those two calls,
// the boundary could theoretically go stale — but only in the direction
// that's safe:
//   - Any transaction that was already active when we read the boundary
//     has a snapshot >= boundary by definition (boundary is the min over
//     exactly those transactions).
//   - Any transaction that begins *after* we read the boundary gets a
//     snapshot equal to the store's current version at that later moment
//     — and the store's version number only ever increases. So a
//     brand-new transaction's snapshot is always >= any boundary computed
//     earlier.
// Either way, no transaction — already active or yet to begin — can ever
// hold a snapshot older than a `boundary` value this class has already
// acted on. That's what makes it safe to compute the boundary and run the
// actual collection as two separate, non-atomic steps rather than
// requiring one giant lock across both TransactionManager and
// VersionStore.
//
// HONEST LIMITATION: RunOnce() performs a full sweep of every key in the
// store, and VersionStore::CollectGarbage holds the store's single mutex
// for the entire sweep — meaning every Get/Put/Delete/transaction commit
// anywhere in the store blocks until a GC pass finishes. This is a
// "stop-the-world" collector: correct, simple, and easy to reason about,
// but not incremental and not concurrent with other store operations.
// For a store with a very large number of keys this would show up as a
// periodic latency spike. Making GC incremental (e.g. sweeping a bounded
// number of keys per call, or moving to a locking/sharding scheme where
// GC doesn't need the whole-store lock) is a natural next step but is
// explicitly out of scope here — it's a Phase 5-adjacent concern, since
// it's really the same "the whole store shares one lock" limitation that
// phase is meant to address, not something specific to GC.
class GarbageCollector {
 public:
  GarbageCollector(VersionStore& store, TransactionManager& txn_manager)
      : store_(store), txn_manager_(txn_manager) {}

  ~GarbageCollector();

  GarbageCollector(const GarbageCollector&) = delete;
  GarbageCollector& operator=(const GarbageCollector&) = delete;

  // Runs one GC pass synchronously, right now, using the current
  // OldestActiveSnapshot() as the safety boundary. Safe to call from any
  // thread, including while a background GC thread (below) is also
  // running — both go through the same mutex-protected VersionStore
  // operations, so they simply serialize with each other like any other
  // two callers would.
  GCStats RunOnce();

  // Starts a background thread that calls RunOnce() every `interval`,
  // until StopBackground() is called or this object is destroyed.
  // No-op if a background thread is already running.
  void StartBackground(std::chrono::milliseconds interval);

  // Stops the background thread if one is running, waiting for its
  // current sleep/pass to finish before returning. No-op (returns
  // immediately) if no background thread is running.
  void StopBackground();

  bool IsBackgroundRunning() const { return running_.load(); }

  // Cumulative totals across every RunOnce() call this object has made,
  // whether triggered manually or by the background thread.
  GCStats CumulativeStats() const;

 private:
  void BackgroundLoop(std::chrono::milliseconds interval);

  VersionStore& store_;
  TransactionManager& txn_manager_;

  mutable std::mutex stats_mutex_;
  GCStats cumulative_stats_;

  std::thread background_thread_;
  std::atomic<bool> running_{false};

  // A condition_variable, not plain sleep_for, so StopBackground() can
  // wake the loop immediately instead of waiting out the rest of a
  // possibly-long interval before it notices the stop request.
  std::mutex cv_mutex_;
  std::condition_variable cv_;
  bool stop_requested_ = false;
};

}  // namespace mvcc
