#include "core/market_data.hpp"
#include <cstring>

namespace engine {

static constexpr uint16_t kMsgNewOrder    = 1;
static constexpr uint16_t kMsgCancelOrder = 2;
static constexpr uint16_t kMsgModify      = 3;
static constexpr uint16_t kMsgHeartbeat   = 99;

int MarketDataIngestion::ingest(std::span<const uint8_t> data) noexcept {
    if (data.size() < sizeof(WireHeader)) {
        stat_errors_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    WireHeader hdr;
    std::memcpy(&hdr, data.data(), sizeof(hdr));

    if (hdr.magic != kMagic) {
        stat_errors_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    // Sequence number gap detection.
    uint64_t expected = expected_seq_.load(std::memory_order_relaxed);
    if (expected != 0 && hdr.seq_num != expected) {
        stat_gaps_.fetch_add(1, std::memory_order_relaxed);
        if (gap_cb_) gap_cb_(expected, hdr.seq_num);
    }
    expected_seq_.store(hdr.seq_num + 1, std::memory_order_relaxed);

    auto payload = data.subspan(sizeof(WireHeader), hdr.payload_len);
    if (payload.size() < hdr.payload_len) {
        stat_errors_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    MarketDataMsg msg{};
    msg.seq = hdr.seq_num;
    bool ok = false;

    switch (hdr.msg_type) {
        case kMsgNewOrder:
            ok = decode_new_order(hdr, payload, msg);
            break;
        case kMsgCancelOrder:
            ok = decode_cancel(hdr, payload, msg);
            break;
        case kMsgModify:
            ok = decode_modify(hdr, payload, msg);
            break;
        case kMsgHeartbeat:
            msg.msg_type = MarketDataMsg::Type::Heartbeat;
            ok = true;
            break;
        default:
            stat_errors_.fetch_add(1, std::memory_order_relaxed);
            return 0;
    }

    if (!ok) {
        stat_errors_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    if (!outbound_.try_push(msg)) {
        // Queue full — in production, trigger back-pressure or alert.
        return 0;
    }

    stat_ingested_.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

bool MarketDataIngestion::decode_new_order(const WireHeader&,
                                            std::span<const uint8_t> payload,
                                            MarketDataMsg& out) const noexcept
{
    if (payload.size() < sizeof(WireNewOrder)) return false;
    WireNewOrder w;
    std::memcpy(&w, payload.data(), sizeof(w));

    out.msg_type   = MarketDataMsg::Type::NewOrder;
    out.order_id   = w.order_id;
    out.price      = static_cast<Price>(w.price_fp);
    out.qty        = w.qty;
    out.symbol     = w.symbol_id;
    out.side       = (w.side == 0) ? Side::Buy : Side::Sell;
    out.order_type = static_cast<OrderType>(w.order_type);
    return true;
}

bool MarketDataIngestion::decode_cancel(const WireHeader&,
                                         std::span<const uint8_t> payload,
                                         MarketDataMsg& out) const noexcept
{
    if (payload.size() < sizeof(WireCancelOrder)) return false;
    WireCancelOrder w;
    std::memcpy(&w, payload.data(), sizeof(w));

    out.msg_type = MarketDataMsg::Type::CancelOrder;
    out.order_id = w.order_id;
    out.symbol   = w.symbol_id;
    return true;
}

bool MarketDataIngestion::decode_modify(const WireHeader&,
                                         std::span<const uint8_t> payload,
                                         MarketDataMsg& out) const noexcept
{
    if (payload.size() < sizeof(WireModifyOrder)) return false;
    WireModifyOrder w;
    std::memcpy(&w, payload.data(), sizeof(w));

    out.msg_type = MarketDataMsg::Type::ModifyOrder;
    out.order_id = w.order_id;
    out.qty      = w.new_qty;
    out.symbol   = w.symbol_id;
    return true;
}

} // namespace engine
