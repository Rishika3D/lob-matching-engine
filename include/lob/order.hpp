#pragma once

#include <cstdint>

namespace lob {

// Prices are stored as integer ticks to avoid floating point issues.
// e.g. with a tick size of $0.01, $100.05 is stored as 10005.
using Price = std::int64_t;
using Quantity = std::uint64_t;
using OrderId = std::uint64_t;

enum class Side { Buy, Sell };

enum class OrderType {
    Limit,   // rest in the book if not immediately marketable
    Market   // fill what you can against the book, discard the rest
};

struct Order {
    OrderId id = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Price price = 0;          // ignored for market orders
    Quantity quantity = 0;    // remaining (unfilled) quantity
};

// Result of two orders crossing.
struct Trade {
    OrderId aggressor_id = 0;  // the incoming order
    OrderId resting_id = 0;    // the order that was sitting in the book
    Price price = 0;           // trades execute at the resting order's price
    Quantity quantity = 0;
};

} // namespace lob
