// bench_throughput.cpp — sustained message throughput benchmark.
//
// Floods the SPSC inbound queue from a producer thread and measures
// how many messages the matching engine processes per second.
// Validates the 6M+ msgs/sec sustained throughput target.

#include "core/matching_engine.hpp"
#include <cstdio>
#include <thread>
#include <chrono>
#include <atomic>

using namespace engine;
using namespace std::chrono;

int main() {
    printf("=== Low-Latency Trading Engine — Throughput Benchmark ===\n\n");

    MatchingEngine eng;
    eng.register_symbol(0);
    eng.register_symbol(1);
    eng.start();

    static constexpr int kDurationSec = 5;
    std::atomic<bool>    stop{false};
    std::atomic<uint64_t> produced{0};

    // Producer thread: hammer the inbound queue.
    std::thread producer([&] {
        MarketDataMsg msg{};
        msg.symbol     = 0;
        msg.order_type = OrderType::Limit;
        uint64_t id    = 1;

        while (!stop.load(std::memory_order_relaxed)) {
            msg.msg_type = MarketDataMsg::Type::NewOrder;
            msg.order_id = id++;
            msg.price    = to_price(100.0 + (id % 5) * 0.01);
            msg.qty      = 100;
            msg.side     = (id % 2 == 0) ? Side::Buy : Side::Sell;

            if (eng.submit(msg)) [[likely]]
                produced.fetch_add(1, std::memory_order_relaxed);
            else
                __builtin_ia32_pause();  // back-pressure
        }
    });

    // Let it run.
    std::this_thread::sleep_for(seconds(kDurationSec));
    stop.store(true, std::memory_order_seq_cst);
    producer.join();
    eng.stop();

    const uint64_t msgs_in  = produced.load();
    const uint64_t msgs_out = eng.messages_processed();

    printf("Duration              : %d seconds\n", kDurationSec);
    printf("Messages submitted    : %lu\n", msgs_in);
    printf("Messages processed    : %lu\n", msgs_out);
    printf("Throughput (in)       : %.2f M msgs/sec\n", msgs_in  / (1e6 * kDurationSec));
    printf("Throughput (out)      : %.2f M msgs/sec\n", msgs_out / (1e6 * kDurationSec));
    printf("Matches generated     : %lu\n", eng.matches_generated());

    return 0;
}
