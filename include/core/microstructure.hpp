#pragma once
// microstructure.hpp — Market microstructure analytics.
//
// Jane Street context:
//   Engineers at JS are expected to understand not just how to build a fast
//   order book, but why the market structure behaves as it does.  This header
//   implements three foundational microstructure models used in every HFT shop:
//
//   1. Kyle's Lambda (price impact coefficient):
//      From Kyle (1985): price moves λ per unit of signed order flow.
//      Used to estimate how much your order moves the market.
//
//   2. Roll's Implied Spread:
//      From Roll (1984): infers the effective bid-ask spread from the
//      serial covariance of price changes.  cov(Δp_t, Δp_{t-1}) = -(s/2)²
//      where s is the effective spread.
//
//   3. Order Flow Imbalance (OFI):
//      A contemporaneous predictor of short-term price direction.
//      OFI = (bid_qty_change - ask_qty_change) at the best quotes.
//      Empirically, a 1-unit OFI predicts a ~λ_OFI price move over ~10ms.
//
//   These are not just academic exercises — they are the inputs to market-
//   making spread-setting, position sizing, and adverse selection filters.

#include "core/types.hpp"
#include "core/order_book_view.hpp"
#include <array>
#include <optional>
#include <cmath>
#include <cstdint>
#include <numeric>

namespace engine {
namespace microstructure {

// ── 1. Kyle's Lambda estimator ────────────────────────────────────────────────
//
// Estimate λ = ΔP / ΔQ from a rolling window of (signed_volume, price_change)
// pairs.  Signed volume: positive = buy-side, negative = sell-side.
//
// In practice: λ ≈ Cov(ΔP, Q) / Var(Q) (OLS regression slope).
// We use a running Welford-style estimator to avoid storing the full window.

class KyleLambda {
    static constexpr int kWindow = 1000;

    struct Sample { double signed_qty; double price_change; };
    std::array<Sample, kWindow> window_{};
    int head_    = 0;
    int filled_  = 0;

    // Running sums for OLS (avoid re-computing over window each time).
    double sum_q_  = 0.0, sum_p_  = 0.0;
    double sum_qq_ = 0.0, sum_qp_ = 0.0;

public:
    // Record a trade: signed_qty > 0 means buyer-initiated.
    void record(double signed_qty, double price_before, double price_after) noexcept {
        const double dp = price_after - price_before;

        // Evict oldest sample.
        if (filled_ == kWindow) {
            const auto& old = window_[head_];
            sum_q_  -= old.signed_qty;
            sum_p_  -= old.price_change;
            sum_qq_ -= old.signed_qty * old.signed_qty;
            sum_qp_ -= old.signed_qty * old.price_change;
        } else {
            ++filled_;
        }

        window_[head_] = { signed_qty, dp };
        head_ = (head_ + 1) % kWindow;

        sum_q_  += signed_qty;
        sum_p_  += dp;
        sum_qq_ += signed_qty * signed_qty;
        sum_qp_ += signed_qty * dp;
    }

    // Returns Kyle's λ (price impact per unit volume), or nullopt if
    // insufficient data or zero variance.
    [[nodiscard]] std::optional<double> lambda() const noexcept {
        if (filled_ < 10) return std::nullopt;
        const double n = static_cast<double>(filled_);
        const double var_q = sum_qq_ / n - (sum_q_ / n) * (sum_q_ / n);
        if (std::abs(var_q) < 1e-12) return std::nullopt;
        const double cov_qp = sum_qp_ / n - (sum_q_ / n) * (sum_p_ / n);
        return cov_qp / var_q;
    }

    // Expected price impact of a trade of size `qty` (signed).
    [[nodiscard]] std::optional<double> impact(double signed_qty) const noexcept {
        auto l = lambda();
        if (!l) return std::nullopt;
        return *l * signed_qty;
    }
};

// ── 2. Roll's Implied Spread ──────────────────────────────────────────────────
//
// From consecutive trade prices, estimate the effective spread via:
//   s = 2 * sqrt(-cov(Δp_t, Δp_{t-1}))
//
// Negative serial covariance in trade prices implies a bid-ask bounce:
// price alternates between bid and ask, creating negative autocorrelation.
// The magnitude reveals how large that bounce is.

class RollSpread {
    static constexpr int kWindow = 500;

    double prev_dp_    = 0.0;
    double sum_cov_    = 0.0;   // Σ Δp_t * Δp_{t-1}
    double prev_price_ = 0.0;
    int    n_          = 0;
    bool   initialized_ = false;

public:
    void record_trade(double price) noexcept {
        if (!initialized_) { prev_price_ = price; initialized_ = true; return; }
        const double dp = price - prev_price_;
        sum_cov_ += prev_dp_ * dp;
        prev_dp_    = dp;
        prev_price_ = price;
        if (n_ < kWindow) ++n_;
    }

    // Returns implied effective spread (in price units), or nullopt if
    // covariance is non-negative (no bounce pattern detected).
    [[nodiscard]] std::optional<double> implied_spread() const noexcept {
        if (n_ < 10) return std::nullopt;
        const double cov = sum_cov_ / n_;
        if (cov >= 0.0) return std::nullopt;  // no bounce — can't estimate
        return 2.0 * std::sqrt(-cov);
    }
};

// ── 3. Order Flow Imbalance ───────────────────────────────────────────────────
//
// OFI_t = ΔBid_qty_t * 1(bid_price unchanged) - ΔAsk_qty_t * 1(ask_price unchanged)
//
// At each tick, OFI captures how much more buying vs selling pressure
// accumulated at the best quotes, after controlling for price moves.
// A large positive OFI predicts upward short-term price pressure.
//
// Reference: Cont, Kukanov, Stoikov (2014), "The Price Impact of Order Book Events"

class OrderFlowImbalance {
    static constexpr int kWindow = 200;

    struct Snapshot {
        Price bid_price = 0; Price ask_price = 0;
        Qty   bid_qty   = 0; Qty   ask_qty   = 0;
    };

    Snapshot prev_{};
    bool     has_prev_ = false;

    double   sum_ofi_  = 0.0;
    int      n_        = 0;

public:
    // Record a new top-of-book snapshot.  Returns OFI for this tick.
    double record(Price bid_p, Qty bid_q, Price ask_p, Qty ask_q) noexcept {
        if (!has_prev_) {
            prev_ = { bid_p, ask_p, bid_q, ask_q };
            has_prev_ = true;
            return 0.0;
        }

        // Bid contribution.
        double bid_ofi = 0.0;
        if (bid_p > prev_.bid_price)
            bid_ofi = static_cast<double>(bid_q);
        else if (bid_p == prev_.bid_price)
            bid_ofi = static_cast<double>(bid_q) - static_cast<double>(prev_.bid_qty);
        else
            bid_ofi = -static_cast<double>(prev_.bid_qty);

        // Ask contribution (inverted: more ask = negative pressure).
        double ask_ofi = 0.0;
        if (ask_p < prev_.ask_price)
            ask_ofi = static_cast<double>(ask_q);
        else if (ask_p == prev_.ask_price)
            ask_ofi = static_cast<double>(ask_q) - static_cast<double>(prev_.ask_qty);
        else
            ask_ofi = -static_cast<double>(prev_.ask_qty);

        const double ofi = bid_ofi - ask_ofi;
        sum_ofi_ += ofi;
        if (n_ < kWindow) ++n_;
        prev_ = { bid_p, ask_p, bid_q, ask_q };
        return ofi;
    }

    // Running mean OFI — positive = net buying pressure.
    [[nodiscard]] double mean_ofi() const noexcept {
        return n_ > 0 ? sum_ofi_ / n_ : 0.0;
    }

    // Directional signal: +1 (buy pressure), -1 (sell pressure), 0 (neutral).
    [[nodiscard]] int signal(double threshold = 0.0) const noexcept {
        const double m = mean_ofi();
        if (m > threshold)  return  1;
        if (m < -threshold) return -1;
        return 0;
    }
};

} // namespace microstructure
} // namespace engine
