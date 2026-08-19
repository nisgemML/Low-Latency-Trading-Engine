# Low-Latency Trading Engine

A from-scratch limit order book engine in C++20, targeting **sub-microsecond per-event latency** and **6M+ messages/sec** sustained throughput on Linux x86-64.

```
~400–500 ns p50  ·  ~1.5–2 µs p99  ·  6M+ msgs/sec sustained
Measured: RDTSCP-timed, isolated core, SCHED_FIFO, mlock'd slab
```

---

## Architecture

```
Feed (UDP/sim)
      │
      ▼
┌──────────────────────┐
│  MarketDataIngestion  │  ← decode wire format, sequence-number gap detect, normalize
└──────────┬───────────┘
           │  SPSCQueue<MarketDataMsg, 65536>
           │  (two atomics, release/acquire pair, false-sharing eliminated)
           ▼
┌──────────────────────┐
│    MatchingEngine     │  ← pinned thread (CPU 1), SCHED_FIFO 50, busy-poll + PAUSE
│  ┌────────────────┐  │
│  │  OrderBook[N]  │  │  ← SoA price levels, pool-allocated order slots, O(1) cancel
│  └────────────────┘  │
└──────────┬───────────┘
           │  SPSCQueue<ExecutionReport, 65536>
           ▼
┌──────────────────────┐
│    ExecutionLayer     │  ← position tracking, P&L, downstream dispatch
└──────────────────────┘
```

Every component is isolated by a typed SPSC queue.  The matching engine's hot path contains **zero mutexes, zero heap allocations, and zero system calls** after startup.

---

## Key Design Decisions

### 1. Lock-free IPC — SPSC ring buffer

The minimum synchronisation required is *two* atomic variables — one producer-owned (`write_pos`), one consumer-owned (`read_pos`).  No mutex, no CAS loop, no ABA problem.

```cpp
// Producer: store payload, then release-store the index.
buffer_[wp] = item;
write_pos_.store(next, std::memory_order_release);   // publishes payload

// Consumer: acquire-load the index, then read payload.
if (rp == write_pos_.load(std::memory_order_acquire)) return false;  // empty
out = buffer_[rp];
read_pos_.store((rp + 1) & kMask, std::memory_order_release);
```

The `release`/`acquire` pair establishes happens-before with no explicit fence.  On x86-TSO, `release` stores are plain `MOV` — essentially free.  Producer and consumer heads live on **separate cache lines** to eliminate false sharing.

**Why not `std::mutex`?**  An uncontended mutex acquisition takes ~15–30 ns on modern hardware — our entire p50 budget.  The SPSC queue's fast path is a single acquire-load + a payload copy.

### 2. Cache-aware order book — struct-of-arrays

The textbook LOB uses `std::map<Price, std::list<Order*>>`: every crossing check is a tree traversal followed by pointer-chasing to scattered heap nodes.  L1 miss rate is the primary bottleneck.

This engine stores price levels in struct-of-arrays:

```
prices[]       [99.95] [99.90] [99.85] ...   ← hot: streamed sequentially in try_match()
qtys[]         [ 1000] [  500] [  200] ...   ← only read on actual match
order_counts[] [    3] [    2] [    1] ...   ← only read on level exhaustion
```

The hardware prefetcher saturates `prices[]` ahead of the comparison loop. At 1000 levels, `prices[]` occupies 62 cache lines vs 1000 cache lines for the AoS equivalent.  See `bench/bench_cache_miss.cpp` for measured numbers.

### 3. Pool allocator — no heap on the hot path

`malloc`/`free` are non-deterministic and touch shared heap state (implicit lock).  Every order slot comes from a pre-allocated `mmap`'d slab, pinned with `mlock` to prevent runtime page faults:

```
alloc() → O(1), deterministic, zero system calls
free()  → O(1), deterministic, zero system calls
```

The slab is advised `MADV_HUGEPAGE` to reduce TLB pressure: 65,536 Order slots (each 48 bytes) spans 3 MB — exactly the coverage of one 2 MB huge page with a 1 MB remainder.

### 4. CPU isolation and scheduling

The matching engine thread is pinned via `pthread_setaffinity_np(CPU 1)` and elevated to `SCHED_FIFO` priority 50.  It busy-polls its inbound queue with `__builtin_ia32_pause()` (the x86 `PAUSE` hint) in the idle case — reducing memory-order traffic and power without blocking.

For production: pair with `/sys/devices/system/cpu/cpu1/cpufreq/scaling_governor = performance` and `isolcpus=1` in the kernel command line.  See `docs/linux-tuning.md`.

### 5. O(1) cancel — hash map order index

Cancel-by-order-ID requires locating the price level and slot in O(1).  We maintain an `OrderIndex` hash map (open-addressing, Robin Hood displacement, backward-shift deletion).

**Why Robin Hood over linear probe + tombstones?**  Under high cancel rates the tombstone approach fills the table with dead slots, degrading lookup toward O(n) and causing the original delete-then-re-probe fixup to fail on certain displacement patterns.  Robin Hood bounds probe variance (≈ln n vs ≈n/2 worst case) and backward-shift deletion requires no tombstones.

### 6. TSC-based latency measurement

`CLOCK_MONOTONIC` has ~20–50 ns syscall overhead per call — enough to dominate p50 measurements.  The `TscClock` in `include/util/tsc.hpp` calibrates TSC→ns once at startup and reads `RDTSCP` (~3 cycles) on the hot measurement path.

### 7. Market microstructure analytics

`include/core/microstructure.hpp` implements three foundational models:

- **Kyle's Lambda** (1985): price impact coefficient, estimated via rolling OLS on (signed volume, price change) pairs.
- **Roll's Implied Spread** (1984): effective spread inferred from serial covariance of trade price changes.
- **Order Flow Imbalance**: contemporaneous short-term direction signal from best-quote quantity changes.

These connect the engine's mechanical correctness to the reason it exists: making or taking markets profitably.

---

## Project Layout

```
trading-engine/
├── include/
│   ├── core/
│   │   ├── types.hpp              # Price (fixed-point), Qty, Order, ExecutionReport
│   │   ├── spsc_queue.hpp         # Lock-free SPSC ring buffer
│   │   ├── order_book.hpp         # SoA limit order book
│   │   ├── order_book_view.hpp    # Functional, immutable book snapshot + VWAP/spread
│   │   ├── matching_engine.hpp    # Orchestrator + thread/affinity management
│   │   ├── market_data.hpp        # Wire format decoder + sequence-gap detection
│   │   ├── execution_layer.hpp    # Position tracking + P&L
│   │   └── microstructure.hpp     # Kyle lambda, Roll spread, Order Flow Imbalance
│   └── util/
│       ├── allocator.hpp          # mmap pool allocator (mlock + MADV_HUGEPAGE)
│       ├── logger.hpp             # Lock-free async logger
│       ├── tsc.hpp                # RDTSCP-based nanosecond timer with calibration
│       └── latency_histogram.hpp  # O(1)-space lock-free latency percentile tracker
├── src/
│   ├── core/                      # Implementations
│   └── util/
├── tests/
│   ├── test_order_book.cpp        # Unit + property-based invariant tests (100K events)
│   ├── test_spsc.cpp              # Concurrent SPSC correctness (TSan-clean)
│   ├── test_matching.cpp          # End-to-end pipeline integration tests
│   ├── test_allocator.cpp         # Pool allocator stress (1M alloc/free cycles)
│   └── test_microstructure.cpp    # Mathematical validation of microstructure estimators
├── bench/
│   ├── bench_latency.cpp          # p50/p99/p99.9 via RDTSCP + LatencyHistogram
│   ├── bench_throughput.cpp       # Sustained msgs/sec measurement
│   └── bench_cache_miss.cpp       # SoA vs AoS measured cache performance
├── docs/
│   ├── design.md                  # Design rationale (memory ordering proofs, SoA analysis)
│   └── linux-tuning.md            # isolcpus, NUMA, IRQ affinity, huge pages
├── scripts/
│   ├── build.sh                   # Build + test + benchmark driver
│   └── profile.sh                 # perf + flamegraph workflow
└── CMakeLists.txt
```

---

## Building

**Requirements:** GCC ≥ 12 or Clang ≥ 16, CMake ≥ 3.22, Ninja, Linux x86-64.

```bash
# Release build + all tests
./scripts/build.sh

# Release + benchmarks
./scripts/build.sh --bench

# ThreadSanitizer build (validates concurrent SPSC correctness)
./scripts/build.sh --tsan

# AddressSanitizer build
./scripts/build.sh --asan

# Manual CMake workflow
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# Cache layout benchmark
./build/bench_cache_miss

# Latency benchmark (run on an isolated core for clean results)
taskset -c 0 ./build/bench_latency
```

---

## Benchmark Results

Measured on an isolated core (Linux, `isolcpus=1`, `SCHED_FIFO 50`, `mlock`'d slab, `scaling_governor=performance`):

| Metric              | Result      | Timer    |
|---------------------|-------------|----------|
| p50 latency         | ~400–500 ns | RDTSCP   |
| p99 latency         | ~1.5–2 µs   | RDTSCP   |
| p99.9 latency       | ~3–5 µs     | RDTSCP   |
| Throughput          | 6M+ msg/sec | Wall clock |

Latency covers: SPSC submit → message decode → match attempt → SPSC push of ExecutionReport.

**SoA vs AoS (bench_cache_miss, 1000 levels, no early exit):**

| Layout | ns/iter | Cache lines read |
|--------|---------|-----------------|
| AoS    | ~28,000 | 1000            |
| SoA    | ~1,200  | 62              |

---

## Testing

- **Property-based invariant test** (`test_order_book`): 100K random orders and cancels; asserts `best_bid ≤ best_ask` holds at every step and quantity conservation holds across all fills.
- **Concurrent SPSC test** (`test_spsc`): 2M items across producer/consumer threads; FIFO ordering verified.  Passes ThreadSanitizer with zero reported races.
- **Integration tests** (`test_matching`): full pipeline cross, partial fill, cancel-before-match, multi-symbol isolation, market order fill, IOC/FOK semantics.
- **Allocator stress** (`test_allocator`): 1M alloc/free cycles; free-list consistency validated throughout.
- **Microstructure tests** (`test_microstructure`): synthetic datasets with known analytical solutions; validates Kyle λ recovery, Roll spread estimation, OFI directional signal.

---

## What's Not Here (Intentionally)

- **Network I/O**: `MarketDataIngestion` accepts a raw `std::span<uint8_t>` and is transport-agnostic.  DPDK / kernel-bypass UDP would bolt on here.
- **Persistence**: A production system would WAL every order event for crash recovery.
- **Pre-trade risk**: Fat-finger limits and position limits live between ingestion and matching.
- **Per-symbol sharding**: The `MatchingEngine::register_symbol` API is designed for it — each `OrderBook` is an independent object.  Adding per-symbol threads is mechanical; the sequential baseline is the correct starting point.
- **FIX/ITCH wire format**: `MarketDataIngestion` implements a simplified binary protocol.  Plugging in NASDAQ ITCH 5.0 is a parser swap.
