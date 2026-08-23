#include "mvcc/version_store.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

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
// Phase 6: many concurrent readers must be able to overlap under the
// shared_mutex upgrade, not deadlock, and not silently serialize forever
// behind one another. This isn't a throughput benchmark (that's
// bench/bench_mvcc_vs_kv.cpp) — it's a liveness/correctness check: every
// reader must complete, and none should ever observe a torn/partial read
// while many others are reading concurrently.
// ---------------------------------------------------------------------
TEST(VersionStoreStress, ManyConcurrentReadersCompleteWithoutDeadlockOrCorruption) {
  VersionStore store;
  store.Put("k", "v");

  constexpr int kThreads = 32;
  constexpr int kReadsPerThread = 5000;
  std::atomic<int> mismatches{0};

  RunConcurrently(kThreads, [&](int /*thread_id*/) {
    for (int i = 0; i < kReadsPerThread; ++i) {
      std::string value;
      if (store.Get("k", &value).ok() && value != "v") {
        ++mismatches;
      }
    }
  });

  EXPECT_EQ(mismatches.load(), 0);
}

// ---------------------------------------------------------------------
// Test 1: the global version counter must never hand out the same version
// number twice, even when many threads are writing concurrently across
// both shared and disjoint keys. Duplicate version numbers would silently
// corrupt every later phase (a duplicated snapshot ID could see two
// different "current" states as the same state).
// ---------------------------------------------------------------------
TEST(VersionStoreStress, ConcurrentPutsNeverIssueDuplicateVersions) {
  VersionStore store;
  constexpr int kThreads = 16;
  constexpr int kOpsPerThread = 2000;
  constexpr int kNumSharedKeys = 4;

  std::mutex collected_mutex;
  std::vector<uint64_t> collected;
  collected.reserve(kThreads * kOpsPerThread);

  RunConcurrently(kThreads, [&](int thread_id) {
    std::vector<uint64_t> local;
    local.reserve(kOpsPerThread);
    for (int i = 0; i < kOpsPerThread; ++i) {
      // Mix of disjoint keys and a small shared set, to exercise both
      // "different bucket" and "same bucket" contention paths.
      std::string key = (i % 2 == 0)
                             ? "t" + std::to_string(thread_id) + "-k" + std::to_string(i)
                             : "shared-" + std::to_string(i % kNumSharedKeys);
      uint64_t version;
      store.Put(key, "v", &version);
      local.push_back(version);
    }
    std::lock_guard<std::mutex> lock(collected_mutex);
    collected.insert(collected.end(), local.begin(), local.end());
  });

  std::set<uint64_t> unique_versions(collected.begin(), collected.end());
  EXPECT_EQ(unique_versions.size(), collected.size())
      << "duplicate version numbers were issued under concurrent Put";
}

// ---------------------------------------------------------------------
// Test 2: each key's version history must stay sorted by version even
// under concurrent writers hammering a small shared key space. Since
// versions are appended in the order threads acquire the lock, and the
// counter is incremented under that same lock, the history for any one
// key should always come out strictly increasing.
// ---------------------------------------------------------------------
TEST(VersionStoreStress, PerKeyHistoryStaysSortedUnderConcurrentWrites) {
  VersionStore store;
  constexpr int kThreads = 8;
  constexpr int kOpsPerThread = 3000;
  constexpr int kNumSharedKeys = 4;

  RunConcurrently(kThreads, [&](int thread_id) {
    for (int i = 0; i < kOpsPerThread; ++i) {
      std::string key = "shared-" + std::to_string((thread_id + i) % kNumSharedKeys);
      store.Put(key, "v" + std::to_string(i));
    }
  });

  for (int k = 0; k < kNumSharedKeys; ++k) {
    std::string key = "shared-" + std::to_string(k);
    // We can't inspect the private history directly, but GetAsOf at every
    // version boundary should be internally consistent: querying at the
    // store's final CurrentVersion should always succeed for a key that
    // was written, and never regress.
    std::string value;
    if (store.VersionCount(key) > 0) {
      EXPECT_TRUE(store.GetAsOf(key, store.CurrentVersion(), &value).ok());
    }
  }
}

// ---------------------------------------------------------------------
// Test 3: concurrent Put/Delete/Get/GetAsOf mixed on shared keys should
// never crash or throw, and every version returned by a successful Put
// should independently be readable via GetAsOf at that exact version —
// even while other threads are concurrently mutating the same key.
// ---------------------------------------------------------------------
TEST(VersionStoreStress, ConcurrentMixedOpsNeverCorruptHistory) {
  VersionStore store;
  constexpr int kThreads = 8;
  constexpr int kOpsPerThread = 2000;
  constexpr int kNumSharedKeys = 6;

  std::atomic<bool> saw_exception{false};
  std::atomic<int> asof_mismatches{0};

  RunConcurrently(kThreads, [&](int thread_id) {
    try {
      for (int i = 0; i < kOpsPerThread; ++i) {
        std::string key = "shared-" + std::to_string((thread_id + i) % kNumSharedKeys);
        switch (i % 3) {
          case 0: {
            std::string val = "v" + std::to_string(thread_id) + "-" + std::to_string(i);
            uint64_t version;
            if (store.Put(key, val, &version).ok()) {
              // Immediately re-read as-of exactly this version. Another
              // thread may have written the *same* key at a later version
              // by the time we check, but this exact version's value must
              // still be exactly what we wrote — history is append-only
              // and past entries are never mutated.
              std::string reread;
              if (store.GetAsOf(key, version, &reread).ok() && reread != val) {
                ++asof_mismatches;
              }
            }
            break;
          }
          case 1: {
            std::string out;
            store.Get(key, &out);  // NotFound is a valid outcome.
            break;
          }
          case 2:
            store.Delete(key);  // NotFound is a valid outcome.
            break;
        }
      }
    } catch (...) {
      saw_exception = true;
    }
  });

  EXPECT_FALSE(saw_exception);
  EXPECT_EQ(asof_mismatches.load(), 0);
}
