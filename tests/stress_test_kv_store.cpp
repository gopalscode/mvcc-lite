#include "mvcc/kv_store.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using mvcc::KVStore;

namespace {

// Small helper: run `fn` on `num_threads` threads, each passed its thread
// index, and join them all.
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
// Test 1: disjoint-key concurrency.
//
// Each thread owns its own private range of keys, so there is no logical
// contention between threads — but every Put/Get still goes through the
// *same* store and the *same* mutex. This is designed to catch:
//   - data races / heap corruption in the underlying unordered_map
//     (run this binary with -DMVCC_SANITIZE=thread to get real teeth)
//   - deadlocks or lost updates from a broken locking implementation
// If KVStore's locking is correct, every thread's writes must be visible,
// intact, and none should be dropped or corrupted by another thread's
// concurrent, unrelated operations.
// ---------------------------------------------------------------------
TEST(KVStoreStress, ConcurrentDisjointKeysNoCorruption) {
  KVStore store;
  constexpr int kThreads = 16;
  constexpr int kOpsPerThread = 2000;

  RunConcurrently(kThreads, [&](int thread_id) {
    for (int i = 0; i < kOpsPerThread; ++i) {
      std::string key = "t" + std::to_string(thread_id) + "-k" + std::to_string(i);
      std::string value = "v" + std::to_string(i);
      ASSERT_TRUE(store.Put(key, value).ok());
    }
  });

  EXPECT_EQ(store.Size(), static_cast<size_t>(kThreads * kOpsPerThread));

  RunConcurrently(kThreads, [&](int thread_id) {
    for (int i = 0; i < kOpsPerThread; ++i) {
      std::string key = "t" + std::to_string(thread_id) + "-k" + std::to_string(i);
      std::string expected = "v" + std::to_string(i);
      std::string actual;
      ASSERT_TRUE(store.Get(key, &actual).ok());
      ASSERT_EQ(actual, expected);
    }
  });
}

// ---------------------------------------------------------------------
// Test 2: concurrent Put/Get/Delete mixed on a *shared* small key space.
//
// This doesn't assert much about final values (many outcomes are valid
// depending on interleaving) — it asserts that nothing crashes, no
// exception escapes, and the store stays internally consistent (Size()
// matches Contains() reality). It's a "does this fall over under chaos"
// smoke test.
// ---------------------------------------------------------------------
TEST(KVStoreStress, ConcurrentMixedOpsOnSharedKeysStaysConsistent) {
  KVStore store;
  constexpr int kThreads = 8;
  constexpr int kOpsPerThread = 5000;
  constexpr int kNumSharedKeys = 8;

  std::atomic<bool> saw_exception{false};

  RunConcurrently(kThreads, [&](int thread_id) {
    try {
      for (int i = 0; i < kOpsPerThread; ++i) {
        std::string key = "shared-" + std::to_string((thread_id + i) % kNumSharedKeys);
        switch (i % 3) {
          case 0:
            store.Put(key, "v" + std::to_string(i));
            break;
          case 1: {
            std::string out;
            store.Get(key, &out);  // NotFound is a valid outcome here.
            break;
          }
          case 2:
            store.Delete(key);  // NotFound is a valid outcome here.
            break;
        }
      }
    } catch (...) {
      saw_exception = true;
    }
  });

  EXPECT_FALSE(saw_exception);

  // Internal consistency check: every key Contains() claims exists must
  // actually be Gettable, and Size() must match the count of keys that
  // Contains() confirms.
  size_t confirmed = 0;
  for (int i = 0; i < kNumSharedKeys; ++i) {
    std::string key = "shared-" + std::to_string(i);
    if (store.Contains(key)) {
      std::string value;
      EXPECT_TRUE(store.Get(key, &value).ok());
      ++confirmed;
    }
  }
  EXPECT_EQ(confirmed, store.Size());
}

// ---------------------------------------------------------------------
// Test 3 (documentation-by-test, not a pass/fail correctness check):
// demonstrates that Phase 1's Get-then-Put is NOT an atomic
// read-modify-write, so concurrent increments lose updates.
//
// This is a KNOWN, ACCEPTED limitation of Phase 1 — Get() and Put() each
// lock independently, so two threads can interleave as:
//   T1: Get("counter") -> 5
//   T2: Get("counter") -> 5
//   T1: Put("counter", 6)
//   T2: Put("counter", 6)   // T1's increment is lost
//
// We don't ASSERT the count is wrong (that would make the test flaky/
// meaningless — under low contention it might occasionally come out
// right). Instead we report how many updates were lost, as a concrete,
// reproducible number that motivates why Phase 3 (transactions with
// snapshot isolation) and Phase 5 (real concurrency control) exist:
// composing two atomic operations does not give you an atomic operation.
// ---------------------------------------------------------------------
TEST(KVStoreStress, DocumentsLostUpdatesUnderReadModifyWrite) {
  KVStore store;
  ASSERT_TRUE(store.Put("counter", "0").ok());

  constexpr int kThreads = 8;
  constexpr int kIncrementsPerThread = 500;
  constexpr int kExpected = kThreads * kIncrementsPerThread;

  RunConcurrently(kThreads, [&](int /*thread_id*/) {
    for (int i = 0; i < kIncrementsPerThread; ++i) {
      std::string current;
      store.Get("counter", &current);
      int n = std::stoi(current);
      // Deliberately widen the window between the read and the write.
      // Without this, a single-core sandbox (or a very fast, lucky
      // scheduler) can run each thread's Get+Put back-to-back and never
      // exhibit the race in practice, even though it is always possible.
      // This yield makes the race reproducible; it does not change what
      // Phase 1's API guarantees.
      std::this_thread::yield();
      store.Put("counter", std::to_string(n + 1));
    }
  });

  std::string final_value;
  store.Get("counter", &final_value);
  int actual = std::stoi(final_value);

  std::fprintf(
      stderr,
      "[KVStoreStress.DocumentsLostUpdatesUnderReadModifyWrite] expected=%d "
      "actual=%d lost_updates=%d (Phase 1 has no atomic read-modify-write; "
      "this is expected and motivates Phase 3/5)\n",
      kExpected, actual, kExpected - actual);

  // The only real invariant we can assert: lost updates only ever move the
  // count *down* from the race-free ideal, never above it or negative.
  EXPECT_LE(actual, kExpected);
  EXPECT_GE(actual, 0);
}
