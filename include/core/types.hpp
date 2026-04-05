#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace engine {

// ── Fundamental types ────────────────────────────────────────────────────────

using Price    = int64_t;   // fixed-point: price * 10^6
using Qty      = uint32_t;
using OrderId  = uint64_t;
using SymbolId = uint16_t;
using SeqNum   = uint64_t;

static constexpr Price PRICE_INVALID = std::numeric_limits<Price>::min();
static constexpr OrderId ORDER_ID_INVALID = 0;

// Convert double to fixed-point price (6 decimal places)
[[nodiscard]] constexpr Price to_price(double d) noexcept {
    return static_cast<Price>(d * 1'000'000.0);
}
[[nodiscard]] constexpr double from_price(Price p) noexcept {
    return static_cast<double>(p) / 1'000'000.0;
}

// ── Enumerations ─────────────────────────────────────────────────────────────

enum class Side : uint8_t { Buy = 0, Sell = 1 };
enum class OrderType : uint8_t { Limit = 0, Market = 1, IOC = 2, FOK = 3 };
enum class OrderStatus : uint8_t {
    New, PartiallyFilled, Filled, Cancelled, Rejected
};
enum class ExecType : uint8_t { New, Fill, PartialFill, Cancelled, Rejected };

// ── Core order struct ─────────────────────────────────────────────────────────
// Kept small to stay L1-cache-friendly; 48 bytes total.

struct alignas(64) Order {
    OrderId   id;
    Price     price;
    Qty       qty;
    Qty       qty_remaining;
    SymbolId  symbol;
    Side      side;
    OrderType type;
    OrderStatus status;
    uint8_t   _pad[5];

    [[nodiscard]] bool is_buy()  const noexcept { return side == Side::Buy; }
    [[nodiscard]] bool is_sell() const noexcept { return side == Side::Sell; }
    [[nodiscard]] bool is_filled() const noexcept { return qty_remaining == 0; }
};
static_assert(sizeof(Order) <= 64, "Order must fit in one cache line");

// ── Execution report ──────────────────────────────────────────────────────────

struct ExecutionReport {
    OrderId  order_id;
    OrderId  contra_order_id;
    Price    exec_price;
    Qty      exec_qty;
    Qty      leaves_qty;
    SymbolId symbol;
    Side     side;
    ExecType exec_type;
    uint8_t  _pad[5];
};

// ── Market data message ───────────────────────────────────────────────────────

struct MarketDataMsg {
    enum class Type : uint8_t {
        NewOrder, CancelOrder, ModifyOrder, Heartbeat
    };

    SeqNum   seq;
    OrderId  order_id;
    Price    price;
    Qty      qty;
    SymbolId symbol;
    Side     side;
    Type     msg_type;
    OrderType order_type;
};

// ── Top-of-book snapshot ──────────────────────────────────────────────────────

struct BestQuote {
    Price bid_price;
    Price ask_price;
    Qty   bid_qty;
    Qty   ask_qty;
    SymbolId symbol;
    uint8_t _pad[6];
};

} // namespace engine
