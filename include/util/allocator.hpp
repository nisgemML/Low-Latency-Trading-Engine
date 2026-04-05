#pragma once

// PoolAllocator — fixed-size slab allocator for Order objects.
//
// Motivation:
//   malloc/free are non-deterministic and touch shared state (heap lock).
//   Every Order allocation on the hot path through malloc adds jitter.
//   A pre-allocated slab with a free-list gives O(1) deterministic
//   allocation with zero system calls after startup.
//
// Design:
//   • Single contiguous mmap'd slab, pinned with mlock to prevent
//     page faults at runtime.
//   • Free list threaded through the slab itself — no external metadata.
//   • NOT thread-safe by design: the matching engine is single-threaded.
//     If MT allocation is needed, use per-thread pools.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <sys/mman.h>
#include <new>
#include <utility>
#include <cstdlib>

namespace engine {

template<typename T, std::size_t MaxObjects>
class PoolAllocator {
    static_assert(sizeof(T) >= sizeof(void*), "Object too small for free-list node");

public:
    PoolAllocator() {
        // mmap anonymous huge page — avoids page-table fragmentation.
        const std::size_t bytes = MaxObjects * sizeof(T);
        slab_ = static_cast<uint8_t*>(
            ::mmap(nullptr, bytes,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                   -1, 0)
        );
        if (slab_ == MAP_FAILED) {
            // In a real system: log and gracefully shut down.
            // -fno-exceptions: cannot throw, terminate instead.
            __builtin_trap();
        }

        // Advise huge pages for TLB efficiency.
        ::madvise(slab_, bytes, MADV_HUGEPAGE);

        // Pin pages — no page faults during trading.
        ::mlock(slab_, bytes);

        // Build free list: thread a pointer through each slot.
        free_head_ = nullptr;
        for (std::size_t i = MaxObjects; i-- > 0;) {
            auto* node = reinterpret_cast<FreeNode*>(slab_ + i * sizeof(T));
            node->next = free_head_;
            free_head_ = node;
        }

        total_      = MaxObjects;
        allocated_  = 0;
    }

    ~PoolAllocator() {
        ::munmap(slab_, MaxObjects * sizeof(T));
    }

    // Non-copyable, non-movable.
    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    [[nodiscard]] T* allocate() noexcept {
        if (!free_head_) [[unlikely]] return nullptr;

        FreeNode* node = free_head_;
        free_head_ = node->next;
        ++allocated_;

        return reinterpret_cast<T*>(node);
    }

    void deallocate(T* ptr) noexcept {
        assert(ptr != nullptr);
        ptr->~T();  // explicit destructor call

        auto* node = reinterpret_cast<FreeNode*>(ptr);
        node->next = free_head_;
        free_head_ = node;
        --allocated_;
    }

    // Construct in-place.
    template<typename... Args>
    [[nodiscard]] T* construct(Args&&... args) noexcept {
        T* mem = allocate();
        if (!mem) [[unlikely]] return nullptr;
        return new (mem) T(std::forward<Args>(args)...);
    }

    [[nodiscard]] std::size_t available()  const noexcept { return total_ - allocated_; }
    [[nodiscard]] std::size_t allocated()  const noexcept { return allocated_; }
    [[nodiscard]] std::size_t capacity()   const noexcept { return total_; }

private:
    struct FreeNode { FreeNode* next; };

    uint8_t*   slab_       = nullptr;
    FreeNode*  free_head_  = nullptr;
    std::size_t total_     = 0;
    std::size_t allocated_ = 0;
};

} // namespace engine
