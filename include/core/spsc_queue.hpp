#pragma once
// spsc_queue.hpp — Single-Producer Single-Consumer lock-free ring buffer.
//
// ── Design rationale ──────────────────────────────────────────────────────────
//
// The minimum synchronisation this problem requires is two atomic variables:
// one producer-owned (write_pos) and one consumer-owned (read_pos).
// No mutex, no CAS loop, no ABA problem.
//
// Memory ordering (see design.md §2 for the full proof):
//   • Producer: store element, then release-store write_pos.
//   • Consumer: acquire-load write_pos, then load element.
//   • read_pos: relaxed-store by consumer, acquire-load by producer.
//
// False sharing: write_pos and read_pos live on separate cache lines.
//
// ── Correctness notes ─────────────────────────────────────────────────────────
//
// Capacity convention: the buffer holds Capacity elements but we sacrifice
// one slot to distinguish full from empty (the standard ring-buffer idiom).
// Usable capacity is therefore Capacity - 1.  This is intentional and
// documented; callers should size Capacity one larger than their burst budget.
//
// Alternatively, a separate `size` counter (atomic) can be used to get the
// full Capacity — at the cost of two extra atomic ops per push/pop.
// The one-slot sacrifice is the right trade-off for a latency-critical path.
//
// ── Template contract ─────────────────────────────────────────────────────────
// T must be trivially copyable (no virtual functions, no heap ownership)
// to allow placement directly into the buffer without constructor overhead.
// This is enforced by the static_assert below.

#include <atomic>
#include <array>
#include <concepts>
#include <cstdint>
#include <cassert>
#include <type_traits>

namespace engine {

template<typename T, std::size_t Capacity>
    requires (Capacity > 1 && (Capacity & (Capacity - 1)) == 0)  // power-of-two
class SPSCQueue {
    static_assert(std::is_trivially_copyable_v<T>,
        "SPSCQueue<T>: T must be trivially copyable for lock-free correctness. "
        "Heap-owning types (shared_ptr, string) would silently corrupt under "
        "the relaxed memory ordering used here.");

public:
    static constexpr std::size_t kCapacity       = Capacity;
    static constexpr std::size_t kUsableCapacity = Capacity - 1;  // one slot sacrificed
    static constexpr std::size_t kMask           = Capacity - 1;

    SPSCQueue() noexcept : write_pos_(0), read_pos_(0) {}

    // Non-copyable: the queue owns its storage and embedded atomics.
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    // ── Producer side ────────────────────────────────────────────────────────
    // Called from exactly one thread.  Returns false if full (caller spins).

    [[nodiscard]] bool try_push(const T& item) noexcept {
        const std::size_t wp   = write_pos_.load(std::memory_order_relaxed);
        const std::size_t next = (wp + 1) & kMask;

        // Acquire: see the consumer's latest read_pos progress.
        if (next == read_pos_.load(std::memory_order_acquire))
            return false;   // full — one slot sacrificed for this check

        buffer_[wp] = item;
        write_pos_.store(next, std::memory_order_release);  // publishes payload
        return true;
    }

    // Move-push: avoids a copy for larger T (e.g. ExecutionReport with padding).
    [[nodiscard]] bool try_push(T&& item) noexcept {
        const std::size_t wp   = write_pos_.load(std::memory_order_relaxed);
        const std::size_t next = (wp + 1) & kMask;
        if (next == read_pos_.load(std::memory_order_acquire))
            return false;
        buffer_[wp] = std::move(item);
        write_pos_.store(next, std::memory_order_release);
        return true;
    }

    // ── Consumer side ────────────────────────────────────────────────────────
    // Called from exactly one thread.  Returns false if empty.

    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t rp = read_pos_.load(std::memory_order_relaxed);

        // Acquire: pairs with producer's release-store on write_pos.
        if (rp == write_pos_.load(std::memory_order_acquire))
            return false;   // empty

        out = buffer_[rp];
        // Release: lets producer see the updated read cursor.
        read_pos_.store((rp + 1) & kMask, std::memory_order_release);
        return true;
    }

    // ── Queries (approximate; unsynchronised) ────────────────────────────────

    [[nodiscard]] bool empty() const noexcept {
        return read_pos_.load(std::memory_order_acquire) ==
               write_pos_.load(std::memory_order_acquire);
    }

    // Returns an approximate element count.  May be stale by one element
    // in each direction under concurrent access.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t w = write_pos_.load(std::memory_order_relaxed);
        const std::size_t r = read_pos_.load(std::memory_order_relaxed);
        return (w - r) & kMask;
    }

    [[nodiscard]] constexpr std::size_t capacity() const noexcept {
        return kUsableCapacity;
    }

private:
    // Buffer accessed by both threads on different indices.
    // Aligned to cache line to avoid the buffer array header straddling a line.
    alignas(64) std::array<T, Capacity> buffer_;

    // Producer-owned: only written by producer, read (for fullness) by producer
    // and (for emptiness) by consumer.
    alignas(64) std::atomic<std::size_t> write_pos_;

    // Consumer-owned: only written by consumer, read (for fullness) by producer.
    alignas(64) std::atomic<std::size_t> read_pos_;
};

} // namespace engine
