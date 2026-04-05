#include "core/execution_layer.hpp"

namespace engine {

void ExecutionLayer::start() {
    running_.store(true, std::memory_order_seq_cst);
    exec_thread_ = std::thread([this] { run_loop(); });
}

void ExecutionLayer::stop() {
    running_.store(false, std::memory_order_seq_cst);
    if (exec_thread_.joinable())
        exec_thread_.join();
}

void ExecutionLayer::run_loop() noexcept {
    while (running_.load(std::memory_order_relaxed)) {
        if (poll(512) == 0)
            __builtin_ia32_pause();
    }
    poll(INT32_MAX);  // drain on shutdown
}

int ExecutionLayer::poll(int max_msgs) noexcept {
    ExecutionReport rpt;
    int count = 0;
    while (count < max_msgs && inbound_.try_pop(rpt)) {
        process_report(rpt);
        ++count;
    }
    return count;
}

void ExecutionLayer::process_report(const ExecutionReport& rpt) noexcept {
    stat_fills_.fetch_add(1, std::memory_order_relaxed);

    Position& pos = positions_[rpt.symbol];
    pos.symbol      = rpt.symbol;
    pos.total_fills += 1;

    const int64_t signed_qty = (rpt.side == Side::Buy)
        ? static_cast<int64_t>(rpt.exec_qty)
        : -static_cast<int64_t>(rpt.exec_qty);

    // Realized P&L on position close.
    if ((pos.net_qty > 0 && signed_qty < 0) ||
        (pos.net_qty < 0 && signed_qty > 0))
    {
        const int64_t close_qty = std::min(std::abs(pos.net_qty), std::abs(signed_qty));
        const int64_t pnl_per_unit = rpt.exec_price - pos.avg_cost;
        pos.realized_pnl += pnl_per_unit * close_qty * (pos.net_qty > 0 ? 1 : -1);
    }

    // Update average cost (weighted).
    const int64_t new_net = pos.net_qty + signed_qty;
    if (new_net != 0) {
        // Simplified: use last exec price as avg cost for new positions.
        if (pos.net_qty == 0)
            pos.avg_cost = rpt.exec_price;
    } else {
        pos.avg_cost = 0;
    }
    pos.net_qty = new_net;

    if (fill_cb_) fill_cb_(rpt, pos);
}

const Position* ExecutionLayer::get_position(SymbolId id) const noexcept {
    auto it = positions_.find(id);
    return (it != positions_.end()) ? &it->second : nullptr;
}

} // namespace engine
