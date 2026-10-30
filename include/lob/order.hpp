#pragma once

#include <cstdint>

namespace lob {

// Prices are stored as integer ticks to avoid floating point issues.
// e.g. with a tick size of $0.01, $100.05 is stored as 10005.
using Price = std::int64_t;
using Quantity = std::uint64_t;
using OrderId = std::uint64_t;

// One byte each so adding a field to Order does not grow it past 32 bytes.
enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t {
    Limit,   // trades at its price or better; the remainder may rest
    Market   // fill what you can against the book, discard the rest
};

// What happens to the part of an order that does not trade immediately.
enum class TimeInForce : std::uint8_t {
    GoodTilCancel,      // rest in the book until filled or cancelled (limit orders)
    ImmediateOrCancel,  // trade what is available now, discard the rest
    FillOrKill          // trade the whole quantity now, or do nothing at all
};

struct Order {
    OrderId id = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GoodTilCancel;
    Price price = 0;          // ignored for market orders
    Quantity quantity = 0;    // remaining (unfilled) quantity
};

static_assert(sizeof(Order) == 32, "Order should stay within half a cache line");

// Result of two orders crossing.
struct Trade {
    OrderId aggressor_id = 0;  // the incoming order
    OrderId resting_id = 0;    // the order that was sitting in the book
    Price price = 0;           // trades execute at the resting order's price
    Quantity quantity = 0;
};

} // namespace lob
