#pragma once
// order_book_view.hpp — functional, immutable view over the order book state.
//
// Jane Street context:
//   Jane Street's codebase is primarily OCaml — a language where immutable
//   data and functional traversal are idiomatic.  When C++ engineers at JS
//   talk about "good code", they mean code that makes data flow explicit and
//   avoids implicit mutation.
//
//   This header provides a zero-copy view type and functional combinators
//   over the book's price levels.  It mirrors the OCaml pattern of:
//
//     List.fold_left f init (Book.bid_levels book)
//
//   as a C++20 range-based equivalent:
//
//     book_view.bid_levels() | vwap_fold(...)
//
// Usage pattern:
//   const auto view = OrderBookView::of(book);
//   auto vwap = view.bid_levels().vwap(depth_qty);
//   view.bid_levels().for_each([](Price p, Qty q, int rank) { ... });

#include "core/types.hpp"
#include "core/order_book.hpp"
#include <span>
#include <functional>
#include <optional>
#include <array>

namespace engine {

// A non-owning, snapshot-style view over one side of the book.
class LevelView {
public:
    // Functional fold over levels (bid or ask, in book priority order).
    // Signature: Acc f(Acc acc, Price p, Qty q, int rank)
    template<typename Acc, typename F>
    [[nodiscard]] Acc fold(Acc init, F&& f) const noexcept {
        for (int i = 0; i < static_cast<int>(levels_.size()); ++i)
            init = f(std::move(init), levels_[i].price, levels_[i].total_qty, i);
        return init;
    }

    // Convenience: iterate without accumulator.
    template<typename F>
    void for_each(F&& f) const noexcept {
        for (int i = 0; i < static_cast<int>(levels_.size()); ++i)
            f(levels_[i].price, levels_[i].total_qty, i);
    }

    // Volume-weighted average price for the first `qty` units available.
    // Returns nullopt if insufficient liquidity.
    [[nodiscard]] std::optional<double> vwap(Qty qty) const noexcept {
        if (qty == 0 || levels_.empty()) return std::nullopt;

        struct Acc { int64_t weighted = 0; Qty filled = 0; };
        auto result = fold(Acc{}, [qty](Acc a, Price p, Qty q, int) -> Acc {
            if (a.filled >= qty) return a;
            const Qty take = std::min(q, qty - a.filled);
            a.weighted += static_cast<int64_t>(p) * take;
            a.filled   += take;
            return a;
        });

        if (result.filled < qty) return std::nullopt;
        return static_cast<double>(result.weighted) / static_cast<double>(result.filled);
    }

    // Market impact: estimate slippage (in price units) to fill `qty`.
    // Returns the difference between VWAP and top-of-book price.
    [[nodiscard]] std::optional<double> market_impact(Qty qty) const noexcept {
        if (levels_.empty()) return std::nullopt;
        auto v = vwap(qty);
        if (!v) return std::nullopt;
        const double top = from_price(levels_[0].price);
        return std::abs(*v / 1'000'000.0 - top);  // convert fixed-point
    }

    // Total available quantity across all levels.
    [[nodiscard]] Qty total_qty() const noexcept {
        return fold(Qty{0}, [](Qty a, Price, Qty q, int) { return a + q; });
    }

    [[nodiscard]] std::size_t depth() const noexcept { return levels_.size(); }
    [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }

    // Access the level span directly (for interop).
    [[nodiscard]] std::span<const PriceLevel> levels() const noexcept {
        return levels_;
    }

    explicit LevelView(std::span<const PriceLevel> lvls) : levels_(lvls) {}

private:
    std::span<const PriceLevel> levels_;
};

// Snapshot view of the full book — populated by copying levels at call time.
// Intentionally not a live view: the book may mutate; the snapshot is stable.
class OrderBookSnapshot {
    static constexpr std::size_t kMaxLevels = 64;

    std::array<PriceLevel, kMaxLevels> bid_buf_{};
    std::array<PriceLevel, kMaxLevels> ask_buf_{};
    std::size_t n_bids_ = 0;
    std::size_t n_asks_ = 0;
    SymbolId symbol_    = 0;

public:
    // Capture a snapshot of the book (called from the matching thread only).
    static OrderBookSnapshot capture(const OrderBook& book) noexcept {
        OrderBookSnapshot s;
        s.symbol_  = book.symbol();
        s.n_bids_  = book.bid_depth(s.bid_buf_);
        s.n_asks_  = book.ask_depth(s.ask_buf_);
        return s;
    }

    [[nodiscard]] LevelView bids() const noexcept {
        return LevelView{ std::span<const PriceLevel>{ bid_buf_.data(), n_bids_ } };
    }
    [[nodiscard]] LevelView asks() const noexcept {
        return LevelView{ std::span<const PriceLevel>{ ask_buf_.data(), n_asks_ } };
    }
    [[nodiscard]] SymbolId symbol() const noexcept { return symbol_; }

    // Midpoint price (undefined if either side is empty).
    [[nodiscard]] std::optional<double> mid() const noexcept {
        if (n_bids_ == 0 || n_asks_ == 0) return std::nullopt;
        return (from_price(bid_buf_[0].price) + from_price(ask_buf_[0].price)) / 2.0;
    }

    // Quoted spread (ask - bid at top of book).
    [[nodiscard]] std::optional<double> spread() const noexcept {
        if (n_bids_ == 0 || n_asks_ == 0) return std::nullopt;
        return from_price(ask_buf_[0].price) - from_price(bid_buf_[0].price);
    }
};

} // namespace engine
