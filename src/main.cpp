// main.cpp — Full exchange simulation demo.
//
// Wires together all components:
//   MarketDataIngestion → MatchingEngine → ExecutionLayer
//
// Simulates a realistic order flow: a mix of passive limit orders,
// aggressive market orders, cancellations, and modifications.
// Prints a live book snapshot and final statistics.

#include "core/types.hpp"
#include "core/matching_engine.hpp"
#include "core/execution_layer.hpp"
#include "core/market_data.hpp"
#include "util/logger.hpp"

#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <atomic>
#include <random>
#include <csignal>

using namespace engine;
using namespace std::chrono_literals;

// ── Global shutdown flag ──────────────────────────────────────────────────────

static std::atomic<bool> g_shutdown{false};

extern "C" void handle_signal(int) {
    g_shutdown.store(true, std::memory_order_release);
}

// ── Simulation ────────────────────────────────────────────────────────────────

static void run_simulation(MatchingEngine& eng, int duration_sec) {
    std::mt19937_64 rng(12345);

    // Price universe: cluster around 100.00 with a 2-tick spread.
    auto rand_price = [&](Side s) -> Price {
        double base = (s == Side::Buy) ? 99.9 : 100.1;
        double jitter = (rng() % 20) * 0.01 - 0.10;
        return to_price(base + jitter);
    };

    auto rand_qty = [&]() -> Qty { return 100 * (1 + rng() % 10); };

    MarketDataMsg msg{};
    msg.symbol = 0;

    std::vector<OrderId> live_ids;
    live_ids.reserve(10000);
    uint64_t id_gen = 1;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);

    while (!g_shutdown.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < deadline)
    {
        int action = rng() % 10;

        if (action < 6 || live_ids.empty()) {
            // New limit order.
            Side s = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            msg.msg_type   = MarketDataMsg::Type::NewOrder;
            msg.order_id   = id_gen++;
            msg.side       = s;
            msg.price      = rand_price(s);
            msg.qty        = rand_qty();
            msg.order_type = OrderType::Limit;

            if (eng.submit(msg)) live_ids.push_back(msg.order_id);

        } else if (action < 8 && !live_ids.empty()) {
            // Cancel random live order.
            std::size_t idx = rng() % live_ids.size();
            msg.msg_type = MarketDataMsg::Type::CancelOrder;
            msg.order_id = live_ids[idx];
            if (eng.submit(msg)) {
                live_ids.erase(live_ids.begin() + idx);
            }

        } else {
            // Aggressive market order.
            Side s = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            msg.msg_type   = MarketDataMsg::Type::NewOrder;
            msg.order_id   = id_gen++;
            msg.side       = s;
            msg.price      = 0;
            msg.qty        = rand_qty();
            msg.order_type = OrderType::Market;
            static_cast<void>(eng.submit(msg));
        }
    }

    printf("[sim] Submitted %lu order IDs total\n", id_gen - 1);
}

int main(int argc, char** argv) {
    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    const int duration = (argc > 1) ? std::atoi(argv[1]) : 3;

    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║       Low-Latency Trading Engine  —  Demo Run        ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    // ── Start logger ──────────────────────────────────────────────────────────
    global_logger().start();  // logs to stderr

    // ── Wire up components ────────────────────────────────────────────────────
    MatchingEngine engine;
    engine.register_symbol(0);   // AAPL-like instrument

    // Execution layer consumes from engine's outbound queue.
    // For demo purposes we drive it from the same thread via poll().
    std::atomic<uint64_t> fill_count{0};

    // Start engine.
    engine.start();

    printf("[main] Matching engine started. Running %d-second simulation...\n\n",
           duration);

    // ── Producer thread simulates market data feed ────────────────────────────
    std::thread sim_thread([&] { run_simulation(engine, duration); });

    // ── Main thread polls for execution reports ───────────────────────────────
    ExecutionReport rpt;
    while (!g_shutdown.load(std::memory_order_relaxed)) {
        if (engine.poll_report(rpt)) {
            fill_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }

        // Check if sim thread is done.
        using namespace std::chrono;
        static auto last_check = steady_clock::now();
        if (steady_clock::now() - last_check > 100ms) {
            last_check = steady_clock::now();
            if (!sim_thread.joinable()) break;
        }
    }

    sim_thread.join();

    // Drain remaining reports.
    int drained = 0;
    while (engine.poll_report(rpt)) { ++drained; ++fill_count; }

    engine.stop();
    global_logger().stop();

    // ── Print final statistics ────────────────────────────────────────────────
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║                  Final Statistics                    ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  Messages processed : %-30lu ║\n", engine.messages_processed());
    printf("║  Matches generated  : %-30lu ║\n", engine.matches_generated());
    printf("║  Fill reports rx    : %-30lu ║\n", fill_count.load());
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    printf("Run bench/bench_latency or bench/bench_throughput for perf numbers.\n");
    return 0;
}
