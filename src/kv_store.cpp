#include "mvcc/kv_store.h"

namespace mvcc {

Status KVStore::Put(const std::string& key, const std::string& value) {
  if (key.empty()) {
    return Status::InvalidArgument("key must not be empty");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  table_[key] = value;
  return Status::OK();
}

Status KVStore::Get(const std::string& key, std::string* out_value) const {
  if (key.empty()) {
    return Status::InvalidArgument("key must not be empty");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = table_.find(key);
  if (it == table_.end()) {
    return Status::NotFound("key not present: " + key);
  }
  *out_value = it->second;
  return Status::OK();
}

Status KVStore::Delete(const std::string& key) {
  if (key.empty()) {
    return Status::InvalidArgument("key must not be empty");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = table_.find(key);
  if (it == table_.end()) {
    return Status::NotFound("key not present: " + key);
  }
  table_.erase(it);
  return Status::OK();
}

bool KVStore::Contains(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return table_.find(key) != table_.end();
}

size_t KVStore::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return table_.size();
}

}  // namespace mvcc
