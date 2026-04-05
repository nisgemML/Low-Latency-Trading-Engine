#include "core/order_book.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>

namespace engine {

// ── OrderBook::Side ───────────────────────────────────────────────────────────

int32_t OrderBook::Side::find_level(Price p, bool /*descending*/) const noexcept {
    for (uint32_t i = 0; i < count; ++i)
        if (prices[i] == p) return static_cast<int32_t>(i);
    return -1;
}

int32_t OrderBook::Side::insert_level(Price p, bool descending) noexcept {
    if (count >= kMaxLevels) return -1;

    uint32_t pos = 0;
    if (descending) {
        while (pos < count && prices[pos] > p) ++pos;
    } else {
        while (pos < count && prices[pos] < p) ++pos;
    }

    if (pos < count) {
        const uint32_t n = count - pos;
        std::memmove(&prices[pos+1],       &prices[pos],       n * sizeof(Price));
        std::memmove(&qtys[pos+1],         &qtys[pos],         n * sizeof(Qty));
        std::memmove(&order_counts[pos+1], &order_counts[pos], n * sizeof(uint32_t));
        std::memmove(&head_idxs[pos+1],    &head_idxs[pos],    n * sizeof(uint32_t));
    }

    prices[pos]       = p;
    qtys[pos]         = 0;
    order_counts[pos] = 0;
    head_idxs[pos]    = NULL_IDX;
    ++count;
    return static_cast<int32_t>(pos);
}

void OrderBook::Side::remove_level(uint32_t idx) noexcept {
    assert(idx < count);
    const uint32_t n = count - idx - 1;
    if (n > 0) {
        std::memmove(&prices[idx],       &prices[idx+1],       n * sizeof(Price));
        std::memmove(&qtys[idx],         &qtys[idx+1],         n * sizeof(Qty));
        std::memmove(&order_counts[idx], &order_counts[idx+1], n * sizeof(uint32_t));
        std::memmove(&head_idxs[idx],    &head_idxs[idx+1],    n * sizeof(uint32_t));
    }
    --count;
}

// ── OrderBook::SlotPool ───────────────────────────────────────────────────────

void OrderBook::SlotPool::init() noexcept {
    for (uint32_t i = 0; i < kMaxOrders - 1; ++i)
        nexts[i] = i + 1;
    nexts[kMaxOrders - 1] = NULL_IDX;
    free_head = 0;
    used      = 0;
}

uint32_t OrderBook::SlotPool::alloc() noexcept {
    if (free_head == NULL_IDX) [[unlikely]] return NULL_IDX;
    uint32_t idx = free_head;
    free_head = nexts[idx];
    ++used;
    return idx;
}

void OrderBook::SlotPool::free_slot(uint32_t idx) noexcept {
    nexts[idx] = free_head;
    free_head  = idx;
    --used;
}

// ── OrderBook::OrderIndex ─────────────────────────────────────────────────────

void OrderBook::OrderIndex::init() noexcept {
    for (auto& e : table) { e.slot = kEmpty; e.key = 0; e.side = 0xFF; }
}

uint32_t OrderBook::OrderIndex::probe(OrderId id) const noexcept {
    uint32_t h = static_cast<uint32_t>(id * 2654435761ULL >> 32) % kTableSize;
    while (table[h].slot != kEmpty && table[h].key != id)
        h = (h + 1) % kTableSize;
    return h;
}

bool OrderBook::OrderIndex::insert(OrderId id, uint32_t slot, uint8_t side) noexcept {
    uint32_t h = probe(id);
    if (table[h].slot != kEmpty && table[h].key != id) return false;
    table[h] = { id, slot, side };
    return true;
}

bool OrderBook::OrderIndex::lookup(OrderId id, uint32_t& slot_out, uint8_t& side_out) const noexcept {
    uint32_t h = probe(id);
    if (table[h].slot == kEmpty) return false;
    slot_out = table[h].slot;
    side_out = table[h].side;
    return true;
}

bool OrderBook::OrderIndex::remove(OrderId id) noexcept {
    uint32_t h = probe(id);
    if (table[h].slot == kEmpty) return false;
    table[h].slot = kEmpty;
    table[h].key  = 0;
    uint32_t j = (h + 1) % kTableSize;
    while (table[j].slot != kEmpty) {
        auto entry = table[j];
        table[j].slot = kEmpty;
        uint32_t k = probe(entry.key);
        table[k] = entry;
        j = (j + 1) % kTableSize;
    }
    return true;
}

// ── OrderBook ─────────────────────────────────────────────────────────────────

OrderBook::OrderBook(SymbolId symbol, MatchCallback on_match)
    : match_cb_(std::move(on_match)), symbol_(symbol)
{
    slots_.init();
    index_.init();
}


bool OrderBook::add_order(const Order& order) noexcept {
    ++stat_orders_;
    Order o = order;

    if (o.type == OrderType::Market || o.type == OrderType::IOC || o.type == OrderType::FOK) {
        try_match(o);
        if (o.type == OrderType::FOK && !o.is_filled()) {
            ++stat_rejected_;
            return false;
        }
        if (o.is_filled() || o.type == OrderType::IOC || o.type == OrderType::FOK)
            return true;
    } else {
        try_match(o);
    }

    if (o.is_filled()) return true;

    bool is_bid = o.is_buy();
    Side& side  = is_bid ? bids_ : asks_;

    int32_t lvl_idx = side.find_level(o.price, is_bid);
    if (lvl_idx < 0) {
        lvl_idx = side.insert_level(o.price, is_bid);
        if (lvl_idx < 0) { ++stat_rejected_; return false; }
    }

    uint32_t slot = slots_.alloc();
    if (slot == NULL_IDX) { ++stat_rejected_; return false; }

    slots_.ids[slot]        = o.id;
    slots_.qtys[slot]       = o.qty_remaining;
    slots_.nexts[slot]      = NULL_IDX;
    // Store price instead of index — index shifts on remove_level.
    slots_.prices[slot]     = o.price;

    // Append to tail.
    if (side.head_idxs[lvl_idx] == NULL_IDX) {
        side.head_idxs[lvl_idx] = slot;
    } else {
        uint32_t cur = side.head_idxs[lvl_idx];
        while (slots_.nexts[cur] != NULL_IDX) cur = slots_.nexts[cur];
        slots_.nexts[cur] = slot;
    }

    side.qtys[lvl_idx]         += o.qty_remaining;
    side.order_counts[lvl_idx] += 1;

    index_.insert(o.id, slot, is_bid ? 0 : 1);
    return true;
}

bool OrderBook::cancel_order(OrderId order_id) noexcept {
    uint32_t slot; uint8_t side_idx;
    if (!index_.lookup(order_id, slot, side_idx)) return false;

    Side& side   = (side_idx == 0) ? bids_ : asks_;
    Price price  = slots_.prices[slot];
    int32_t lvl  = side.find_level(price, side_idx == 0);
    if (lvl < 0) {
        // Level gone (fully matched) — just clean up index.
        index_.remove(order_id);
        return false;
    }

    remove_order_from_level(side_idx, static_cast<uint32_t>(lvl), slot);
    index_.remove(order_id);
    ++stat_cancelled_;
    return true;
}

bool OrderBook::modify_order(OrderId order_id, Qty new_qty) noexcept {
    uint32_t slot; uint8_t side_idx;
    if (!index_.lookup(order_id, slot, side_idx)) return false;

    Side& side  = (side_idx == 0) ? bids_ : asks_;
    Price price = slots_.prices[slot];
    int32_t lvl = side.find_level(price, side_idx == 0);
    if (lvl < 0) { index_.remove(order_id); return false; }

    const Qty old_qty = slots_.qtys[slot];
    if (new_qty >= old_qty) {
        side.qtys[lvl]    += (new_qty - old_qty);
        slots_.qtys[slot]  = new_qty;
    } else {
        side.qtys[lvl]    -= (old_qty - new_qty);
        slots_.qtys[slot]  = new_qty;
        if (new_qty == 0) {
            remove_order_from_level(side_idx, static_cast<uint32_t>(lvl), slot);
            index_.remove(order_id);
        }
    }
    return true;
}

void OrderBook::try_match(Order& incoming) noexcept {
    bool aggressor_is_buy = incoming.is_buy();
    Side& passive_side    = aggressor_is_buy ? asks_ : bids_;

    while (incoming.qty_remaining > 0 && passive_side.count > 0) {
        Price best_passive = passive_side.prices[0];

        if (incoming.type == OrderType::Limit) {
            if (aggressor_is_buy  && incoming.price < best_passive) break;
            if (!aggressor_is_buy && incoming.price > best_passive) break;
        }

        // Always work level index 0 (best).
        while (passive_side.head_idxs[0] != NULL_IDX && incoming.qty_remaining > 0) {
            uint32_t slot     = passive_side.head_idxs[0];
            const Qty fill_qty = std::min(incoming.qty_remaining, slots_.qtys[slot]);

            // Update quantities.
            slots_.qtys[slot]       -= fill_qty;
            passive_side.qtys[0]    -= fill_qty;
            incoming.qty_remaining  -= fill_qty;

            // Fire execution report.
            if (match_cb_) {
                ExecutionReport rpt{};
                rpt.order_id        = incoming.id;
                rpt.contra_order_id = slots_.ids[slot];
                rpt.exec_price      = best_passive;
                rpt.exec_qty        = fill_qty;
                rpt.leaves_qty      = incoming.qty_remaining;
                rpt.symbol          = symbol_;
                rpt.side            = incoming.side;
                rpt.exec_type       = (incoming.qty_remaining == 0)
                                      ? ExecType::Fill : ExecType::PartialFill;
                match_cb_(rpt);
            }
            ++stat_matches_;

            if (slots_.qtys[slot] == 0) {
                // Passive order fully filled — dequeue and free.
                passive_side.head_idxs[0] = slots_.nexts[slot];
                passive_side.order_counts[0] -= 1;
                index_.remove(slots_.ids[slot]);
                slots_.free_slot(slot);
            }
        }

        // Remove exhausted level.
        if (passive_side.order_counts[0] == 0)
            passive_side.remove_level(0);
    }
}

void OrderBook::remove_order_from_level(uint8_t side_idx, uint32_t level_idx,
                                         uint32_t slot) noexcept
{
    Side& side = (side_idx == 0) ? bids_ : asks_;

    side.qtys[level_idx]         -= slots_.qtys[slot];
    side.order_counts[level_idx] -= 1;

    // Unlink from intrusive chain.
    if (side.head_idxs[level_idx] == slot) {
        side.head_idxs[level_idx] = slots_.nexts[slot];
    } else {
        uint32_t prev = side.head_idxs[level_idx];
        while (prev != NULL_IDX && slots_.nexts[prev] != slot)
            prev = slots_.nexts[prev];
        if (prev != NULL_IDX)
            slots_.nexts[prev] = slots_.nexts[slot];
    }

    slots_.free_slot(slot);

    if (side.order_counts[level_idx] == 0)
        side.remove_level(level_idx);
}

BestQuote OrderBook::best_quote() const noexcept {
    BestQuote q{};
    q.symbol    = symbol_;
    q.bid_price = bids_.count > 0 ? bids_.prices[0] : PRICE_INVALID;
    q.ask_price = asks_.count > 0 ? asks_.prices[0] : PRICE_INVALID;
    q.bid_qty   = bids_.count > 0 ? bids_.qtys[0]   : 0;
    q.ask_qty   = asks_.count > 0 ? asks_.qtys[0]   : 0;
    return q;
}

std::size_t OrderBook::bid_depth(std::span<PriceLevel> out) const noexcept {
    std::size_t n = std::min(out.size(), static_cast<std::size_t>(bids_.count));
    for (std::size_t i = 0; i < n; ++i)
        out[i] = { bids_.prices[i], bids_.qtys[i], bids_.order_counts[i], bids_.head_idxs[i] };
    return n;
}

std::size_t OrderBook::ask_depth(std::span<PriceLevel> out) const noexcept {
    std::size_t n = std::min(out.size(), static_cast<std::size_t>(asks_.count));
    for (std::size_t i = 0; i < n; ++i)
        out[i] = { asks_.prices[i], asks_.qtys[i], asks_.order_counts[i], asks_.head_idxs[i] };
    return n;
}

} // namespace engine
