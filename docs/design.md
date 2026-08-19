# Design Notes: Low-Latency Trading Engine

These notes document the reasoning behind every non-obvious decision in the
codebase. They're written for an audience that will ask "why not X?" and
expects a real answer.

---

## 1. Why single-threaded matching?

The first instinct for a high-throughput system is to parallelize. For a limit
order book this is almost always wrong, and it's important to understand why.

A limit order book has a fundamental **sequential constraint**: the result of
matching order N can determine whether order N+1 matches at all (e.g. order N
exhausts a level, changing the best ask). There is no safe way to pipeline
across this boundary without coordination.

The options for parallelism are:

**a) Per-symbol sharding:** Each symbol runs on its own core with its own book.
Orders for different symbols are truly independent and this parallelizes cleanly.
This is what production systems do, and the `MatchingEngine::register_symbol`
API is designed to support it — each `OrderBook` is an independent object with
no shared state. Adding per-symbol threads is a mechanical change.

**b) Speculative execution:** Process orders speculatively in parallel, then
detect and replay conflicts. This adds complexity proportional to conflict rate
and is only profitable if conflicts are rare (< ~5%). For a continuous two-sided
book with crossing orders this assumption does not hold.

**c) Lock-based parallelism:** Adding a mutex to the matching path is strictly
worse than single-threaded: you pay the synchronization cost on every order even
when there's no actual contention. A lock acquisition on an uncontended mutex
takes ~15–30 ns on modern hardware — that's our entire p50 budget.

The baseline single-threaded design is correct, fast, and the right foundation
before adding sharding.

---

## 2. Memory ordering in the SPSC queue

The SPSC queue uses exactly two atomic variables and the weakest memory
orderings that still guarantee correctness. This is worth spelling out precisely
because it's easy to get wrong.

```
Producer                          Consumer
────────────────────────────────  ────────────────────────────────
buffer_[wp] = item;               if (rp == write_pos_.load(acquire))
write_pos_.store(next, release);      return false;    // empty
                                  out = buffer_[rp];
                                  read_pos_.store(next, release);
```

**Why `release` on the producer's write_pos store?**

The `release` establishes a happens-before relationship: any write that
precedes the `release` store (the payload write `buffer_[wp] = item`) is
guaranteed to be visible to any thread that subsequently does an `acquire`
load of the same variable.

Without `release`, the CPU (or compiler) is free to reorder the index update
before the payload write. The consumer would see the updated index, load an
uninitialized slot, and produce garbage.

**Why `acquire` on the consumer's write_pos load?**

The `acquire` is the receiving end of the release/acquire pair. It ensures the
consumer sees all writes that happened before the producer's `release` store.
Without it the consumer might load the old (pre-write) contents of `buffer_[rp]`
even after observing the updated `write_pos`.

**Why `relaxed` for the producer's read of `read_pos`?**

The producer reads `read_pos` only to check for fullness. It doesn't need to
synchronize any payload data — it just needs a recent-enough value to avoid
overwriting live slots. A `relaxed` load is sufficient: if the consumer has
advanced `read_pos` but the producer hasn't seen it yet, the producer will
think the queue is full and spin, then retry. This is safe (just a brief
stall) and avoids an unnecessary barrier on the common path.

**The total barrier count on the hot path:**
- Producer fast path (not full): 1 relaxed load + 1 payload write + 1 release store
- Consumer fast path (not empty): 1 relaxed load + 1 acquire load + 1 payload read + 1 release store

No explicit fence instructions. The compiler generates `MFENCE`/`SFENCE` only
where the release/acquire semantics require it (which on x86-TSO is typically
a `MOV` with implicit store-store ordering for releases, and a `MOV` with
acquire semantics for loads — essentially free on TSO).

---

## 3. Struct-of-Arrays order book layout

### The pointer-based baseline

A textbook LOB uses:
```cpp
std::map<Price, std::list<Order*>> bids;
```

Walking to the best bid is O(1) amortized (the map keeps sorted order). But
matching requires:
1. Tree node lookup → random pointer chase → probable L2/L3 miss
2. List node traversal → random pointer chase → another probable miss
3. Order object access → another random pointer chase → another miss

At 100 ns/cache miss × 3 misses per match = ~300 ns just in cache misses,
before any actual work is done.

### The SoA transformation

```cpp
struct Side {
    Price    prices[kMaxLevels];       // hot: iterated during matching
    Qty      qtys[kMaxLevels];         // cold: touched only on match
    uint32_t order_counts[kMaxLevels]; // cold
    uint32_t head_idxs[kMaxLevels];    // cold
};
```

The matching loop iterates `prices[]` to find the crossing level:
```cpp
while (incoming.qty_remaining > 0 && passive_side.count > 0) {
    Price best_passive = passive_side.prices[0]; // always index 0 (best)
    if (incoming.price < best_passive) break;
    ...
}
```

Since we always work the front of the sorted array, `prices[0]` is almost
always in L1. For deeper books (scanning multiple levels), the sequential
access pattern lets the hardware prefetcher pull in the next cache line before
we need it.

`qtys[]` and `order_counts[]` are only accessed after a cross is confirmed.
By keeping them in separate arrays, the price comparison loop doesn't pollute
the cache with quantity data that's not needed for the decision.

### The level-index stability problem

The design went through one significant bug: the original implementation stored
a `level_idx` (array index) in each order slot to make cancel O(1). But
`remove_level()` uses `memmove` to shift the sorted arrays — invalidating any
stored indices.

The fix stores the order's **price** instead. Cancelling an order requires
a `find_level(price)` call — O(depth) — but depth is typically small (< 20
levels with active resting orders) and the scan is cache-friendly. This is the
right trade-off: O(1) cancel via stale index is only O(1) if the index stays
valid, which it doesn't after any level removal.

An alternative would be to lazily update stored indices after every `memmove`.
But updating all live orders' stored indices after a level removal requires
scanning all slots — worse than the linear scan approach.

---

## 4. The pool allocator

### Why not `malloc`?

`malloc` in glibc uses a thread-local arena + a global fallback. Even the
happy path (local arena, no contention) involves:
- Checking the freelist for the right size class
- Updating the freelist pointers
- Potentially zeroing memory
- A store fence to ensure visibility

This is ~50–100 ns for a small allocation and non-deterministic in the worst
case (fragmented heap, arena lock contention).

### The slab design

```
[mmap'd page] [Order][Order][Order]...[Order]
               ↑                            ↑
               slab_                        slab_ + N*sizeof(Order)
```

The free list is threaded through the slab itself — each free slot's first
bytes hold a pointer to the next free slot. No external metadata.

`mlock` pins the pages. After startup, every allocation and deallocation is:
1. Load `free_head`
2. Follow one pointer
3. Store new `free_head`

Three memory accesses, no system calls, no locks. If `free_head` is in L1
(likely — it's touched constantly), allocation is ~3–5 ns.

### Why not per-thread pools?

The matching engine is deliberately single-threaded, so there's no contention
and no need for per-thread pools. Adding per-thread pools would be the right
move if we sharded matching across cores (one pool per matching thread, no
synchronization needed).

---

## 5. CPU isolation and scheduling

The matching engine thread does two things to minimize latency jitter:

**a) Core affinity (`pthread_setaffinity_np`):**

Pinning to a specific core means the OS scheduler will not migrate the thread
to another core mid-execution. Core migration causes TLB flushes and L1/L2
cache invalidation — potentially hundreds of nanoseconds of disruption.

In production, the target core is also removed from the Linux scheduler's
general pool (`isolcpus=2` kernel parameter) and from interrupt routing
(`/proc/irq/*/smp_affinity`), so no other work ever runs on it.

**b) SCHED_FIFO at priority 50:**

The default scheduler (CFS) will preempt the matching thread when its
timeslice expires (typically 1–4 ms). A preemption stall is catastrophic for
p99.9 latency.

`SCHED_FIFO` is a realtime policy: the thread runs until it voluntarily yields
or blocks. Since the matching loop never blocks (it busy-polls the SPSC queue),
it will not be preempted.

**c) PAUSE instruction:**

When the inbound queue is empty, the loop spins with `__builtin_ia32_pause()`
(compiles to the x86 `PAUSE` instruction). This does two things:
- Signals to the CPU's memory order speculation that we're in a spin-wait,
  reducing speculative load penalties
- Reduces power consumption slightly (prevents the CPU from burning all
  its power budget spinning, which could cause thermal throttling)

---

## 6. The order index hash map

Cancel requires O(1) lookup from `OrderId` to `(slot, side)`. The standard
choice is `std::unordered_map`, but it has several problems on the hot path:

- Heap allocation for bucket arrays and chained nodes
- `std::hash<uint64_t>` is typically modulo-based — poor distribution
- Iterator invalidation on rehash causes unpredictable latency spikes

The implementation uses a hand-rolled open-addressed hash table with:

**Fibonacci hashing:** `id * 2654435761 >> 32`

Fibonacci hashing (multiplication by the golden ratio, scaled to the hash
table size) distributes keys more uniformly than modulo-based hashing because
it exploits the full bit width rather than just the low-order bits. For
sequential `OrderId` values (which are common), modulo hashing would cluster
all entries into the same few buckets.

**Fixed-size table (load factor ~0.5):**

No rehashing, ever. The table is allocated upfront at 2× the maximum order
count. At 0.5 load factor, expected probe length is ~1.5 slots on lookup.

**Robin Hood / backward-shift deletion:**

On deletion, we use backward-shift to fill the hole rather than marking the
slot as a tombstone. Tombstones accumulate over time and degrade lookup
performance. Backward-shift maintains the invariant that every key is as close
as possible to its hash slot.

---

## 7. What the latency numbers actually measure

The benchmark measures wall-clock time from SPSC push to SPSC pop including
all of:
- Producer-side: SPSC write barrier + index store
- Inter-thread communication: cache coherence traffic (MESI state transitions)
- Consumer-side: SPSC acquire load + message decode
- Order book: price comparison, level insertion/lookup, slot allocation, chain append
- Execution report: SPSC push on the outbound queue
- Back to producer: SPSC acquire load on the outbound queue

**What it does NOT include:**
- Network I/O (kernel bypass / DPDK)
- Clock synchronization (PTP/GPS for timestamping)
- NIC timestamping

In a production co-located system, the dominant latency is typically the NIC
receive path (DPDK + busy-poll kernel bypass gets to ~1 µs from wire to
application), and then the matching engine adds the ~400–500 ns we measure here.
The round-trip from a market data update to an outbound order is therefore
~2–3 µs — competitive with co-located HFT systems.

---

## 8. Things deliberately left out

**Persistence / crash recovery:** A production LOB would write-ahead-log every
order event to a ring buffer on persistent memory (pmem) or NVMe. On restart,
replay the log to reconstruct book state. Left out to keep the core matching
logic clear.

**Network transport:** The `MarketDataIngestion` interface accepts a raw
`span<uint8_t>`. Plugging in DPDK, RDMA, or kernel UDP is a one-function
change. Left out because transport is orthogonal to matching correctness and
latency.

**Risk / pre-trade checks:** Fat-finger limits, position limits, credit checks.
These live between ingestion and matching. They are latency-sensitive but not
architecturally interesting — just comparisons against pre-computed limits.

**Order types:** IOC and FOK are partially implemented. GTC, GTD, pegged
orders, iceberg orders, and stop-loss orders each require additional state and
matching logic. The framework supports adding them without architectural change.

**Cross-symbol arbitrage detection:** Detecting spread relationships across
symbols (e.g. cash-futures basis) requires a global view across books. This
is the job of the strategy layer, not the matching engine.

---

## 6. TSC-based timing and the coordinated omission problem

### Why not `clock_gettime(CLOCK_MONOTONIC)`?

`CLOCK_MONOTONIC` has a vDSO implementation on Linux — typically ~20 ns per call.
On a 400 ns p50 target, the timer overhead is 5% of the measured value.  More
importantly, the vDSO path still touches the HPET or TSC counter via a kernel-
mediated read, adding jitter in the 10–50 ns range.

`RDTSCP` reads the CPU's 64-bit cycle counter in ~3 cycles (≈1 ns at 3 GHz).
It serialises the read with respect to preceding instructions (unlike `RDTSC`),
preventing the CPU from speculatively issuing the counter read before earlier
stores have completed.  On invariant TSC hardware (all post-2010 Intel/AMD),
the counter runs at a fixed rate independent of C-states or frequency scaling.

Calibration: we measure a 10 ms `CLOCK_MONOTONIC` interval and divide by the
TSC delta.  This gives ns_per_cycle accurate to ±0.01% — sufficient for our
purposes.

### Coordinated omission

A classic benchmark mistake: you submit as fast as possible and measure
round-trip latency.  When the engine is busy, submissions queue up — slow
samples never accumulate because the sender was stalled waiting to submit.
The result: an optimistic latency distribution that understates tail risk.

Gil Tene's fix (Azul Systems): submit at a fixed inter-arrival rate regardless
of service time.  If a response is late, the sender doesn't "absorb" the delay
by submitting the next request late.  Our benchmark uses a spin-submit that
records the sample even when the sender had to spin for queue space.

### LatencyHistogram vs vector sort

Recording 1M samples in a `vector<uint64_t>` and sorting is fine offline.
In a production system that monitors latency continuously, it requires O(n)
memory and O(n log n) time per reporting interval.

A 64-bucket log₂ histogram gives O(1) update and O(64) percentile query in
512 bytes of memory.  Resolution is ±50% per bucket — adequate for alarm
thresholds and trend detection.  HDR Histogram (Azul) gives ±0.1% resolution
in ~1 KB by using sub-bucket partitioning within each power-of-two range.

---

## 7. Market microstructure: why it matters

A trading engine that doesn't model market impact is a gun without a target.
The microstructure module (`include/core/microstructure.hpp`) implements the
three models every market-maker needs:

### Kyle's Lambda — price impact

Kyle (1985) models the market as a Bayesian inference game between a dealer
and an informed trader.  The equilibrium price impact is linear in signed
order flow: `ΔP = λ · Q`.

In practice, λ is estimated from recent (signed_volume, price_change) pairs via
OLS.  A market-maker uses λ to:
- Set quotes wide enough that adverse selection cost < spread / 2.
- Size positions: a 1000-lot position with λ = 0.005 costs 5 bps to unwind.

### Roll's Implied Spread — spread estimation without quotes

In markets where you can't observe quotes (only trades), Roll (1984) shows
that the effective spread can be inferred from the serial covariance of trade
price changes.  Buyer-initiated trades print at the ask; seller-initiated at
the bid.  This creates negative autocorrelation in price changes proportional
to the spread.

`implied_spread() = 2 * sqrt(-cov(Δp_t, Δp_{t-1}))`

### Order Flow Imbalance — direction prediction

Cont, Kukanov, Stoikov (2014) show that OFI — the signed change in best-quote
quantities — is the dominant predictor of short-term price moves, with R² > 0.6
at 10ms horizons.  More predictive than trade flow alone because it captures
the passive side's commitment.

`OFI_t = ΔBidQty_t · 1(bid unchanged) − ΔAskQty_t · 1(ask unchanged)`

A positive OFI means more buyers queued up at the bid without the price moving
— upward pressure without yet reflecting in price.

