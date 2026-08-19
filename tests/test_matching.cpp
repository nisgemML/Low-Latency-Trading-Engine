// test_matching.cpp — end-to-end matching engine integration tests.
//
// Tests the full pipeline: MarketDataMsg → MatchingEngine → ExecutionReport.
// Validates both correctness and that the SPSC queues wire up properly.

#include "core/matching_engine.hpp"
#include <cstdio>
#include <thread>
#include <chrono>
#include <vector>
#include <cassert>

using namespace engine;
using namespace std::chrono_literals;

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

// Poll the engine's outbound queue with a timeout.
static bool wait_for_report(MatchingEngine& eng, ExecutionReport& out,
                             int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (eng.poll_report(out)) return true;
        __builtin_ia32_pause();
    }
    return false;
}

static MarketDataMsg make_msg(MarketDataMsg::Type type, OrderId id,
                               SymbolId sym, Side side, Price price, Qty qty,
                               OrderType otype = OrderType::Limit) {
    MarketDataMsg m{};
    m.msg_type   = type;
    m.order_id   = id;
    m.symbol     = sym;
    m.side       = side;
    m.price      = price;
    m.qty        = qty;
    m.order_type = otype;
    return m;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

static void test_cross_generates_report() {
    MatchingEngine eng;
    eng.register_symbol(0);
    eng.start();

    // Post a resting sell.
    auto sell = make_msg(MarketDataMsg::Type::NewOrder, 1, 0,
                         Side::Sell, to_price(100.0), 100);
    while (!eng.submit(sell)) {}
    std::this_thread::sleep_for(100ms);  // let engine process

    // Aggressive buy — should cross.
    auto buy  = make_msg(MarketDataMsg::Type::NewOrder, 2, 0,
                         Side::Buy,  to_price(100.0), 100);
    while (!eng.submit(buy)) {}

    ExecutionReport rpt;
    bool got = wait_for_report(eng, rpt);
    CHECK(got,                           "Execution report received");
    CHECK(rpt.exec_qty == 100,           "Fill qty == 100");
    CHECK(rpt.exec_price == to_price(100.0), "Fill price == 100.0");

    eng.stop();
}

static void test_cancel_before_match() {
    MatchingEngine eng;
    eng.register_symbol(0);
    eng.start();

    auto sell = make_msg(MarketDataMsg::Type::NewOrder, 10, 0,
                         Side::Sell, to_price(100.0), 200);
    while (!eng.submit(sell)) {}
    std::this_thread::sleep_for(100ms);

    auto cancel = make_msg(MarketDataMsg::Type::CancelOrder, 10, 0,
                           Side::Sell, 0, 0);
    while (!eng.submit(cancel)) {}
    std::this_thread::sleep_for(100ms);

    // Buy should NOT produce a fill (sell was cancelled).
    auto buy = make_msg(MarketDataMsg::Type::NewOrder, 11, 0,
                        Side::Buy, to_price(100.0), 200);
    while (!eng.submit(buy)) {}

    ExecutionReport rpt;
    bool got = wait_for_report(eng, rpt, 500);  // short timeout
    CHECK(!got, "No fill after cancel");

    eng.stop();
}

static void test_multi_symbol_isolation() {
    MatchingEngine eng;
    eng.register_symbol(0);
    eng.register_symbol(1);
    eng.start();

    // Sell on symbol 0 at 100.
    while (!eng.submit(make_msg(MarketDataMsg::Type::NewOrder, 20, 0,
                                Side::Sell, to_price(100.0), 100))) {}
    std::this_thread::sleep_for(100ms);

    // Buy on symbol 1 at 100 — different symbol, should NOT cross with symbol 0.
    while (!eng.submit(make_msg(MarketDataMsg::Type::NewOrder, 21, 1,
                                Side::Buy,  to_price(100.0), 100))) {}

    ExecutionReport rpt;
    bool got = wait_for_report(eng, rpt, 500);
    CHECK(!got, "Cross-symbol orders do not match");

    eng.stop();
}

static void test_throughput_smoke() {
    MatchingEngine eng;
    eng.register_symbol(0);
    eng.start();

    static constexpr int kOrders = 100'000;
    auto t0 = std::chrono::steady_clock::now();

    for (int i = 1; i <= kOrders; ++i) {
        Side s = (i % 2 == 0) ? Side::Buy : Side::Sell;
        auto msg = make_msg(MarketDataMsg::Type::NewOrder, i, 0, s,
                            to_price(100.0 + (i % 5) * 0.01), 100);
        while (!eng.submit(msg)) __builtin_ia32_pause();
    }

    // Wait for engine to drain.
    std::this_thread::sleep_for(200ms);
    eng.stop();

    auto elapsed = std::chrono::steady_clock::now() - t0;
    double sec = std::chrono::duration<double>(elapsed).count();
    double rate = kOrders / sec / 1e6;

    printf("  Throughput smoke: %.2f M msgs/sec (%d orders in %.1f ms)\n",
           rate, kOrders, sec * 1000.0);
    CHECK(eng.messages_processed() > 0, "Engine processed messages");
}

static void test_market_order_e2e() {
    MatchingEngine eng;
    eng.register_symbol(0);
    eng.start();

    // Rest a sell.
    while (!eng.submit(make_msg(MarketDataMsg::Type::NewOrder, 30, 0,
                                Side::Sell, to_price(50.0), 500))) {}
    std::this_thread::sleep_for(100ms);

    // Market buy for 300.
    while (!eng.submit(make_msg(MarketDataMsg::Type::NewOrder, 31, 0,
                                Side::Buy, 0, 300, OrderType::Market))) {}

    ExecutionReport rpt;
    bool got = wait_for_report(eng, rpt);
    CHECK(got,                "Market order filled");
    CHECK(rpt.exec_qty == 300, "Full fill for market order");

    eng.stop();
}

int main() {
    printf("=== Matching Engine Integration Tests ===\n\n");

    test_cross_generates_report();
    test_cancel_before_match();
    test_multi_symbol_isolation();
    test_market_order_e2e();
    test_throughput_smoke();

    printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
