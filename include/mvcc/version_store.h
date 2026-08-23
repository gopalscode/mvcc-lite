#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "mvcc/status.h"

namespace mvcc {

// A single entry in a key's version history.
//
// `value == std::nullopt` represents a tombstone: the key was deleted as of
// this version. We don't just erase deleted keys from the table, because
// the whole point of MVCC is that a reader holding an older snapshot must
// still be able to see the value as it was *before* the delete — erasing
// history would break that.
struct VersionedValue {
  uint64_t version;
  std::optional<std::string> value;
};

// A single write in a batch applied atomically by ApplyBatch. `value ==
// std::nullopt` is a delete (tombstone), same convention as VersionedValue.
struct WriteOp {
  std::string key;
  std::optional<std::string> value;
};

// Result of a single garbage collection pass — see CollectGarbage.
struct GCStats {
  size_t versions_reclaimed = 0;  // history entries actually erased
  size_t keys_removed = 0;        // keys whose entire entry was dropped
                                   // (fully-tombstoned, nothing left worth
                                   // keeping around)
};

// VersionStore is the Phase 2 building block: like KVStore, but every write
// (Put or Delete) is appended as a new version rather than overwriting in
// place. Nothing is ever removed here — that's Phase 4's job (garbage
// collection), once we know which versions are no longer visible to any
// active transaction.
//
// DESIGN NOTE (see README for the full write-up):
//   - Versions are numbered by a single counter shared across *all* keys,
//     not per-key. This matters: Phase 3's snapshot isolation needs one
//     global "as of version N" number that means the same thing regardless
//     of which key you're asking about. A per-key counter couldn't do that.
//   - Locking is still a single coarse mutex, same as Phase 1, and for the
//     same reason: this phase's job is to get versioning *correct*, not
//     fast. Phase 5 is where locking gets revisited.
//   - Version history only grows. Long-lived keys with many writes will
//     accumulate unbounded history until Phase 4 adds GC. This is a known,
//     documented limitation of this phase, not an oversight.
class VersionStore {
 public:
  VersionStore() = default;

  VersionStore(const VersionStore&) = delete;
  VersionStore& operator=(const VersionStore&) = delete;

  // Appends a new version of `key` with `value`. Always succeeds (subject
  // to `key` being non-empty) — Put never fails because a key already
  // exists; that's the point of versioning. Returns the version number
  // assigned to this write via `out_version` if non-null.
  Status Put(const std::string& key, const std::string& value,
             uint64_t* out_version = nullptr);

  // Appends a tombstone version for `key`, marking it deleted as of the
  // returned version. Returns Status::NotFound if `key` has no currently
  // visible value (either it was never written, or its latest version is
  // already a tombstone) — mirroring Phase 1's Delete semantics, so
  // double-deleting is caught rather than silently accepted.
  Status Delete(const std::string& key, uint64_t* out_version = nullptr);

  // Returns the value from the *latest* version of `key`. Status::NotFound
  // if the key was never written, or its latest version is a tombstone.
  Status Get(const std::string& key, std::string* out_value,
             uint64_t* out_version = nullptr) const;

  // Returns the value that would be visible to a reader whose snapshot is
  // "as of version `as_of`" — i.e. the value from the latest version of
  // `key` with version <= as_of. This is the primitive Phase 3's snapshot
  // isolation will be built directly on top of.
  //
  // Status::NotFound if `key` had no version at or before `as_of` (it
  // didn't exist yet), or if the version visible at that point was a
  // tombstone (it had already been deleted).
  Status GetAsOf(const std::string& key, uint64_t as_of,
                 std::string* out_value) const;

  // Number of distinct keys ever written, including ones whose latest
  // version is a tombstone. Mainly for tests.
  size_t KeyCount() const;

  // Number of versions (including tombstones) stored for `key`. 0 if the
  // key was never written. Exposed so tests (and later, GC) can observe
  // history growth directly.
  size_t VersionCount(const std::string& key) const;

  // The most recently assigned version number, or 0 if nothing has been
  // written yet. Version numbers start at 1, so 0 is safe to use as
  // "before anything existed."
  uint64_t CurrentVersion() const;

  // Applies every write in `ops` at a single, shared new version number,
  // under one lock acquisition. This is the primitive Phase 3 transaction
  // commits are built on: a multi-key transaction's writes must become
  // visible to readers all-at-once, at one version, never partially. If
  // Put/Delete were called once per key instead, a concurrent reader could
  // observe a version where only some of a transaction's writes had
  // landed — breaking atomicity. `ops` must be non-empty and every key
  // non-empty; each key should appear at most once (a transaction's write
  // set is already deduplicated by key before it gets here).
  Status ApplyBatch(const std::vector<WriteOp>& ops, uint64_t* out_version = nullptr);

  // Reclaims history entries that can no longer be needed by ANY reader,
  // given that no active snapshot is older than `safe_boundary`. The
  // caller (GarbageCollector, see gc.h) is responsible for computing
  // `safe_boundary` correctly — normally
  // TransactionManager::OldestActiveSnapshot() — and for the guarantee
  // that no future GetAsOf call will ever ask for a version older than
  // whatever boundary was last passed here. VersionStore itself has no
  // way to verify that guarantee; it trusts its caller.
  //
  // Per key, this keeps: every version strictly after `safe_boundary`
  // (untouched, always — a future reader may need any of these), plus
  // exactly one version at or before `safe_boundary` — the newest such
  // version, since that's the one a reader whose snapshot is exactly
  // `safe_boundary` would need. Everything else — versions older than
  // that retained one — is erased.
  //
  // One further step: if, after that trim, a key's *entire* remaining
  // history is a single tombstone (the key is deleted, and nothing newer
  // exists), the key's entry is dropped from the table entirely rather
  // than left behind as a permanent single-tombstone record. No reader
  // will ever need it again: any future snapshot is >= the current
  // version, which is >= `safe_boundary`, so it would see "not present"
  // either way — an absent key and a tombstone-at-or-before-my-snapshot
  // mean the same thing to GetAsOf.
  //
  // Returns counts of what was reclaimed, for observability/testing.
  //
  // CAUTION: after this call, GetAsOf(key, as_of) for any as_of strictly
  // less than the `safe_boundary` used here is no longer reliable — the
  // history it would have needed may have just been erased. This is
  // intentional and is the entire point of the method; it is the caller's
  // job to never issue such a query once a boundary has been advanced
  // past it.
  GCStats CollectGarbage(uint64_t safe_boundary);

  // Phase 5: the OCC-aware counterpart to ApplyBatch. Applies `ops`
  // atomically, exactly like ApplyBatch, but FIRST validates — under the
  // same lock, so the check-then-apply is one indivisible step — that no
  // key in `ops` has been written by anyone else since `snapshot_version`.
  // This is "first committer wins": if any key in the batch has a latest
  // version strictly newer than `snapshot_version`, that means some other
  // transaction committed a write to that key that this caller's snapshot
  // never saw. Applying our write anyway would silently discard that
  // other commit's effect — the exact lost-update scenario documented (and
  // demonstrated) in Phase 3. Instead, nothing in the batch is applied —
  // it's all-or-nothing, not "apply the non-conflicting keys" — and
  // Status::Conflict() is returned so the caller can retry with a fresh
  // snapshot (see RunWithRetry in transaction.h).
  //
  // `out_conflicting_keys`, if non-null, is populated with the specific
  // keys that conflicted when the result is a conflict — useful for
  // logging/debugging, not required for correctness.
  //
  // This is the primitive Transaction::Commit uses; ApplyBatch (no
  // validation) remains available as the lower-level, unconditional
  // building block for direct callers that don't need OCC semantics.
  Status ApplyBatchIfNoConflict(const std::vector<WriteOp>& ops, uint64_t snapshot_version,
                                 uint64_t* out_version = nullptr,
                                 std::vector<std::string>* out_conflicting_keys = nullptr);

 private:
  // Phase 6 upgrade: a shared_mutex instead of a plain mutex. Reads
  // (Get, GetAsOf, KeyCount, VersionCount, CurrentVersion) take a shared
  // lock, so concurrent readers no longer serialize behind each other.
  // Writes (Put, Delete, ApplyBatch, ApplyBatchIfNoConflict,
  // CollectGarbage) take an exclusive lock, same as before.
  //
  // HONEST SCOPE OF THIS CHANGE — read this before assuming it gives full
  // lock-free MVCC: this makes readers concurrent WITH EACH OTHER. It does
  // NOT make readers immune to a writer's critical section — a writer
  // holding the exclusive lock still blocks every reader for the
  // (brief — O(batch size), a handful of map lookups and vector
  // push_backs) duration of that one write. True "a reader can never be
  // blocked by a writer, ever, even for a moment" MVCC requires a
  // different data structure entirely — e.g. per-key lock-free version
  // chains updated via atomic compare-and-swap, so a writer publishes a
  // new version without ever taking a lock a reader could be waiting on.
  // That's a legitimately harder, riskier rewrite (real lock-free
  // programming, not just picking a different mutex type) and is
  // deliberately out of scope here — this upgrade is the well-understood,
  // low-risk half of the story: eliminate reader-vs-reader contention,
  // which is real and was previously unnecessary, while being explicit
  // that writer-vs-reader contention still exists, just minimized to a
  // short critical section instead of a store-wide lock shared by
  // everything indiscriminately.
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, std::vector<VersionedValue>> table_;
  uint64_t next_version_ = 1;
};

}  // namespace mvcc
