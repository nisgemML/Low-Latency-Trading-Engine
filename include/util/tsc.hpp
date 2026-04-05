#pragma once
// tsc.hpp — TSC-based nanosecond timer with calibration.
//
// Jane Street context: wall-clock timers (CLOCK_MONOTONIC) have ~20–50 ns
// syscall overhead per invocation — enough to dominate p50 measurements.
// The TSC (Time Stamp Counter) is a hardware register read with RDTSC in
// ~3–5 cycles.  We calibrate once at startup to convert cycles → ns.
//
// Caveats handled:
//   • Modern Linux (5.15+) with invariant TSC: safe across C-states.
//   • We use RDTSCP (not RDTSC) to prevent the CPU from speculatively
//     retiring the read before preceding loads/stores complete.
//   • For the *difference* between two RDTSCP readings on the same core
//     (our use case) there is no need for CPUID serialisation.

#include <cstdint>
#include <ctime>
#include <x86intrin.h>

namespace engine {

struct TscClock {
    double ns_per_cycle;  // calibrated at startup

    // Calibrate by comparing a 10ms CLOCK_MONOTONIC interval to TSC delta.
    void calibrate() noexcept {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        const uint64_t c0 = __rdtsc();

        // Busy-wait 10ms.
        struct timespec target = t0;
        target.tv_nsec += 10'000'000;
        if (target.tv_nsec >= 1'000'000'000) { ++target.tv_sec; target.tv_nsec -= 1'000'000'000; }
        do { clock_gettime(CLOCK_MONOTONIC, &t1); }
        while (t1.tv_sec < target.tv_sec ||
               (t1.tv_sec == target.tv_sec && t1.tv_nsec < target.tv_nsec));

        const uint64_t c1 = __rdtsc();
        const uint64_t elapsed_ns =
            static_cast<uint64_t>(t1.tv_sec  - t0.tv_sec)  * 1'000'000'000ULL +
            static_cast<uint64_t>(t1.tv_nsec - t0.tv_nsec);
        ns_per_cycle = static_cast<double>(elapsed_ns) / static_cast<double>(c1 - c0);
    }

    [[nodiscard]] static uint64_t now_cycles() noexcept {
        unsigned aux;
        return __rdtscp(&aux);  // serialising read
    }

    [[nodiscard]] uint64_t cycles_to_ns(uint64_t cycles) const noexcept {
        return static_cast<uint64_t>(static_cast<double>(cycles) * ns_per_cycle);
    }
};

// Singleton — calibrated once in main().
inline TscClock& tsc_clock() noexcept {
    static TscClock clk;
    return clk;
}

} // namespace engine
