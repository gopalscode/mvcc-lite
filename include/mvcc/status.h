#pragma once

#include <string>

namespace mvcc {

// A small, explicit result type instead of throwing exceptions for expected
// outcomes (like "key not found"). Exceptions are reserved for programmer
// errors / truly exceptional conditions, not normal control flow.
//
// This mirrors the Status pattern used in real storage engines (LevelDB,
// RocksDB): callers are forced to look at what happened rather than assume
// success.
enum class Code {
  kOk,
  kNotFound,
  kInvalidArgument,
  kConflict,
};

class Status {
 public:
  static Status OK() { return Status(Code::kOk, ""); }

  static Status NotFound(const std::string& msg = "") {
    return Status(Code::kNotFound, msg);
  }

  static Status InvalidArgument(const std::string& msg) {
    return Status(Code::kInvalidArgument, msg);
  }

  // A transactional write conflicted with a concurrently-committed write
  // to the same key (Phase 5: optimistic concurrency control). Distinct
  // from InvalidArgument — this isn't a caller mistake, it's an expected,
  // recoverable outcome under contention that the caller should generally
  // respond to by retrying with a fresh transaction (see RunWithRetry in
  // transaction.h), not by treating it as a bug.
  static Status Conflict(const std::string& msg = "") {
    return Status(Code::kConflict, msg);
  }

  bool ok() const { return code_ == Code::kOk; }
  bool is_not_found() const { return code_ == Code::kNotFound; }
  bool is_invalid_argument() const { return code_ == Code::kInvalidArgument; }
  bool is_conflict() const { return code_ == Code::kConflict; }

  Code code() const { return code_; }
  const std::string& message() const { return message_; }

  // Human-readable form, mainly for logging/tests.
  std::string ToString() const {
    switch (code_) {
      case Code::kOk:
        return "OK";
      case Code::kNotFound:
        return "NotFound: " + message_;
      case Code::kInvalidArgument:
        return "InvalidArgument: " + message_;
      case Code::kConflict:
        return "Conflict: " + message_;
    }
    return "Unknown status";
  }

 private:
  Status(Code code, std::string msg) : code_(code), message_(std::move(msg)) {}

  Code code_;
  std::string message_;
};

}  // namespace mvcc
