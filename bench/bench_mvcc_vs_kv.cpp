// Phase 6 benchmark: measures the naive single-lock mvcc::KVStore (Phase 1)
// against the full MVCC transaction stack (mvcc::VersionStore +
// mvcc::TransactionManager, with and without background GC) under
// concurrent read/write load, across several reader:writer mixes.
//
// This is a standalone program, not a gtest binary — run it directly:
//   ./mvcc_bench
//
// METHODOLOGY, stated plainly so results can be judged rather than trusted:
//   - Each "reader op" for KVStore is one store.Get() call. Each "reader
//     op" for the MVCC stack is one full read-only transaction:
//     Begin() -> Get() -> Commit(). That's the realistic unit of work a
//     client actually performs, not just the raw storage call — comparing
//     "one Get()" against "one full transaction" would understate the
//     MVCC stack's real per-operation cost.
//   - Each "writer op" for KVStore is one store.Put() call. Each "writer
//     op" for the MVCC stack is one call to RunWithRetry() wrapping a
//     read-modify-write Put — i.e. the realistic client-side pattern,
//     including the cost of any retries triggered by OCC conflicts. A
//     writer op's measured latency is the FULL end-to-end cost including
//     retries, not just the final successful attempt.
//   - Keys are drawn uniformly at random from a fixed key space per
//     scenario, so contention is real and reproducible in shape (not
//     every op hits the same single key, but the space is small enough
//     that collisions — and therefore OCC conflicts on the MVCC side —
//     genuinely happen).
//   - Latency is measured per-operation with std::chrono::steady_clock
//     around exactly the unit of work described above, on the calling
//     thread. Throughput is (operation count) / (measured wall-clock
//     duration of the run), not derived from summed latencies, since
//     operations from different threads overlap in real time.
//   - No separate warm-up phase: the first handful of operations per
//     thread may run slightly colder (allocator/cache effects) than
//     steady state. Over a multi-second run with thousands of operations
//     per thread this is a minor effect, but it's not hidden — it's why
//     p50/p95/p99 are reported instead of just a mean, so a few slow
//     early ops don't quietly distort a single summary number.
//   - This program detects and prints the actual hardware concurrency
//     available at runtime rather than assuming any particular core
//     count — see the printed header and the interpretation notes it
//     prints alongside it.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "mvcc/gc.h"
#include "mvcc/kv_store.h"
#include "mvcc/transaction.h"
#include "mvcc/version_store.h"

namespace {

using mvcc::GarbageCollector;
using mvcc::KVStore;
using mvcc::RunWithRetry;
using mvcc::Status;
using mvcc::Transaction;
using mvcc::TransactionManager;
using mvcc::VersionStore;

struct LatencyStats {
  size_t count = 0;
  double throughput_ops_per_sec = 0.0;
  double p50_us = 0.0;
  double p95_us = 0.0;
  double p99_us = 0.0;
  double max_us = 0.0;
};

LatencyStats Summarize(std::vector<double> latencies_us, double wall_seconds) {
  LatencyStats s;
  s.count = latencies_us.size();
  if (s.count == 0) {
    return s;
  }
  std::sort(latencies_us.begin(), latencies_us.end());
  auto percentile = [&](double p) {
    size_t idx = static_cast<size_t>(p * static_cast<double>(latencies_us.size() - 1));
    return latencies_us[idx];
  };
  s.p50_us = percentile(0.50);
  s.p95_us = percentile(0.95);
  s.p99_us = percentile(0.99);
  s.max_us = latencies_us.back();
  s.throughput_ops_per_sec = wall_seconds > 0.0 ? static_cast<double>(s.count) / wall_seconds : 0.0;
  return s;
}

struct WorkloadResult {
  LatencyStats reader_stats;
  LatencyStats writer_stats;
  mvcc::GCStats gc_stats;  // zeroed/unused if GC wasn't enabled
};

// Shared by both workload runners: spins up `num_readers` + `num_writers`
// threads running `reader_body`/`writer_body` in a loop until `run_for`
// elapses, then stops and joins them, returning per-role latency
// summaries built from wall-clock timing of the actual concurrent run.
WorkloadResult RunWorkload(int num_readers, int num_writers, std::chrono::seconds run_for,
                            const std::function<double()>& reader_body,
                            const std::function<double()>& writer_body) {
  std::atomic<bool> stop{false};
  std::vector<std::vector<double>> reader_latencies(static_cast<size_t>(num_readers));
  std::vector<std::vector<double>> writer_latencies(static_cast<size_t>(num_writers));
  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(num_readers + num_writers));

  for (int i = 0; i < num_readers; ++i) {
    threads.emplace_back([&, i] {
      auto& lat = reader_latencies[static_cast<size_t>(i)];
      while (!stop.load(std::memory_order_relaxed)) {
        lat.push_back(reader_body());
      }
    });
  }
  for (int i = 0; i < num_writers; ++i) {
    threads.emplace_back([&, i] {
      auto& lat = writer_latencies[static_cast<size_t>(i)];
      while (!stop.load(std::memory_order_relaxed)) {
        lat.push_back(writer_body());
      }
    });
  }

  auto wall_start = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(run_for);
  stop.store(true, std::memory_order_relaxed);
  for (auto& t : threads) {
    t.join();
  }
  auto wall_end = std::chrono::steady_clock::now();
  double wall_seconds = std::chrono::duration<double>(wall_end - wall_start).count();

  std::vector<double> all_readers;
  for (auto& v : reader_latencies) {
    all_readers.insert(all_readers.end(), v.begin(), v.end());
  }
  std::vector<double> all_writers;
  for (auto& v : writer_latencies) {
    all_writers.insert(all_writers.end(), v.begin(), v.end());
  }

  WorkloadResult result;
  result.reader_stats = Summarize(std::move(all_readers), wall_seconds);
  result.writer_stats = Summarize(std::move(all_writers), wall_seconds);
  return result;
}

std::string KeyName(int i) { return "key" + std::to_string(i); }

WorkloadResult RunKVStoreWorkload(int num_readers, int num_writers, int key_space_size,
                                   std::chrono::seconds run_for) {
  KVStore store;
  for (int i = 0; i < key_space_size; ++i) {
    store.Put(KeyName(i), "init");
  }

  std::atomic<int> writer_counter{0};

  auto reader_body = [&]() -> double {
    thread_local std::mt19937 rng(std::random_device{}());
    thread_local std::uniform_int_distribution<int> dist(0, key_space_size - 1);
    std::string key = KeyName(dist(rng));
    auto t0 = std::chrono::steady_clock::now();
    std::string value;
    store.Get(key, &value);
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
  };

  auto writer_body = [&]() -> double {
    thread_local std::mt19937 rng(std::random_device{}());
    thread_local std::uniform_int_distribution<int> dist(0, key_space_size - 1);
    std::string key = KeyName(dist(rng));
    std::string value = "v" + std::to_string(writer_counter.fetch_add(1, std::memory_order_relaxed));
    auto t0 = std::chrono::steady_clock::now();
    store.Put(key, value);
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
  };

  return RunWorkload(num_readers, num_writers, run_for, reader_body, writer_body);
}

WorkloadResult RunMVCCWorkload(int num_readers, int num_writers, int key_space_size,
                                std::chrono::seconds run_for, bool enable_gc) {
  VersionStore store;
  TransactionManager mgr(store);
  std::unique_ptr<GarbageCollector> gc;
  if (enable_gc) {
    gc = std::make_unique<GarbageCollector>(store, mgr);
    gc->StartBackground(std::chrono::milliseconds(5));
  }

  for (int i = 0; i < key_space_size; ++i) {
    store.Put(KeyName(i), "init");
  }

  std::atomic<int> writer_counter{0};

  auto reader_body = [&]() -> double {
    thread_local std::mt19937 rng(std::random_device{}());
    thread_local std::uniform_int_distribution<int> dist(0, key_space_size - 1);
    std::string key = KeyName(dist(rng));
    auto t0 = std::chrono::steady_clock::now();
    auto txn = mgr.Begin();
    std::string value;
    txn->Get(key, &value);
    txn->Commit();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
  };

  auto writer_body = [&]() -> double {
    thread_local std::mt19937 rng(std::random_device{}());
    thread_local std::uniform_int_distribution<int> dist(0, key_space_size - 1);
    std::string key = KeyName(dist(rng));
    auto t0 = std::chrono::steady_clock::now();
    RunWithRetry(
        mgr,
        [&](Transaction& txn) -> Status {
          std::string current;
          txn.Get(key, &current);
          std::string value =
              "v" + std::to_string(writer_counter.fetch_add(1, std::memory_order_relaxed));
          return txn.Put(key, value);
        },
        /*max_attempts=*/32);
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
  };

  WorkloadResult result = RunWorkload(num_readers, num_writers, run_for, reader_body, writer_body);

  if (gc) {
    gc->StopBackground();
    result.gc_stats = gc->CumulativeStats();
  }
  return result;
}

void PrintRow(const std::string& label, const LatencyStats& s) {
  if (s.count == 0) {
    std::printf("  %-14s  %10s\n", label.c_str(), "(none)");
    return;
  }
  std::printf(
      "  %-14s  ops=%-8zu  throughput=%9.1f ops/s   p50=%7.2fus  p95=%7.2fus  "
      "p99=%7.2fus  max=%9.2fus\n",
      label.c_str(), s.count, s.throughput_ops_per_sec, s.p50_us, s.p95_us, s.p99_us, s.max_us);
}

// Phase 6 methodology fix, added after a real before/after comparison
// turned out to be unreliable: on real hardware, throughput swung by 5-8x
// between separate runs of the SAME UNCHANGED code (KVStore), purely from
// system noise — background load, thermal/frequency scaling, scheduler
// variance. A single run of anything cannot be trusted as evidence of a
// code change's effect when the noise floor is that large. Every
// configuration below is now run kTrialsPerConfig times; every trial's
// raw throughput is printed (so the variance itself is visible, not
// papered over), and the reported summary row uses the MEDIAN across
// trials — robust to one unlucky/lucky outlier run in a way a mean or a
// single sample is not.
constexpr int kTrialsPerConfig = 3;

double Median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

// Aggregates several trials' LatencyStats into one summary row using the
// median of each field independently. This is a display aggregate, not a
// re-derivation from raw per-op data (the raw per-op samples from
// different trials aren't pooled) — clearly labeled as such wherever it's
// printed, so it's never mistaken for a single larger, more precise
// measurement.
LatencyStats MedianAcrossTrials(const std::vector<LatencyStats>& trials) {
  std::vector<double> throughput, p50, p95, p99, max_us;
  size_t total_count = 0;
  for (const auto& t : trials) {
    throughput.push_back(t.throughput_ops_per_sec);
    p50.push_back(t.p50_us);
    p95.push_back(t.p95_us);
    p99.push_back(t.p99_us);
    max_us.push_back(t.max_us);
    total_count += t.count;
  }
  LatencyStats agg;
  agg.count = trials.empty() ? 0 : total_count / trials.size();  // avg ops per trial, for display
  if (trials.empty() || throughput.empty()) {
    return agg;
  }
  agg.throughput_ops_per_sec = Median(throughput);
  agg.p50_us = Median(p50);
  agg.p95_us = Median(p95);
  agg.p99_us = Median(p99);
  agg.max_us = Median(max_us);
  return agg;
}

void PrintRowWithVariance(const std::string& label, const std::vector<LatencyStats>& trials) {
  LatencyStats agg = MedianAcrossTrials(trials);
  PrintRow(label, agg);
  bool any_nonzero = false;
  std::printf("    (per-trial throughput ops/s: ");
  for (size_t i = 0; i < trials.size(); ++i) {
    std::printf("%s%.0f", i == 0 ? "" : ", ", trials[i].throughput_ops_per_sec);
    if (trials[i].count > 0) any_nonzero = true;
  }
  std::printf(")%s\n", any_nonzero ? "" : "  [all trials empty]");
}

struct Scenario {
  std::string name;
  int readers;
  int writers;
};

}  // namespace

int main() {
  const int key_space_size = 64;
  const auto run_for = std::chrono::seconds(1);

  unsigned hw_concurrency = std::thread::hardware_concurrency();
  std::printf(
      "MVCC-lite Phase 6 benchmark\n"
      "Detected hardware_concurrency() = %u logical core(s) on this machine.\n"
      "Each configuration below runs %d times (%llds each); the summary row shown is the\n"
      "MEDIAN across those trials, with every trial's raw throughput printed alongside it —\n"
      "on real hardware, throughput can vary several-fold between runs of the SAME unchanged\n"
      "code purely from system noise (background load, thermal/frequency scaling, scheduler\n"
      "variance), so a single run is not reliable evidence for comparing two configurations,\n"
      "or for judging the effect of a code change. Interpret medians across several trials,\n"
      "not any single number, and treat two results as meaningfully different only when their\n"
      "trial ranges don't substantially overlap.\n"
      "Interpretation note: on a single-core (or heavily oversubscribed) machine, no locking\n"
      "strategy can show true parallel throughput gains, since only one thread ever actually\n"
      "runs at a time regardless of what locks are held. The 'readers never block writers'\n"
      "claim this project is built around is fundamentally a multi-core parallelism claim.\n"
      "See the project README for this repository's own measured results and their\n"
      "interpretation, including a root-caused explanation for why the MVCC stack has so far\n"
      "underperformed the naive baseline even on real multi-core hardware.\n\n",
      hw_concurrency, kTrialsPerConfig, static_cast<long long>(run_for.count()));

  std::vector<Scenario> scenarios = {
      {"Read-heavy (8R/1W)", 8, 1},
      {"Balanced (4R/4W)", 4, 4},
      {"Write-heavy (1R/8W)", 1, 8},
  };

  for (const auto& sc : scenarios) {
    std::printf("=== Scenario: %s | key space = %d | %d trials x %llds each ===\n",
                sc.name.c_str(), key_space_size, kTrialsPerConfig,
                static_cast<long long>(run_for.count()));

    std::printf(" -- KVStore (Phase 1 baseline, single exclusive mutex) --\n");
    {
      std::vector<LatencyStats> reader_trials, writer_trials;
      for (int t = 0; t < kTrialsPerConfig; ++t) {
        WorkloadResult r = RunKVStoreWorkload(sc.readers, sc.writers, key_space_size, run_for);
        reader_trials.push_back(r.reader_stats);
        writer_trials.push_back(r.writer_stats);
      }
      PrintRowWithVariance("readers", reader_trials);
      PrintRowWithVariance("writers", writer_trials);
    }

    std::printf(" -- MVCC stack (shared_mutex reads, OCC writes, no GC) --\n");
    {
      std::vector<LatencyStats> reader_trials, writer_trials;
      for (int t = 0; t < kTrialsPerConfig; ++t) {
        WorkloadResult r =
            RunMVCCWorkload(sc.readers, sc.writers, key_space_size, run_for, /*enable_gc=*/false);
        reader_trials.push_back(r.reader_stats);
        writer_trials.push_back(r.writer_stats);
      }
      PrintRowWithVariance("readers", reader_trials);
      PrintRowWithVariance("writers", writer_trials);
    }

    std::printf(" -- MVCC stack (shared_mutex reads, OCC writes, background GC every 5ms) --\n");
    {
      std::vector<LatencyStats> reader_trials, writer_trials;
      size_t total_versions_reclaimed = 0;
      size_t total_keys_removed = 0;
      for (int t = 0; t < kTrialsPerConfig; ++t) {
        WorkloadResult r =
            RunMVCCWorkload(sc.readers, sc.writers, key_space_size, run_for, /*enable_gc=*/true);
        reader_trials.push_back(r.reader_stats);
        writer_trials.push_back(r.writer_stats);
        total_versions_reclaimed += r.gc_stats.versions_reclaimed;
        total_keys_removed += r.gc_stats.keys_removed;
      }
      PrintRowWithVariance("readers", reader_trials);
      PrintRowWithVariance("writers", writer_trials);
      std::printf("  GC across all %d trials: versions_reclaimed=%zu keys_removed=%zu\n\n",
                  kTrialsPerConfig, total_versions_reclaimed, total_keys_removed);
    }
  }

  return 0;
}
