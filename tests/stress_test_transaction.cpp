#include "mvcc/gc.h"
#include "mvcc/transaction.h"

#include <atomic>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using mvcc::GarbageCollector;
using mvcc::RunWithRetry;
using mvcc::Status;
using mvcc::Transaction;
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
// Test 1: many threads, each running full begin/write/commit cycles on
// disjoint keys concurrently. Nothing should crash, every commit should
// succeed, and by the end every write from every thread should be visible.
// This is the "does the transaction layer work under real concurrency"
// smoke test.
// ---------------------------------------------------------------------
TEST(TransactionStress, ConcurrentDisjointKeyTransactionsAllCommitCleanly) {
  VersionStore store;
  TransactionManager mgr(store);
  constexpr int kThreads = 16;
  constexpr int kTxnsPerThread = 500;

  std::atomic<int> commit_failures{0};

  RunConcurrently(kThreads, [&](int thread_id) {
    for (int i = 0; i < kTxnsPerThread; ++i) {
      auto txn = mgr.Begin();
      std::string key = "t" + std::to_string(thread_id) + "-k" + std::to_string(i);
      txn->Put(key, "v" + std::to_string(i));
      if (!txn->Commit().ok()) {
        ++commit_failures;
      }
    }
  });

  EXPECT_EQ(commit_failures.load(), 0);
  EXPECT_EQ(mgr.ActiveTransactionCount(), 0u);

  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kTxnsPerThread; ++i) {
      std::string key = "t" + std::to_string(t) + "-k" + std::to_string(i);
      std::string value;
      ASSERT_TRUE(store.Get(key, &value).ok());
      EXPECT_EQ(value, "v" + std::to_string(i));
    }
  }
}

// ---------------------------------------------------------------------
// Test 2: a long-lived reader must keep seeing its original snapshot even
// while many other threads race ahead committing writes concurrently.
// This is the actual guarantee snapshot isolation is supposed to provide,
// under real contention rather than a single-threaded test.
// ---------------------------------------------------------------------
TEST(TransactionStress, LongLivedSnapshotIsUnaffectedByConcurrentCommits) {
  VersionStore store;
  store.Put("k", "original");
  TransactionManager mgr(store);

  auto reader = mgr.Begin();  // pins snapshot at "original"

  constexpr int kThreads = 8;
  constexpr int kWritesPerThread = 1000;
  RunConcurrently(kThreads, [&](int thread_id) {
    for (int i = 0; i < kWritesPerThread; ++i) {
      auto txn = mgr.Begin();
      txn->Put("k", "writer" + std::to_string(thread_id) + "-" + std::to_string(i));
      txn->Commit();
    }
  });

  std::string value;
  ASSERT_TRUE(reader->Get("k", &value).ok());
  EXPECT_EQ(value, "original");
  ASSERT_TRUE(reader->Commit().ok());  // read-only commit, always clean
}

// ---------------------------------------------------------------------
// Test 3 (this replaces what used to be a documented gap): concurrent
// transactions that read-then-write the SAME key without any retry logic
// now correctly conflict instead of silently clobbering each other.
// "First committer wins" gives an airtight guarantee here: every
// SUCCESSFUL commit's increment is guaranteed to have been observed
// against the truly-current value at the moment it committed (because if
// it hadn't been, validation would have caught it) — so the final counter
// value must equal exactly the number of commits that reported success.
// Zero lost updates among the ones that succeeded; the rest report
// Status::Conflict() to their caller instead of vanishing silently.
// ---------------------------------------------------------------------
TEST(TransactionStress, DirectConflictingCommitsAreDetectedNotSilentlyLost) {
  VersionStore store;
  store.Put("counter", "0");
  TransactionManager mgr(store);

  constexpr int kThreads = 8;
  constexpr int kAttemptsPerThread = 300;

  std::atomic<int> commit_successes{0};
  std::atomic<int> commit_conflicts{0};

  RunConcurrently(kThreads, [&](int /*thread_id*/) {
    for (int i = 0; i < kAttemptsPerThread; ++i) {
      auto txn = mgr.Begin();
      std::string current;
      txn->Get("counter", &current);
      int n = std::stoi(current);
      // Same deliberate widening as Phase 1/3's lost-update tests: makes
      // the race reliably observable rather than schedule-dependent.
      std::this_thread::yield();
      txn->Put("counter", std::to_string(n + 1));
      Status s = txn->Commit();
      if (s.ok()) {
        ++commit_successes;
      } else {
        ASSERT_TRUE(s.is_conflict()) << "unexpected non-conflict failure: " << s.ToString();
        ++commit_conflicts;
      }
    }
  });

  EXPECT_EQ(commit_successes.load() + commit_conflicts.load(), kThreads * kAttemptsPerThread);
  // The headline proof: at least some conflicts actually occurred (the
  // race is real and being exercised)...
  EXPECT_GT(commit_conflicts.load(), 0);

  std::string final_value;
  store.Get("counter", &final_value);
  int actual = std::stoi(final_value);

  std::fprintf(stderr,
               "[TransactionStress.DirectConflictingCommitsAreDetectedNotSilentlyLost] "
               "successes=%d conflicts=%d final_counter=%d (must equal successes)\n",
               commit_successes.load(), commit_conflicts.load(), actual);

  // ...and every commit that DID report success is fully accounted for:
  // zero lost updates among the successful ones.
  EXPECT_EQ(actual, commit_successes.load());
}

// ---------------------------------------------------------------------
// Test 4: the actual resolution of Phase 3's documented gap. Using
// RunWithRetry — the intended way to drive OCC transactions under
// contention — every single increment eventually lands. Compare this
// directly to Phase 3's DocumentsUndetectedWriteWriteConflicts, which
// showed ~90% of increments silently vanishing under the same shape of
// contention. Here, the final count is exact.
// ---------------------------------------------------------------------
TEST(TransactionStress, RunWithRetryAppliesEveryIncrementWithZeroLostUpdates) {
  VersionStore store;
  store.Put("counter", "0");
  TransactionManager mgr(store);

  constexpr int kThreads = 8;
  constexpr int kIncrementsPerThread = 300;
  constexpr int kExpected = kThreads * kIncrementsPerThread;

  RunConcurrently(kThreads, [&](int /*thread_id*/) {
    for (int i = 0; i < kIncrementsPerThread; ++i) {
      Status s = RunWithRetry(
          mgr,
          [](Transaction& txn) -> Status {
            std::string current;
            txn.Get("counter", &current);
            std::this_thread::yield();
            return txn.Put("counter", std::to_string(std::stoi(current) + 1));
          },
          // A generous ceiling, not a tight one: under this test's
          // deliberately widened race window (the yield() above) and a
          // heavily oversubscribed CPU, a single key under contention
          // from 8 threads can need more than a handful of retries in
          // the worst case. The point being proven is "eventually
          // succeeds with zero lost updates," not "succeeds within N
          // retries" — so the ceiling is set high enough to make
          // exhaustion a non-issue rather than tuned to just barely pass.
          /*max_attempts=*/10000);
      ASSERT_TRUE(s.ok()) << "RunWithRetry exhausted its attempts: " << s.ToString();
    }
  });

  std::string final_value;
  store.Get("counter", &final_value);
  int actual = std::stoi(final_value);

  EXPECT_EQ(actual, kExpected);
}

// ---------------------------------------------------------------------
// Test 5: OldestActiveSnapshot must stay a correct lower bound even while
// transactions are concurrently beginning and finishing — never reporting
// a value newer than some transaction that is, at that instant, actually
// still active with an older snapshot. We check this by sampling
// concurrently and just confirming no exception/crash and that the value
// is always <= the store's current version (a sanity invariant it must
// never violate).
// ---------------------------------------------------------------------
TEST(TransactionStress, OldestActiveSnapshotRemainsSaneUnderConcurrency) {
  VersionStore store;
  store.Put("seed", "v");
  TransactionManager mgr(store);

  std::atomic<bool> stop{false};
  std::atomic<bool> saw_violation{false};

  std::thread sampler([&] {
    while (!stop.load()) {
      uint64_t oldest = mgr.OldestActiveSnapshot();
      uint64_t current = store.CurrentVersion();
      if (oldest > current) {
        saw_violation = true;
      }
    }
  });

  RunConcurrently(6, [&](int thread_id) {
    for (int i = 0; i < 500; ++i) {
      auto txn = mgr.Begin();
      txn->Put("t" + std::to_string(thread_id) + "-" + std::to_string(i), "v");
      txn->Commit();
    }
  });

  stop = true;
  sampler.join();

  EXPECT_FALSE(saw_violation.load());
}

// ---------------------------------------------------------------------
// Phase 7: this is the test that would actually catch a flaw in the
// sharded two-phase registration protocol (placeholder-then-refine), if
// there were one. Many threads run tight Begin()/read/Commit() cycles —
// heavy enough traffic that transactions land across every shard and
// frequently collide on the same one — while a SEPARATE thread runs
// GarbageCollector::RunOnce() aggressively and CONCURRENTLY with all of
// it. If the two-phase registration had a hole (e.g. a transaction
// visible to GC with no protection during some window), this is exactly
// the shape of race that would expose it: a long-lived reader holding an
// old snapshot, hammered by concurrent Begin() traffic on other shards
// and concurrent GC passes, must never lose the version it needs.
// ---------------------------------------------------------------------
TEST(TransactionStress, ShardedRegistrationSurvivesConcurrentGCAndHeavyBeginTraffic) {
  VersionStore store;
  TransactionManager mgr(store);
  GarbageCollector gc(store, mgr);

  store.Put("k", "original");
  auto reader = mgr.Begin();  // pins snapshot at "original"; must survive everything below

  std::atomic<bool> stop_gc{false};
  std::thread gc_thread([&] {
    while (!stop_gc.load(std::memory_order_relaxed)) {
      gc.RunOnce();
    }
  });

  constexpr int kThreads = 16;
  constexpr int kOpsPerThread = 2000;
  RunConcurrently(kThreads, [&](int thread_id) {
    for (int i = 0; i < kOpsPerThread; ++i) {
      auto txn = mgr.Begin();
      txn->Put("k", "writer" + std::to_string(thread_id) + "-" + std::to_string(i));
      txn->Commit();
    }
  });

  stop_gc.store(true, std::memory_order_relaxed);
  gc_thread.join();

  std::string value;
  ASSERT_TRUE(reader->Get("k", &value).ok())
      << "sharded registration incorrectly let GC reclaim a version the reader still needed";
  EXPECT_EQ(value, "original");
  ASSERT_TRUE(reader->Commit().ok());
}
