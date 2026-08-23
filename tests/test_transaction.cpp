#include "mvcc/transaction.h"

#include <gtest/gtest.h>

using mvcc::RunWithRetry;
using mvcc::Status;
using mvcc::Transaction;
using mvcc::TransactionManager;
using mvcc::VersionStore;

// ---------------------------------------------------------------------
// Snapshot isolation for reads
// ---------------------------------------------------------------------

TEST(TransactionSnapshot, SeesStateAsOfBeginNotLaterWrites) {
  VersionStore store;
  store.Put("k", "v1");

  TransactionManager mgr(store);
  auto txn = mgr.Begin();

  // A write that lands *after* Begin(), bypassing the transaction
  // entirely, must not be visible to it.
  store.Put("k", "v2");

  std::string value;
  ASSERT_TRUE(txn->Get("k", &value).ok());
  EXPECT_EQ(value, "v1");
}

TEST(TransactionSnapshot, ReadOnlyTransactionSeesConsistentSnapshotAcrossKeys) {
  VersionStore store;
  store.Put("a", "a1");
  store.Put("b", "b1");

  TransactionManager mgr(store);
  auto txn = mgr.Begin();

  store.Put("a", "a2");
  store.Put("b", "b2");

  std::string va, vb;
  ASSERT_TRUE(txn->Get("a", &va).ok());
  ASSERT_TRUE(txn->Get("b", &vb).ok());
  EXPECT_EQ(va, "a1");
  EXPECT_EQ(vb, "b1");
}

// ---------------------------------------------------------------------
// Read-your-own-writes
// ---------------------------------------------------------------------

TEST(TransactionWriteBuffer, ReadYourOwnUncommittedWrite) {
  VersionStore store;
  TransactionManager mgr(store);
  auto txn = mgr.Begin();

  ASSERT_TRUE(txn->Put("k", "buffered").ok());

  std::string value;
  ASSERT_TRUE(txn->Get("k", &value).ok());
  EXPECT_EQ(value, "buffered");

  // Nobody outside the transaction can see it yet.
  EXPECT_TRUE(store.Get("k", &value).is_not_found());
}

TEST(TransactionWriteBuffer, ReadYourOwnBufferedDelete) {
  VersionStore store;
  store.Put("k", "v1");

  TransactionManager mgr(store);
  auto txn = mgr.Begin();

  ASSERT_TRUE(txn->Delete("k").ok());

  std::string value;
  EXPECT_TRUE(txn->Get("k", &value).is_not_found());
  // The store itself still has it — nothing committed yet.
  ASSERT_TRUE(store.Get("k", &value).ok());
  EXPECT_EQ(value, "v1");
}

TEST(TransactionWriteBuffer, PutThenDeleteSameKeyWithinTransaction) {
  VersionStore store;
  TransactionManager mgr(store);
  auto txn = mgr.Begin();

  // "newkey" doesn't exist in the store or the snapshot, but it exists in
  // this transaction's own view because we just buffered a Put for it.
  ASSERT_TRUE(txn->Put("newkey", "v").ok());
  ASSERT_TRUE(txn->Delete("newkey").ok());

  std::string value;
  EXPECT_TRUE(txn->Get("newkey", &value).is_not_found());
}

// ---------------------------------------------------------------------
// Commit
// ---------------------------------------------------------------------

TEST(TransactionCommit, CommitMakesWritesVisibleToEveryoneAfterward) {
  VersionStore store;
  TransactionManager mgr(store);
  auto txn = mgr.Begin();

  ASSERT_TRUE(txn->Put("k", "v").ok());
  uint64_t commit_version;
  ASSERT_TRUE(txn->Commit(&commit_version).ok());
  EXPECT_GT(commit_version, 0u);

  std::string value;
  ASSERT_TRUE(store.Get("k", &value).ok());
  EXPECT_EQ(value, "v");

  // A transaction begun after the commit sees it.
  auto txn2 = mgr.Begin();
  ASSERT_TRUE(txn2->Get("k", &value).ok());
  EXPECT_EQ(value, "v");
}

TEST(TransactionCommit, MultiKeyCommitIsAtomicAtOneVersion) {
  VersionStore store;
  TransactionManager mgr(store);
  auto txn = mgr.Begin();

  ASSERT_TRUE(txn->Put("a", "1").ok());
  ASSERT_TRUE(txn->Put("b", "2").ok());
  uint64_t commit_version;
  ASSERT_TRUE(txn->Commit(&commit_version).ok());

  // Both keys must be visible exactly at commit_version...
  std::string value;
  ASSERT_TRUE(store.GetAsOf("a", commit_version, &value).ok());
  EXPECT_EQ(value, "1");
  ASSERT_TRUE(store.GetAsOf("b", commit_version, &value).ok());
  EXPECT_EQ(value, "2");

  // ...and neither must be visible at any version before it — there must
  // be no in-between state where only one of the two writes had landed.
  EXPECT_TRUE(store.GetAsOf("a", commit_version - 1, &value).is_not_found());
  EXPECT_TRUE(store.GetAsOf("b", commit_version - 1, &value).is_not_found());
}

TEST(TransactionCommit, ReadOnlyCommitDoesNotAdvanceStoreVersion) {
  VersionStore store;
  store.Put("k", "v");
  uint64_t before = store.CurrentVersion();

  TransactionManager mgr(store);
  auto txn = mgr.Begin();
  std::string value;
  txn->Get("k", &value);  // read only, nothing buffered
  ASSERT_TRUE(txn->Commit().ok());

  EXPECT_EQ(store.CurrentVersion(), before);
}

TEST(TransactionCommit, DeleteBufferedThenCommitRemovesKey) {
  VersionStore store;
  store.Put("k", "v1");

  TransactionManager mgr(store);
  auto txn = mgr.Begin();
  ASSERT_TRUE(txn->Delete("k").ok());
  ASSERT_TRUE(txn->Commit().ok());

  std::string value;
  EXPECT_TRUE(store.Get("k", &value).is_not_found());
}

// ---------------------------------------------------------------------
// Abort
// ---------------------------------------------------------------------

TEST(TransactionAbort, AbortDiscardsBufferedWrites) {
  VersionStore store;
  TransactionManager mgr(store);
  auto txn = mgr.Begin();

  ASSERT_TRUE(txn->Put("k", "should not persist").ok());
  ASSERT_TRUE(txn->Abort().ok());

  std::string value;
  EXPECT_TRUE(store.Get("k", &value).is_not_found());
}

TEST(TransactionAbort, DroppingTransactionWithoutCommitAutoAborts) {
  VersionStore store;
  TransactionManager mgr(store);
  {
    auto txn = mgr.Begin();
    txn->Put("k", "should not persist");
    // txn destructs here without Commit()/Abort().
  }

  std::string value;
  EXPECT_TRUE(store.Get("k", &value).is_not_found());
  EXPECT_EQ(mgr.ActiveTransactionCount(), 0u);
}

// ---------------------------------------------------------------------
// State-machine discipline: operations after commit/abort are rejected
// ---------------------------------------------------------------------

TEST(TransactionStateMachine, OperationsAfterCommitAreRejected) {
  VersionStore store;
  TransactionManager mgr(store);
  auto txn = mgr.Begin();
  ASSERT_TRUE(txn->Commit().ok());

  std::string value;
  EXPECT_TRUE(txn->Get("k", &value).is_invalid_argument());
  EXPECT_TRUE(txn->Put("k", "v").is_invalid_argument());
  EXPECT_TRUE(txn->Delete("k").is_invalid_argument());
  EXPECT_TRUE(txn->Commit().is_invalid_argument());
  EXPECT_TRUE(txn->Abort().is_invalid_argument());
}

TEST(TransactionStateMachine, OperationsAfterAbortAreRejected) {
  VersionStore store;
  TransactionManager mgr(store);
  auto txn = mgr.Begin();
  ASSERT_TRUE(txn->Abort().ok());

  std::string value;
  EXPECT_TRUE(txn->Get("k", &value).is_invalid_argument());
  EXPECT_TRUE(txn->Commit().is_invalid_argument());
  EXPECT_TRUE(txn->Abort().is_invalid_argument());
}

// ---------------------------------------------------------------------
// Delete semantics are validated against the transaction's own view
// ---------------------------------------------------------------------

TEST(TransactionDeleteSemantics, DeleteOfKeyNotInSnapshotFails) {
  VersionStore store;
  TransactionManager mgr(store);
  auto txn = mgr.Begin();
  EXPECT_TRUE(txn->Delete("nope").is_not_found());
}

TEST(TransactionDeleteSemantics, DoubleDeleteWithinSameTransactionFails) {
  VersionStore store;
  store.Put("k", "v");
  TransactionManager mgr(store);
  auto txn = mgr.Begin();
  ASSERT_TRUE(txn->Delete("k").ok());
  EXPECT_TRUE(txn->Delete("k").is_not_found());
}

TEST(TransactionDeleteSemantics, DeleteValidatesAgainstOwnSnapshotNotLiveStoreState) {
  // A key deleted by someone else *after* our snapshot must still look
  // deletable to us, because as far as our transaction's view is
  // concerned, it's still there.
  VersionStore store;
  store.Put("k", "v1");

  TransactionManager mgr(store);
  auto txn = mgr.Begin();

  store.Delete("k");  // happens after our snapshot was taken

  EXPECT_TRUE(txn->Delete("k").ok());
}

// ---------------------------------------------------------------------
// TransactionManager bookkeeping
// ---------------------------------------------------------------------

TEST(TransactionManagerBookkeeping, OldestActiveSnapshotTracksMinimum) {
  VersionStore store;
  store.Put("seed", "v");  // version 1
  TransactionManager mgr(store);

  auto t1 = mgr.Begin();  // snapshot 1
  store.Put("x", "1");    // version 2
  auto t2 = mgr.Begin();  // snapshot 2

  EXPECT_EQ(mgr.OldestActiveSnapshot(), t1->snapshot_version());

  ASSERT_TRUE(t1->Commit().ok());
  EXPECT_EQ(mgr.OldestActiveSnapshot(), t2->snapshot_version());

  ASSERT_TRUE(t2->Commit().ok());
  // Nothing active: the boundary is "now."
  EXPECT_EQ(mgr.OldestActiveSnapshot(), store.CurrentVersion());
}

TEST(TransactionManagerBookkeeping, ActiveTransactionCountTracksLifecycle) {
  VersionStore store;
  TransactionManager mgr(store);
  EXPECT_EQ(mgr.ActiveTransactionCount(), 0u);

  auto t1 = mgr.Begin();
  EXPECT_EQ(mgr.ActiveTransactionCount(), 1u);
  auto t2 = mgr.Begin();
  EXPECT_EQ(mgr.ActiveTransactionCount(), 2u);

  t1->Commit();
  EXPECT_EQ(mgr.ActiveTransactionCount(), 1u);
  t2->Abort();
  EXPECT_EQ(mgr.ActiveTransactionCount(), 0u);
}

TEST(TransactionManagerBookkeeping, EachTransactionGetsAUniqueId) {
  VersionStore store;
  TransactionManager mgr(store);
  auto t1 = mgr.Begin();
  auto t2 = mgr.Begin();
  EXPECT_NE(t1->id(), t2->id());
}

// ---------------------------------------------------------------------
// Phase 7: sharded active-snapshot tracking. These specifically exercise
// behavior that spans multiple shards (TransactionManager uses 16
// internally), rather than the 2-transaction cases above which might
// happen to land on the same shard.
// ---------------------------------------------------------------------

TEST(TransactionManagerSharding, OldestActiveSnapshotCorrectAcrossManyShards) {
  VersionStore store;
  store.Put("seed", "v");
  TransactionManager mgr(store);

  // 40 transactions guarantees collisions AND spread across all 16
  // shards (40 > 16), exercising both same-shard and cross-shard paths.
  std::vector<std::unique_ptr<Transaction>> txns;
  for (int i = 0; i < 40; ++i) {
    store.Put("k", "v" + std::to_string(i));  // advances CurrentVersion() each time
    txns.push_back(mgr.Begin());
  }

  // The oldest active snapshot must be the FIRST transaction's — the one
  // begun before any of the others, regardless of which shard each
  // landed on.
  EXPECT_EQ(mgr.OldestActiveSnapshot(), txns.front()->snapshot_version());

  // Commit them in reverse order; the reported oldest should always
  // track whichever remaining transaction actually began earliest.
  for (int i = 39; i >= 1; --i) {
    txns[static_cast<size_t>(i)]->Commit();
    EXPECT_EQ(mgr.OldestActiveSnapshot(), txns.front()->snapshot_version());
  }
  txns.front()->Commit();
  EXPECT_EQ(mgr.OldestActiveSnapshot(), store.CurrentVersion());
}

TEST(TransactionManagerSharding, ActiveTransactionCountSumsAcrossShards) {
  VersionStore store;
  TransactionManager mgr(store);

  std::vector<std::unique_ptr<Transaction>> txns;
  for (int i = 0; i < 40; ++i) {
    txns.push_back(mgr.Begin());
  }
  EXPECT_EQ(mgr.ActiveTransactionCount(), 40u);

  for (int i = 0; i < 20; ++i) {
    txns[static_cast<size_t>(i)]->Commit();
  }
  EXPECT_EQ(mgr.ActiveTransactionCount(), 20u);
}

TEST(TransactionManagerSharding, ManyTransactionsAllReadTheirOwnCorrectSnapshot) {
  // Sanity check that sharding doesn't scramble which transaction sees
  // which snapshot — every one of many concurrently-open transactions
  // must still see exactly the value that existed at its own Begin().
  VersionStore store;
  TransactionManager mgr(store);

  std::vector<std::unique_ptr<Transaction>> txns;
  std::vector<std::string> expected;
  for (int i = 0; i < 40; ++i) {
    std::string value = "v" + std::to_string(i);
    store.Put("k", value);
    txns.push_back(mgr.Begin());
    expected.push_back(value);
  }

  for (size_t i = 0; i < txns.size(); ++i) {
    std::string value;
    ASSERT_TRUE(txns[i]->Get("k", &value).ok());
    EXPECT_EQ(value, expected[i]);
  }
}

// ---------------------------------------------------------------------
// Phase 5: optimistic concurrency control — write-write conflict detection
// ---------------------------------------------------------------------

TEST(TransactionConflict, CommitDetectsConflictWhenAnotherTxnCommittedNewerVersionOfWrittenKey) {
  VersionStore store;
  store.Put("k", "v1");
  TransactionManager mgr(store);

  auto txn_a = mgr.Begin();
  auto txn_b = mgr.Begin();  // same snapshot as txn_a

  ASSERT_TRUE(txn_a->Put("k", "from_a").ok());
  ASSERT_TRUE(txn_a->Commit().ok());  // succeeds, no prior conflicting commit

  ASSERT_TRUE(txn_b->Put("k", "from_b").ok());
  Status s = txn_b->Commit();
  EXPECT_TRUE(s.is_conflict());

  // txn_a's write must be what's actually visible — txn_b never applied.
  std::string value;
  ASSERT_TRUE(store.Get("k", &value).ok());
  EXPECT_EQ(value, "from_a");
}

TEST(TransactionConflict, CommitSucceedsWhenConcurrentWritesTouchDisjointKeys) {
  VersionStore store;
  TransactionManager mgr(store);

  auto txn_a = mgr.Begin();
  auto txn_b = mgr.Begin();

  ASSERT_TRUE(txn_a->Put("a", "1").ok());
  ASSERT_TRUE(txn_b->Put("b", "1").ok());

  EXPECT_TRUE(txn_a->Commit().ok());
  EXPECT_TRUE(txn_b->Commit().ok());  // disjoint key — no conflict, even though concurrent
}

TEST(TransactionConflict, ConflictLeavesTransactionAbortedAndUnregistered) {
  VersionStore store;
  store.Put("k", "v1");
  TransactionManager mgr(store);

  auto txn_a = mgr.Begin();
  auto txn_b = mgr.Begin();
  txn_a->Put("k", "from_a");
  txn_a->Commit();

  txn_b->Put("k", "from_b");
  ASSERT_TRUE(txn_b->Commit().is_conflict());

  EXPECT_FALSE(txn_b->is_active());
  EXPECT_EQ(mgr.ActiveTransactionCount(), 0u);

  // Terminal state — further operations rejected, same as any other abort.
  std::string value;
  EXPECT_TRUE(txn_b->Get("k", &value).is_invalid_argument());
  EXPECT_TRUE(txn_b->Commit().is_invalid_argument());
}

TEST(TransactionConflict, ConflictOnAnyKeyAbortsEntireCommitNotJustThatKey) {
  // All-or-nothing: if part of a multi-key write conflicts, none of it
  // should land — not even the non-conflicting key.
  VersionStore store;
  store.Put("a", "a1");
  TransactionManager mgr(store);

  auto txn_a = mgr.Begin();
  auto txn_b = mgr.Begin();

  ASSERT_TRUE(txn_a->Put("a", "a2").ok());
  ASSERT_TRUE(txn_a->Commit().ok());

  // txn_b writes both "a" (which will conflict) and "b" (which wouldn't,
  // on its own).
  ASSERT_TRUE(txn_b->Put("a", "a_from_b").ok());
  ASSERT_TRUE(txn_b->Put("b", "b_from_b").ok());
  EXPECT_TRUE(txn_b->Commit().is_conflict());

  // "b" must NOT have been written despite not being the conflicting key.
  std::string value;
  EXPECT_TRUE(store.Get("b", &value).is_not_found());
}

TEST(TransactionConflict, ReadOnlyCommitNeverConflictsRegardlessOfConcurrentWrites) {
  VersionStore store;
  store.Put("k", "v1");
  TransactionManager mgr(store);

  auto reader = mgr.Begin();
  auto writer = mgr.Begin();
  writer->Put("k", "v2");
  ASSERT_TRUE(writer->Commit().ok());

  std::string value;
  reader->Get("k", &value);  // read-only — nothing buffered
  EXPECT_TRUE(reader->Commit().ok());
}

TEST(TransactionConflict, ConflictReportsOffendingKeys) {
  VersionStore store;
  store.Put("k", "v1");
  TransactionManager mgr(store);

  auto txn_a = mgr.Begin();
  auto txn_b = mgr.Begin();
  txn_a->Put("k", "from_a");
  txn_a->Commit();

  txn_b->Put("k", "from_b");
  std::vector<std::string> conflicting_keys;
  Status s = txn_b->Commit(nullptr, &conflicting_keys);
  ASSERT_TRUE(s.is_conflict());
  EXPECT_EQ(conflicting_keys, std::vector<std::string>{"k"});
}

// ---------------------------------------------------------------------
// RunWithRetry
// ---------------------------------------------------------------------

TEST(RunWithRetryTest, SucceedsImmediatelyWhenNoConflict) {
  VersionStore store;
  TransactionManager mgr(store);

  Status s = RunWithRetry(mgr, [](Transaction& txn) { return txn.Put("k", "v"); });
  EXPECT_TRUE(s.ok());

  std::string value;
  ASSERT_TRUE(store.Get("k", &value).ok());
  EXPECT_EQ(value, "v");
}

TEST(RunWithRetryTest, PropagatesNonConflictFailureFromBodyWithoutRetrying) {
  VersionStore store;
  TransactionManager mgr(store);

  int call_count = 0;
  Status s = RunWithRetry(mgr, [&](Transaction& /*txn*/) {
    ++call_count;
    return Status::InvalidArgument("caller declined");
  });

  EXPECT_TRUE(s.is_invalid_argument());
  EXPECT_EQ(call_count, 1);  // not retried — this isn't a conflict
}

TEST(RunWithRetryTest, RetriesOnConflictAndEventuallySucceeds) {
  VersionStore store;
  store.Put("k", "v0");
  TransactionManager mgr(store);

  // Force exactly one conflict: an external write lands between the
  // first attempt's read and its commit.
  int attempt = 0;
  Status s = RunWithRetry(mgr, [&](Transaction& txn) {
    ++attempt;
    std::string current;
    txn.Get("k", &current);
    if (attempt == 1) {
      // Sneak in a conflicting external commit before this attempt's own
      // commit runs, forcing exactly one retry.
      auto interloper = mgr.Begin();
      interloper->Put("k", "interloper");
      interloper->Commit();
    }
    return txn.Put("k", current + "-updated");
  });

  EXPECT_TRUE(s.ok());
  EXPECT_EQ(attempt, 2);  // first attempt conflicted, second succeeded
}

TEST(RunWithRetryTest, ReturnsConflictAfterExhaustingMaxAttempts) {
  VersionStore store;
  store.Put("k", "v0");
  TransactionManager mgr(store);

  // Every attempt conflicts: an external commit sneaks in every time.
  Status s = RunWithRetry(
      mgr,
      [&](Transaction& txn) {
        std::string current;
        txn.Get("k", &current);
        auto interloper = mgr.Begin();
        interloper->Put("k", "interloper");
        interloper->Commit();
        return txn.Put("k", current + "-updated");
      },
      /*max_attempts=*/3);

  EXPECT_TRUE(s.is_conflict());
}

TEST(RunWithRetryTest, RejectsNonPositiveMaxAttempts) {
  VersionStore store;
  TransactionManager mgr(store);
  Status s = RunWithRetry(
      mgr, [](Transaction& txn) { return txn.Put("k", "v"); }, 0);
  EXPECT_TRUE(s.is_invalid_argument());
}
