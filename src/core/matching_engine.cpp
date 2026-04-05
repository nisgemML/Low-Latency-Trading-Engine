#include "core/matching_engine.hpp"
#include <pthread.h>
#include <sched.h>
#include <cstring>

namespace engine {

MatchingEngine::MatchingEngine() {
    // unique_ptr array is value-initialized to nullptr by default.
}

MatchingEngine::~MatchingEngine() {
    stop();
}

bool MatchingEngine::register_symbol(SymbolId id) {
    if (id >= kMaxSymbols) return false;
    if (books_[id]) return false;  // already registered

    books_[id] = std::make_unique<OrderBook>(
        id,
        [this](const ExecutionReport& rpt) { on_execution(rpt); }
    );
    return true;
}

void MatchingEngine::start() {
    running_.store(true, std::memory_order_seq_cst);
    engine_thread_ = std::thread([this] { run_loop(); });

    // Pin to CPU 1 (leave CPU 0 for OS and interrupts).
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    pthread_setaffinity_np(engine_thread_.native_handle(), sizeof(cpuset), &cpuset);

    // Elevate scheduling priority (requires CAP_SYS_NICE in production).
    sched_param sp{ .sched_priority = 50 };
    pthread_setschedparam(engine_thread_.native_handle(), SCHED_FIFO, &sp);
}

void MatchingEngine::stop() {
    running_.store(false, std::memory_order_seq_cst);
    if (engine_thread_.joinable())
        engine_thread_.join();
}

void MatchingEngine::run_loop() noexcept {
    MarketDataMsg msg;

    while (running_.load(std::memory_order_relaxed)) {
        // Busy-poll: no condition variable, no mutex.
        // The SPSC queue's acquire-load is the only synchronization.
        if (inbound_.try_pop(msg)) [[likely]] {
            process_message(msg);
            stat_messages_.fetch_add(1, std::memory_order_relaxed);
        } else {
            // Yield occasionally to avoid monopolizing the CPU when idle.
            // In a production system you'd typically stay on a dedicated
            // isolated core and never yield.
            __builtin_ia32_pause();  // PAUSE hint: reduces power, tells CPU we're spinning
        }
    }

    // Drain remaining messages on shutdown.
    while (inbound_.try_pop(msg))
        process_message(msg);
}

void MatchingEngine::process_message(const MarketDataMsg& msg) noexcept {
    if (msg.symbol >= kMaxSymbols) return;
    OrderBook* book = books_[msg.symbol].get();
    if (!book) return;

    switch (msg.msg_type) {
        case MarketDataMsg::Type::NewOrder: {
            Order o{};
            o.id            = msg.order_id ? msg.order_id : next_order_id_++;
            o.price         = msg.price;
            o.qty           = msg.qty;
            o.qty_remaining = msg.qty;
            o.symbol        = msg.symbol;
            o.side          = msg.side;
            o.type          = msg.order_type;
            o.status        = OrderStatus::New;
            book->add_order(o);
            break;
        }
        case MarketDataMsg::Type::CancelOrder:
            book->cancel_order(msg.order_id);
            break;

        case MarketDataMsg::Type::ModifyOrder:
            book->modify_order(msg.order_id, msg.qty);
            break;

        case MarketDataMsg::Type::Heartbeat:
            // No-op: used to verify queue liveness.
            break;
    }
}

void MatchingEngine::on_execution(const ExecutionReport& rpt) noexcept {
    stat_matches_.fetch_add(1, std::memory_order_relaxed);
    // Best-effort enqueue: if the outbound queue is full, drop the report
    // and let the execution layer detect missing fills via sequence gaps.
    static_cast<void>(outbound_.try_push(rpt));
}

} // namespace engine
