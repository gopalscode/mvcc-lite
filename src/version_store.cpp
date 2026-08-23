#include "mvcc/version_store.h"

#include <algorithm>

namespace mvcc {

Status VersionStore::Put(const std::string& key, const std::string& value,
                          uint64_t* out_version) {
  if (key.empty()) {
    return Status::InvalidArgument("key must not be empty");
  }

  std::unique_lock<std::shared_mutex> lock(mutex_);
  uint64_t version = next_version_++;
  table_[key].push_back(VersionedValue{version, value});
  if (out_version != nullptr) {
    *out_version = version;
  }
  return Status::OK();
}

Status VersionStore::Delete(const std::string& key, uint64_t* out_version) {
  if (key.empty()) {
    return Status::InvalidArgument("key must not be empty");
  }

  std::unique_lock<std::shared_mutex> lock(mutex_);
  auto it = table_.find(key);
  bool currently_visible =
      it != table_.end() && !it->second.empty() && it->second.back().value.has_value();
  if (!currently_visible) {
    return Status::NotFound("key not present: " + key);
  }

  uint64_t version = next_version_++;
  it->second.push_back(VersionedValue{version, std::nullopt});
  if (out_version != nullptr) {
    *out_version = version;
  }
  return Status::OK();
}

Status VersionStore::Get(const std::string& key, std::string* out_value,
                          uint64_t* out_version) const {
  if (key.empty()) {
    return Status::InvalidArgument("key must not be empty");
  }

  std::shared_lock<std::shared_mutex> lock(mutex_);
  auto it = table_.find(key);
  if (it == table_.end() || it->second.empty()) {
    return Status::NotFound("key not present: " + key);
  }

  const VersionedValue& latest = it->second.back();
  if (!latest.value.has_value()) {
    return Status::NotFound("key not present (deleted): " + key);
  }

  *out_value = *latest.value;
  if (out_version != nullptr) {
    *out_version = latest.version;
  }
  return Status::OK();
}

Status VersionStore::GetAsOf(const std::string& key, uint64_t as_of,
                              std::string* out_value) const {
  if (key.empty()) {
    return Status::InvalidArgument("key must not be empty");
  }

  std::shared_lock<std::shared_mutex> lock(mutex_);
  auto it = table_.find(key);
  if (it == table_.end()) {
    return Status::NotFound("key not present: " + key);
  }

  const std::vector<VersionedValue>& history = it->second;
  // History is append-only with a strictly increasing version number, so
  // it's already sorted by version — binary search for the latest entry
  // with version <= as_of using partition_point (the array is partitioned
  // into "version <= as_of" followed by "version > as_of").
  auto pred = [as_of](const VersionedValue& v) { return v.version <= as_of; };
  auto boundary = std::partition_point(history.begin(), history.end(), pred);

  if (boundary == history.begin()) {
    // No version of this key existed at or before `as_of`.
    return Status::NotFound("key did not exist as of version " + std::to_string(as_of));
  }

  const VersionedValue& visible = *std::prev(boundary);
  if (!visible.value.has_value()) {
    return Status::NotFound("key was deleted as of version " + std::to_string(as_of));
  }

  *out_value = *visible.value;
  return Status::OK();
}

size_t VersionStore::KeyCount() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return table_.size();
}

size_t VersionStore::VersionCount(const std::string& key) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  auto it = table_.find(key);
  if (it == table_.end()) {
    return 0;
  }
  return it->second.size();
}

uint64_t VersionStore::CurrentVersion() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return next_version_ - 1;
}

Status VersionStore::ApplyBatch(const std::vector<WriteOp>& ops, uint64_t* out_version) {
  if (ops.empty()) {
    return Status::InvalidArgument("ops must not be empty");
  }
  for (const auto& op : ops) {
    if (op.key.empty()) {
      return Status::InvalidArgument("key must not be empty");
    }
  }

  std::unique_lock<std::shared_mutex> lock(mutex_);
  uint64_t version = next_version_++;
  for (const auto& op : ops) {
    table_[op.key].push_back(VersionedValue{version, op.value});
  }
  if (out_version != nullptr) {
    *out_version = version;
  }
  return Status::OK();
}

GCStats VersionStore::CollectGarbage(uint64_t safe_boundary) {
  std::unique_lock<std::shared_mutex> lock(mutex_);

  GCStats stats;
  for (auto it = table_.begin(); it != table_.end();) {
    std::vector<VersionedValue>& history = it->second;

    // Find the first entry with version > safe_boundary. Everything
    // before it has version <= safe_boundary; everything from it onward
    // must be kept untouched (a future reader may need any of it).
    auto pred = [safe_boundary](const VersionedValue& v) { return v.version <= safe_boundary; };
    auto boundary_it = std::partition_point(history.begin(), history.end(), pred);

    if (boundary_it == history.begin()) {
      // No version of this key is <= safe_boundary yet (the key was
      // created after the boundary) — nothing here is collectible.
      ++it;
      continue;
    }

    // The newest version <= safe_boundary is the one a reader whose
    // snapshot is exactly safe_boundary would need — it must be kept.
    // Everything strictly before it is superseded and safe to erase.
    auto keep_it = std::prev(boundary_it);
    size_t num_reclaimable = static_cast<size_t>(std::distance(history.begin(), keep_it));
    if (num_reclaimable > 0) {
      history.erase(history.begin(), keep_it);
      stats.versions_reclaimed += num_reclaimable;
    }

    // After trimming, if all that's left is a single tombstone, no reader
    // — now or in the future — can ever need this key's entry again:
    // any snapshot from here on is >= the current version, which is
    // itself >= safe_boundary, so "absent" and "tombstoned as of my
    // snapshot" are indistinguishable outcomes. Drop it entirely so a
    // long history of create/delete churn doesn't leave permanent
    // single-entry debris in the table.
    if (history.size() == 1 && !history.front().value.has_value()) {
      stats.versions_reclaimed += 1;  // the tombstone itself
      stats.keys_removed += 1;
      it = table_.erase(it);
      continue;
    }

    ++it;
  }

  return stats;
}

Status VersionStore::ApplyBatchIfNoConflict(const std::vector<WriteOp>& ops,
                                             uint64_t snapshot_version, uint64_t* out_version,
                                             std::vector<std::string>* out_conflicting_keys) {
  if (ops.empty()) {
    return Status::InvalidArgument("ops must not be empty");
  }
  for (const auto& op : ops) {
    if (op.key.empty()) {
      return Status::InvalidArgument("key must not be empty");
    }
  }

  std::unique_lock<std::shared_mutex> lock(mutex_);

  // Validation pass: has anyone committed a newer version of any key in
  // this batch since our snapshot was taken? This check and the apply
  // below happen under the same lock acquisition, so there's no window
  // for a second transaction to sneak a conflicting write in between
  // "we checked" and "we applied" — that gap is exactly what would make
  // OCC validation meaningless.
  std::vector<std::string> conflicts;
  for (const auto& op : ops) {
    auto it = table_.find(op.key);
    if (it != table_.end() && !it->second.empty()) {
      uint64_t latest_version = it->second.back().version;
      if (latest_version > snapshot_version) {
        conflicts.push_back(op.key);
      }
    }
  }

  if (!conflicts.empty()) {
    if (out_conflicting_keys != nullptr) {
      *out_conflicting_keys = std::move(conflicts);
    }
    return Status::Conflict("write-write conflict on " + std::to_string(conflicts.size()) +
                             " key(s)");
  }

  // No conflicts: apply exactly as ApplyBatch would, at one new shared
  // version number.
  uint64_t version = next_version_++;
  for (const auto& op : ops) {
    table_[op.key].push_back(VersionedValue{version, op.value});
  }
  if (out_version != nullptr) {
    *out_version = version;
  }
  return Status::OK();
}

}  // namespace mvcc
