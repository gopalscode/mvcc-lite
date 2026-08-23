#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "mvcc/status.h"
#include "mvcc/version_store.h"

namespace mvcc {

class TransactionManager;

// A Transaction gives a caller a consistent, unchanging view of the whole
// store — a snapshot — for the lifetime of the transaction, plus a local
// buffer of writes that only become visible to anyone else atomically, all
// at once, on Commit().
//
// Snapshot isolation, concretely:
//   - Reads (Get) see the store exactly as it was at the moment Begin() was
//     called, via VersionStore::GetAsOf(key, snapshot_version). Writes made
//     by *other* transactions after that moment — committed or not — are
//     invisible to this transaction, no matter when they land.
//   - Writes (Put/Delete) are buffered locally, not applied to the store.
//     A transaction always sees its own buffered writes when it reads a key
//     it already wrote (read-your-own-writes) even though nobody else can.
//   - Commit() validates the write set against every other transaction
//     that has committed since this transaction's snapshot was taken
//     (Phase 5: optimistic concurrency control — see below), then, if
//     validation passes, applies the whole write buffer to the store in
//     one atomic step (VersionStore::ApplyBatchIfNoConflict) at one new
//     version number. No other reader can ever observe a state with only
//     some of this transaction's writes applied.
//   - Abort() (or letting the Transaction go out of scope without
//     committing) discards the write buffer. Nothing it did is ever visible
//     to anyone.
//
// WRITE CONFLICT DETECTION (Phase 5): Commit() uses "first committer
// wins" validation — if any key this transaction wrote has been written
// by someone else's *already-committed* transaction since this
// transaction's snapshot was taken, Commit() fails with
// Status::Conflict(), the transaction is aborted (its write buffer
// discarded), and the caller is expected to retry with a brand-new
// transaction and a fresh snapshot — see RunWithRetry() below, which does
// exactly that. This closes the lost-update gap Phase 3 documented and
// deliberately left open (see DocumentsUndetectedWriteWriteConflicts in
// the Phase 3 stress tests, and its Phase 5 counterpart which proves the
// fix with the same scenario).
//
// STILL AN OPEN LIMITATION even after Phase 5: this validates write-write
// conflicts only — it does not track each transaction's *read* set, so it
// cannot detect write-skew anomalies (two transactions that read
// overlapping data and write disjoint keys in a way that violates some
// invariant spanning both). Catching that requires full Serializable
// Snapshot Isolation (tracking rw-antidependencies, not just ww-conflicts)
// — real, more complex machinery (as used in PostgreSQL's SSI) that is
// out of scope here. What this phase guarantees is exactly "snapshot
// isolation with first-committer-wins," which is what most production
// MVCC databases actually run by default — not full serializability. This
// distinction is worth being precise about rather than overclaiming it.
//
// A Transaction is created only via TransactionManager::Begin() and is
// move-only in spirit (non-copyable; owned via std::unique_ptr).
class Transaction {
 public:
  ~Transaction();

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  // Reads `key` as of this transaction's snapshot, or from this
  // transaction's own uncommitted write buffer if it already wrote `key`.
  // Status::InvalidArgument if the transaction is no longer active
  // (already committed or aborted).
  Status Get(const std::string& key, std::string* out_value);

  // Buffers a write; not visible to any other transaction or direct
  // VersionStore reader until Commit(). Always succeeds for a non-empty
  // key on an active transaction — like VersionStore::Put, it doesn't
  // matter whether the key currently exists.
  Status Put(const std::string& key, const std::string& value);

  // Buffers a delete. Fails with Status::NotFound if `key` is not visible
  // to this transaction — checked against this transaction's own view:
  // its write buffer first, then its snapshot — NOT against the store's
  // current live state, which may have moved on. This is what makes
  // transactional deletes consistent with what the transaction has
  // actually seen.
  Status Delete(const std::string& key);

  // Validates this transaction's write set (Phase 5 OCC — see class
  // comment) against everything committed since this transaction's
  // snapshot, and if that passes, atomically applies every buffered write
  // at one new version number and marks the transaction committed. A
  // read-only transaction (nothing buffered) always commits as a no-op:
  // nothing to validate, no new version is created, since nothing
  // changed. Status::InvalidArgument if the transaction is not active.
  //
  // On success, if `out_commit_version` is non-null: set to the new
  // version for a write transaction, or to this transaction's own
  // snapshot version for a read-only one (since no new version exists).
  //
  // On Status::Conflict() (a write in this transaction collided with
  // someone else's already-committed write), the transaction is left
  // aborted — its write buffer is discarded and it can't be reused — and
  // `out_conflicting_keys`, if non-null, is populated with the specific
  // keys that conflicted. The caller should retry the whole operation
  // with a brand-new transaction (a stale snapshot will just conflict
  // again); RunWithRetry() automates exactly that pattern.
  Status Commit(uint64_t* out_commit_version = nullptr,
                std::vector<std::string>* out_conflicting_keys = nullptr);

  // Discards the write buffer; nothing this transaction did becomes
  // visible. Status::InvalidArgument if the transaction is not active.
  Status Abort();

  uint64_t snapshot_version() const { return snapshot_version_; }
  uint64_t id() const { return txn_id_; }
  bool is_active() const { return state_ == State::kActive; }

 private:
  friend class TransactionManager;

  enum class State { kActive, kCommitted, kAborted };

  Transaction(VersionStore& store, TransactionManager& manager, uint64_t txn_id,
              uint64_t snapshot_version);

  VersionStore& store_;
  TransactionManager& manager_;
  uint64_t txn_id_;
  uint64_t snapshot_version_;
  State state_;

  // Per-key buffered write. std::nullopt means "buffered delete."
  // unordered_map (not a vector) because within one transaction a key can
  // only have one pending outcome at commit time — the latest Put/Delete
  // on that key within this transaction wins locally, same as if you
  // overwrote a local variable twice before using it.
  std::unordered_map<std::string, std::optional<std::string>> write_set_;
};

// TransactionManager is the entry point for Phase 3: it hands out
// Transactions with correctly-assigned snapshot versions and tracks which
// transactions are currently active.
//
// That active-transaction tracking isn't just bookkeeping — it's the exact
// input Phase 4's garbage collector will need: a version is only safe to
// reclaim once no active transaction's snapshot could still need it, i.e.
// once it's older than the oldest currently-active snapshot. This class is
// what will answer "what is that boundary" when GC is built.
//
// PHASE 7: SHARDED ACTIVE-SNAPSHOT TRACKING
//
// Phase 6's real multi-core benchmarking found that every transaction —
// read-only or not — was serializing through ONE global exclusive
// std::mutex, twice (Begin() and Unregister()), because active-snapshot
// tracking requires a MUTATION (insert/erase) on every single transaction
// lifecycle event — the same shared_mutex trick that fixed VersionStore's
// read path doesn't apply here, since there's no "read-only majority" to
// free up.
//
// The fix: active_snapshots_ is now split across kNumShards independent
// shards, each with its own mutex. A transaction's shard is
// `txn_id % kNumShards`, so concurrent Begin()/Unregister() calls for
// DIFFERENT transactions usually land on different shards and don't
// contend with each other at all — only calls that happen to collide on
// the same shard still serialize, which is ~1/kNumShards as much
// contention as before. `next_txn_id_` is a lock-free atomic counter, not
// protected by any shard lock.
//
// CORRECTNESS ARGUMENT — this is the part that actually matters, and the
// reason this wasn't a same-day fix once diagnosed: removing the single
// global lock removes a property the original design got "for free" —
// that GC's OldestActiveSnapshot() could never observe a transaction
// mid-registration (snapshot read but not yet recorded), because
// computing that boundary took the SAME lock Begin() held for its entire
// duration. Sharding breaks that unless something else preserves it.
//
// The fix is a two-phase registration protocol in Begin():
//   1. Insert a PLACEHOLDER value of 0 into this transaction's shard,
//      under that shard's lock, BEFORE reading the store's current
//      version.
//   2. Read the store's actual current version (no lock held during
//      this step — VersionStore's own shared_mutex handles that read
//      independently).
//   3. Update the shard entry from the placeholder to the real snapshot
//      value, under the shard's lock again.
//
// Why this is safe: OldestActiveSnapshot() takes the MINIMUM over every
// entry it observes across all shards. A transaction can be in exactly
// one of three states relative to any single GC scan:
//   - Not yet inserted at all: GC simply doesn't see it — identical to
//     the original "begins after the boundary was read" case. Since the
//     store's version counter only ever increases, and this transaction
//     will read its actual snapshot at some point strictly AFTER GC's
//     scan (it hadn't even placeholder-registered yet), its eventual
//     snapshot is guaranteed >= whatever CurrentVersion() was at GC's
//     scan time, which is >= the boundary GC used. Safe.
//   - Placeholder (0) visible: the most conservative possible value.
//     GC's boundary collapses toward 0 for that pass, meaning
//     CollectGarbage() reclaims nothing this cycle rather than
//     something it shouldn't. Always safe, merely a missed opportunity
//     for one GC cycle, not a correctness risk.
//   - Refined (real) value visible: behaves exactly like the original,
//     single-lock design — this transaction's true snapshot correctly
//     lower-bounds the computed boundary.
// In every case, GC's computed boundary is guaranteed <= every active
// (or about-to-become-active) transaction's eventual snapshot — which is
// exactly the property GC's correctness depends on (see gc.h). This
// holds regardless of exactly when, during a multi-shard scan, each
// individual shard happens to be read — OldestActiveSnapshot() reads
// shards one at a time (not holding multiple shard locks
// simultaneously), which is sufficient: each shard's read reflects a
// valid, safe state of that shard at the instant it was taken, and the
// combined minimum across all of them remains a valid lower bound.
//
// kNumShards = 16 is a fixed, reasonable default — enough to
// substantially reduce collision-driven contention without adding the
// complexity of runtime-sizing this based on measured load or
// hardware_concurrency(), which isn't the point of this phase.
class TransactionManager {
 public:
  explicit TransactionManager(VersionStore& store) : store_(store) {}

  TransactionManager(const TransactionManager&) = delete;
  TransactionManager& operator=(const TransactionManager&) = delete;

  // Starts a new transaction with its snapshot pinned to the store's
  // current version right now. Returned by unique_ptr so its lifetime is
  // explicit and RAII-managed: a Transaction that's dropped without
  // Commit()/Abort() still auto-aborts in its destructor rather than
  // leaking as "active" forever, which would otherwise block GC
  // indefinitely.
  std::unique_ptr<Transaction> Begin();

  // The snapshot version of the oldest currently-active transaction, or
  // the store's current version if no transaction is active (i.e. nothing
  // is holding back the boundary — everything up to "now" is fair game).
  // Scans all shards sequentially (see class comment for why this is
  // still safe under concurrent Begin()/Unregister() activity).
  uint64_t OldestActiveSnapshot() const;

  // Number of transactions currently active (begun, not yet committed or
  // aborted). Mainly for tests and GC bookkeeping/observability. Sums
  // across all shards.
  size_t ActiveTransactionCount() const;

 private:
  friend class Transaction;

  static constexpr size_t kNumShards = 16;

  static constexpr size_t ShardFor(uint64_t txn_id) { return txn_id % kNumShards; }

  struct Shard {
    mutable std::mutex mutex;
    std::unordered_map<uint64_t, uint64_t> active_snapshots;  // txn_id -> snapshot
  };

  // Called by a Transaction when it commits, aborts, or is destroyed
  // while still active. Not part of the public API: end-users interact
  // with transactions through Transaction itself, not by manipulating the
  // manager's bookkeeping directly.
  void Unregister(uint64_t txn_id);

  VersionStore& store_;
  std::atomic<uint64_t> next_txn_id_{1};
  std::array<Shard, kNumShards> shards_;
};

// Runs `fn` inside a fresh transaction from `mgr`, and if the commit fails
// with Status::Conflict() (Phase 5 OCC validation lost a race with another
// committer), retries the whole thing — a brand-new Begin(), so a fresh
// snapshot — up to `max_attempts` times. This is the standard way OCC is
// meant to be used: conflicts are expected under contention, not
// exceptional, and the correct response is "try again with current data,"
// not "give up" or "apply it anyway."
//
// `fn` receives the active transaction and should perform its reads and
// writes through it, returning Status::OK() to proceed to commit, or any
// other Status to abort immediately without retrying — a non-OK return
// from `fn` is treated as the caller's own logic declining to proceed
// (e.g. a business-rule check failed), not a concurrency conflict, so it
// is not retried.
//
// Returns Status::OK() on an eventual successful commit, `fn`'s own
// Status if `fn` itself returned non-OK, or the last Status::Conflict()
// if every attempt was exhausted without success.
Status RunWithRetry(TransactionManager& mgr, const std::function<Status(Transaction&)>& fn,
                     int max_attempts = 8);

}  // namespace mvcc
