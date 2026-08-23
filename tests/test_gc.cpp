#include "mvcc/gc.h"

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

using mvcc::GarbageCollector;
using mvcc::GCStats;
using mvcc::TransactionManager;
using mvcc::VersionStore;

TEST(GarbageCollectorBasic, RunOnceReclaimsVersionsNotNeededByAnyActiveTransaction) {
  VersionStore store;
  TransactionManager mgr(store);
  GarbageCollector gc(store, mgr);

  store.Put("k", "v1");
  store.Put("k", "v2");
  store.Put("k", "v3");
  // No active transactions, so OldestActiveSnapshot() == CurrentVersion():
  // everything but the very latest version is fair game.

  GCStats stats = gc.RunOnce();
  EXPECT_EQ(stats.versions_reclaimed, 2u);  // v1 and v2
  EXPECT_EQ(store.VersionCount("k"), 1u);

  std::string value;
  ASSERT_TRUE(store.Get("k", &value).ok());
  EXPECT_EQ(value, "v3");
}

TEST(GarbageCollectorBasic, ActiveTransactionProtectsItsSnapshotFromCollection) {
  // This is the core promise of Phase 4: GC must never break a reader
  // that's still legitimately using an old version.
  VersionStore store;
  TransactionManager mgr(store);
  GarbageCollector gc(store, mgr);

  store.Put("k", "v1");
  auto reader = mgr.Begin();  // snapshot pinned at v1
  store.Put("k", "v2");
  store.Put("k", "v3");

  gc.RunOnce();  // boundary == reader's snapshot; v1 must survive

  std::string value;
  ASSERT_TRUE(reader->Get("k", &value).ok());
  EXPECT_EQ(value, "v1");
}

TEST(GarbageCollectorBasic, ReclaimsMoreOnceProtectingTransactionEnds) {
  VersionStore store;
  TransactionManager mgr(store);
  GarbageCollector gc(store, mgr);

  store.Put("k", "v1");
  auto reader = mgr.Begin();
  store.Put("k", "v2");
  store.Put("k", "v3");

  GCStats first_pass = gc.RunOnce();
  EXPECT_EQ(first_pass.versions_reclaimed, 0u);  // v1 still protected by reader

  ASSERT_TRUE(reader->Commit().ok());  // read-only commit, releases the snapshot

  GCStats second_pass = gc.RunOnce();
  EXPECT_EQ(second_pass.versions_reclaimed, 2u);  // now v1 and v2 are collectible
  EXPECT_EQ(store.VersionCount("k"), 1u);
}

TEST(GarbageCollectorBasic, CumulativeStatsAccumulateAcrossRuns) {
  VersionStore store;
  TransactionManager mgr(store);
  GarbageCollector gc(store, mgr);

  store.Put("a", "1");
  store.Put("a", "2");
  gc.RunOnce();

  store.Put("b", "1");
  store.Put("b", "2");
  gc.RunOnce();

  GCStats cumulative = gc.CumulativeStats();
  EXPECT_EQ(cumulative.versions_reclaimed, 2u);  // 1 from each pass
}

TEST(GarbageCollectorBackground, StartAndStopCleanly) {
  VersionStore store;
  TransactionManager mgr(store);
  GarbageCollector gc(store, mgr);

  EXPECT_FALSE(gc.IsBackgroundRunning());
  gc.StartBackground(std::chrono::milliseconds(5));
  EXPECT_TRUE(gc.IsBackgroundRunning());
  gc.StopBackground();
  EXPECT_FALSE(gc.IsBackgroundRunning());
}

TEST(GarbageCollectorBackground, StopIsIdempotentWhenNotRunning) {
  VersionStore store;
  TransactionManager mgr(store);
  GarbageCollector gc(store, mgr);
  gc.StopBackground();  // never started — must not crash or hang
  gc.StopBackground();  // calling twice must also be safe
}

TEST(GarbageCollectorBackground, StartIsIdempotentWhileAlreadyRunning) {
  VersionStore store;
  TransactionManager mgr(store);
  GarbageCollector gc(store, mgr);
  gc.StartBackground(std::chrono::milliseconds(5));
  gc.StartBackground(std::chrono::milliseconds(5));  // no-op, must not crash/spawn a second thread
  EXPECT_TRUE(gc.IsBackgroundRunning());
  gc.StopBackground();
}

TEST(GarbageCollectorBackground, PeriodicallyReclaimsGarbageOverTime) {
  VersionStore store;
  TransactionManager mgr(store);
  GarbageCollector gc(store, mgr);

  gc.StartBackground(std::chrono::milliseconds(5));

  for (int i = 0; i < 50; ++i) {
    store.Put("k", "v" + std::to_string(i));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  // Give the background thread a few more cycles to catch up.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  gc.StopBackground();

  EXPECT_GT(gc.CumulativeStats().versions_reclaimed, 0u);
  // Only the latest version should remain — no active transactions ever
  // existed to protect anything older.
  EXPECT_EQ(store.VersionCount("k"), 1u);
}

TEST(GarbageCollectorBackground, DestructorStopsBackgroundThreadWithoutHanging) {
  VersionStore store;
  TransactionManager mgr(store);
  {
    GarbageCollector gc(store, mgr);
    gc.StartBackground(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // gc destructs here without an explicit StopBackground() call. If the
    // destructor didn't join the thread, this test would either hang (bad
    // join) or crash (thread outliving its GarbageCollector and touching
    // freed memory via a dangling `this`).
  }
  SUCCEED();
}
