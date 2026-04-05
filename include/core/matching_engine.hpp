#pragma once

// MatchingEngine — orchestrates order books and routes execution reports.
//
// Threading model:
//   The matching engine runs on a single dedicated CPU core, pinned via
//   pthread_setaffinity_np.  All order processing is single-threaded.
//   Orders arrive via SPSC queues from the market data ingestion thread(s)
//   and execution reports leave via SPSC queues to the execution layer.
//
//   This is a deliberate design choice: the cost of a mutex acquisition
//   (CAS + memory barrier + potential OS reschedule) on the hot path dwarfs
//   the benefit of parallel matching.  LOB matching is inherently sequential
//   per symbol anyway; parallelism at the engine level adds synchronization
//   cost without proportional throughput gain.

#include "core/types.hpp"
#include "core/spsc_queue.hpp"
#include "core/order_book.hpp"
#include <array>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <memory>

namespace engine {

class MatchingEngine {
public:
    static constexpr std::size_t kQueueDepth  = 1 << 16;  // 65536 slots
    static constexpr std::size_t kMaxSymbols  = 256;
    static constexpr int         kSpinUsec    = 1;

    using InboundQueue  = SPSCQueue<MarketDataMsg, kQueueDepth>;
    using OutboundQueue = SPSCQueue<ExecutionReport, kQueueDepth>;

    MatchingEngine();
    ~MatchingEngine();

    // Not copyable or movable — owns threads and shared queues.
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    // Register a symbol before starting.
    bool register_symbol(SymbolId id);

    // Start/stop the engine thread.
    void start();
    void stop();

    // Enqueue an inbound message (called from producer thread).
    [[nodiscard]] bool submit(const MarketDataMsg& msg) noexcept {
        return inbound_.try_push(msg);
    }

    // Drain one execution report (called from consumer thread).
    [[nodiscard]] bool poll_report(ExecutionReport& out) noexcept {
        return outbound_.try_pop(out);
    }

    // Statistics (approximate, no lock).
    [[nodiscard]] uint64_t messages_processed() const noexcept {
        return stat_messages_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t matches_generated() const noexcept {
        return stat_matches_.load(std::memory_order_relaxed);
    }

private:
    void run_loop() noexcept;
    void process_message(const MarketDataMsg& msg) noexcept;
    void on_execution(const ExecutionReport& rpt) noexcept;

    InboundQueue  inbound_;
    OutboundQueue outbound_;

    std::array<std::unique_ptr<OrderBook>, kMaxSymbols> books_;

    std::thread engine_thread_;
    std::atomic<bool> running_{false};

    std::atomic<uint64_t> stat_messages_{0};
    std::atomic<uint64_t> stat_matches_{0};

    // Next synthetic order id for converted market orders.
    uint64_t next_order_id_{1};
};

} // namespace engine
