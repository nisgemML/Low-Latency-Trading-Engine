#include <time.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

// Estimate cycles per ns using a calibration loop
static double cycles_per_ns() {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t c0 = rdtsc();
    for (volatile int i = 0; i < 10000000; ++i) {}
    uint64_t c1 = rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
    return (c1 - c0) / ns;
}

struct AoSLevel {
    int64_t  price;
    uint32_t qty;
    uint32_t count;
    uint32_t head_idx;
    uint8_t  _pad[12];
};

struct AoSBook {
    static constexpr int kMax = 4096;
    AoSLevel levels[kMax];
    int count = 0;

    __attribute__((noinline))
    int find_crossing(int64_t price) const noexcept {
        for (int i = 0; i < count; ++i)
            if (levels[i].price <= price) return i;
        return -1;
    }
};

struct SoABook {
    static constexpr int kMax = 4096;
    int64_t  prices[kMax];
    uint32_t qtys[kMax];
    uint32_t counts[kMax];
    uint32_t head_idxs[kMax];
    int count = 0;

    __attribute__((noinline))
    int find_crossing(int64_t price) const noexcept {
        for (int i = 0; i < count; ++i)
            if (prices[i] <= price) return i;
        return -1;
    }
};

template<typename Book>
static double bench(Book& book, int64_t target, int iters, double cpns) {
    // Warm up L1/L2
    for (int i = 0; i < 50000; ++i) (void)book.find_crossing(target);

    volatile int64_t sink = 0;
    uint64_t t0 = rdtsc();
    for (int i = 0; i < iters; ++i)
        sink += book.find_crossing(target + (i & 1));
    uint64_t elapsed = rdtsc() - t0;
    (void)sink;
    return (elapsed / (double)iters) / cpns;
}

static void run(int depth, double cpns) {
    AoSBook aos; SoABook soa;
    for (int i = 0; i < depth; ++i) {
        int64_t p = 100'000'000LL - i * 1000;
        aos.levels[i] = { p, 1000, 3, 0, {} };
        soa.prices[i] = p; soa.qtys[i] = 1000;
        soa.counts[i] = 3; soa.head_idxs[i] = 0;
    }
    aos.count = soa.count = depth;
    int64_t target = 100'000'000LL - depth * 1000 - 500;

    int iters = (depth <= 64) ? 5'000'000 : 1'000'000;
    double ans = bench(aos, target, iters, cpns);
    double sns = bench(soa, target, iters, cpns);

    size_t aos_cl = (depth * sizeof(AoSLevel) + 63) / 64;
    size_t soa_cl = (depth * sizeof(int64_t)  + 63) / 64;

    printf("  depth=%4d  AoS=%7.1f ns  SoA=%6.1f ns  speedup=%.2fx  "
           "(AoS=%zu CL, SoA=%zu CL)\n",
           depth, ans, sns, ans/sns, aos_cl, soa_cl);
}

int main() {
    double cpns = cycles_per_ns();
    printf("=== SoA vs AoS Cache Layout Benchmark (%.2f GHz)\n\n", cpns);
    printf("  Scanning for crossing price — worst case (target below all levels)\n\n");
    printf("  %-6s  %-14s  %-13s  %-10s\n", "Depth", "AoS (ns/scan)", "SoA (ns/scan)", "Speedup");
    printf("  %s\n", std::string(62, '-').c_str());

    for (int d : {1, 4, 8, 16, 32, 64, 128, 256, 512, 1024})
        run(d, cpns);

    printf("\n  AoS level size: %zu bytes  |  SoA price element: %zu bytes\n",
           sizeof(AoSLevel), sizeof(int64_t));
    printf("  At depth=64: AoS touches %zu cache lines vs SoA's %zu\n",
           (64*sizeof(AoSLevel)+63)/64, (64*sizeof(int64_t)+63)/64);
    return 0;
}
