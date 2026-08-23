#include "mvcc/version_store.h"

#include <algorithm>

#include <gtest/gtest.h>

using mvcc::GCStats;
using mvcc::Status;
using mvcc::VersionStore;

// ---------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------

TEST(VersionStoreBasic, PutThenGetReturnsLatestValue) {
  VersionStore store;
  uint64_t v;
  ASSERT_TRUE(store.Put("k", "v1", &v).ok());
  EXPECT_EQ(v, 1u);

  std::string value;
  uint64_t got_version;
  ASSERT_TRUE(store.Get("k", &value, &got_version).ok());
  EXPECT_EQ(value, "v1");
  EXPECT_EQ(got_version, 1u);
}

TEST(VersionStoreBasic, MultiplePutsCreateGrowingHistory) {
  VersionStore store;
  store.Put("k", "v1");
  store.Put("k", "v2");
  store.Put("k", "v3");

  EXPECT_EQ(store.VersionCount("k"), 3u);

  std::string value;
  ASSERT_TRUE(store.Get("k", &value).ok());
  EXPECT_EQ(value, "v3");  // Get always returns the latest.
}

TEST(VersionStoreBasic, VersionNumbersAreGlobalNotPerKey) {
  VersionStore store;
  uint64_t va, vb, vc;
  store.Put("a", "1", &va);
  store.Put("b", "1", &vb);
  store.Put("a", "2", &vc);

  // One shared counter across all keys: strictly increasing regardless of
  // which key each write touched.
  EXPECT_EQ(va, 1u);
  EXPECT_EQ(vb, 2u);
  EXPECT_EQ(vc, 3u);
  EXPECT_EQ(store.CurrentVersion(), 3u);
}

TEST(VersionStoreBasic, DeleteThenGetReturnsNotFound) {
  VersionStore store;
  store.Put("k", "v1");
  uint64_t delete_version;
  ASSERT_TRUE(store.Delete("k", &delete_version).ok());
  EXPECT_EQ(delete_version, 2u);

  std::string value;
  EXPECT_TRUE(store.Get("k", &value).is_not_found());
}

TEST(VersionStoreBasic, ReinsertAfterDeleteWorks) {
  VersionStore store;
  store.Put("k", "v1");
  store.Delete("k");
  ASSERT_TRUE(store.Put("k", "v2").ok());

  std::string value;
  ASSERT_TRUE(store.Get("k", &value).ok());
  EXPECT_EQ(value, "v2");
  // History retains all three versions: v1, tombstone, v2.
  EXPECT_EQ(store.VersionCount("k"), 3u);
}

// ---------------------------------------------------------------------
// The actual point of this phase: as-of / snapshot reads
// ---------------------------------------------------------------------

TEST(VersionStoreAsOf, ReturnsValueVisibleAtGivenVersion) {
  VersionStore store;
  uint64_t v1, v2, v3;
  store.Put("k", "v1", &v1);  // version 1
  store.Put("k", "v2", &v2);  // version 2
  store.Put("k", "v3", &v3);  // version 3

  std::string value;
  ASSERT_TRUE(store.GetAsOf("k", v1, &value).ok());
  EXPECT_EQ(value, "v1");

  ASSERT_TRUE(store.GetAsOf("k", v2, &value).ok());
  EXPECT_EQ(value, "v2");

  ASSERT_TRUE(store.GetAsOf("k", v3, &value).ok());
  EXPECT_EQ(value, "v3");
}

TEST(VersionStoreAsOf, VersionBetweenTwoWritesSeesTheOlderOne) {
  VersionStore store;
  uint64_t v1;
  store.Put("k", "v1", &v1);
  store.Put("k", "v2");

  // A snapshot taken exactly at v1 (before v2 was written) must not see v2.
  std::string value;
  ASSERT_TRUE(store.GetAsOf("k", v1, &value).ok());
  EXPECT_EQ(value, "v1");
}

TEST(VersionStoreAsOf, VersionBeforeKeyExistedReturnsNotFound) {
  VersionStore store;
  store.Put("other", "x");  // bumps the global counter to 1
  uint64_t v_k;
  store.Put("k", "v1", &v_k);  // version 2

  // Asking "as of version 1" for "k" — before "k" was ever written.
  std::string value;
  EXPECT_TRUE(store.GetAsOf("k", 1, &value).is_not_found());
}

TEST(VersionStoreAsOf, AsOfZeroAlwaysReturnsNotFound) {
  VersionStore store;
  store.Put("k", "v1");
  std::string value;
  EXPECT_TRUE(store.GetAsOf("k", 0, &value).is_not_found());
}

TEST(VersionStoreAsOf, SnapshotBeforeDeleteStillSeesOldValue) {
  // This is the entire point of MVCC: a reader with an older snapshot must
  // not be affected by a delete that happens after their snapshot was
  // taken.
  VersionStore store;
  uint64_t v1;
  store.Put("k", "v1", &v1);
  store.Delete("k");  // version 2 (tombstone)

  std::string value;
  ASSERT_TRUE(store.GetAsOf("k", v1, &value).ok());
  EXPECT_EQ(value, "v1");
}

TEST(VersionStoreAsOf, SnapshotAtOrAfterDeleteSeesTombstone) {
  VersionStore store;
  store.Put("k", "v1");
  uint64_t delete_version;
  store.Delete("k", &delete_version);

  std::string value;
  EXPECT_TRUE(store.GetAsOf("k", delete_version, &value).is_not_found());
}

TEST(VersionStoreAsOf, AsOfFarInFutureSeesLatestValue) {
  VersionStore store;
  store.Put("k", "v1");
  store.Put("k", "v2");

  std::string value;
  ASSERT_TRUE(store.GetAsOf("k", 999999, &value).ok());
  EXPECT_EQ(value, "v2");
}

// ---------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------

TEST(VersionStoreEdgeCases, DeletingNeverWrittenKeyReturnsNotFound) {
  VersionStore store;
  EXPECT_TRUE(store.Delete("nope").is_not_found());
}

TEST(VersionStoreEdgeCases, DoubleDeleteReturnsNotFoundOnSecondCall) {
  VersionStore store;
  store.Put("k", "v1");
  ASSERT_TRUE(store.Delete("k").ok());
  EXPECT_TRUE(store.Delete("k").is_not_found());
}

TEST(VersionStoreEdgeCases, EmptyKeyRejectedOnAllOperations) {
  VersionStore store;
  std::string value;
  EXPECT_TRUE(store.Put("", "v").is_invalid_argument());
  EXPECT_TRUE(store.Get("", &value).is_invalid_argument());
  EXPECT_TRUE(store.Delete("").is_invalid_argument());
  EXPECT_TRUE(store.GetAsOf("", 1, &value).is_invalid_argument());
}

TEST(VersionStoreEdgeCases, GetOnNeverWrittenKeyReturnsNotFound) {
  VersionStore store;
  std::string value;
  EXPECT_TRUE(store.Get("nope", &value).is_not_found());
}

TEST(VersionStoreEdgeCases, GetAsOfOnNeverWrittenKeyReturnsNotFound) {
  VersionStore store;
  std::string value;
  EXPECT_TRUE(store.GetAsOf("nope", 100, &value).is_not_found());
}

TEST(VersionStoreEdgeCases, KeyCountReflectsDistinctKeysIncludingTombstoned) {
  VersionStore store;
  store.Put("a", "1");
  store.Put("b", "1");
  store.Delete("a");
  // "a" is tombstoned but still counted as a key that was written.
  EXPECT_EQ(store.KeyCount(), 2u);
}

TEST(VersionStoreEdgeCases, VersionCountZeroForUnknownKey) {
  VersionStore store;
  EXPECT_EQ(store.VersionCount("nope"), 0u);
}

TEST(VersionStoreEdgeCases, EmptyValueIsAllowed) {
  VersionStore store;
  ASSERT_TRUE(store.Put("k", "").ok());
  std::string value = "sentinel";
  ASSERT_TRUE(store.Get("k", &value).ok());
  EXPECT_EQ(value, "");
}

// ---------------------------------------------------------------------
// CollectGarbage
// ---------------------------------------------------------------------

TEST(VersionStoreGC, ReclaimsVersionsStrictlyOlderThanBoundary) {
  VersionStore store;
  uint64_t v1, v2, v3;
  store.Put("k", "v1", &v1);
  store.Put("k", "v2", &v2);
  store.Put("k", "v3", &v3);

  GCStats stats = store.CollectGarbage(v2);
  EXPECT_EQ(stats.versions_reclaimed, 1u);  // only v1
  EXPECT_EQ(stats.keys_removed, 0u);
  EXPECT_EQ(store.VersionCount("k"), 2u);  // v2, v3 remain

  std::string value;
  EXPECT_TRUE(store.GetAsOf("k", v2, &value).ok());
  EXPECT_EQ(value, "v2");
  EXPECT_TRUE(store.GetAsOf("k", v3, &value).ok());
  EXPECT_EQ(value, "v3");
}

TEST(VersionStoreGC, NeverTouchesVersionsAtOrAfterBoundary) {
  VersionStore store;
  uint64_t v1, v2, v3;
  store.Put("k", "v1", &v1);
  store.Put("k", "v2", &v2);
  store.Put("k", "v3", &v3);

  store.CollectGarbage(v1);  // reclaim nothing before v1, keep v1 as retain point

  std::string value;
  ASSERT_TRUE(store.GetAsOf("k", v1, &value).ok());
  EXPECT_EQ(value, "v1");
  ASSERT_TRUE(store.GetAsOf("k", v2, &value).ok());
  EXPECT_EQ(value, "v2");
  ASSERT_TRUE(store.GetAsOf("k", v3, &value).ok());
  EXPECT_EQ(value, "v3");
  ASSERT_TRUE(store.Get("k", &value).ok());
  EXPECT_EQ(value, "v3");
}

TEST(VersionStoreGC, BoundaryOlderThanAllVersionsReclaimsNothing) {
  VersionStore store;
  store.Put("k", "v1");
  store.Put("k", "v2");

  GCStats stats = store.CollectGarbage(0);
  EXPECT_EQ(stats.versions_reclaimed, 0u);
  EXPECT_EQ(stats.keys_removed, 0u);
  EXPECT_EQ(store.VersionCount("k"), 2u);
}

TEST(VersionStoreGC, FullyRemovesKeyWhoseOnlyRemainingVersionIsATombstone) {
  VersionStore store;
  store.Put("k", "v1");
  uint64_t delete_version;
  store.Delete("k", &delete_version);

  GCStats stats = store.CollectGarbage(delete_version);
  EXPECT_EQ(stats.versions_reclaimed, 2u);  // v1 trimmed + the tombstone itself
  EXPECT_EQ(stats.keys_removed, 1u);
  EXPECT_EQ(store.VersionCount("k"), 0u);
  EXPECT_EQ(store.KeyCount(), 0u);
}

TEST(VersionStoreGC, KeepsTombstoneIfNewerVersionsExistAfterIt) {
  VersionStore store;
  store.Put("k", "v1");
  uint64_t delete_version;
  store.Delete("k", &delete_version);  // tombstone
  store.Put("k", "v3");                // recreated after the delete

  GCStats stats = store.CollectGarbage(delete_version);
  EXPECT_EQ(stats.versions_reclaimed, 1u);  // just v1
  EXPECT_EQ(stats.keys_removed, 0u);        // key stays — v3 still exists after it
  EXPECT_EQ(store.VersionCount("k"), 2u);   // tombstone + v3

  std::string value;
  ASSERT_TRUE(store.Get("k", &value).ok());
  EXPECT_EQ(value, "v3");
}

TEST(VersionStoreGC, RunningTwiceWithSameBoundaryIsIdempotent) {
  VersionStore store;
  uint64_t v1, v2;
  store.Put("k", "v1", &v1);
  store.Put("k", "v2", &v2);

  store.CollectGarbage(v2);
  GCStats second_pass = store.CollectGarbage(v2);
  EXPECT_EQ(second_pass.versions_reclaimed, 0u);
  EXPECT_EQ(second_pass.keys_removed, 0u);
}

TEST(VersionStoreGC, HandlesMultipleKeysIndependently) {
  VersionStore store;
  uint64_t a2, b1;
  store.Put("a", "a1");
  store.Put("a", "a2", &a2);
  store.Put("b", "b1", &b1);  // only one version, nothing to trim ahead of it

  uint64_t boundary = std::max(a2, b1);
  GCStats stats = store.CollectGarbage(boundary);
  EXPECT_EQ(stats.versions_reclaimed, 1u);  // just a1
  EXPECT_EQ(store.VersionCount("a"), 1u);
  EXPECT_EQ(store.VersionCount("b"), 1u);
}
