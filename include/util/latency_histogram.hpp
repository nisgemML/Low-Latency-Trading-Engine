#pragma once
// latency_histogram.hpp — lock-free, allocation-free latency histogram.
//
// Design choices relevant to a Jane Street interview:
//
//   Naive approach: collect all samples in a vector, sort, compute percentiles.
//   Problems: O(n log n), unbounded memory, unsuitable for continuous monitoring.
//
//   HDR Histogram approach (Azul Systems): sub-bucket power-of-two partitioning
//   giving ±0.1% precision from 1ns to 3600s in ~1KB.  This is the industry
//   standard for latency tracking in production.
//
//   Our approach here: simplified 64-bucket log₂ histogram sufficient for
//   <1ms latency ranges.  Each bucket covers a power-of-two range; we report
//   p50/p99/p99.9 with resolution matching the bucket granularity.  The
//   histogram struct is 64 * 8 = 512 bytes — fits in one page, never allocates.
//
//   Updating a bucket is a single atomic increment — safe to read from a
//   monitoring thread without stopping the world.

#include <cstdint>
#include <cstdio>
#include <atomic>
#include <bit>

namespace engine {

class LatencyHistogram {
    static constexpr int kBuckets = 64;  // covers 1ns – 2^63 ns (~292 years)

    // Each bucket counts samples in [2^i, 2^(i+1)) ns.
    std::atomic<uint64_t> counts_[kBuckets]{};
    std::atomic<uint64_t> total_{0};
    std::atomic<uint64_t> sum_ns_{0};  // for mean

public:
    // Record a latency sample (nanoseconds).
    void record(uint64_t ns) noexcept {
        // __builtin_clzll(0) is UB; clamp 0 to bucket 0.
        const int bucket = (ns == 0) ? 0 : (63 - __builtin_clzll(ns));
        counts_[bucket < kBuckets ? bucket : kBuckets - 1]
            .fetch_add(1, std::memory_order_relaxed);
        total_.fetch_add(1, std::memory_order_relaxed);
        sum_ns_.fetch_add(ns, std::memory_order_relaxed);
    }

    // Return the percentile value (0.0–1.0) as nanoseconds.
    // Uses the lower bound of the bucket as the reported value.
    [[nodiscard]] uint64_t percentile(double p) const noexcept {
        const uint64_t n = total_.load(std::memory_order_relaxed);
        if (n == 0) return 0;
        const uint64_t target = static_cast<uint64_t>(p * static_cast<double>(n));
        uint64_t cumulative = 0;
        for (int i = 0; i < kBuckets; ++i) {
            cumulative += counts_[i].load(std::memory_order_relaxed);
            if (cumulative > target)
                return (i == 0) ? 0ULL : (1ULL << i);
        }
        return 1ULL << (kBuckets - 1);
    }

    [[nodiscard]] uint64_t count() const noexcept {
        return total_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] double mean_ns() const noexcept {
        const uint64_t n = total_.load(std::memory_order_relaxed);
        if (n == 0) return 0.0;
        return static_cast<double>(sum_ns_.load(std::memory_order_relaxed))
               / static_cast<double>(n);
    }

    void reset() noexcept {
        for (auto& c : counts_) c.store(0, std::memory_order_relaxed);
        total_.store(0, std::memory_order_relaxed);
        sum_ns_.store(0, std::memory_order_relaxed);
    }

    void print_summary(const char* label = "") const noexcept {
        printf("%-24s  count=%6lu  mean=%5.0f ns  "
               "p50=%5lu ns  p99=%6lu ns  p99.9=%7lu ns\n",
               label,
               count(), mean_ns(),
               percentile(0.50), percentile(0.99), percentile(0.999));
    }
};

} // namespace engine
