#pragma once

// MarketDataIngestion — decodes raw market data feed into normalized messages.
//
// Responsibilities:
//   • Parse a simplified binary feed protocol.
//   • Sequence-number gap detection and recovery.
//   • Rate statistics for health monitoring.
//   • Push decoded messages into the matching engine's SPSC inbound queue.
//
// Architecture note:
//   This layer runs on its own thread (or I/O completion thread) and
//   is the *only* producer of the SPSC queue.  Its output is normalized
//   MarketDataMsg structs — the matching engine is completely decoupled
//   from wire format concerns.

#include "core/types.hpp"
#include "core/spsc_queue.hpp"
#include <cstdint>
#include <span>
#include <functional>
#include <atomic>

namespace engine {

// ── Wire format (simplified binary protocol) ──────────────────────────────────

#pragma pack(push, 1)
struct WireHeader {
    uint32_t magic;         // 0xFEED1234
    uint16_t version;       // protocol version
    uint16_t msg_type;
    uint64_t seq_num;
    uint32_t payload_len;
};

struct WireNewOrder {
    uint64_t order_id;
    uint64_t price_fp;      // fixed-point price * 10^6
    uint32_t qty;
    uint16_t symbol_id;
    uint8_t  side;          // 0=buy, 1=sell
    uint8_t  order_type;    // 0=limit, 1=market, 2=ioc, 3=fok
};

struct WireCancelOrder {
    uint64_t order_id;
    uint16_t symbol_id;
    uint8_t  _pad[6];
};

struct WireModifyOrder {
    uint64_t order_id;
    uint32_t new_qty;
    uint16_t symbol_id;
    uint8_t  _pad[2];
};
#pragma pack(pop)

static constexpr uint32_t kMagic = 0xFEED1234;

// ── Ingestion engine ──────────────────────────────────────────────────────────

class MarketDataIngestion {
public:
    static constexpr std::size_t kQueueDepth = 1 << 16;
    using OutboundQueue = SPSCQueue<MarketDataMsg, kQueueDepth>;

    using GapCallback = std::function<void(uint64_t expected, uint64_t got)>;

    explicit MarketDataIngestion(OutboundQueue& outbound)
        : outbound_(outbound) {}

    // Set callback invoked on sequence-number gap.
    void on_gap(GapCallback cb) { gap_cb_ = std::move(cb); }

    // Process a raw UDP datagram (or any byte buffer from the feed).
    // Returns number of messages successfully decoded.
    [[nodiscard]] int ingest(std::span<const uint8_t> data) noexcept;

    // Inject a synthetic message (used for testing and simulation).
    [[nodiscard]] bool inject(const MarketDataMsg& msg) noexcept {
        return outbound_.try_push(msg);
    }

    [[nodiscard]] uint64_t messages_ingested() const noexcept {
        return stat_ingested_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t gaps_detected() const noexcept {
        return stat_gaps_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t parse_errors() const noexcept {
        return stat_errors_.load(std::memory_order_relaxed);
    }

    void reset_sequence() noexcept {
        expected_seq_.store(0, std::memory_order_relaxed);
    }

private:
    [[nodiscard]] bool decode_new_order(const WireHeader& hdr,
                                        std::span<const uint8_t> payload,
                                        MarketDataMsg& out) const noexcept;

    [[nodiscard]] bool decode_cancel(const WireHeader& hdr,
                                     std::span<const uint8_t> payload,
                                     MarketDataMsg& out) const noexcept;

    [[nodiscard]] bool decode_modify(const WireHeader& hdr,
                                     std::span<const uint8_t> payload,
                                     MarketDataMsg& out) const noexcept;

    OutboundQueue& outbound_;
    GapCallback    gap_cb_;

    std::atomic<uint64_t> expected_seq_{0};
    std::atomic<uint64_t> stat_ingested_{0};
    std::atomic<uint64_t> stat_gaps_{0};
    std::atomic<uint64_t> stat_errors_{0};
};

} // namespace engine
