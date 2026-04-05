#pragma once
// itch_parser.hpp — NASDAQ TotalView-ITCH 5.0 wire protocol decoder.
//
// Specification: NASDAQ TotalView-ITCH 5.0 (2014-02-20)
// https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf
//
// Architecture context:
//   ITCH 5.0 is a one-directional (feed only) protocol carried over Multicast
//   UDP or SoupBinTCP.  Each message is length-prefixed; message types are
//   identified by a single byte type field at offset 0 of each message body.
//
//   This parser fits the MarketDataIngestion interface: it accepts a raw UDP
//   datagram (std::span<const uint8_t>), decodes all complete ITCH messages
//   inside it, and calls a message handler for each one.  The matching engine
//   never sees ITCH types — it consumes normalized MarketDataMsg structs.
//
// Key message types decoded:
//   'A'  Add Order (no MPID)
//   'F'  Add Order with MPID
//   'U'  Order Replace
//   'D'  Order Delete
//   'E'  Order Executed
//   'C'  Order Executed with Price
//   'X'  Order Cancel (partial)
//   'S'  System Event
//   'H'  Stock Trading Action (halt / resume)
//   'R'  Stock Directory
//
// Wire encoding: big-endian, no padding, length-prefixed with uint16_t header.
//
// Integration:
//   Replace MarketDataIngestion::ingest() call site with ITCHParser::ingest().
//   Both accept std::span<const uint8_t> and push to the same OutboundQueue.

#include "core/types.hpp"
#include "core/spsc_queue.hpp"
#include <cstdint>
#include <span>
#include <array>
#include <atomic>
#include <cstring>

namespace engine {
namespace itch {

// ── Big-endian read helpers ────────────────────────────────────────────────────
// ITCH 5.0 is big-endian throughout.  We avoid htons/ntohl to stay
// constexpr-friendly and compiler-intrinsic-free for portability.

[[nodiscard]] inline uint16_t read_u16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
[[nodiscard]] inline uint32_t read_u32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
           static_cast<uint32_t>(p[3]);
}
[[nodiscard]] inline uint64_t read_u48(const uint8_t* p) noexcept {
    // 6-byte big-endian integer (used for Timestamp in ITCH).
    return (static_cast<uint64_t>(p[0]) << 40) |
           (static_cast<uint64_t>(p[1]) << 32) |
           (static_cast<uint64_t>(p[2]) << 24) |
           (static_cast<uint64_t>(p[3]) << 16) |
           (static_cast<uint64_t>(p[4]) <<  8) |
           static_cast<uint64_t>(p[5]);
}
[[nodiscard]] inline uint64_t read_u64(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(read_u32(p)) << 32) | read_u32(p + 4);
}

// ── ITCH 5.0 message type bytes ────────────────────────────────────────────────
namespace MsgType {
    static constexpr uint8_t SystemEvent        = 'S';
    static constexpr uint8_t StockDirectory     = 'R';
    static constexpr uint8_t TradingAction      = 'H';
    static constexpr uint8_t RegSHORestriction  = 'Y';
    static constexpr uint8_t MarketParticipant  = 'L';
    static constexpr uint8_t AddOrder           = 'A';
    static constexpr uint8_t AddOrderMPID       = 'F';
    static constexpr uint8_t OrderExecuted      = 'E';
    static constexpr uint8_t OrderExecutedPrice = 'C';
    static constexpr uint8_t OrderCancel        = 'X';
    static constexpr uint8_t OrderDelete        = 'D';
    static constexpr uint8_t OrderReplace       = 'U';
    static constexpr uint8_t Trade              = 'P';
    static constexpr uint8_t CrossTrade         = 'Q';
    static constexpr uint8_t BrokenTrade        = 'B';
    static constexpr uint8_t NOII               = 'I';
}

// ── Decoded ITCH message structs ───────────────────────────────────────────────
// These are decoded-and-normalized forms, not wire structs.  Fixed-point prices
// are converted to our internal Price (int64_t * 10^6) at decode time.

struct AddOrderMsg {
    uint64_t  timestamp_ns;    // nanoseconds since midnight
    uint64_t  order_ref;       // ITCH "Order Reference Number"
    char      buy_sell;        // 'B' or 'S'
    uint32_t  shares;
    char      stock[9];        // 8 chars + null
    Price     price;           // internal fixed-point (10^6)
};

struct OrderExecutedMsg {
    uint64_t  timestamp_ns;
    uint64_t  order_ref;
    uint32_t  executed_shares;
    uint64_t  match_number;
    Price     exec_price;      // only set for 'C' messages; 0 otherwise
    bool      has_exec_price;
};

struct OrderCancelMsg {
    uint64_t  timestamp_ns;
    uint64_t  order_ref;
    uint32_t  cancelled_shares;  // partial cancel; 0 = full delete
};

struct OrderReplaceMsg {
    uint64_t  timestamp_ns;
    uint64_t  orig_order_ref;
    uint64_t  new_order_ref;
    uint32_t  shares;
    Price     price;
};

struct TradingActionMsg {
    uint64_t  timestamp_ns;
    char      stock[9];
    char      trading_state;  // 'H'=halted, 'P'=paused, 'Q'=quotation, 'T'=trading
};

// ── ITCH 5.0 parser ────────────────────────────────────────────────────────────

class ITCHParser {
public:
    static constexpr std::size_t kQueueDepth = 1 << 16;
    using OutboundQueue = SPSCQueue<MarketDataMsg, kQueueDepth>;

    // Symbol map: ITCH uses 8-char ticker strings; we resolve to SymbolId.
    // Up to 256 distinct symbols (adequate for a single-feed testbed).
    static constexpr std::size_t kMaxSymbols = 256;

    explicit ITCHParser(OutboundQueue& outbound) : outbound_(outbound) {}

    // Register a ticker → SymbolId mapping.
    void register_symbol(const char* ticker, SymbolId id) noexcept {
        if (n_symbols_ >= kMaxSymbols) return;
        std::strncpy(symbols_[n_symbols_].ticker, ticker, 8);
        symbols_[n_symbols_].id = id;
        ++n_symbols_;
    }

    // Process one raw SoupBinTCP/MoldUDP64 payload.
    //
    // SoupBinTCP framing: each message is preceded by a uint16_t length.
    // MoldUDP64 has a 20-byte session header + message count header before
    // the same length-prefixed messages.  This parser handles the inner
    // ITCH message stream (after any session-layer header is stripped).
    //
    // Returns number of ITCH messages successfully decoded.
    [[nodiscard]] int ingest(std::span<const uint8_t> data) noexcept;

    // Inject a synthetic MarketDataMsg directly (testing / simulation).
    [[nodiscard]] bool inject(const MarketDataMsg& msg) noexcept {
        return outbound_.try_push(msg);
    }

    [[nodiscard]] uint64_t messages_decoded()  const noexcept { return stat_decoded_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t parse_errors()      const noexcept { return stat_errors_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t messages_dropped()  const noexcept { return stat_dropped_.load(std::memory_order_relaxed); }

private:
    // ── Decode individual message types ─────────────────────────────────────

    [[nodiscard]] bool decode_add_order(const uint8_t* body, std::size_t len,
                                        bool has_mpid, MarketDataMsg& out) noexcept;

    [[nodiscard]] bool decode_order_executed(const uint8_t* body, std::size_t len,
                                             bool has_price, MarketDataMsg& out) noexcept;

    [[nodiscard]] bool decode_order_cancel(const uint8_t* body, std::size_t len,
                                           MarketDataMsg& out) noexcept;

    [[nodiscard]] bool decode_order_delete(const uint8_t* body, std::size_t len,
                                           MarketDataMsg& out) noexcept;

    [[nodiscard]] bool decode_order_replace(const uint8_t* body, std::size_t len,
                                            MarketDataMsg& out) noexcept;

    // ── Symbol resolution ────────────────────────────────────────────────────

    [[nodiscard]] SymbolId resolve_symbol(const char* ticker_8) const noexcept {
        for (std::size_t i = 0; i < n_symbols_; ++i) {
            if (std::strncmp(symbols_[i].ticker, ticker_8, 8) == 0)
                return symbols_[i].id;
        }
        return 0;  // unknown symbol → 0 (caller may filter)
    }

    // ── Price conversion ─────────────────────────────────────────────────────
    // ITCH 5.0 prices are 4-byte big-endian integers with an implied
    // 4-decimal-place fixed point (i.e. $10.5000 = 105000).
    // We convert to our internal 6-decimal-place format (* 100).
    [[nodiscard]] static Price itch_price_to_internal(uint32_t raw) noexcept {
        // ITCH: 4 decimal places → multiply by 100 to get 6 decimal places.
        return static_cast<Price>(raw) * 100LL;
    }

    // ── Order reference tracking ─────────────────────────────────────────────
    // ITCH uses a 64-bit Order Reference Number that correlates Add→Execute/Cancel/Replace.
    // We maintain a compact hash table mapping ITCH ref → (our OrderId, side).
    struct RefEntry {
        uint64_t itch_ref  = 0;
        OrderId  our_id    = 0;
        uint8_t  side      = 0xFF;  // 0=buy, 1=sell; 0xFF=empty
    };

    static constexpr std::size_t kRefTableSize = 1 << 17;  // 128K slots
    static constexpr std::size_t kRefMask      = kRefTableSize - 1;

    std::array<RefEntry, kRefTableSize> ref_table_{};

    void ref_insert(uint64_t itch_ref, OrderId our_id, uint8_t side) noexcept {
        std::size_t pos = (itch_ref * 11400714819323198485ULL) >> (64 - 17);
        while (ref_table_[pos].side != 0xFF) pos = (pos + 1) & kRefMask;
        ref_table_[pos] = { itch_ref, our_id, side };
    }

    bool ref_lookup(uint64_t itch_ref, OrderId& our_id_out, uint8_t& side_out) const noexcept {
        std::size_t pos = (itch_ref * 11400714819323198485ULL) >> (64 - 17);
        for (;;) {
            const auto& e = ref_table_[pos];
            if (e.side == 0xFF) return false;
            if (e.itch_ref == itch_ref) { our_id_out = e.our_id; side_out = e.side; return true; }
            pos = (pos + 1) & kRefMask;
        }
    }

    void ref_delete(uint64_t itch_ref) noexcept {
        std::size_t pos = (itch_ref * 11400714819323198485ULL) >> (64 - 17);
        for (;;) {
            auto& e = ref_table_[pos];
            if (e.side == 0xFF) return;
            if (e.itch_ref == itch_ref) { e.side = 0xFF; return; }
            pos = (pos + 1) & kRefMask;
        }
    }

    // ── State ────────────────────────────────────────────────────────────────

    OutboundQueue& outbound_;

    struct SymbolEntry { char ticker[9]; SymbolId id; };
    std::array<SymbolEntry, kMaxSymbols> symbols_{};
    std::size_t n_symbols_ = 0;

    // Monotonically increasing synthetic order IDs for messages we emit.
    uint64_t next_order_id_ = 1;

    std::atomic<uint64_t> stat_decoded_{0};
    std::atomic<uint64_t> stat_errors_{0};
    std::atomic<uint64_t> stat_dropped_{0};
};

} // namespace itch
} // namespace engine
