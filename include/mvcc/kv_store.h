#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include "mvcc/status.h"

namespace mvcc {

// KVStore is the Phase 1 building block: a plain, thread-safe key-value
// store with no versioning and no transactions. Every key holds exactly one
// value; a Put overwrites whatever was there.
//
// DESIGN NOTE (see README for the full write-up):
// This store protects its internal map with a single std::mutex, held for
// the duration of every Get/Put/Delete. That is a deliberate, *temporary*
// choice, not an oversight:
//   1. It gives us a correct, simple baseline to build versioning on top of
//      in Phase 2+.
//   2. It doubles as the "naive single-lock KV store" that Phase 6 will
//      benchmark the finished MVCC engine against, to demonstrate the
//      readers-never-block-writers advantage with real numbers. Because it
//      is coarse-grained, readers and writers here *do* block each other —
//      that contention is exactly what MVCC is meant to remove.
class KVStore {
 public:
  KVStore() = default;

  // Non-copyable: this type owns a mutex and represents a single logical
  // store; copying it would silently duplicate or share state in confusing
  // ways. Callers should pass it by reference or hold it via pointer/smart
  // pointer.
  KVStore(const KVStore&) = delete;
  KVStore& operator=(const KVStore&) = delete;

  // Inserts `key` with `value`, overwriting any existing value for `key`.
  // Returns Status::InvalidArgument if `key` is empty — every other input
  // (including an empty value) is accepted.
  Status Put(const std::string& key, const std::string& value);

  // Looks up `key`. On success, `*out_value` is set and Status::OK() is
  // returned. If `key` is not present, returns Status::NotFound() and
  // `*out_value` is left unmodified.
  Status Get(const std::string& key, std::string* out_value) const;

  // Removes `key` if present. Returns Status::OK() whether or not the key
  // existed beforehand... except that in this store we deliberately
  // distinguish the two: deleting an absent key returns Status::NotFound()
  // so callers/tests can tell "no-op" apart from "actually removed
  // something." (RocksDB-style stores typically return OK either way; we
  // chose the stricter behavior here because it makes bugs in later phases
  // — e.g. GC deleting a version twice — easier to catch.)
  Status Delete(const std::string& key);

  // Returns true if `key` is present. Mainly a convenience for tests; Get()
  // is the primary API.
  bool Contains(const std::string& key) const;

  // Number of keys currently stored.
  size_t Size() const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::string> table_;
};

}  // namespace mvcc
