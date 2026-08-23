# MVCC-lite

![CI](https://github.com/gopalscode/mvcc-lite/actions/workflows/ci.yml/badge.svg)

A key-value store I built from scratch in C++ that implements MVCC (multi-version concurrency control) — the same technique behind how Postgres, CockroachDB, and similar databases let transactions see a consistent snapshot of the data without readers and writers blocking each other.

I built this in phases: a basic locked hashmap first, then versioning, then transactions with snapshot isolation, garbage collection, optimistic concurrency control for write conflicts, and finally a benchmark against the naive version to see if any of it actually paid off. Each phase has its own tests, and I ran everything through ThreadSanitizer along the way, since concurrent code can look fine and still have a data race hiding in it.

```
git clone https://github.com/gopalscode/mvcc-lite.git
cd mvcc-lite && cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Why MVCC

A normal key-value store overwrites a value in place when you write to it. That's fine until you have concurrent readers and writers — either the reader blocks until the writer finishes, or it risks seeing a half-written value. MVCC sidesteps this by never overwriting anything: every write appends a new version, and a reader just asks for "the value as of the version I started with." Readers never wait on writers, and writers never wait on readers. The cost is that you now have to manage a growing pile of old versions, which is where garbage collection comes in, and you have to handle two transactions racing to write the same key, which is where the concurrency control comes in.

## How it's put together

```
                    +-----------------------+
                    |   TransactionManager  |
                    |-----------------------|
                    | hands out snapshots,  |
                    | tracks active txns    |
                    | across 16 lock shards |
                    +----------+------------+
                               |
                    +----------+-----------+
                    |                      |
                    v                      v
        +----------------------+  +----------------------+
        |      Transaction     |  |    GarbageCollector   |
        |----------------------|  |----------------------|
        | buffered writes      |  | reclaims versions     |
        | read-your-own-writes |  | older than the        |
        | OCC on commit        |  | oldest live snapshot  |
        +-----------+----------+  +-----------+-----------+
                    |                          |
                    +------------+-------------+
                                 |
                                 v
                       +---------------------+
                       |     VersionStore    |
                       |---------------------|
                       | key -> list of      |
                       | (version, value)    |
                       | shared_mutex: many  |
                       | readers, one writer |
                       +---------------------+


        +-----------------------------+
        |      KVStore (baseline)     |
        |-----------------------------|
        | unordered_map + 1 mutex     |
        | no versions at all          |
        | exists to benchmark against |
        +-----------------------------+
        (separate, deliberately dumb - not part of the MVCC path)
```

The idea: instead of overwriting a value when you `Put` a key, you append a new version. Every key ends up with a little history — `[(version 1, "a"), (version 3, "b"), (version 7, "c")]` — and a reader can ask "what did this look like as of version 4" and get a consistent answer, even while other writes are happening concurrently.

- **`KVStore`** is the boring baseline: a plain `unordered_map` behind one mutex. No versions, no transactions. It's there specifically so I'd have something to benchmark the real thing against.
- **`VersionStore`** holds the actual version history per key, plus one global version counter shared across every key (needed so a transaction's snapshot means the same thing regardless of which key it's looking at). Reads take a `shared_mutex` in shared mode so they don't block each other; writes take it exclusively.
- **`Transaction` / `TransactionManager`** hand out snapshot-isolated transactions. Writes get buffered locally and only actually hit the store when you commit, all at once, atomically. Keeping track of which transactions are currently active (so GC knows what it's safe to throw away) used to go through one global lock — I ended up splitting that into 16 shards after a benchmark showed it was a real bottleneck.
- **`GarbageCollector`** figures out the oldest snapshot any live transaction could still need, and cleans up anything older than that, per key. Can run once on demand or on its own background thread.
- **`bench/`** is a standalone program comparing the MVCC stack against the plain `KVStore` under different read/write mixes.

## A transaction's life

```
Begin()  ->  pins a snapshot version, registers it so GC knows not
             to touch anything that snapshot might still need

Get/Put  ->  reads go straight to VersionStore, "as of my snapshot"
             writes just get buffered locally, nothing touches
             the store yet, and re-reading a key you already wrote
             gives you back your own buffered value

Commit() ->  checks every key you wrote for conflicts (did anyone
             else commit a newer version since your snapshot?)
             if clean: applies every write at once, one new version
             if conflicted: nothing gets applied, you get an error
             back and can retry with a fresh snapshot

Abort()  ->  throws away the buffer, nothing you did is ever visible
```

## Building it

You need CMake 3.16+ and a C++17 compiler. Tests get pulled in automatically via CMake on first configure (needs internet for that step).

```bash
git clone https://github.com/gopalscode/mvcc-lite.git
cd mvcc-lite
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

That should give you `100% tests passed, 0 tests failed out of 98`.

To run the benchmark:
```bash
./build/mvcc_bench
```

And if you want to check the concurrency stuff isn't secretly racy:
```bash
cmake -S . -B build-tsan -DMVCC_SANITIZE=thread -DCMAKE_BUILD_TYPE=Debug -DMVCC_BUILD_BENCHMARKS=OFF
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

CI does both of these automatically on every push (see the badge up top).

## A few decisions worth explaining

**Why optimistic concurrency control instead of just locking?** Two-phase locking would mean transactions grab a read lock on every key they touch, which kind of defeats the whole point of building MVCC in the first place — the entire premise is that readers shouldn't block writers. OCC checks for conflicts when you commit instead, which fits naturally since transactions already buffer their writes and only touch the store once, at commit time.

**Why a shared_mutex on VersionStore?** So concurrent reads don't queue up behind each other. Writers still get exclusive access while they're writing, so this isn't lock-free MVCC or anything fancy — a writer briefly blocks everyone else, it's just that readers no longer block each other, which they had no real reason to do.

**The sharding thing.** When I actually benchmarked this on real multi-core hardware, MVCC was still way slower than the naive baseline, even after the shared_mutex fix. Turned out there was a second bottleneck I hadn't noticed: every single transaction, whether it read or wrote anything, had to grab one global lock twice just for bookkeeping (tracking which transactions were currently active). I split that tracking across 16 independent locks instead. The tricky part was doing this without breaking garbage collection's safety guarantee — sharding a structure like that can create a race where GC "looks" at the active-transaction list and misses one that's mid-registration. I fixed it with a two-phase registration (write a conservative placeholder first, then fill in the real value), which is documented in `transaction.h` if you want the actual proof.

## Testing

98 tests. Split roughly into normal unit tests (edge cases, empty keys, double deletes, that kind of thing) and concurrency stress tests that actually try to break things with lots of threads — e.g. one test runs a background GC thread as aggressively as possible while a reader holds an old snapshot open and 16 other threads hammer writes, just to make sure GC never rips away a version someone still needs.

Everything's also been run under ThreadSanitizer, which actually catches races at runtime instead of just hoping the tests happen to expose one. CI runs the whole suite (regular + TSan) on both Linux and macOS on every push.

## Benchmarks — and the honest results

I compared the MVCC stack against the plain `KVStore` across three workloads (read-heavy, balanced, write-heavy), and the MVCC version came out slower across the board — somewhere around 8-18% of the naive baseline's throughput depending on the scenario. That's not really a failure, though: every transaction here does real extra work (an allocated object, appending to a version history instead of overwriting, OCC checks on commit) that a bare locked hashmap just doesn't do. The whole point of MVCC is that it avoids blocking under contention, which is genuinely hard to show cleanly on a laptop with a handful of threads and a tiny key space — real payoff shows up at a scale this benchmark can't really simulate.

One thing that did hold up consistently: turning on background GC made both reads and writes faster, not just reduce memory usage. Makes sense — without GC, a key that gets written a lot builds up a huge version history, and every read/write has to deal with that longer list.

I also went back and re-benchmarked after the sharding fix a few times, and the results were kind of mixed — better in the low-contention scenario, worse in the high-contention ones, possibly because the extra locking step from the two-phase registration costs more than the sharding saves once there's a lot of retry traffic. Two runs agreed with each other on this pattern, which is something, but I wouldn't call it fully proven — it's not the kind of thing you nail down without a lot more repeated, controlled measurement than I did here.

## What's not solved here

- No write-skew detection — this catches write-write conflicts (OCC), but not the more subtle write-skew anomalies that need full serializability to catch. This matches what most real databases actually ship by default, though.
- A writer still blocks all readers for the brief moment it's writing. Not true lock-free MVCC.
- The active-transaction tracking is sharded, not lock-free — still real locks, just sixteen of them instead of one.
- GC does a stop-the-world sweep each pass, not an incremental one.
- These numbers are about this specific implementation, not MVCC as an idea — a production engine with lock-free version chains and incremental GC would look pretty different.

## Layout

```
include/mvcc/
  status.h            result type (OK / NotFound / InvalidArgument / Conflict)
  kv_store.h          the naive baseline
  version_store.h     versioned storage + GC logic
  transaction.h        transactions, OCC, sharded active-txn tracking
  gc.h                garbage collector
src/                  implementations of the above
tests/                98 tests total, unit + concurrency stress
bench/                the KVStore-vs-MVCC benchmark
.github/workflows/    CI config
```

## Roadmap I followed

1. Basic KV store, single mutex
2. Add versioning
3. Transactions with snapshot isolation
4. Garbage collection
5. Optimistic concurrency control for writes
6. Benchmark against the baseline
7. Fix the transaction-tracking bottleneck the benchmark exposed
