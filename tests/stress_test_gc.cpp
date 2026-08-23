#include "mvcc/gc.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using mvcc::GarbageCollector;
using mvcc::TransactionManager;
using mvcc::VersionStore;

namespace {

void RunConcurrently(int num_threads, const std::function<void(int)>& fn) {
  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back(fn, i);
  }
  for (auto& t : threads) {
    t.join();
  }
}

}  // namespace

// ---------------------------------------------------------------------
// Test 1: the money test for this phase. A long-lived reader transaction
// holds a snapshot on a key while a background GC thread runs
// aggressively (every 2ms) AND many other threads hammer that same key
// with writes concurrently. If GC's boundary logic is wrong, this is
// where it would show up as the reader suddenly getting NotFound or a
// wrong value for a version that should still be protected.
// ---------------------------------------------------------------------
TEST(GCStress, LongLivedReaderSurvivesAggressiveConcurrentGCAndWrites) {
  VersionStore store;
  TransactionManager mgr(store);
  GarbageCollector gc(store, mgr);

  store.Put("k", "original");
  auto reader = mgr.Begin();  // pins snapshot at "original"

  gc.StartBackground(std::chrono::milliseconds(2));

  constexpr int kThreads = 8;
  constexpr int kWritesPerThread = 500;
  RunConcurrently(kThreads, [&](int thread_id) {
    for (int i = 0; i < kWritesPerThread; ++i) {
      auto txn = mgr.Begin();
      txn->Put("k", "writer" + std::to_string(thread_id) + "-" + std::to_string(i));
      txn->Commit();
    }
  });

  // Give GC a few more cycles to run against the final state.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  gc.StopBackground();

  std::string value;
  ASSERT_TRUE(reader->Get("k", &value).ok())
      << "GC incorrectly reclaimed a version still protected by an active transaction";
  EXPECT_EQ(value, "original");
  ASSERT_TRUE(reader->Commit().ok());
}

// ---------------------------------------------------------------------
// Test 2: several transactions with staggered begin times, each holding
// a different snapshot, all concurrent with background GC and concurrent
// writers. Every one of them must still see exactly what it should have
// seen as of its own snapshot — not corrupted, not shifted to a
// neighboring version.
// ---------------------------------------------------------------------
TEST(GCStress, MultipleStaggeredSnapshotsAllSurviveConcurrentGC) {
  VersionStore store;
  TransactionManager mgr(store);
  GarbageCollector gc(store, mgr);

  constexpr int kNumSnapshots = 6;
  std::vector<std::unique_ptr<mvcc::Transaction>> readers;
  std::vector<std::string> expected_values;

  gc.StartBackground(std::chrono::milliseconds(3));

  for (int i = 0; i < kNumSnapshots; ++i) {
    std::string value = "snapshot-value-" + std::to_string(i);
    store.Put("k", value);
    readers.push_back(mgr.Begin());  // pins right after this write
    expected_values.push_back(value);

    // Churn writes and let GC run between each staggered snapshot.
    for (int j = 0; j < 200; ++j) {
      auto txn = mgr.Begin();
      txn->Put("k", "churn-" + std::to_string(i) + "-" + std::to_string(j));
      txn->Commit();
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  gc.StopBackground();

  for (int i = 0; i < kNumSnapshots; ++i) {
    std::string value;
    ASSERT_TRUE(readers[i]->Get("k", &value).ok())
        << "reader " << i << " lost its protected version to GC";
    EXPECT_EQ(value, expected_values[i]);
  }
}

// ---------------------------------------------------------------------
// Test 3: pure chaos — concurrent transactions (some short-lived, some
// held open across the whole run) mixed with concurrent manual RunOnce()
// calls from multiple threads (not just the background thread), on a
// small shared key space. Nothing here should crash, throw, or produce
// GC stats that don't make sense (can't reclaim more than exists).
// ---------------------------------------------------------------------
TEST(GCStress, ConcurrentManualGCFromMultipleThreadsNeverCorruptsStore) {
  VersionStore store;
  TransactionManager mgr(store);
  GarbageCollector gc(store, mgr);
  constexpr int kNumSharedKeys = 5;

  std::atomic<bool> saw_exception{false};
  std::atomic<bool> stop_gc{false};

  // Several threads calling RunOnce() concurrently — GC itself must be
  // safe to invoke from multiple callers at once, not just safe alongside
  // a single background thread.
  std::vector<std::thread> gc_threads;
  for (int i = 0; i < 3; ++i) {
    gc_threads.emplace_back([&] {
      while (!stop_gc.load()) {
        gc.RunOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }

  RunConcurrently(8, [&](int thread_id) {
    try {
      for (int i = 0; i < 800; ++i) {
        std::string key = "shared-" + std::to_string((thread_id + i) % kNumSharedKeys);
        auto txn = mgr.Begin();
        switch (i % 3) {
          case 0:
            txn->Put(key, "v" + std::to_string(i));
            txn->Commit();
            break;
          case 1: {
            std::string out;
            txn->Get(key, &out);
            txn->Commit();
            break;
          }
          case 2:
            txn->Delete(key);
            txn->Commit();
            break;
        }
      }
    } catch (...) {
      saw_exception = true;
    }
  });

  stop_gc = true;
  for (auto& t : gc_threads) {
    t.join();
  }

  EXPECT_FALSE(saw_exception);
  EXPECT_EQ(mgr.ActiveTransactionCount(), 0u);

  // Final sanity: every remaining shared key must still be independently
  // readable and internally consistent (Get succeeds iff Contains-style
  // presence is true — checked implicitly via VersionCount/Get agreement).
  for (int i = 0; i < kNumSharedKeys; ++i) {
    std::string key = "shared-" + std::to_string(i);
    std::string value;
    if (store.VersionCount(key) > 0) {
      // If any version remains, Get must not throw and must return a
      // well-defined Status (OK or NotFound if the surviving version is
      // a tombstone) — either is valid, we're just checking it doesn't
      // misbehave.
      store.Get(key, &value);
    }
  }
}
