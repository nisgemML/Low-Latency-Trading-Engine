#pragma once

// ExecutionLayer — consumes execution reports from the matching engine
// and manages position tracking, P&L accounting, and downstream dispatch.
//
// Latency isolation:
//   Non-deterministic operations (logging, external dispatch, risk checks)
//   are deliberately isolated here — outside the matching engine's hot path.
//   The matching engine sees only the SPSC outbound queue push; anything
//   that might block or jitter lives in this layer.

#include "core/types.hpp"
#include "core/spsc_queue.hpp"
#include <unordered_map>
#include <atomic>
#include <functional>
#include <thread>

namespace engine {

struct Position {
    SymbolId symbol;
    int64_t  net_qty;        // signed: positive = long, negative = short
    int64_t  realized_pnl;  // fixed-point (price units * qty)
    Price    avg_cost;
    uint64_t total_fills;
};

class ExecutionLayer {
public:
    static constexpr std::size_t kQueueDepth = 1 << 16;
    using InboundQueue = SPSCQueue<ExecutionReport, kQueueDepth>;

    using FillCallback = std::function<void(const ExecutionReport&, const Position&)>;

    explicit ExecutionLayer(InboundQueue& inbound)
        : inbound_(inbound) {}

    // Register a callback invoked after each fill is processed.
    void on_fill(FillCallback cb) { fill_cb_ = std::move(cb); }

    // Start/stop background processing thread.
    void start();
    void stop();

    // Process one batch (up to `max_msgs`).  Call from a polling loop.
    int poll(int max_msgs = 256) noexcept;

    [[nodiscard]] const Position* get_position(SymbolId id) const noexcept;
    [[nodiscard]] uint64_t fills_processed() const noexcept {
        return stat_fills_.load(std::memory_order_relaxed);
    }

private:
    void run_loop() noexcept;
    void process_report(const ExecutionReport& rpt) noexcept;

    InboundQueue& inbound_;
    FillCallback  fill_cb_;

    std::unordered_map<SymbolId, Position> positions_;

    std::thread       exec_thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> stat_fills_{0};
};

} // namespace engine
