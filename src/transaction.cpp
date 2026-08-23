#include "mvcc/transaction.h"

#include <algorithm>
#include <limits>

namespace mvcc {

// ---------------------------------------------------------------------
// Transaction
// ---------------------------------------------------------------------

Transaction::Transaction(VersionStore& store, TransactionManager& manager, uint64_t txn_id,
                          uint64_t snapshot_version)
    : store_(store),
      manager_(manager),
      txn_id_(txn_id),
      snapshot_version_(snapshot_version),
      state_(State::kActive) {}

Transaction::~Transaction() {
  // A Transaction that goes out of scope without an explicit Commit() or
  // Abort() must not leave itself registered as "active" forever — that
  // would permanently pin the GC boundary (Phase 4) at this transaction's
  // snapshot, blocking reclamation of everything after it, indefinitely.
  // We deliberately don't call Abort() here (which would re-check state
  // and return a Status nobody could observe anyway) — just do the one
  // thing that matters: unregister.
  if (state_ == State::kActive) {
    manager_.Unregister(txn_id_);
  }
}

Status Transaction::Get(const std::string& key, std::string* out_value) {
  if (state_ != State::kActive) {
    return Status::InvalidArgument("transaction is not active");
  }

  auto it = write_set_.find(key);
  if (it != write_set_.end()) {
    // Read-your-own-writes: this transaction's own buffered state always
    // wins over the snapshot, even though nobody else can see it yet.
    if (it->second.has_value()) {
      *out_value = *it->second;
      return Status::OK();
    }
    return Status::NotFound("key deleted within this transaction: " + key);
  }

  return store_.GetAsOf(key, snapshot_version_, out_value);
}

Status Transaction::Put(const std::string& key, const std::string& value) {
  if (state_ != State::kActive) {
    return Status::InvalidArgument("transaction is not active");
  }
  if (key.empty()) {
    return Status::InvalidArgument("key must not be empty");
  }

  write_set_[key] = value;
  return Status::OK();
}

Status Transaction::Delete(const std::string& key) {
  if (state_ != State::kActive) {
    return Status::InvalidArgument("transaction is not active");
  }
  if (key.empty()) {
    return Status::InvalidArgument("key must not be empty");
  }

  auto it = write_set_.find(key);
  if (it != write_set_.end()) {
    if (!it->second.has_value()) {
      // Already buffered as a delete in this same transaction.
      return Status::NotFound("key already deleted within this transaction: " + key);
    }
    // It was buffered as a Put within this transaction; deleting it is
    // legitimate because the key exists in this transaction's own view,
    // regardless of what the underlying snapshot says.
    it->second = std::nullopt;
    return Status::OK();
  }

  // Not touched yet by this transaction — validate against the snapshot,
  // not against the store's live state. A key that was deleted by some
  // other, later-committing transaction after our snapshot was taken must
  // still look deletable (or not) exactly as it did to us.
  std::string unused;
  Status s = store_.GetAsOf(key, snapshot_version_, &unused);
  if (!s.ok()) {
    return s;
  }

  write_set_[key] = std::nullopt;
  return Status::OK();
}

Status Transaction::Commit(uint64_t* out_commit_version,
                            std::vector<std::string>* out_conflicting_keys) {
  if (state_ != State::kActive) {
    return Status::InvalidArgument("transaction is not active");
  }

  if (write_set_.empty()) {
    // Read-only transaction: nothing to validate or apply, so no new
    // version is created. A read-only transaction never conflicts —
    // there's nothing in its write set for anyone else's commit to have
    // raced against.
    state_ = State::kCommitted;
    manager_.Unregister(txn_id_);
    if (out_commit_version != nullptr) {
      *out_commit_version = snapshot_version_;
    }
    return Status::OK();
  }

  std::vector<WriteOp> ops;
  ops.reserve(write_set_.size());
  for (const auto& [key, value] : write_set_) {
    ops.push_back(WriteOp{key, value});
  }

  uint64_t commit_version = 0;
  Status s = store_.ApplyBatchIfNoConflict(ops, snapshot_version_, &commit_version,
                                            out_conflicting_keys);
  if (!s.ok()) {
    // Whether this is a genuine conflict or (in principle) some other
    // failure, this transaction cannot proceed: its snapshot is what was
    // validated against, and that snapshot doesn't change. Abort outright
    // — discard the write buffer and release the snapshot — rather than
    // leaving it active for a caller to retry against the same stale
    // view, which would just conflict again.
    write_set_.clear();
    state_ = State::kAborted;
    manager_.Unregister(txn_id_);
    return s;
  }

  state_ = State::kCommitted;
  manager_.Unregister(txn_id_);
  if (out_commit_version != nullptr) {
    *out_commit_version = commit_version;
  }
  return Status::OK();
}

Status Transaction::Abort() {
  if (state_ != State::kActive) {
    return Status::InvalidArgument("transaction is not active");
  }

  write_set_.clear();
  state_ = State::kAborted;
  manager_.Unregister(txn_id_);
  return Status::OK();
}

// ---------------------------------------------------------------------
// TransactionManager
// ---------------------------------------------------------------------

std::unique_ptr<Transaction> TransactionManager::Begin() {
  // Lock-free ID assignment: fetch_add returns the pre-increment value,
  // matching the original next_txn_id_++ semantics (first call returns 1).
  uint64_t txn_id = next_txn_id_.fetch_add(1, std::memory_order_relaxed);
  Shard& shard = shards_[ShardFor(txn_id)];

  // Two-phase registration — see the class comment for the full
  // correctness argument. Phase 1: register a maximally-conservative
  // placeholder BEFORE reading the store's current version, so this
  // transaction is never invisible-but-about-to-need-protection to a
  // concurrent GC scan.
  {
    std::lock_guard<std::mutex> lock(shard.mutex);
    shard.active_snapshots[txn_id] = 0;
  }

  // No lock held here: this read goes through VersionStore's own
  // shared_mutex independently. A concurrent GC pass that scans this
  // shard right now sees the placeholder (0) — safe, just maximally
  // conservative for this one cycle.
  uint64_t snapshot = store_.CurrentVersion();

  // Phase 2: refine the placeholder to the real snapshot value.
  {
    std::lock_guard<std::mutex> lock(shard.mutex);
    shard.active_snapshots[txn_id] = snapshot;
  }

  // The heap allocation below is deliberately outside any lock: touching
  // the allocator while holding a lock other threads may be waiting on
  // (even a per-shard one) would extend that critical section for no
  // correctness reason.
  //
  // Can't use std::make_unique here: Transaction's constructor is private
  // (only TransactionManager should construct one), and make_unique isn't
  // a friend even when the class granting friendship is. Direct new +
  // unique_ptr wrapping is the standard workaround for a friended private
  // constructor.
  return std::unique_ptr<Transaction>(new Transaction(store_, *this, txn_id, snapshot));
}

uint64_t TransactionManager::OldestActiveSnapshot() const {
  // Shards are scanned one at a time, not all locked simultaneously —
  // see the class comment for why this is still safe under concurrent
  // Begin()/Unregister() activity on other shards.
  bool any_active = false;
  uint64_t oldest = std::numeric_limits<uint64_t>::max();
  for (const Shard& shard : shards_) {
    std::lock_guard<std::mutex> lock(shard.mutex);
    for (const auto& [txn_id, snapshot] : shard.active_snapshots) {
      (void)txn_id;
      any_active = true;
      oldest = std::min(oldest, snapshot);
    }
  }
  if (!any_active) {
    return store_.CurrentVersion();
  }
  return oldest;
}

size_t TransactionManager::ActiveTransactionCount() const {
  size_t total = 0;
  for (const Shard& shard : shards_) {
    std::lock_guard<std::mutex> lock(shard.mutex);
    total += shard.active_snapshots.size();
  }
  return total;
}

void TransactionManager::Unregister(uint64_t txn_id) {
  Shard& shard = shards_[ShardFor(txn_id)];
  std::lock_guard<std::mutex> lock(shard.mutex);
  shard.active_snapshots.erase(txn_id);
}

// ---------------------------------------------------------------------
// RunWithRetry
// ---------------------------------------------------------------------

Status RunWithRetry(TransactionManager& mgr, const std::function<Status(Transaction&)>& fn,
                     int max_attempts) {
  if (max_attempts <= 0) {
    return Status::InvalidArgument("max_attempts must be positive");
  }

  Status last_status = Status::OK();
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    auto txn = mgr.Begin();

    Status body_status = fn(*txn);
    if (!body_status.ok()) {
      // fn itself declined to proceed — this is the caller's own logic,
      // not a concurrency conflict, so it is not retried.
      txn->Abort();
      return body_status;
    }

    Status commit_status = txn->Commit();
    if (commit_status.ok()) {
      return Status::OK();
    }
    if (!commit_status.is_conflict()) {
      // Some other failure (shouldn't normally happen given fn already
      // succeeded, but don't silently retry an unexpected error class).
      return commit_status;
    }

    last_status = commit_status;
    // Loop: retry with a brand-new transaction and a fresh snapshot. A
    // stale snapshot would just conflict again for the same reason.
  }

  return last_status;  // retries exhausted; report the last conflict
}

}  // namespace mvcc
