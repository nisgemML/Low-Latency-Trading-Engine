#pragma once
// inventory_market_maker.hpp — Inventory-aware spread-setting and quoting.
//
// Theory:
//   A market maker faces two risks when quoting a two-sided market:
//
//   1. Adverse selection: counterparties who are better informed than you are.
//      When an informed trader hits your bid, you buy stock that will fall.
//      Kyle's λ measures this: it is the price impact per unit of signed flow,
//      i.e., how much the "true" price moves per unit of trading.  A large λ
//      means high adverse selection — you should widen your spread.
//
//   2. Inventory risk: you accumulate a position.  A long inventory in a
//      falling market is painful.  To reduce inventory, a market maker skews
//      quotes asymmetrically: if long, post a tighter ask (to sell) and a
//      wider bid (to discourage more buying).
//
// This module implements the Avellaneda-Stoikov (2008) reservation price and
// spread formula, augmented with:
//
//   a. Dynamic adverse selection cost via Kyle's λ (from microstructure.hpp).
//   b. Inventory skew: reservation price shifts by γ * σ² * q * T where q is
//      net inventory, γ is risk aversion, σ is volatility, T is time to reset.
//   c. Hard inventory limits: if |q| > kMaxInventory, quotes are one-sided
//      until the position unwinds below the threshold.
//
// Avellaneda-Stoikov (2008) optimal spread:
//   r(s, q, t)  = s - q * γ * σ² * (T - t)          [reservation price]
//   δ_bid       = r - (γ * σ² * (T - t) / 2) - λ * q
//   δ_ask       = r + (γ * σ² * (T - t) / 2) + λ * q
//   optimal spread = γ * σ² * (T - t) + 2/γ * ln(1 + γ/κ)
//
// Integration:
//   1. After each top-of-book change, call update_book_state().
//   2. After each trade report, call record_trade().
//   3. Call compute_quotes() to get the bid/ask prices to post.
//   4. Compare against current posted quotes; re-quote if they've moved
//      by more than kRequoteThreshold ticks.

#include "core/types.hpp"
#include "core/microstructure.hpp"
#include "core/order_book_view.hpp"
#include <optional>
#include <cmath>
#include <algorithm>

namespace engine {

// ── Parameters ─────────────────────────────────────────────────────────────────

struct MarketMakerParams {
    // Risk aversion parameter γ ∈ (0, 1].  Higher = wider spread, stronger
    // inventory skew.  Typical value: 0.1–0.5 for equity market-making.
    double gamma = 0.1;

    // Volatility σ (annualised, in price units per unit time).
    // Estimated externally (e.g. from an EWM on mid-price returns) and passed in.
    double sigma = 0.01;

    // Inventory reset horizon T (in seconds).  Avellaneda-Stoikov assumes a
    // finite horizon over which the market maker aims to unwind.
    double T_seconds = 300.0;

    // Order arrival rate κ — the intensity of market orders hitting our quotes.
    // Used in the optimal spread formula.  Estimated empirically or set to 1.
    double kappa = 1.0;

    // Hard inventory cap.  If |inventory| exceeds this, one-side quoting kicks in.
    int64_t max_inventory = 500;

    // Minimum quoted spread (in internal price units, i.e. 10^-6 of $ per unit).
    // Never post tighter than this regardless of model output.
    Price min_spread = to_price(0.01);  // 1 cent

    // Requote threshold: re-quote only if computed price differs by this many
    // internal price units from the currently posted quote.
    Price requote_threshold = to_price(0.001);  // 0.1 cent

    // Number of adverse selection λ units to add to spread per side.
    // Empirically calibrated.  Higher = more defensive against informed flow.
    double lambda_multiplier = 2.0;
};

// ── Quote output ───────────────────────────────────────────────────────────────

struct QuoteDecision {
    Price    bid_price;
    Price    ask_price;
    Qty      bid_qty;
    Qty      ask_qty;
    bool     quote_bid;   // false if inventory too long
    bool     quote_ask;   // false if inventory too short
    double   reservation_price;   // for logging / debugging
    double   model_spread;        // for logging / debugging
};

// ── Inventory-aware market maker ───────────────────────────────────────────────

class InventoryMarketMaker {
public:
    explicit InventoryMarketMaker(MarketMakerParams params, Qty default_qty = 100)
        : params_(params), default_qty_(default_qty) {}

    // ── Inputs ───────────────────────────────────────────────────────────────

    // Call when a new top-of-book snapshot is available.
    void update_book_state(Price mid_price, double elapsed_seconds) noexcept {
        mid_price_ = mid_price;
        elapsed_   = elapsed_seconds;

        // Update OFI for this tick.
        // (Caller may also pass bid/ask qty if available — use overload below.)
    }

    void update_book_state(Price bid_p, Qty bid_q, Price ask_p, Qty ask_q,
                           double elapsed_seconds) noexcept {
        mid_price_ = (bid_p + ask_p) / 2;
        elapsed_   = elapsed_seconds;
        ofi_.record(bid_p, bid_q, ask_p, ask_q);
    }

    // Call on each completed trade (fills against our quotes or market trades).
    // signed_qty > 0 = buyer-initiated.
    void record_trade(double signed_qty, double price_before, double price_after) noexcept {
        kyle_.record(signed_qty, price_before, price_after);
        roll_.record_trade(price_after);
    }

    // Update our net inventory (positive = long, negative = short).
    void set_inventory(int64_t qty) noexcept { inventory_ = qty; }
    void adjust_inventory(int64_t delta) noexcept { inventory_ += delta; }

    // Update volatility estimate (e.g. from an external EWM estimator).
    void set_sigma(double sigma) noexcept { params_.sigma = sigma; }

    // ── Compute quotes ────────────────────────────────────────────────────────

    // Returns the bid and ask prices/sizes to post given current state.
    [[nodiscard]] QuoteDecision compute_quotes() const noexcept {
        const double mid  = from_price(mid_price_);
        const double q    = static_cast<double>(inventory_);
        const double T_t  = std::max(0.0, params_.T_seconds - elapsed_);
        const double g    = params_.gamma;
        const double s2   = params_.sigma * params_.sigma;
        const double k    = std::max(params_.kappa, 1e-9);

        // Avellaneda-Stoikov reservation price:
        //   r = mid - q * γ * σ² * (T-t)
        const double reservation = mid - q * g * s2 * T_t;

        // Optimal half-spread:
        //   δ = (γ * σ² * (T-t)) / 2 + (1/γ) * ln(1 + γ/κ)
        const double as_half_spread =
            (g * s2 * T_t) / 2.0 +
            (1.0 / g) * std::log(1.0 + g / k);

        // Adverse selection adjustment from Kyle's λ.
        // Wider spread when λ is large (informed flow detected).
        double adverse_adj = 0.0;
        if (auto lam = kyle_.lambda()) {
            adverse_adj = std::abs(*lam) * params_.lambda_multiplier;
        }

        // Inventory skew: pull the reservation price toward zero inventory.
        // Already captured in reservation price above — this additionally
        // affects the asymmetric spread width.
        const double inventory_skew = q * g * s2 * T_t * 0.5;

        const double raw_bid = reservation - as_half_spread - adverse_adj - inventory_skew;
        const double raw_ask = reservation + as_half_spread + adverse_adj - inventory_skew;

        // Convert to fixed-point, enforce minimum spread.
        Price bid_p = to_price(raw_bid);
        Price ask_p = to_price(raw_ask);

        const Price min_half = params_.min_spread / 2;
        const Price mid_fp   = mid_price_;
        if (ask_p - bid_p < params_.min_spread) {
            bid_p = mid_fp - min_half;
            ask_p = mid_fp + min_half;
        }

        // Hard inventory limits → one-sided quoting.
        const bool too_long  = inventory_ >  params_.max_inventory;
        const bool too_short = inventory_ < -params_.max_inventory;

        QuoteDecision qd;
        qd.bid_price          = bid_p;
        qd.ask_price          = ask_p;
        qd.bid_qty            = default_qty_;
        qd.ask_qty            = default_qty_;
        qd.quote_bid          = !too_long;
        qd.quote_ask          = !too_short;
        qd.reservation_price  = reservation;
        qd.model_spread       = from_price(ask_p - bid_p);
        return qd;
    }

    // ── Accessors ─────────────────────────────────────────────────────────────

    [[nodiscard]] int64_t  inventory()  const noexcept { return inventory_; }
    [[nodiscard]] Price    mid_price()  const noexcept { return mid_price_; }

    [[nodiscard]] std::optional<double> kyle_lambda() const noexcept {
        return kyle_.lambda();
    }
    [[nodiscard]] std::optional<double> roll_spread() const noexcept {
        return roll_.implied_spread();
    }
    [[nodiscard]] double ofi_signal() const noexcept {
        return ofi_.mean_ofi();
    }

    // Returns true if the new quote differs from the posted quote by more
    // than the requote threshold — i.e., we should cancel and re-quote.
    [[nodiscard]] bool should_requote(Price posted_bid, Price posted_ask,
                                     const QuoteDecision& qd) const noexcept {
        return std::abs(qd.bid_price - posted_bid) > params_.requote_threshold ||
               std::abs(qd.ask_price - posted_ask) > params_.requote_threshold;
    }

private:
    MarketMakerParams params_;
    Qty               default_qty_;

    // State.
    Price    mid_price_  = 0;
    double   elapsed_    = 0.0;
    int64_t  inventory_  = 0;

    // Microstructure estimators.
    microstructure::KyleLambda          kyle_;
    microstructure::RollSpread          roll_;
    microstructure::OrderFlowImbalance  ofi_;
};

} // namespace engine
