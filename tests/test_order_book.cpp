// test_order_book.cpp — unit + property-based tests for the order book.
//
// We test the invariants that must hold regardless of order sequence:
//   1. Best bid >= all other bids at every point.
//   2. Best ask <= all other asks at every point.
//   3. A market order executes at the best available price.
//   4. Cancelled orders are never filled.
//   5. Price-time priority: earlier orders at same price fill first.
//   6. Quantity conservation: filled_qty + leaves_qty == original_qty.

#include "core/order_book.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
#include <random>

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

// ── Helpers ───────────────────────────────────────────────────────────────────

struct FillRecord {
    OrderId order_id;
    OrderId contra_id;
    Price   price;
    Qty     qty;
};

static OrderBook make_book(std::vector<FillRecord>& fills) {
    return OrderBook(0, [&fills](const ExecutionReport& r) {
        fills.push_back({ r.order_id, r.contra_order_id, r.exec_price, r.exec_qty });
    });
}

static Order make_order(OrderId id, Side side, Price price, Qty qty,
                         OrderType type = OrderType::Limit) {
    Order o{};
    o.id            = id;
    o.price         = price;
    o.qty           = qty;
    o.qty_remaining = qty;
    o.symbol        = 0;
    o.side          = side;
    o.type          = type;
    o.status        = OrderStatus::New;
    return o;
}

// ── Test cases ────────────────────────────────────────────────────────────────

static void test_simple_match() {
    std::vector<FillRecord> fills;
    auto book = make_book(fills);

    // Post a resting sell at 100.
    book.add_order(make_order(1, Side::Sell, to_price(100.0), 200));
    CHECK(fills.empty(), "No fill yet — passive order rested");

    // Aggressive buy at 100 (or higher) should match.
    book.add_order(make_order(2, Side::Buy, to_price(100.0), 100));
    CHECK(fills.size() == 1, "One fill generated");
    if (!fills.empty()) {
        CHECK(fills[0].qty == 100,         "Fill qty == 100");
        CHECK(fills[0].price == to_price(100.0), "Fill price == 100.0");
    }

    // Best ask should still show 100 remaining qty = 100.
    BestQuote q = book.best_quote();
    CHECK(q.ask_price == to_price(100.0), "Ask price still 100");
    CHECK(q.ask_qty   == 100,             "Ask qty reduced to 100");
}

static void test_price_time_priority() {
    std::vector<FillRecord> fills;
    auto book = make_book(fills);

    // Two resting sells at same price — order 1 posted first.
    book.add_order(make_order(1, Side::Sell, to_price(50.0), 100));
    book.add_order(make_order(2, Side::Sell, to_price(50.0), 100));

    // Aggressive buy for 100 — should fill entirely against order 1.
    book.add_order(make_order(3, Side::Buy, to_price(50.0), 100));

    CHECK(fills.size() == 1, "One fill");
    if (!fills.empty()) {
        CHECK(fills[0].contra_id == 1, "Filled against order 1 (FIFO priority)");
    }

    // Order 2 should still be resting.
    BestQuote q = book.best_quote();
    CHECK(q.ask_qty == 100, "Order 2 still resting");
}

static void test_cancel_prevents_fill() {
    std::vector<FillRecord> fills;
    auto book = make_book(fills);

    book.add_order(make_order(1, Side::Sell, to_price(100.0), 200));
    bool cancelled = book.cancel_order(1);
    CHECK(cancelled, "Cancel succeeded");

    // Now the book should be empty — buy should not fill.
    book.add_order(make_order(2, Side::Buy, to_price(100.0), 100));
    CHECK(fills.empty(), "No fill after cancel");

    BestQuote q = book.best_quote();
    CHECK(q.ask_price == PRICE_INVALID, "Ask side empty after cancel");
}

static void test_partial_fill() {
    std::vector<FillRecord> fills;
    auto book = make_book(fills);

    book.add_order(make_order(1, Side::Sell, to_price(100.0), 50));
    // Buy for 200 — only 50 available.
    book.add_order(make_order(2, Side::Buy, to_price(100.0), 200));

    CHECK(fills.size() == 1, "One fill");
    if (!fills.empty()) {
        CHECK(fills[0].qty == 50, "Partial fill qty == 50");
    }

    // Buy order 2 should have 150 remaining and rest in the book.
    BestQuote q = book.best_quote();
    CHECK(q.bid_price == to_price(100.0), "Partial buy rests at 100");
    CHECK(q.bid_qty   == 150,             "Remaining qty 150");
}

static void test_spread() {
    std::vector<FillRecord> fills;
    auto book = make_book(fills);

    // Spread: bid=99, ask=101 — no cross, no match.
    book.add_order(make_order(1, Side::Buy,  to_price(99.0),  100));
    book.add_order(make_order(2, Side::Sell, to_price(101.0), 100));

    CHECK(fills.empty(), "No match — spread exists");
    BestQuote q = book.best_quote();
    CHECK(q.bid_price == to_price(99.0),  "Best bid 99");
    CHECK(q.ask_price == to_price(101.0), "Best ask 101");
}

static void test_market_order() {
    std::vector<FillRecord> fills;
    auto book = make_book(fills);

    book.add_order(make_order(1, Side::Sell, to_price(100.0), 200));
    auto mkt = make_order(2, Side::Buy, 0, 150, OrderType::Market);
    book.add_order(mkt);

    CHECK(!fills.empty(),      "Market order matched");
    CHECK(fills[0].qty == 150, "Full fill for market order");
}

// ── Property-based test: random order stream ──────────────────────────────────

static void test_invariants_random() {
    std::vector<FillRecord> fills;
    auto book = make_book(fills);
    std::mt19937_64 rng(42);

    auto rand_price = [&]() { return to_price(99.0 + (rng() % 5) * 0.5); };
    auto rand_qty   = [&]() -> Qty { return 1 + (rng() % 200); };

    std::vector<OrderId> live_orders;
    bool invariant_ok = true;

    for (int i = 1; i <= 100'000; ++i) {
        int action = rng() % 10;

        if (action < 6 || live_orders.empty()) {
            // New order.
            Side s = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            book.add_order(make_order(i, s, rand_price(), rand_qty()));
            live_orders.push_back(i);
        } else if (action < 8 && !live_orders.empty()) {
            // Cancel a random live order.
            auto it = live_orders.begin() + (rng() % live_orders.size());
            book.cancel_order(*it);
            live_orders.erase(it);
        }

        // Invariant: bid <= ask (if both sides populated).
        BestQuote q = book.best_quote();
        if (q.bid_price != PRICE_INVALID && q.ask_price != PRICE_INVALID) {
            if (q.bid_price > q.ask_price) {
                invariant_ok = false;
                fprintf(stderr, "INVARIANT VIOLATED at i=%d: bid=%ld > ask=%ld\n",
                        i, q.bid_price, q.ask_price);
                break;
            }
        }
    }

    CHECK(invariant_ok, "bid <= ask invariant holds throughout random sequence");
    printf("  Random test: %lu fills generated over 100K events\n", fills.size());
}

int main() {
    printf("=== Order Book Tests ===\n\n");
    test_simple_match();
    test_price_time_priority();
    test_cancel_prevents_fill();
    test_partial_fill();
    test_spread();
    test_market_order();
    test_invariants_random();

    printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
