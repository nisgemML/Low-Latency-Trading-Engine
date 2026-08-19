// test_itch_parser.cpp — Unit tests for the ITCH 5.0 wire protocol decoder.
//
// Strategy: hand-craft binary wire buffers that exactly match the ITCH 5.0
// specification and verify the parser produces the correct MarketDataMsg.
//
// Why hand-crafted buffers instead of canned pcaps?
//   Pcaps hide the byte layout behind tooling.  Hand-crafted buffers prove
//   the developer understands the wire format.  Each test documents the
//   offset-by-offset layout as a comment, making it a living spec reference.
//
// Message types covered:
//   'A'  Add Order (no MPID)  — the most common feed message
//   'D'  Order Delete (full cancel)
//   'U'  Order Replace (atomic cancel + new order)
//
// Wire encoding reminder (big-endian throughout):
//   The ITCHParser::ingest() path expects SoupBinTCP framing:
//     [0..1] uint16_t message_length  (counts type byte + body, not itself)
//     [2]    uint8_t  message_type
//     [3..]  message body
//
// The parsed Add Order body layout ('A' without MPID, total body = 34):
//   [0..1]   Stock Locate      (ignored)
//   [2..3]   Tracking Number   (ignored)
//   [4..9]   Timestamp ns      (6-byte big-endian uint48)
//   [10..17] Order Reference   (uint64 big-endian)
//   [18]     Buy/Sell          ('B' or 'S')
//   [19..22] Shares            (uint32 big-endian)
//   [23..30] Stock ticker      (8 chars, space-padded, not null-terminated)
//   [31..34] Price             (uint32 big-endian, in ITCH units: 10^-4 dollar)
//
// The Order Delete body layout ('D', total body = 17):
//   [0..1]   Stock Locate
//   [2..3]   Tracking Number
//   [4..9]   Timestamp ns
//   [10..17] Order Reference   (uint64 big-endian)
//
// The Order Replace body layout ('U', total body = 33):
//   [0..1]   Stock Locate
//   [2..3]   Tracking Number
//   [4..9]   Timestamp ns
//   [10..17] Original Order Reference
//   [18..25] New Order Reference
//   [26..29] Shares
//   [30..33] New Price

#include "core/itch_parser.hpp"
#include "core/market_data.hpp"
#include "core/spsc_queue.hpp"
#include "core/types.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <cstdint>
#include <vector>
#include <cstring>
#include <array>

namespace engine::itch::test {

// ── Wire-building helpers ──────────────────────────────────────────────────────

// Write a big-endian uint16 at dst.
static void w16(uint8_t* dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v >> 8);
    dst[1] = static_cast<uint8_t>(v);
}
// Write a big-endian uint32 at dst.
static void w32(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v >> 24);
    dst[1] = static_cast<uint8_t>(v >> 16);
    dst[2] = static_cast<uint8_t>(v >>  8);
    dst[3] = static_cast<uint8_t>(v);
}
// Write a big-endian uint48 (6 bytes) at dst.
static void w48(uint8_t* dst, uint64_t v) {
    dst[0] = static_cast<uint8_t>(v >> 40);
    dst[1] = static_cast<uint8_t>(v >> 32);
    dst[2] = static_cast<uint8_t>(v >> 24);
    dst[3] = static_cast<uint8_t>(v >> 16);
    dst[4] = static_cast<uint8_t>(v >>  8);
    dst[5] = static_cast<uint8_t>(v);
}
// Write a big-endian uint64 at dst.
static void w64(uint8_t* dst, uint64_t v) {
    w32(dst,     static_cast<uint32_t>(v >> 32));
    w32(dst + 4, static_cast<uint32_t>(v));
}

// Build a SoupBinTCP-framed buffer: [uint16 len][type][body...]
// len = 1 (type byte) + body.size()
static std::vector<uint8_t> frame(uint8_t msg_type, const std::vector<uint8_t>& body) {
    const uint16_t len = static_cast<uint16_t>(1 + body.size());
    std::vector<uint8_t> buf(2 + 1 + body.size());
    w16(buf.data(), len);
    buf[2] = msg_type;
    std::memcpy(buf.data() + 3, body.data(), body.size());
    return buf;
}

// ── Test fixture ───────────────────────────────────────────────────────────────

class ITCHParserTest : public ::testing::Test {
protected:
    using Queue = SPSCQueue<MarketDataMsg, 65536>;

    Queue                       queue_;
    std::unique_ptr<ITCHParser> parser_;

    void SetUp() override {
        parser_ = std::make_unique<ITCHParser>(queue_);
    }

    // Ingest a framed buffer and return all MarketDataMsgs emitted.
    std::vector<MarketDataMsg> ingest(const std::vector<uint8_t>& buf) {
        const int n = parser_->ingest(std::span<const uint8_t>(buf));
        (void)n;
        std::vector<MarketDataMsg> out;
        MarketDataMsg msg;
        while (queue_.try_pop(msg)) out.push_back(msg);
        return out;
    }

    // Build a valid 'A' (Add Order, no MPID) body.
    //   order_ref: ITCH reference number
    //   buy_sell : 'B' or 'S'
    //   shares   : uint32
    //   ticker   : up to 8 chars (space-padded to 8)
    //   itch_price: in ITCH units (10^-4 dollar, e.g. 150.0000 = 1500000)
    static std::vector<uint8_t> make_add_order_body(
        uint64_t order_ref, char buy_sell, uint32_t shares,
        const char* ticker, uint32_t itch_price)
    {
        std::vector<uint8_t> body(35, 0);
        // [0..1] Stock Locate = 1
        w16(body.data() + 0, 1);
        // [2..3] Tracking Number = 0
        w16(body.data() + 2, 0);
        // [4..9] Timestamp = 0x000012345678 ns
        w48(body.data() + 4, 0x12345678ULL);
        // [10..17] Order Reference
        w64(body.data() + 10, order_ref);
        // [18] Buy/Sell
        body[18] = static_cast<uint8_t>(buy_sell);
        // [19..22] Shares
        w32(body.data() + 19, shares);
        // [23..30] Ticker (8 bytes, space-padded)
        std::memset(body.data() + 23, ' ', 8);
        const std::size_t tlen = std::min(std::strlen(ticker), std::size_t{8});
        std::memcpy(body.data() + 23, ticker, tlen);
        // [31..34] Price
        w32(body.data() + 31, itch_price);
        return body;
    }

    // Build a valid 'D' (Order Delete) body.
    static std::vector<uint8_t> make_delete_body(uint64_t order_ref) {
        std::vector<uint8_t> body(18, 0);
        w16(body.data() + 0, 1);   // Stock Locate
        w16(body.data() + 2, 0);   // Tracking Number
        w48(body.data() + 4, 0x12345679ULL);  // Timestamp
        w64(body.data() + 10, order_ref);
        return body;
    }

    // Build a valid 'U' (Order Replace) body.
    static std::vector<uint8_t> make_replace_body(
        uint64_t orig_ref, uint64_t new_ref, uint32_t shares, uint32_t itch_price)
    {
        std::vector<uint8_t> body(34, 0);
        w16(body.data() + 0, 1);   // Stock Locate
        w16(body.data() + 2, 0);   // Tracking Number
        w48(body.data() + 4, 0x1234567AULL);  // Timestamp
        w64(body.data() + 10, orig_ref);
        w64(body.data() + 18, new_ref);
        w32(body.data() + 26, shares);
        w32(body.data() + 30, itch_price);
        return body;
    }
};

// ── Test: Add Order ('A') — buy side ──────────────────────────────────────────
//
// Wire: Add Order, ref=1001, buy, 500 shares of "AAPL    " at $150.0000.
// ITCH price encoding: $150.0000 = 1_500_000 (units of $0.0001).
// Internal price:      $150.0000 = 150_000_000 (units of $0.000001).
TEST_F(ITCHParserTest, AddOrder_Buy_ParsesCorrectly) {
    // ITCH price 1_500_000 = $150.0000.
    // Internal conversion: itch_price * 100 → 150_000_000.
    const uint32_t itch_price = 1'500'000;
    const uint64_t order_ref  = 1001ULL;

    auto body = make_add_order_body(order_ref, 'B', 500, "AAPL", itch_price);
    auto msgs = ingest(frame('A', body));

    ASSERT_EQ(msgs.size(), 1u) << "Expected exactly one MarketDataMsg for 'A'";
    const auto& m = msgs[0];

    EXPECT_EQ(m.msg_type, MarketDataMsg::Type::NewOrder);
    EXPECT_EQ(m.side,     Side::Buy);
    EXPECT_EQ(m.qty,      500u);

    // Price: ITCH 1_500_000 (10^-4 $) → internal 150_000_000 (10^-6 $)
    EXPECT_EQ(m.price, Price(150000000))
        << "Price mismatch: expected 150000000 got " << m.price;
}

// ── Test: Add Order ('A') — sell side ─────────────────────────────────────────
TEST_F(ITCHParserTest, AddOrder_Sell_ParsesCorrectly) {
    const uint64_t order_ref = 2002ULL;
    auto body = make_add_order_body(order_ref, 'S', 300, "MSFT", 2000000u);
    auto msgs = ingest(frame('A', body));

    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].side, Side::Sell);
    EXPECT_EQ(msgs[0].qty,  300u);
    EXPECT_EQ(msgs[0].price, Price(200000000));
}

// ── Test: Order Delete ('D') after Add ────────────────────────────────────────
//
// We first add an order (to populate the parser's ref-table), then delete it.
// The delete should emit a CancelOrder with qty=0 (full cancel convention).
TEST_F(ITCHParserTest, OrderDelete_AfterAdd_EmitsCancelOrder) {
    const uint64_t order_ref = 3003ULL;

    // 1. Add the order so its ref is known.
    auto add_body = make_add_order_body(order_ref, 'B', 100, "GOOG", 1750000u);
    auto add_msgs = ingest(frame('A', add_body));
    ASSERT_EQ(add_msgs.size(), 1u) << "Add should produce one message";

    // 2. Delete the same ref.
    auto del_body = make_delete_body(order_ref);
    auto del_msgs = ingest(frame('D', del_body));

    ASSERT_EQ(del_msgs.size(), 1u) << "Delete should produce one message";
    const auto& d = del_msgs[0];

    EXPECT_EQ(d.msg_type, MarketDataMsg::Type::CancelOrder);
    EXPECT_EQ(d.side,     Side::Buy);
    EXPECT_EQ(d.qty,      0u) << "qty=0 signals full cancel";
}

// ── Test: Order Delete on unknown ref is silently dropped ─────────────────────
//
// The parser may receive deletes for orders it never saw (e.g. gaps at startup).
// It should silently drop them rather than corrupt state or crash.
TEST_F(ITCHParserTest, OrderDelete_UnknownRef_Dropped) {
    const uint64_t unknown_ref = 9999999ULL;
    auto del_body = make_delete_body(unknown_ref);
    auto msgs     = ingest(frame('D', del_body));
    EXPECT_EQ(msgs.size(), 0u) << "Unknown ref delete should produce no message";
}

// ── Test: Order Replace ('U') ─────────────────────────────────────────────────
//
// Replace order ref=4004 with new_ref=4005, changing qty 200→150 and
// price $175 → $176.  The parser should emit a ModifyOrder.
TEST_F(ITCHParserTest, OrderReplace_EmitsModifyOrder) {
    const uint64_t orig_ref = 4004ULL;
    const uint64_t new_ref  = 4005ULL;

    // Add the original order.
    auto add_body = make_add_order_body(orig_ref, 'S', 200, "AMZN", 1750000u);
    ingest(frame('A', add_body));

    // Replace it.
    auto rep_body = make_replace_body(orig_ref, new_ref, 150, 1760000u);
    auto msgs     = ingest(frame('U', rep_body));

    ASSERT_EQ(msgs.size(), 1u);
    const auto& r = msgs[0];

    EXPECT_EQ(r.msg_type, MarketDataMsg::Type::ModifyOrder);
    EXPECT_EQ(r.qty,      150u);
    EXPECT_EQ(r.price,    Price(176000000));
    // Side is inherited from the original order's ref-table entry.
    EXPECT_EQ(r.side,     Side::Sell);
}

// ── Test: Multiple messages in a single datagram ──────────────────────────────
//
// ITCHParser::ingest() must process all complete messages in one span.
// Concatenate an Add ('A') + Delete ('D') in a single buffer and verify
// both are decoded in order.
TEST_F(ITCHParserTest, MultipleMsgsInOneDatagram) {
    const uint64_t ref1 = 5001ULL;
    const uint64_t ref2 = 5002ULL;

    auto add1 = frame('A', make_add_order_body(ref1, 'B', 400, "TSLA", 2500000u));
    auto add2 = frame('A', make_add_order_body(ref2, 'S', 600, "NVDA", 8000000u));

    // Concatenate both frames into one datagram.
    std::vector<uint8_t> datagram;
    datagram.insert(datagram.end(), add1.begin(), add1.end());
    datagram.insert(datagram.end(), add2.begin(), add2.end());

    auto msgs = ingest(datagram);
    ASSERT_EQ(msgs.size(), 2u) << "Two adds in one datagram should emit two messages";

    EXPECT_EQ(msgs[0].msg_type, MarketDataMsg::Type::NewOrder);
    EXPECT_EQ(msgs[0].side,     Side::Buy);
    EXPECT_EQ(msgs[0].qty,      400u);
    EXPECT_EQ(msgs[0].price,    Price(250000000));

    EXPECT_EQ(msgs[1].msg_type, MarketDataMsg::Type::NewOrder);
    EXPECT_EQ(msgs[1].side,     Side::Sell);
    EXPECT_EQ(msgs[1].qty,      600u);
    EXPECT_EQ(msgs[1].price,    Price(800000000));
}

// ── Test: Truncated message is silently dropped (robustness) ──────────────────
//
// The 'A' body is 34 bytes; sending only 10 bytes should produce no output.
TEST_F(ITCHParserTest, TruncatedMessage_IsDropped) {
    std::vector<uint8_t> short_body(10, 0);
    short_body[18] = 'B';  // set buy/sell so it looks plausible
    auto msgs = ingest(frame('A', short_body));
    EXPECT_EQ(msgs.size(), 0u) << "Truncated 'A' body should produce no message";
}

} // namespace engine::itch::test
