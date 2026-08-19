// test_allocator.cpp — pool allocator correctness tests.

#include "util/allocator.hpp"
#include "core/types.hpp"
#include <cstdio>
#include <cassert>
#include <vector>

using namespace engine;

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
            ++failed; \
        } else { \
            ++passed; \
        } \
    } while(0)

static void test_basic_alloc_free() {
    PoolAllocator<Order, 128> pool;

    CHECK(pool.available() == 128, "Full pool initially");

    Order* o = pool.allocate();
    CHECK(o != nullptr, "Allocation succeeds");
    CHECK(pool.available() == 127, "Available decremented");

    pool.deallocate(o);
    CHECK(pool.available() == 128, "Available restored after free");
}

static void test_exhaust_and_recover() {
    static constexpr std::size_t N = 64;
    PoolAllocator<Order, N> pool;

    std::vector<Order*> ptrs;
    ptrs.reserve(N);

    // Exhaust pool.
    for (std::size_t i = 0; i < N; ++i) {
        Order* p = pool.allocate();
        CHECK(p != nullptr, "Allocation within capacity");
        ptrs.push_back(p);
    }

    // One more should fail.
    CHECK(pool.allocate() == nullptr, "Allocation fails when pool exhausted");
    CHECK(pool.available() == 0,      "Zero available when exhausted");

    // Free all — pool should recover.
    for (Order* p : ptrs) pool.deallocate(p);
    CHECK(pool.available() == N, "All slots recovered");

    // Can allocate again.
    Order* p2 = pool.allocate();
    CHECK(p2 != nullptr, "Re-allocation after recovery");
    pool.deallocate(p2);
}

static void test_no_double_free_corruption() {
    // Allocate two objects, free them both, then reallocate —
    // verify the free list is internally consistent.
    PoolAllocator<Order, 32> pool;

    Order* a = pool.allocate();
    Order* b = pool.allocate();
    CHECK(a != b,    "Two distinct allocations");
    CHECK(a != nullptr && b != nullptr, "Both non-null");

    pool.deallocate(b);
    pool.deallocate(a);

    // After freeing both, pool should have 32 available.
    CHECK(pool.available() == 32, "Full availability after freeing a and b");

    // Allocate twice more — should succeed.
    Order* c = pool.allocate();
    Order* d = pool.allocate();
    CHECK(c != nullptr, "Third alloc ok");
    CHECK(d != nullptr, "Fourth alloc ok");
    CHECK(c != d,       "Third and fourth are distinct");

    pool.deallocate(c);
    pool.deallocate(d);
}

static void test_construct_helper() {
    PoolAllocator<Order, 8> pool;

    Order* o = pool.construct();  // default construct
    CHECK(o != nullptr, "construct() returns non-null");

    o->id  = 42;
    o->qty = 100;
    CHECK(o->id  == 42,  "Field write ok");
    CHECK(o->qty == 100, "Field write ok");

    pool.deallocate(o);
    CHECK(pool.available() == 8, "Available restored");
}

static void test_cycling_stress() {
    static constexpr int kSlots  = 64;
    static constexpr int kRounds = 1'000'000;
    PoolAllocator<Order, kSlots> pool;

    // Alternate between allocating and freeing to stress the free list.
    Order* buf[kSlots] = {};
    int live = 0;
    bool ok  = true;

    for (int i = 0; i < kRounds; ++i) {
        if (live < kSlots && (live == 0 || i % 3 != 0)) {
            buf[live] = pool.allocate();
            if (!buf[live]) { ok = false; break; }
            ++live;
        } else if (live > 0) {
            pool.deallocate(buf[--live]);
            buf[live] = nullptr;
        }
    }

    CHECK(ok, "No allocation failure during cycling stress");

    // Clean up remaining live objects.
    for (int i = 0; i < live; ++i) if (buf[i]) pool.deallocate(buf[i]);
    CHECK(pool.available() == kSlots, "Full recovery after stress");
}

int main() {
    printf("=== Pool Allocator Tests ===\n\n");

    test_basic_alloc_free();
    test_exhaust_and_recover();
    test_no_double_free_corruption();
    test_construct_helper();

    printf("Running cycling stress test (1M rounds)...\n");
    test_cycling_stress();

    printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
