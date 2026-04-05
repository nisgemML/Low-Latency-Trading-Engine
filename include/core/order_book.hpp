#pragma once

#include "core/types.hpp"
#include "util/allocator.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <span>

namespace engine {

struct PriceLevel {
    Price    price;
    Qty      total_qty;
    uint32_t order_count;
    uint32_t head_idx;
};

static constexpr uint32_t NULL_IDX = UINT32_MAX;

class OrderBook {
public:
    static constexpr std::size_t kMaxLevels = 4096;
    static constexpr std::size_t kMaxOrders = 65536;

    using MatchCallback = std::function<void(const ExecutionReport&)>;

    explicit OrderBook(SymbolId symbol, MatchCallback on_match);

    bool add_order(const Order& order) noexcept;
    bool cancel_order(OrderId order_id) noexcept;
    bool modify_order(OrderId order_id, Qty new_qty) noexcept;

    [[nodiscard]] BestQuote best_quote() const noexcept;
    [[nodiscard]] std::size_t bid_depth(std::span<PriceLevel> out) const noexcept;
    [[nodiscard]] std::size_t ask_depth(std::span<PriceLevel> out) const noexcept;

    [[nodiscard]] SymbolId  symbol()          const noexcept { return symbol_; }
    [[nodiscard]] uint64_t  total_orders()    const noexcept { return stat_orders_; }
    [[nodiscard]] uint64_t  total_matches()   const noexcept { return stat_matches_; }
    [[nodiscard]] uint64_t  total_cancelled() const noexcept { return stat_cancelled_; }

    // ── Internal types exposed for the .cpp implementation ──────────────────

    struct Side {
        // Struct-of-arrays — prices[] is the hot field iterated during matching.
        std::array<Price,     kMaxLevels> prices;
        std::array<Qty,       kMaxLevels> qtys;
        std::array<uint32_t,  kMaxLevels> order_counts;
        std::array<uint32_t,  kMaxLevels> head_idxs;
        uint32_t count = 0;

        [[nodiscard]] int32_t find_level(Price p, bool descending) const noexcept;
        int32_t insert_level(Price p, bool descending) noexcept;
        void remove_level(uint32_t idx) noexcept;
    };

    struct SlotPool {
        // Parallel arrays for each order slot.
        std::array<OrderId,  kMaxOrders> ids;
        std::array<Qty,      kMaxOrders> qtys;
        std::array<uint32_t, kMaxOrders> nexts;
        // Store price (not level index) — stable across remove_level() shifts.
        std::array<Price,    kMaxOrders> prices;

        uint32_t free_head = 0;
        uint32_t used      = 0;

        void     init() noexcept;
        [[nodiscard]] uint32_t alloc() noexcept;
        void     free_slot(uint32_t idx) noexcept;
    };

    struct OrderIndex {
        static constexpr uint32_t kTableSize = kMaxOrders * 2;
        static constexpr uint32_t kEmpty     = UINT32_MAX;

        struct Entry {
            OrderId  key   = 0;
            uint32_t slot  = kEmpty;
            uint8_t  side  = 0xFF;
        };

        std::array<Entry, kTableSize> table;

        void init() noexcept;
        bool insert(OrderId id, uint32_t slot, uint8_t side) noexcept;
        bool lookup(OrderId id, uint32_t& slot_out, uint8_t& side_out) const noexcept;
        bool remove(OrderId id) noexcept;

    private:
        [[nodiscard]] uint32_t probe(OrderId id) const noexcept;
    };

private:
    void try_match(Order& incoming) noexcept;
    void remove_order_from_level(uint8_t side_idx, uint32_t level_idx,
                                 uint32_t slot) noexcept;

    MatchCallback match_cb_;
    SymbolId      symbol_;

    Side      bids_;
    Side      asks_;
    SlotPool  slots_;
    OrderIndex index_;

    uint64_t stat_orders_    = 0;
    uint64_t stat_matches_   = 0;
    uint64_t stat_cancelled_ = 0;
    uint64_t stat_rejected_  = 0;
};

} // namespace engine
