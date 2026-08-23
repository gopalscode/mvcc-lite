# MVCC-lite

A multi-version concurrency control key-value store, built in C++ from
scratch. This is a portfolio project built in phases; each phase's README
section is written as it's completed, documenting *why* things were built
the way they were, not just what was built.

## Roadmap

| Phase | Status | What it adds |
|---|---|---|
| 1 | **Done** | Basic KV store (get/put/delete), no versioning |
| 2 | **Done** | Versioning: each key maps to a list of `(version, value)` |
| 3 | **Done** | Transactions with snapshot isolation |
| 4 | **Done** | Garbage collection of unreachable old versions |
| 5 | **Done** | Write concurrency control (optimistic concurrency control) |
| 6 | **Done** | Benchmark vs. a naive single-lock store |
| 7 | **Done** | Fix: sharded active-snapshot tracking (found via Phase 6's own benchmark) |

## Building

Requires CMake 3.16+ and a C++17 compiler. Tests are fetched and built
automatically via CMake's `FetchContent` (needs network access on first
configure).

```sh
mkdir build && cd build
cmake ..
cmake --build . -j
ctest --output-on-failure
```

To build with ThreadSanitizer (recommended when touching anything
concurrency-related):

```sh
mkdir build-tsan && cd build-tsan
cmake .. -DMVCC_SANITIZE=thread -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j
./kv_store_stress_tests
```

## Project layout

```
include/mvcc/   public headers (the interface)
src/            implementation
tests/          unit tests + concurrency stress tests
```

The header/impl split, and keeping tests in their own directory that
mirrors the library's structure, is intentional: as Phase 2+ adds
`version_store`, `transaction`, and `gc` components, each gets its own
header, source file, and test file. Nothing should end up as one giant
`kv_store.cpp` with everything jammed in.

## Phase 1: the basic store

### What it is

`mvcc::KVStore` is a plain `std::unordered_map<std::string, std::string>`
behind a single `std::mutex`. `Get`, `Put`, and `Delete` each take the lock
for their whole duration. No versioning, no transactions — every key holds
exactly one current value.

### Design decisions and trade-offs

**Why a single coarse-grained mutex, when the whole point of this project
is to eventually avoid that?**

Two reasons:

1. **It's the simplest correct thing.** Before building anything clever
   (versioning, snapshots, lock-free reads), there should be a
   dead-simple, obviously-correct baseline to build on and compare
   against. Getting fancy on day one is how you end up debugging a data
   race *and* a design at the same time.
2. **It doubles as the Phase 6 baseline.** Phase 6's whole goal is to show
   MVCC's "readers never block writers" advantage with real throughput
   numbers. That requires a naive, single-lock store to benchmark
   against. Rather than write that baseline twice, Phase 1's `KVStore`
   *is* that baseline — coarse-grained locking is exactly the thing MVCC
   is meant to remove, so keeping this implementation deliberately naive
   is what makes the eventual comparison meaningful. This is called out
   directly in the header comment for `KVStore` so it doesn't read as an
   oversight later.

**Why a `Status` return type instead of exceptions?**

`NotFound` (on `Get`/`Delete` of a missing key) is an expected, common
outcome — not an exceptional one. Using exceptions for it would either
force every caller into try/catch for normal control flow, or encourage
people to ignore errors by not catching at all. A `Status` return, in the
style LevelDB/RocksDB use, makes callers look at the result. Genuinely
invalid input (an empty key) also returns a `Status` rather than throwing,
for the same reason: it's a predictable, checkable condition, not a bug in
the program.

**Why does `Delete` on a missing key return `NotFound` instead of `OK`?**

Many real stores treat "delete something that wasn't there" as a
successful no-op. Here it's treated as `NotFound` on purpose: in later
phases, garbage collection will be deleting specific versions, and a GC
bug that tries to delete something twice should be loud, not silently
swallowed. Being stricter now costs nothing and catches more later.

**Why is an empty key rejected but an empty value allowed?**

An empty key has no obvious meaning as an identifier and is more likely a
caller bug (an unset variable, a bad string split) than an intentional
key. An empty *value*, on the other hand, is a completely reasonable thing
to store (e.g. a key used purely as a marker/flag), so there's no reason
to disallow it.

**Why is `KVStore` non-copyable?**

It owns a `std::mutex`, which is itself non-copyable, and represents one
logical store — copying it would either fail to compile confusingly or
require deciding whether a "copy" duplicates the data, shares it, or
something else. Disabling copy makes the ownership model explicit instead
of leaving it to whoever calls `KVStore store2 = store1;` to find out the
hard way.

### Testing approach

Tests are split into two files on purpose:

- `test_kv_store.cpp` — ordinary unit tests: happy path (put/get/delete,
  overwrite) plus edge cases (missing keys, empty store, empty key/value,
  delete-then-reinsert, large values, keys with embedded/special bytes).
- `stress_test_kv_store.cpp` — concurrency tests, run with many threads:
  - Disjoint-key concurrency, to catch races/corruption in the underlying
    map itself.
  - Mixed put/get/delete on a small shared key space, asserting only that
    nothing crashes and the store stays internally consistent (no
    assertion on final values, since many interleavings are valid).
  - A **documented limitation**: concurrent `Get`-then-`Put` increments
    lose updates, because `Get()` and `Put()` each lock independently —
    there's no atomicity across the two calls. The test doesn't treat
    this as a failure; it measures and reports the number of lost
    updates as a concrete number. This is intentional: it's the concrete
    motivation for Phase 3 (transactions) and Phase 5 (concurrency
    control) — composing two atomic operations does not produce an
    atomic operation, and the project needs to actually feel that pain
    before "fixing" it.

All stress tests are also run under ThreadSanitizer (`-DMVCC_SANITIZE=thread`)
to confirm the mutex genuinely prevents data races at the memory level —
separately from the lost-update issue above, which is a higher-level
atomicity problem, not a data race.

### Known limitations (carried forward, not hidden)

- No versioning — Phase 2.
- No isolation between concurrent read-modify-write sequences — Phase 3/5.
- Locking is coarse-grained (whole-store, not per-key) — revisited once
  versioning exists, since MVCC's whole point is that readers shouldn't
  need this lock at all.

## Phase 2: versioning

### What it is

`mvcc::VersionStore` replaces "one value per key" with "an append-only list
of `(version, value_or_tombstone)` per key." `Put` never overwrites; it
appends a new version. `Delete` appends a tombstone (a version whose value
is empty) rather than removing anything. `Get` returns the latest version's
value. `GetAsOf(key, as_of)` returns whatever value was visible to a reader
whose snapshot is "as of version `as_of`" — the core primitive Phase 3's
snapshot isolation will be built directly on.

`KVStore` from Phase 1 is untouched and still lives at `include/mvcc/kv_store.h`
— it stays exactly as it was so it can still serve as the naive baseline
in Phase 6, independent of everything versioning adds.

### Design decisions and trade-offs

**Why one global version counter instead of a counter per key?**

Snapshot isolation (Phase 3) needs a single number that means the same
thing everywhere: "the state of the *entire store* as of version N." If
each key had its own counter, "version 5" for key `a` and "version 5" for
key `b` would be unrelated points in time, and a transaction couldn't take
one consistent snapshot across multiple keys. A shared, monotonically
increasing counter — incremented under the same lock that performs the
write — makes every version number a single, comparable point on one
timeline. This costs some throughput (every write anywhere contends on the
same counter) but that trade-off is explicitly deferred to Phase 5, not
solved here.

**Why tombstones instead of actually removing the key on delete?**

Erasing history on delete would break the guarantee that motivated this
whole rewrite: a reader holding an older snapshot must still see the value
as it existed at that snapshot, even if the key has since been deleted by
someone else. A tombstone is a real, ordered version like any other — it
just carries "no value" instead of a value — so `GetAsOf` can tell a
caller "this key existed but had already been deleted by the time your
snapshot was taken" instead of conflating that with "this key never
existed."

**Why does `Delete` still reject deleting an already-tombstoned key?**

Same reasoning as Phase 1: this makes a bug that tries to delete something
twice fail loudly (`NotFound`) instead of silently appending a second,
redundant tombstone. Consistency with Phase 1's `KVStore` semantics also
means callers don't have to relearn delete behavior between the two
stores.

**Why `std::partition_point` for `GetAsOf` instead of a manual loop?**

The version history for a key is append-only and versions only increase,
so it's already sorted — `partition_point` does a proper binary search
(`O(log n)`) for the first entry whose version exceeds `as_of`, rather
than scanning linearly. For a key with a long history (which, without GC,
is expected — see below), this matters.

**Why does `VersionStore` still use one coarse mutex, same as Phase 1?**

Same answer as Phase 1: getting the versioning *logic* correct is this
phase's job. The lock also has to protect the version counter and the
per-key vector append together, atomically, or two threads could each grab
the same version number for different writes — a bug the stress tests in
this phase specifically check for (`ConcurrentPutsNeverIssueDuplicateVersions`).
Revisiting the locking strategy is explicitly Phase 5's job, once there's
an actual concurrency-control design (OCC vs. locking) to replace it with
— not something to improvise now.

**Known, accepted limitation: unbounded history growth.**

Nothing in this phase ever removes an old version. A key that's written
1000 times accumulates 1000 entries forever. This is intentional — Phase 4
(garbage collection) is specifically the phase responsible for deciding
which versions are no longer visible to *any* active transaction and safe
to reclaim. Doing that here, before transactions exist in Phase 3, would
mean guessing at a safety condition instead of deriving it from the actual
transaction model.

### Testing approach

- `test_version_store.cpp` — happy path (put/get, growing history, global
  version numbering across different keys), the as-of semantics that are
  the actual point of this phase (mid-history reads, reads before a key
  existed, reads before vs. at/after a delete, far-future reads), and edge
  cases (empty key/value, double delete, unknown-key queries).
- `stress_test_version_store.cpp` — three concurrency tests:
  - `ConcurrentPutsNeverIssueDuplicateVersions`: collects every version
    number issued across 16 threads and checks for duplicates with a
    `std::set`. A duplicate here would silently corrupt every later
    phase, since two different writes would become indistinguishable to
    a snapshot reader.
  - `PerKeyHistoryStaysSortedUnderConcurrentWrites`: sanity-checks that
    concurrent writers to a small shared key space don't break the
    sortedness `GetAsOf`'s binary search depends on.
  - `ConcurrentMixedOpsNeverCorruptHistory`: mixes concurrent
    Put/Get/Delete/GetAsOf on shared keys and, for every successful Put,
    immediately re-reads that *exact* version via `GetAsOf` to confirm
    past entries are never mutated by later concurrent writes — even
    while other threads are actively appending to the same key's history.

All stress tests pass cleanly under ThreadSanitizer (`-DMVCC_SANITIZE=thread`),
confirming the shared mutex correctly serializes both the version counter
increment and the per-key history append.

### Known limitations (carried forward, not hidden)

- Version history grows without bound — Phase 4 (GC).
- No transactions yet — `GetAsOf` is a raw primitive; nothing yet assigns
  a transaction a consistent snapshot version or coordinates multiple
  reads/writes against one — Phase 3.
- Still one coarse mutex for the whole store — Phase 5.

## Phase 3: transactions with snapshot isolation

### What it is

`mvcc::TransactionManager` hands out `mvcc::Transaction` objects. Each
`Transaction`:

- pins a **snapshot version** the instant it's created (`Begin()`), and
  every `Get` is answered from `VersionStore::GetAsOf(key, snapshot)` — so
  the transaction sees the store exactly as it looked at that instant,
  no matter what commits after
- buffers every `Put`/`Delete` **locally** instead of touching the store,
  so nobody else can see a transaction's writes until it commits — but the
  transaction itself always sees its own buffered writes when it re-reads
  a key it already touched (read-your-own-writes)
- on `Commit()`, applies its entire write buffer to the `VersionStore` in
  one call to the new `ApplyBatch` primitive, which assigns **one** new
  version number to every key in the batch under a single lock
  acquisition — so no concurrent reader can ever observe a state with only
  half of a transaction's writes applied
- on `Abort()`, or on going out of scope without either, discards its
  write buffer entirely; nothing it did is ever visible to anyone

`TransactionManager` also tracks every currently-active transaction's
snapshot version, and exposes `OldestActiveSnapshot()` — the version
number below which nothing could still be needed by any live transaction.
That's not decorative: it's the exact input Phase 4's garbage collector
will need to decide what's safe to reclaim.

### Design decisions and trade-offs

**Why does `Commit()` go through a new `VersionStore::ApplyBatch` instead
of just calling `Put`/`Delete` once per key?**

Atomicity. If a transaction committed by calling `Put` once per buffered
key, each call would grab the store's lock, bump the version counter, and
release the lock — separately, one key at a time. A concurrent reader
could then observe a version number where key `a`'s write had landed but
key `b`'s hadn't, even though both were part of the same logical
transaction. `ApplyBatch` takes the lock exactly once, assigns exactly one
new version, and appends every op in the batch before releasing it — so a
multi-key commit either fully happened or (from any outside observer's
point of view) hasn't happened at all yet. This is tested directly in
`MultiKeyCommitIsAtomicAtOneVersion`: both keys are confirmed visible
*at* the commit version and confirmed *absent* one version earlier.

**Why does `Delete` inside a transaction check the transaction's own
snapshot/write-buffer instead of the store's live state?**

Because "is this key deletable" has to mean "is it visible to *me*," not
"is it visible right now, globally." If transaction A's snapshot is taken
before some other transaction B deletes `k`, A should still be able to
call `Delete("k")` and have it behave exactly as it would have if B had
never run — because from A's isolated point of view, B hasn't happened
yet. Checking against `store_.GetAsOf(key, snapshot_version_)` instead of
`store_.Get(key)` is what makes that true; it's directly exercised by
`DeleteValidatesAgainstOwnSnapshotNotLiveStoreState`.

**Why is a read-only commit a no-op that doesn't bump the version
counter?**

`ApplyBatch` requires a non-empty op list for a reason: creating a new,
empty version for a transaction that changed nothing would be an
observable side effect (`CurrentVersion()` would jump) with no
corresponding actual change to explain it. A transaction that only reads
should be free — literally free, not "cheap" — to run and commit.

**Why is the `Transaction` constructor private, only reachable through
`TransactionManager::Begin()`?**

A `Transaction` has to be registered in `TransactionManager`'s active set
*before* anyone can use it — otherwise there's a window where a
transaction exists with a snapshot version that GC (Phase 4) doesn't know
to protect. Making the constructor private and friending `TransactionManager`
means there is no code path that produces a `Transaction` without also
registering it: construction and registration happen together, under the
same lock, in `Begin()`. This is a case of using the type system to make
an invalid state (an unregistered but usable transaction) unrepresentable,
rather than trusting every call site to remember to register manually.

**Why does the destructor auto-abort instead of requiring an explicit
`Abort()`?**

A `Transaction` that's dropped — because of an early return, an
exception, a scope exit — without an explicit `Commit()`/`Abort()` must
not stay "active" forever. If it did, `OldestActiveSnapshot()` would stay
pinned at that transaction's snapshot permanently, and Phase 4's GC would
never be able to reclaim anything from that point onward, for the
lifetime of the program. RAII cleanup in the destructor makes "forgot to
abort" fail safe instead of leaking a GC-blocking phantom transaction.
Tested directly in `DroppingTransactionWithoutCommitAutoAborts`.

**Why does every method reject calls once the transaction is no longer
active, rather than, say, allowing further reads after commit?**

A `Transaction` is a small state machine: active → (committed | aborted),
terminal either way. Allowing some operations (reads) but not others
(writes) after a terminal state would mean every caller has to remember
which subset is still legal — a worse API than "once it's done, it's
done." It's also just not clearly meaningful: what would a "read" through
an already-committed transaction even mean — its old snapshot, or the
store's live state? Rejecting outright avoids answering an ambiguous
question.

**THE HONEST LIMITATION — no write-write conflict detection yet.**

This is worth stating plainly rather than glossing over: two concurrent
transactions can both read the same key, both write it, and both commit
successfully — with the second commit's value silently overwriting the
effect of the first, and no error to either caller. Real snapshot
isolation requires detecting that and aborting the loser ("first committer
wins"). That validation does not exist yet in this phase — deciding
*how* to do it (optimistic concurrency control's validate-at-commit
approach, vs. actual locking) is explicitly Phase 5's job, not something
to bolt on early and get wrong.

This isn't a bug that slipped through: `DocumentsUndetectedWriteWriteConflicts`
demonstrates it with real numbers — 8 threads each incrementing a shared
counter 300 times through full transactions, all 2400 commits reporting
success, only ~300 increments actually surviving. That's the concrete,
measured shape of the gap Phase 5 exists to close. Calling this phase
"snapshot isolation" without that caveat would overstate what it
guarantees; what it actually provides today is **snapshot reads with
last-committer-wins writes**, and that distinction matters.

### Testing approach

- `test_transaction.cpp` — snapshot correctness (a transaction is blind to
  writes made after it began, and sees a consistent view across multiple
  keys), read-your-own-writes (including a buffered put-then-delete of a
  brand-new key that isn't in the store at all), commit semantics
  (visibility after commit, atomicity of multi-key commits, read-only
  commits not bumping the version counter), abort semantics (explicit and
  via destructor), the active/committed/aborted state machine (every
  operation rejected once terminal, in both terminal states), delete
  semantics validated against the transaction's own view rather than live
  store state, and `TransactionManager` bookkeeping (`OldestActiveSnapshot`
  tracking the true minimum as transactions begin/end, `ActiveTransactionCount`
  through a full lifecycle, unique transaction IDs).
- `stress_test_transaction.cpp` — four concurrency tests:
  - `ConcurrentDisjointKeyTransactionsAllCommitCleanly`: 16 threads running
    full begin/write/commit cycles on disjoint keys; every commit must
    succeed and every write must land correctly.
  - `LongLivedSnapshotIsUnaffectedByConcurrentCommits`: one long-lived
    reader transaction holds its snapshot while 8 threads race ahead
    committing thousands of writes to the same key — the reader must still
    see its original value at the end. This is snapshot isolation's core
    promise, tested under actual contention rather than in isolation.
  - `DocumentsUndetectedWriteWriteConflicts`: the honest-limitation
    demonstration described above, made reproducible with a deliberate
    `yield()` between read and write (same technique as Phase 1's
    lost-update test, for the same reason — without it, a lucky scheduler
    can make the race rare enough to hide in a single run).
  - `OldestActiveSnapshotRemainsSaneUnderConcurrency`: a background thread
    continuously samples `OldestActiveSnapshot()` while 6 threads run
    transactions concurrently, asserting it's never observed to exceed the
    store's current version — a basic sanity invariant it must never
    violate no matter how transactions interleave.

All stress tests pass cleanly under ThreadSanitizer, confirming no data
races across `TransactionManager`, `Transaction`, and `VersionStore`
working together concurrently — separate from, and not to be confused
with, the write-write conflict limitation above, which is a
higher-level correctness gap, not a memory-safety issue.

### Known limitations (carried forward, not hidden)

- **No write-write conflict detection** — concurrent transactions can
  silently clobber each other's committed writes. This is the headline
  limitation of this phase; see above. Phase 5.
- Version history still grows without bound — Phase 4 (GC).
- `TransactionManager::Begin()` reads the store's current version and
  registers it as this transaction's snapshot under the manager's own
  lock, not the store's — so the snapshot is guaranteed to be *some*
  consistent, correctly-tracked point in time, but there's an inherent
  (and harmless) scheduling-dependent window in exactly which version a
  transaction started concurrently with a burst of writes will land on.
  This does not affect correctness of what "snapshot as of version N"
  means once assigned — only which N gets assigned when several
  operations race to begin at nearly the same instant.
- Still one coarse mutex inside `VersionStore` for the whole store, now
  shared by every transaction's commits too — Phase 5.

## Phase 4: garbage collection

### What it is

`mvcc::VersionStore::CollectGarbage(safe_boundary)` trims each key's
version history down to just what could still be needed: everything
strictly after `safe_boundary` is left untouched, plus exactly one
version at or before it — the newest one, since that's what a reader
whose snapshot is exactly `safe_boundary` would ask for. Everything else
is erased. If, after trimming, a key's whole remaining history is one
lone tombstone, the key's entry is dropped from the table entirely — a
deleted key that will never be queried again doesn't need to keep taking
up space.

`mvcc::GarbageCollector` is the driver: it asks `TransactionManager` for
`OldestActiveSnapshot()` and passes that straight to `CollectGarbage` as
the boundary. It can be run on demand (`RunOnce()`) or continuously in a
background thread (`StartBackground(interval)` / `StopBackground()`),
mirroring how a real storage engine runs compaction/GC as an ongoing
background task rather than something the caller has to remember to
trigger.

### Design decisions and trade-offs

**Why is `safe_boundary` computed as `OldestActiveSnapshot()`, and why is
that provably safe even though it's read before GC actually runs (not
atomically with it)?**

This is the correctness argument the whole phase depends on, so it's
worth stating precisely (it's also in the `GarbageCollector` header
comment, not just here): between reading the boundary and running the
actual sweep, two things could happen —
1. A transaction that was *already active* when the boundary was read has
   a snapshot >= that boundary, by construction (the boundary is the
   minimum over exactly those transactions' snapshots).
2. A transaction that begins *after* the boundary was read gets a
   snapshot equal to the store's current version *at that later moment*
   — and the store's version counter only ever increases, never resets
   or goes backward.

Either way, nothing — already running or yet to start — can ever hold a
snapshot older than a boundary this class has already acted on. That's
what makes it safe to treat "compute the boundary" and "sweep the store"
as two separate, non-atomic steps instead of needing one lock that spans
both `TransactionManager` and `VersionStore` for the whole operation.
`GCStress.LongLivedReaderSurvivesAggressiveConcurrentGCAndWrites` is the
test that actually exercises this under real, adversarial concurrency
(GC running every 2ms while 8 threads hammer the exact key a long-lived
reader is holding) rather than just trusting the argument on paper.

**Why does `CollectGarbage` live on `VersionStore` instead of inside
`GarbageCollector`?**

Encapsulation: `table_` is `VersionStore`'s private internal
representation, and pruning it requires the exact same lock discipline
every other operation on it already uses. Letting `GarbageCollector`
reach in and mutate `VersionStore`'s internals directly would mean two
different classes independently need to agree on locking invariants.
Instead, `VersionStore` owns the one operation that's allowed to shrink
its own history, and `GarbageCollector` is just the policy layer that
decides *when* and *with what boundary* to call it — it has no privileged
access `VersionStore`'s own API doesn't already grant.

**Why fully remove a key whose only remaining version is a tombstone,
instead of always leaving at least one version behind?**

Otherwise the table would only ever grow. A workload with a lot of
create/delete churn on short-lived keys — think temporary locks, session
tokens, queue markers — would otherwise leave a permanent tombstone
behind for every key that was ever deleted, forever, defeating the
purpose of GC. Once a key's entire remaining history is "deleted, and
nothing after," no future reader can ever legitimately ask for it again
(any future snapshot is >= the current version, which is itself >= any
past `safe_boundary`), so keeping the record around serves nobody.
`FullyRemovesKeyWhoseOnlyRemainingVersionIsATombstone` and
`KeepsTombstoneIfNewerVersionsExistAfterIt` both check the boundary of
this behavior directly — the tombstone stays if the key was later
recreated, since then it's not "the end of this key's story" anymore.

**Why offer both `RunOnce()` and a background thread, instead of just
one?**

They serve different needs and both are realistic: `RunOnce()` is useful
for tests (deterministic, one pass, inspect the stats) and for a caller
that wants explicit control (e.g. "collect now, right before this
benchmark run"). `StartBackground()` is closer to how a real system would
actually operate GC — continuously, without every caller needing to
remember to trigger it. Building only the synchronous version would leave
a gap in demonstrating that GC can safely run *concurrently* with
everything else, which is the part of this phase that's actually hard.

**Why a `std::condition_variable` for the background loop's sleep instead
of plain `std::this_thread::sleep_for`?**

`StopBackground()` needs to be responsive — a caller shutting down
shouldn't have to wait out the rest of a possibly-long GC interval before
the thread notices and exits. A condition variable lets `StopBackground()`
wake the loop immediately via `notify_all()`, while the loop still wakes
naturally on its own if left alone for the full interval. This is checked
directly: `DestructorStopsBackgroundThreadWithoutHanging` starts a
background GC thread and lets the owning object destruct without an
explicit stop — if shutdown weren't responsive (or the destructor forgot
to join), that test would hang or crash rather than complete.

**HONEST LIMITATION — this is a stop-the-world collector.**

`CollectGarbage` holds `VersionStore`'s single mutex for the entire
sweep across every key in the table. That means every `Get`, `Put`,
`Delete`, and transaction commit anywhere in the store is blocked until
one GC pass finishes. For a store with a large number of keys, that's a
periodic latency spike, not a graceful background task. This is the same
"one lock for the whole store" limitation Phase 5 exists to address —
making GC incremental (e.g. sweeping a bounded batch of keys per call
instead of everything) is a natural improvement once that phase changes
how the store is locked in the first place, so it's treated as part of
that work rather than patched in early with a design that would likely
need to change again anyway.

### Testing approach

- `test_version_store.cpp` (`VersionStoreGC` section) — the collection
  logic itself in isolation: reclaiming only what's older than the
  boundary, never touching anything at or after it, a boundary older than
  all history reclaiming nothing, full key removal on an
  all-history-is-one-tombstone key, a tombstone surviving if the key was
  later recreated, idempotency of running the same boundary twice, and
  multiple keys being trimmed independently of each other.
- `test_gc.cpp` — `GarbageCollector` as the policy layer: a run with no
  active transactions reclaiming everything but the latest version, an
  active transaction's snapshot surviving a GC pass untouched, more
  becoming collectible once that transaction ends, cumulative stats
  correctly summing across multiple passes, and the background thread's
  full lifecycle (start/stop, idempotent start and stop, periodic
  collection actually happening over real wall-clock time, and clean
  destructor shutdown without hanging).
- `stress_test_gc.cpp` — three tests aimed specifically at the scenario
  this phase exists to get right, under real concurrency rather than
  single-threaded simulation:
  - `LongLivedReaderSurvivesAggressiveConcurrentGCAndWrites`: a reader
    holds a snapshot while background GC runs every 2ms *and* 8 threads
    concurrently hammer writes to the exact key it's reading — the
    reader's original value must still be intact at the end.
  - `MultipleStaggeredSnapshotsAllSurviveConcurrentGC`: six transactions
    with staggered begin times, each holding a different snapshot,
    interleaved with churn and background GC — every one must still see
    exactly its own version, not a neighbor's.
  - `ConcurrentManualGCFromMultipleThreadsNeverCorruptsStore`: three
    threads independently calling `RunOnce()` at the same time (not just
    one background thread) while 8 more threads run mixed
    read/write/delete transactions — checks GC itself is safe to invoke
    concurrently from multiple callers, not just safe alongside a single
    background loop.

All of the above — including the background-thread start/stop lifecycle
— pass cleanly under ThreadSanitizer, confirming no data races in the GC
path, the condition-variable-based shutdown signaling, or its interaction
with concurrent transaction commits.

### Known limitations (carried forward, not hidden)

- **Stop-the-world sweeps** — a GC pass locks the entire store for its
  duration; not incremental. See above; revisited alongside Phase 5.
- **No write-write conflict detection** (carried over from Phase 3) —
  still open, still Phase 5's job.
- GC's correctness depends on `GarbageCollector` being the only caller of
  `VersionStore::CollectGarbage` with a boundary sourced from
  `TransactionManager::OldestActiveSnapshot()`. `CollectGarbage` is a
  public method on `VersionStore` and has no way to verify that whoever
  calls it directly is passing a safe value — it trusts its caller. This
  is a deliberate simplicity trade-off for now (encapsulating that
  invariant fully would mean either making `CollectGarbage` private to a
  friended `GarbageCollector`, or threading a capability/token through
  the call, either of which is more machinery than this phase needs) —
  worth revisiting if the codebase grows enough callers that the trust
  boundary becomes a real risk rather than a theoretical one.

## Phase 5: write concurrency control

### The decision: optimistic concurrency control, not locking

Phase 5's brief was explicitly open — OCC or locking, to be decided and
justified, not handed down. OCC won, for a reason specific to this
project rather than a generic "OCC is usually better": **two-phase
locking would mean transactions take read locks on every key they
touch, and that directly contradicts the entire premise of Phases 1–4.**
The whole point of building MVCC instead of a plain locked store was
"readers never block writers" — that's the exact claim Phase 6 exists to
measure. Introducing read-locking in Phase 5 would mean building the
thing this project set out to demonstrate *not* needing, one phase before
proving it wasn't needed.

OCC, by contrast, is a natural extension of what's already built rather
than a new mechanism bolted on: `Transaction` already buffers every write
locally and only touches the store once, at `Commit()`. Optimistic
validation is just "check at that one existing touch-point before
applying it" — no new locking discipline, no lock-ordering/deadlock
concerns (2PL's classic operational headache), and it stays true to the
premise that concurrent readers should never be blocked by anything at
all — which remains exactly true after this phase: `Get()` never takes
any lock beyond what `VersionStore` already briefly holds per call, and
never contends with a concurrent writer's *commit* logic beyond that same
brief window.

### What it is

`Transaction::Commit()` now validates before applying: it checks, for
every key in its write set, whether anyone else has committed a newer
version of that key since this transaction's snapshot was taken. This is
**first-committer-wins**: if a conflict is found, nothing in the write
set is applied — not even the non-conflicting keys — the transaction is
aborted, and `Status::Conflict()` is returned so the caller can retry.
The check-and-apply happens as one atomic step, under one lock
acquisition (`VersionStore::ApplyBatchIfNoConflict`), so there's no
window between "we validated" and "we applied" for a second transaction
to sneak in and invalidate the check.

`RunWithRetry(mgr, fn, max_attempts)` is the intended way to drive this
under contention: it runs `fn` in a fresh transaction, and on
`Status::Conflict()` retries with a brand-new transaction (a fresh
snapshot — retrying with the stale one would just conflict again) up to
`max_attempts` times.

### Design decisions and trade-offs

**Why does a conflict abort the *entire* commit, not just the
conflicting key(s)?**

Partial application would silently change what the transaction actually
means. If a transaction wrote `a` and `b` together because the caller's
logic required both to change together (a transfer: debit one account,
credit another), applying only `b` because `a` happened to conflict would
commit a broken, half-finished state — worse than the lost-update problem
this phase exists to fix. All-or-nothing is the only option that
preserves what "transaction" is supposed to mean.
`ConflictOnAnyKeyAbortsEntireCommitNotJustThatKey` checks this directly.

**Why does `ApplyBatchIfNoConflict` live on `VersionStore` (a new
sibling to `ApplyBatch`) instead of validating inside `Transaction`
before calling the existing `ApplyBatch`?**

Because that would reopen exactly the race this phase is closing: if
`Transaction` checked "is anyone's version newer than my snapshot?" as a
separate step before calling `ApplyBatch`, another transaction could
commit in the gap between that check and the apply — the validation would
be correct at the moment it ran and stale by the time it mattered. The
validation and the write have to happen while holding the *same* lock
acquisition, which means they have to be one operation inside
`VersionStore`, where the lock actually lives. This is a direct,
concrete instance of "check-then-act needs to be atomic," the same
principle (at a different layer) as why `ApplyBatch` itself exists in
the first place back in Phase 3.

**Why is conflict resolution "retry the whole transaction," not "retry
just the failed write"?**

By the time a conflict is detected, the transaction's snapshot is already
stale — not just for the conflicting key, but as a matter of principle:
the transaction's whole view of the world was taken at that snapshot, and
if any part of what it read or reasoned about could have changed, the
safest correct move is to redo the read-then-decide logic against current
data, not patch just the write. This is why `RunWithRetry` takes a
function that performs the *entire* body (reads included) rather than
just a value to write — `TransactionStress.RunWithRetryAppliesEveryIncrementWithZeroLostUpdates`
depends on this: each retry re-reads the counter's current value rather
than blindly reapplying a stale increment.

**Why is `RunWithRetry` a free function rather than a method on
`TransactionManager` or `Transaction`?**

It doesn't hold any state of its own — it's a usage pattern (a loop
around `Begin`/`fn`/`Commit`) built entirely from the existing public
API of both classes. Making it a free function keeps it honestly
optional: nothing about `Transaction` or `TransactionManager` depends on
it existing, and a caller who wants different retry behavior (backoff,
a retry budget shared across many operations, etc.) can write their own
version using the same primitives without needing to work around a
method that assumes one specific policy.

**What this phase does *not* fix — full serializability is still out of
scope.**

Worth stating precisely rather than letting "OCC" sound like it solved
everything: this validates *write-write* conflicts only. It does not
track each transaction's read set, so it cannot detect **write skew** —
two transactions that each read some overlapping data and then write to
*different* keys in a way that violates an invariant spanning both (the
classic example: two doctors both check "am I the last on-call doctor"
and both go off-call because each individually saw someone else still
on, even though the invariant "at least one doctor on call" required
that check and the write to be atomic together). Catching that requires
full **Serializable Snapshot Isolation** — tracking rw-antidependencies,
not just ww-conflicts — which is real, meaningfully more complex
machinery (this is what PostgreSQL's SSI implementation does). What this
phase actually guarantees is precisely "snapshot isolation with
first-committer-wins," which happens to be the default isolation level
most production MVCC databases actually ship — not a toy simplification,
but also genuinely not the same guarantee as full serializability. Being
able to state that distinction precisely is worth more than claiming a
stronger guarantee than what's built.

### Testing approach

- `test_transaction.cpp` (`TransactionConflict` and `RunWithRetryTest`
  sections) — direct, deterministic proof of the mechanism: a real
  conflict detected and reported, disjoint-key writes never conflicting
  despite being concurrent, a conflicted transaction left aborted and
  unregistered (so it stops blocking GC's boundary too), the
  all-or-nothing property under a mixed conflicting/non-conflicting
  multi-key write, read-only commits never conflicting regardless of
  concurrent activity, and the conflicting-keys list being reported
  correctly. For `RunWithRetry`: an immediate no-conflict success, a
  non-conflict failure from the body propagating without any retry, a
  conflict forced by an "interloper" transaction being retried exactly
  once and then succeeding, retries genuinely exhausting when every
  attempt is made to conflict, and input validation on `max_attempts`.
- `stress_test_transaction.cpp` — two tests that directly complete the
  story Phase 3 started:
  - `DirectConflictingCommitsAreDetectedNotSilentlyLost`: the same 8
    threads × 300 increments scenario Phase 3 used to demonstrate the
    gap, run again with no retry logic at all — this time every commit
    either succeeds or reports `Status::Conflict()`, never silently
    vanishes, and the final counter value is proven to equal *exactly*
    the count of successful commits — zero lost updates among the ones
    that succeeded.
  - `RunWithRetryAppliesEveryIncrementWithZeroLostUpdates`: the actual
    fix in action — using `RunWithRetry`, all 2400 increments across 8
    threads land with zero loss, compared directly to Phase 3's ~90%
    loss rate under the identical contention pattern. This needed a
    generous retry ceiling (documented in the test itself) once
    measured against this sandbox's real single-core scheduling
    behavior — a case where a first-pass assumption (the default
    `max_attempts=8`) turned out to be too tight under adversarial
    contention and was corrected against actual measured behavior
    rather than guessed at.

All of the above pass cleanly under ThreadSanitizer, alongside the full
existing suite from Phases 1–4 (93 tests total), confirming the new
validation path introduces no data races with concurrent commits, reads,
or GC.

### Known limitations (carried forward, not hidden)

- **No write-skew detection** — this phase closes the write-write
  lost-update gap but is not full serializability. See above.
- **Stop-the-world GC sweeps** (carried over from Phase 4) — unrelated to
  OCC, still an open item, still tied to the store's single-lock design.
- Conflict validation itself is O(size of the write set) per commit and
  happens under the store's single mutex, same as every other operation
  — so heavy write contention on a small number of hot keys will still
  serialize through that lock, same as it always has. OCC changes what
  happens when transactions collide (fail fast and retry, instead of
  silently corrupting), not how much they contend for the lock in the
  first place. Actually reducing that contention (finer-grained locking,
  sharding) remains the province of whatever Phase 6's benchmark reveals
  is worth pursuing next.

## Phase 6: benchmark vs. a naive single-lock store

### A prerequisite fix, found and made before benchmarking anything

Before writing a single line of benchmark code, I checked something
directly rather than assuming it: did `VersionStore`'s read path
(`Get`, `GetAsOf`) actually allow concurrent readers, or was it still
serializing behind the exact same lock as every write? It was the
latter — `Get` and `GetAsOf` were taking the identical `std::mutex` that
`Put`, `ApplyBatch`, `ApplyBatchIfNoConflict`, and `CollectGarbage` all
used. Every phase from 2 through 5 had correctly flagged "still one
coarse mutex" as a known limitation, but nothing had actually revisited
it — none of those phases' scopes required it. Benchmarking as-is would
have produced a result that **directly contradicted this project's own
thesis**: the MVCC stack would show pure overhead (transaction
bookkeeping, version history) with zero offsetting concurrency benefit,
because the exclusivity "readers never block writers" is supposed to
remove was still fully present.

The fix: `VersionStore`'s internal `std::mutex` became a `std::shared_mutex`.
Reads take a `std::shared_lock` (concurrent with each other); writes keep
a `std::unique_lock` (exclusive, as before). This is a genuine, scoped,
industry-standard technique — a classic reader-writer lock — not a
redesign. **What it does and does not achieve is stated precisely, not
oversold**: it makes readers concurrent with *each other*. It does not
make a reader immune to a writer's critical section — a writer holding
the exclusive lock still briefly blocks every reader. A design where a
reader is *never* blocked by a writer even for an instant needs a
fundamentally different data structure — per-key lock-free version
chains published via atomic compare-and-swap — which is real,
substantially harder lock-free programming and is explicitly out of
scope here. This upgrade is the well-understood half of that story:
eliminate reader-vs-reader contention, which existed for no reason
before this, while being honest that writer-vs-reader contention is
reduced to a short critical section, not eliminated.

After this change, the **entire existing 94-test suite (Phases 1–5,
unmodified) was re-run** — 3 consecutive clean passes plus a full
ThreadSanitizer run — before any benchmark code was written, specifically
to prove the lock-type swap introduced no regression and no new race. A
new liveness test (`ManyConcurrentReadersCompleteWithoutDeadlockOrCorruption`,
32 threads × 5000 reads each) was added to check the swap directly.

### The benchmark

`bench/bench_mvcc_vs_kv.cpp` is a standalone program (`./mvcc_bench`,
built by default alongside the tests) — not a gtest binary, since it
measures wall-clock performance rather than asserting pass/fail.
Methodology (also stated in full in the file's own header comment):

- **Unit of work, not raw call.** A KVStore "reader op" is one `Get()`
  call; an MVCC "reader op" is one full read-only transaction
  (`Begin()` → `Get()` → `Commit()`). A KVStore "writer op" is one
  `Put()`; an MVCC "writer op" is one `RunWithRetry()`-wrapped
  read-modify-write, **including the cost of any OCC retries** — the
  full end-to-end cost a real caller would pay, not just the final
  successful attempt.
- Keys are drawn uniformly from a fixed 64-key space per run, so
  contention (and OCC conflicts) genuinely occur rather than being
  simulated.
- Throughput is `op count / measured wall-clock duration` of the whole
  concurrent run, not derived from summed per-op latencies (which would
  double-count overlapping work across threads).
- Latency is reported as p50/p95/p99/max in microseconds, not just a
  mean — so a handful of slow outliers (there always are some, from
  scheduling and lock contention) show up honestly instead of being
  averaged away.
- Three reader:writer mixes are run (read-heavy 8R/1W, balanced 4R/4W,
  write-heavy 1R/8W), each against three configurations: `KVStore`
  (Phase 1 baseline), the MVCC stack with GC off, and the MVCC stack with
  background GC running every 5ms.
- Every configuration is run **3 times** (1 second each), and the
  reported summary uses the **median** across those trials, with every
  individual trial's raw throughput printed alongside it. This was not
  the original design — a real before/after comparison on this project's
  own hardware (see below) showed the unchanged control configuration
  swinging several-fold between single runs purely from system noise,
  which made a single-run comparison worthless for judging anything. The
  fix is direct evidence of the project's own measurements being taken
  seriously enough to correct the tool that produced them.

### Results actually measured — single-core first, then the correction that mattered

This sandbox reports `std::thread::hardware_concurrency() == 1` — a
single logical core. The results below are the real, unedited output of
running `./mvcc_bench` here (read-heavy scenario shown; balanced and
write-heavy follow the same pattern — full output is reproducible by
running the binary yourself):

```
-- KVStore (Phase 1 baseline, single exclusive mutex) --
  readers   ops=13,075,008  throughput=6,536,647 ops/s   p50=0.07us  p95=0.07us  p99=0.08us
  writers   ops=1,355,492   throughput=  677,657 ops/s   p50=0.07us  p95=0.07us  p99=0.08us
-- MVCC stack (shared_mutex reads, OCC writes, no GC) --
  readers   ops=7,899,844   throughput=3,947,311 ops/s   p50=0.15us  p95=0.19us  p99=0.23us
  writers   ops=213,802     throughput=  106,830 ops/s   p50=0.44us  p95=0.73us  p99=1.57us
-- MVCC stack (shared_mutex reads, OCC writes, background GC every 5ms) --
  readers   ops=8,272,660   throughput=4,118,851 ops/s   p50=0.14us  p95=0.18us  p99=0.21us
  writers   ops=283,371     throughput=  141,087 ops/s   p50=0.33us  p95=0.48us  p99=0.61us
```

**The honest conclusion, stated without hedging: on this single-core
sandbox, the MVCC stack does not outperform the naive baseline — it is
measurably slower, on every metric, for both readers and writers.**
Readers run roughly 2x slower per operation; writers run roughly 5–6x
slower. I had drafted an initial hypothesis in the benchmark's own
printed output claiming reader *tail* latency might still favor the
MVCC stack even on one core, reasoning that a shared_mutex should reduce
reader-vs-reader queueing. **The measured p50/p95/p99 numbers do not
support that** — KVStore is faster at every percentile, not just on
average. I corrected the benchmark's printed interpretation note to stop
asserting that, rather than let a plausible-sounding hypothesis stand
uncontradicted by the very data sitting below it.

**Why this is the expected result, not a failure of the project:** "readers
never block writers" is fundamentally a *multi-core parallelism* claim —
it describes what happens when two threads can genuinely execute at the
same instant. On one core, nothing ever executes at the same instant as
anything else, regardless of which locking strategy is used; the OS
scheduler just decides who runs next. What a single core *can* and does
measure honestly is the MVCC stack's real, fixed overhead: a
`Transaction` object, a version-history entry per write instead of an
in-place overwrite, and (for writers) OCC validation — all real costs
that only pay for themselves once genuine concurrent execution exists to
convert "avoided blocking" into "actual additional throughput." Measuring
that overhead precisely, on hardware that can't yet show the benefit
meant to outweigh it, is itself a legitimate and useful result — it's
the honest price tag of this design, decoupled from the payoff.

### Real multi-core results, properly measured — and what they actually show

This project was tested on a 10-core machine (a MacBook Air; results
reproduced independently outside this repository's own development
environment, on a different OS and compiler toolchain than it was built
with, and all 94 tests passed there unmodified before the benchmark was
run). The first two attempts at this exposed a real problem before they
answered the question they were meant to: single-run throughput on real
hardware turned out to vary roughly 8x between separate process
launches, purely from system noise (background load, thermal/frequency
scaling, scheduler variance) — even for `KVStore`, code that was never
touched between those runs. A before/after comparison built on single
runs couldn't separate a real code effect from that noise, so before
trusting any number, the benchmark itself was fixed: every configuration
now runs 3 times, reporting the median with every individual trial's
throughput printed alongside it. The result below is from that corrected
methodology, and the trial-to-trial spread is now tight (under 2% for
`KVStore`, under 0.5% for the MVCC stack) — evidence the measurement
itself is finally trustworthy, not just the number it produced.

Median across 3 trials, all three scenarios:

```
                          KVStore readers   MVCC readers   ratio    KVStore writers   MVCC writers   ratio
Read-heavy (8R/1W)          3,953,728         478,195      0.121       293,165          31,440       0.107
Balanced   (4R/4W)          1,759,862         190,574      0.108     1,299,139         124,263       0.096
Write-heavy(1R/8W)            365,265          36,136      0.099     2,247,019         193,078       0.086
```

**The honest, well-supported headline: on real 10-core hardware, the
MVCC stack runs at roughly 9–12% of the naive baseline's throughput,
consistently across every reader:writer mix.** This is not a single
noisy sample — it closely matches an earlier independent single-run
measurement taken with the same code (ratios of 0.13/0.11/0.09), which
is exactly the kind of cross-check that makes a result trustworthy
rather than lucky.

One thing this data explicitly **cannot** settle, stated plainly rather
than glossed over: whether the `TransactionManager::Begin()` allocation
fix (below) helped, hurt, or made no difference. The only pre-fix
measurement available is a single, unreplicated run — not a tight
median like the post-fix numbers above — so comparing it against this
result would repeat the exact methodological mistake that prompted
fixing the benchmark in the first place. That comparison is not
reconstructable after the fact with the rigor it would need; it's an
honest gap, not a claimed result.

### Root cause, found by reading the code rather than guessing again

`TransactionManager::Begin()` and `TransactionManager::Unregister()`
(called by every `Commit()`, `Abort()`, and by a transaction's destructor
if neither was called) both lock `TransactionManager`'s own `mutex_` —
a plain `std::mutex`, not a `shared_mutex`. **Every single transaction,
whether it ever reads or writes any actual data or not, must acquire this
one exclusive lock twice in its lifetime.** The Phase 6 `shared_mutex`
upgrade fixed contention on `VersionStore`'s data — but a benchmark
"reader op" is a full transaction (`Begin()` → `Get()` → `Commit()`), and
two of those three steps go through a *second*, completely separate
exclusive lock that was never examined. Fixing `VersionStore` alone was
necessary but not sufficient; there was a second coarse lock hiding one
layer up, in the bookkeeping layer built on top of it. **This — not
single-core hardware, which was the first, incomplete explanation — is
the primary reason the MVCC stack underperforms the naive baseline even
with real parallelism available**, and it is consistent with, and
explains, the ~10x gap measured above.

One genuine, safe fix was made and verified (full 94-test suite + full
ThreadSanitizer, zero regressions): `Begin()` was allocating the
`Transaction` object with `new` *while still holding* `mutex_`, because
in the original code the lock guard's destructor doesn't run until after
the function's return value is constructed. That heap allocation now
happens after the lock is released — a real, correct improvement to the
critical section's length, though (as above) not one this project can
currently claim a measured before/after effect for.

**Why this can't be fixed the same way `VersionStore` was**, and this is
the more important insight: a `shared_mutex` helps when most callers only
need to *read* shared state. Every single call into
`TransactionManager`'s bookkeeping — `Begin()` and `Unregister()` alike —
needs to *mutate* `active_snapshots_` (insert or erase an entry). There
is no "read-only majority" here for a shared lock to free up; the map
requires exclusive access on every transaction lifecycle event, full
stop, by construction. A real fix needs a genuinely different concurrent
data structure for tracking active snapshots — a sharded/lock-free map,
or an epoch-based scheme that derives "the oldest active snapshot"
without a single global lock every transaction must pass through. That is
real, non-trivial lock-free/concurrent-programming work — a different
category of problem than "pick a different mutex type," and meaningfully
riskier to get right this late in a project that has spent five phases
building and proving correctness guarantees I was not willing to put at
risk with a rushed rewrite. It is correctly diagnosed and precisely
scoped here, and deliberately left as the clearly-identified next step
rather than an unexamined blind spot.

### A genuinely interesting, now well-confirmed finding: GC makes both reads and writes faster, not just leaner

This wasn't hypothesized going in — it fell out of the actual
measurements, and the corrected multi-trial methodology confirms it more
strongly than the original single-run measurement did. Enabling
background GC increased throughput for **both** readers and writers, in
**every** scenario:

```
                       reader gain from GC   writer gain from GC
Read-heavy (8R/1W)           +14.3%               +9.1%
Balanced   (4R/4W)           +23.6%              +13.7%
Write-heavy(1R/8W)           +18.5%              +13.7%
```

The original single-run measurement only clearly showed the writer
effect; this cleaner data shows readers benefit at least as much, and the
explanation extends naturally: with GC off, a fixed 64-key space under
sustained load means individual keys' version-history vectors grow very
large — `push_back` on an ever-growing vector has a real, growing
constant factor (occasional reallocation copies, worsening cache
locality), and `GetAsOf`'s binary search over a much longer vector does
more comparison work and touches more cache lines. With GC running every
5ms, per-key histories stay small, and both `Put` and `Get` stay cheap.
Phase 4's README asserted, qualitatively, that unbounded version growth
was a real cost worth collecting — this benchmark, now properly
controlled, is the first place in the project that actually **measures**
that cost precisely, on two different machines, for both reads and
writes.

### Known limitations (carried forward, not hidden)

- **The TransactionManager bookkeeping bottleneck, above.** This is the
  primary, root-caused explanation for why the MVCC stack underperforms
  the naive baseline (~9–12% of its throughput, consistently, across a
  properly controlled multi-trial measurement) even with real parallel
  hardware available. A precise fix is scoped but not implemented.
- **No reliable measurement of the Begin() allocation fix's isolated
  effect.** The fix is real and verified for correctness (full suite +
  ThreadSanitizer). The only pre-fix measurement available is a single
  unreplicated run, not a tight multi-trial median like the post-fix
  numbers above — comparing the two would repeat the exact single-run
  fallacy that prompted correcting the benchmark's methodology in the
  first place, so that comparison is deliberately not claimed.
- **Writer-vs-reader blocking still exists** within `VersionStore` itself,
  just minimized to a short critical section — see the shared_mutex
  discussion above. True lock-free reads (immune to a writer even
  briefly) remain unbuilt and out of scope, and are secondary to the
  TransactionManager issue above in explaining the measured results.
- **The benchmark measures this implementation, not MVCC in the
  abstract.** A production-grade MVCC engine (lock-free snapshot
  tracking, per-key lock-free version chains, sharded storage,
  incremental GC) would show a materially different result than this
  deliberately simple, mutex-based implementation — that gap between
  "this project" and "a production engine" is exactly what Phases 1–6's
  documented limitations, taken together, describe. This project's honest
  contribution is a correct, well-tested MVCC *design* with its real
  costs precisely measured — not a claim that this specific
  implementation is fast.

## Phase 7: fixing the TransactionManager bottleneck

Phase 6 found and root-caused a real bottleneck: `TransactionManager`
tracked active transaction snapshots in one `unordered_map` behind one
plain `std::mutex`, and *every* transaction — read-only or not — had to
acquire that lock exclusively twice in its lifetime (`Begin()` and
`Unregister()`), regardless of the Phase 6 `shared_mutex` upgrade to
`VersionStore`'s data path. Phase 6 explicitly scoped this as a real fix
needed, deliberately not attempted in a rush. This phase is that fix.

### What it is

`active_snapshots_` is now split across **16 independent shards**, each
with its own `std::mutex`. A transaction's shard is `txn_id % 16`, so
concurrent `Begin()`/`Unregister()` calls for different transactions
usually land on different shards and don't contend with each other at
all — only calls that happen to collide on the same shard still
serialize, roughly 1/16th as much contention as the single global lock.
`next_txn_id_` is now a lock-free `std::atomic<uint64_t>` counter, not
protected by any lock at all.

### The correctness problem sharding introduces, and how it's solved

Removing the single global lock removes a property the original design
got "for free": `OldestActiveSnapshot()` (which GC depends on for
correctness) could never previously observe a transaction that had read
its snapshot but not yet recorded it, because computing that boundary
took the *same* lock `Begin()` held for its *entire* duration — snapshot
read and registration happened as one atomic, indivisible step. Sharding
breaks that unless something else preserves it.

**The concrete danger, precisely:** if a transaction reads its snapshot
`S`, then (before registering) a concurrent GC pass computes a boundary
that doesn't account for it and reclaims versions older than `S`, that
transaction's later reads at `S` could silently return wrong data —
exactly the kind of severe, silent correctness bug this project has been
built from the ground up to avoid.

**The fix — a two-phase registration protocol in `Begin()`:**
1. Insert a **placeholder value of `0`** into this transaction's shard,
   under that shard's lock, *before* reading the store's current version.
2. Read the store's actual current version (no lock held — this goes
   through `VersionStore`'s own `shared_mutex` independently).
3. Update the shard entry from the placeholder to the real snapshot
   value, under the shard's lock again.

**Why this is safe:** `OldestActiveSnapshot()` takes the minimum over
every entry it observes, scanning shards one at a time. A transaction is
in exactly one of three states relative to any given GC scan:
- **Not yet inserted at all** — GC simply doesn't see it, identical to
  the original "begins after the boundary was read" case. Since the
  store's version counter only increases, this transaction's eventual
  snapshot (read later) is guaranteed ≥ whatever `CurrentVersion()` was
  at GC's scan time, which is ≥ the boundary GC used. Safe.
- **Placeholder (`0`) visible** — the most conservative value possible.
  GC's boundary collapses toward `0` for that pass, so
  `CollectGarbage()` reclaims nothing that cycle rather than something
  it shouldn't — a missed opportunity for one cycle, never a
  correctness risk.
- **Refined (real) value visible** — behaves exactly like the original
  single-lock design: this transaction's true snapshot correctly
  lower-bounds the computed boundary.

In every case, GC's computed boundary is guaranteed ≤ every active (or
about-to-become-active) transaction's eventual snapshot — precisely the
property GC's correctness has depended on since Phase 4. This holds even
though shards are scanned one at a time rather than all locked
simultaneously: each shard's read reflects a valid, safe state of that
shard at the instant it's taken, and a minimum over several
independently-valid safe values remains a valid safe value.

### Design decisions and trade-offs

**Why 16 shards, not a number derived from `hardware_concurrency()`?**
A fixed, reasonable default is enough to demonstrate and gain most of the
benefit of the technique without adding the complexity of runtime
sizing based on measured load or detected core count — that's real
additional engineering that isn't the point of this phase. Revisiting
shard count as a tunable, informed by actual measured contention, is a
natural next refinement if this ever mattered enough to chase further.

**Why two lock acquisitions per `Begin()` (placeholder, then refine)
instead of one?** This is the direct cost of the correctness fix above:
a single insert-with-real-value step reopens exactly the race being
closed. Two per-shard lock acquisitions are still far cheaper than the
single *global* lock acquisition they replace, especially once multiple
threads are involved and would otherwise be serialized against every
other transaction in the system, not just ones sharing a shard.

**Why sharding instead of a full lock-free structure (atomics/CAS-based
concurrent map)?** Sharding is a well-understood, provably-correct
technique (lock striping) that meaningfully reduces contention without
requiring genuinely hard lock-free programming — a different, much
higher-risk category of work. Phase 6 explicitly declined to attempt
that hastily against a project with six phases of proven correctness
riding on it; sharding delivers a real, substantial improvement at a
risk level consistent with how the rest of this project has been built.
A true lock-free active-transaction structure remains a legitimate
further step, now with a correctly-scoped, well-understood problem
statement to build from.

### Testing approach

- `test_transaction.cpp` (`TransactionManagerSharding` section) — three
  tests specifically targeting multi-shard behavior (40 transactions,
  guaranteeing both same-shard collisions and cross-shard spread, since
  40 > 16): `OldestActiveSnapshot()` correctly tracks the true minimum
  as transactions begin and commit in a different order than they
  began, `ActiveTransactionCount()` correctly sums across all shards,
  and many concurrently-open transactions each still see exactly their
  own correct snapshot value (sharding must never scramble which
  transaction sees which version).
- `stress_test_transaction.cpp` —
  `ShardedRegistrationSurvivesConcurrentGCAndHeavyBeginTraffic`: the test
  that would actually catch a flaw in the two-phase registration
  protocol if one existed. A long-lived reader holds an old snapshot
  while 16 threads run 2,000 full transaction cycles each (32,000 total
  `Begin()`/`Commit()` calls, spread across every shard, frequently
  colliding) — concurrently with a **separate thread running
  `GarbageCollector::RunOnce()` in a tight loop**, as aggressively as
  possible. The reader's original value must survive all of it.
- The full existing 94-test suite from Phases 1–6 — unmodified — was
  re-run repeatedly (5 consecutive clean passes, plus 3 more under
  ThreadSanitizer) before any new test was even written, specifically to
  confirm the sharding rewrite introduced no regression to behavior
  every earlier phase had already proven correct.

All 98 tests (94 existing + 4 new) pass cleanly under ThreadSanitizer,
run 3 times consecutively with no data races reported — including the
adversarial GC-vs-sharded-registration test above, which is precisely
the scenario the correctness argument above depends on holding.

### Measured real-hardware impact — four attempts, a pattern emerges once weighted by reliability

Re-run on the same 10-core machine as Phase 6, four separate times,
same three scenarios, same multi-trial/median methodology each time:

```
                     reader ratio                             writer ratio
                baseline  run2  run3  run4           baseline  run2  run3  run4
Read-heavy         0.121  0.145 0.149 0.145             0.107  0.183 0.133 0.118
Balanced           0.108  0.077 0.100 0.093             0.096  0.127 0.090 0.082
Write-heavy        0.099  0.070 0.098 0.087             0.086  0.095 0.076 0.068
```

Run 2 had noticeably higher internal trial-to-trial variance (12–24%)
than the others; runs 3 and 4 were both tight (~3–5%), comparable to the
cleanest Phase 6 baseline measurement. Not all four runs are equally
trustworthy, and treating them as if they were would be a mistake —
**but runs 3 and 4, the two internally-reliable ones, agree with each
other closely** (within 3–11% on every value), while run 2 stands apart
from both. That agreement between the two low-noise runs is itself
informative: it's the first evidence in this whole investigation that a
real, reproducible signal might exist underneath the between-run noise,
rather than the noise simply dominating everything, as the three-run
picture suggested.

Averaging runs 3 and 4 (weighted toward the internally-reliable
measurements) against the Phase 6 baseline:

```
                reader change    writer change
Read-heavy          +22%             +17%      (improved)
Balanced            -11%             -10%      (regressed)
Write-heavy          -6%             -16%      (regressed)
```

**A pattern, not a single verdict: the fix appears to help under low
writer contention and hurt under high writer contention.** Read-heavy
(8 readers, 1 writer, little OCC conflict) is the only scenario where
both readers and writers improved. Balanced and write-heavy (4–8
writers hammering a 64-key space, real conflict/retry pressure via
`RunWithRetry`) both regressed, writers more than readers.

**A plausible mechanism, stated as a hypothesis, not a proven fact:**
the two-phase registration protocol (see above) costs *two* shard-lock
acquisitions per `Begin()` instead of the original design's one. Under
low contention, the reduced collision rate from sharding more than pays
for that extra round-trip. Under heavy contention — many writer threads
retrying rapidly across only 16 shards — the extra lock acquisition may
start costing more than sharding saves, especially since frequent
retries mean many threads are hammering `Begin()`/`Unregister()` in a
tight loop where even a modest per-call overhead compounds quickly.

**How much confidence this deserves, stated plainly:** two mutually
agreeing runs is a real improvement over one run or three disagreeing
runs, but it is still a thin statistical basis — an n of two,
however well the two agree. This reads as a genuine, scenario-dependent
trade-off worth taking seriously, not as a settled result. A rigorous
confirmation would need several more repeats of each scenario,
ideally alternating between the pre- and post-Phase-7 builds
back-to-back to rule out any remaining time-based drift — real further
work, not performed here.

**What stands regardless:** Phase 7's contribution is the
correctness-preserving sharded architecture and the two-phase
registration protocol's proof — verified by 98 passing tests including
an adversarial GC-vs-heavy-Begin-traffic stress test, clean under
ThreadSanitizer across every run. That result does not depend on, and is
not weakened by, whichever way the throughput trade-off ultimately
resolves.

### Known limitations (carried forward, not hidden)

- **Real-hardware throughput effect of this fix is a scenario-dependent
  trade-off, tentatively.** Two internally-reliable runs (of four total)
  suggest an improvement under low writer contention and a regression
  under high writer contention, with a plausible mechanism (extra
  lock-acquisition cost from the two-phase registration protocol
  compounding under heavy retry traffic). This is better-supported than
  "no signal at all" but still rests on an n of two agreeing
  measurements — worth taking seriously, not yet worth calling settled.
- **Shard count is fixed, not adaptive.** See above.
- **This does not make active-snapshot tracking lock-free**, only
  lower-contention. Every `Begin()`/`Unregister()` still takes a real
  mutex, just one of sixteen instead of one of one. A genuinely
  lock-free structure remains a further, harder step.
- **No write-skew detection, stop-the-world GC sweeps, writer-vs-reader
  blocking within `VersionStore`'s own critical sections** — all carried
  forward unchanged from earlier phases; none of these are touched by
  this fix.
