#include "core/itch_parser.hpp"
#include <cstring>

namespace engine {
namespace itch {

// ── Message length table (spec §4) ────────────────────────────────────────────
// Each ITCH 5.0 message has a fixed length (not counting the 2-byte length prefix).
// We use this to validate before decoding.

// Message length lookup table (ITCH 5.0 spec §4).
// Indexed by message-type byte.  Non-zero = expected wire length (excluding
// the 2-byte SoupBinTCP length prefix).  Zero = unknown/ignored type.
// We build this at static init time to avoid C++ designated-init limitations.
static const uint16_t* build_msg_len_table() {
    static uint16_t t[256] = {};
    t['S'] = 11;   // System Event
    t['R'] = 38;   // Stock Directory
    t['H'] = 24;   // Stock Trading Action
    t['Y'] = 19;   // Reg SHO Short Sale Price Test Restriction
    t['L'] = 25;   // Market Participant Position
    t['V'] = 34;   // MWCB Decline Level
    t['W'] = 11;   // MWCB Status
    t['K'] = 27;   // IPO Quoting Period Update
    t['J'] = 34;   // LULD Auction Collar
    t['A'] = 35;   // Add Order (no MPID)
    t['F'] = 39;   // Add Order with MPID
    t['E'] = 30;   // Order Executed
    t['C'] = 35;   // Order Executed with Price
    t['X'] = 22;   // Order Cancel (partial)
    t['D'] = 18;   // Order Delete
    t['U'] = 34;   // Order Replace
    t['P'] = 43;   // Trade (non-cross)
    t['Q'] = 39;   // Cross Trade
    t['B'] = 18;   // Broken Trade
    t['I'] = 49;   // Net Order Imbalance Indicator
    t['N'] = 19;   // Retail Price Improvement Indicator
    return t;
}
static const uint16_t* kMsgLen = build_msg_len_table();

// ── ingest() ──────────────────────────────────────────────────────────────────
//
// Parses a buffer of length-prefixed ITCH 5.0 messages.
// Wire layout for each message:
//   [uint16_t length][uint8_t msg_type][<length-1 bytes of payload>]

int ITCHParser::ingest(std::span<const uint8_t> data) noexcept {
    int decoded = 0;
    const uint8_t* p   = data.data();
    const uint8_t* end = p + data.size();

    while (p + 2 <= end) {
        const uint16_t msg_len = read_u16(p);
        p += 2;

        if (p + msg_len > end) {
            // Truncated message — partial datagram.
            stat_errors_.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        const uint8_t  msg_type = p[0];
        const uint8_t* body     = p + 1;          // skip type byte
        const std::size_t body_len = msg_len - 1;

        // Validate length against spec table.
        const uint16_t expected = kMsgLen[msg_type];
        if (expected != 0 && msg_len != expected) {
            stat_errors_.fetch_add(1, std::memory_order_relaxed);
            p += msg_len;
            continue;
        }

        MarketDataMsg msg{};
        bool ok = false;

        switch (msg_type) {
        case MsgType::AddOrder:
            ok = decode_add_order(body, body_len, /*has_mpid=*/false, msg);
            break;
        case MsgType::AddOrderMPID:
            ok = decode_add_order(body, body_len, /*has_mpid=*/true, msg);
            break;
        case MsgType::OrderExecuted:
            ok = decode_order_executed(body, body_len, /*has_price=*/false, msg);
            break;
        case MsgType::OrderExecutedPrice:
            ok = decode_order_executed(body, body_len, /*has_price=*/true, msg);
            break;
        case MsgType::OrderCancel:
            ok = decode_order_cancel(body, body_len, msg);
            break;
        case MsgType::OrderDelete:
            ok = decode_order_delete(body, body_len, msg);
            break;
        case MsgType::OrderReplace:
            ok = decode_order_replace(body, body_len, msg);
            break;
        default:
            // Heartbeat / system / directory messages: silently skip.
            p += msg_len;
            continue;
        }

        if (ok) {
            if (!outbound_.try_push(msg))
                stat_dropped_.fetch_add(1, std::memory_order_relaxed);
            else
                stat_decoded_.fetch_add(1, std::memory_order_relaxed);
            ++decoded;
        } else {
            stat_errors_.fetch_add(1, std::memory_order_relaxed);
        }

        p += msg_len;
    }

    return decoded;
}

// ── Add Order ('A' / 'F') ─────────────────────────────────────────────────────
//
// 'A' wire layout (34 bytes after type byte):
//   [2]  Stock Locate
//   [2]  Tracking Number
//   [6]  Timestamp (nanoseconds since midnight, 48-bit big-endian)
//   [8]  Order Reference Number
//   [1]  Buy/Sell Indicator  ('B' or 'S')
//   [4]  Shares
//   [8]  Stock (right-padded with spaces)
//   [4]  Price  (4 decimal implied)
// 'F' appends [4] MPID (ignored here).

bool ITCHParser::decode_add_order(const uint8_t* body, std::size_t len,
                                  bool has_mpid, MarketDataMsg& out) noexcept
{
    const std::size_t min_len = has_mpid ? 38 : 34;
    if (len < min_len) return false;

    // body[0..1]  = Stock Locate
    // body[2..3]  = Tracking Number
    const uint64_t ts_ns      = read_u48(body + 4);
    const uint64_t order_ref  = read_u64(body + 10);
    const uint8_t  buysell    = body[18];
    const uint32_t shares     = read_u32(body + 19);
    const char*    stock_ptr  = reinterpret_cast<const char*>(body + 23);
    const uint32_t raw_price  = read_u32(body + 31);

    (void)ts_ns;  // timestamp available if caller needs it

    const Price price = itch_price_to_internal(raw_price);
    const SymbolId sym = resolve_symbol(stock_ptr);
    const uint8_t side = (buysell == 'B') ? 0 : 1;

    // Allocate a synthetic OrderId and remember the ITCH ref.
    const OrderId our_id = next_order_id_++;
    ref_insert(order_ref, our_id, side);

    out.msg_type   = MarketDataMsg::Type::NewOrder;
    out.seq        = 0;           // ITCH doesn't carry a per-message seqnum here
    out.order_id   = our_id;
    out.price      = price;
    out.qty        = shares;
    out.symbol     = sym;
    out.side       = (side == 0) ? Side::Buy : Side::Sell;
    out.order_type = OrderType::Limit;
    return true;
}

// ── Order Executed ('E' / 'C') ────────────────────────────────────────────────
//
// 'E' wire layout (29 bytes after type byte):
//   [2]  Stock Locate
//   [2]  Tracking Number
//   [6]  Timestamp
//   [8]  Order Reference Number
//   [4]  Executed Shares
//   [8]  Match Number
// 'C' appends:
//   [1]  Printable ('Y'/'N')
//   [4]  Execution Price

bool ITCHParser::decode_order_executed(const uint8_t* body, std::size_t len,
                                       bool has_price, MarketDataMsg& out) noexcept
{
    const std::size_t min_len = has_price ? 34 : 29;
    if (len < min_len) return false;

    const uint64_t order_ref     = read_u64(body + 10);
    const uint32_t exec_shares   = read_u32(body + 18);

    OrderId our_id; uint8_t side;
    if (!ref_lookup(order_ref, our_id, side)) return false;

    Price exec_price = 0;
    if (has_price) {
        exec_price = itch_price_to_internal(read_u32(body + 31));
    }
    (void)exec_price;  // available; currently we emit a CancelOrder for partial fills

    // Model execution as a cancel of the executed portion.
    out.msg_type = MarketDataMsg::Type::CancelOrder;
    out.order_id = our_id;
    out.qty      = exec_shares;
    out.side     = (side == 0) ? Side::Buy : Side::Sell;
    return true;
}

// ── Order Cancel ('X') ────────────────────────────────────────────────────────
//
// Partial cancel.  Wire layout (21 bytes after type byte):
//   [2]  Stock Locate
//   [2]  Tracking Number
//   [6]  Timestamp
//   [8]  Order Reference Number
//   [4]  Cancelled Shares

bool ITCHParser::decode_order_cancel(const uint8_t* body, std::size_t len,
                                     MarketDataMsg& out) noexcept
{
    if (len < 21) return false;

    const uint64_t order_ref       = read_u64(body + 10);
    const uint32_t cancelled_shares = read_u32(body + 18);

    OrderId our_id; uint8_t side;
    if (!ref_lookup(order_ref, our_id, side)) return false;

    out.msg_type = MarketDataMsg::Type::CancelOrder;
    out.order_id = our_id;
    out.qty      = cancelled_shares;
    out.side     = (side == 0) ? Side::Buy : Side::Sell;
    return true;
}

// ── Order Delete ('D') ────────────────────────────────────────────────────────
//
// Full cancel.  Wire layout (17 bytes after type byte):
//   [2]  Stock Locate
//   [2]  Tracking Number
//   [6]  Timestamp
//   [8]  Order Reference Number

bool ITCHParser::decode_order_delete(const uint8_t* body, std::size_t len,
                                     MarketDataMsg& out) noexcept
{
    if (len < 17) return false;

    const uint64_t order_ref = read_u64(body + 10);

    OrderId our_id; uint8_t side;
    if (!ref_lookup(order_ref, our_id, side)) return false;
    ref_delete(order_ref);

    out.msg_type = MarketDataMsg::Type::CancelOrder;
    out.order_id = our_id;
    out.qty      = 0;  // qty=0 → full cancel in our convention
    out.side     = (side == 0) ? Side::Buy : Side::Sell;
    return true;
}

// ── Order Replace ('U') ───────────────────────────────────────────────────────
//
// Atomically replaces an order with a new one (new ref, qty, price).
// Wire layout (33 bytes after type byte):
//   [2]  Stock Locate
//   [2]  Tracking Number
//   [6]  Timestamp
//   [8]  Original Order Reference
//   [8]  New Order Reference
//   [4]  Shares
//   [4]  Price

bool ITCHParser::decode_order_replace(const uint8_t* body, std::size_t len,
                                      MarketDataMsg& out) noexcept
{
    if (len < 33) return false;

    const uint64_t orig_ref = read_u64(body + 10);
    const uint64_t new_ref  = read_u64(body + 18);
    const uint32_t shares   = read_u32(body + 26);
    const Price    price    = itch_price_to_internal(read_u32(body + 30));

    OrderId our_id; uint8_t side;
    if (!ref_lookup(orig_ref, our_id, side)) return false;

    // Cancel the original and emit a new order with the replacement parameters.
    ref_delete(orig_ref);
    const OrderId new_our_id = next_order_id_++;
    ref_insert(new_ref, new_our_id, side);

    // Emit the replacement as a ModifyOrder.
    out.msg_type   = MarketDataMsg::Type::ModifyOrder;
    out.order_id   = our_id;     // original — matching engine will replace it
    out.qty        = shares;
    out.price      = price;
    out.side       = (side == 0) ? Side::Buy : Side::Sell;
    return true;
}

} // namespace itch
} // namespace engine
