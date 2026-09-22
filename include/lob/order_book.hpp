#pragma once

#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>

#include "lob/order.hpp"

namespace lob {

// One side of the book: price level -> FIFO queue of resting orders.
// Bids are sorted descending (best bid first), asks ascending (best ask first).
using BidLevels = std::map<Price, std::deque<Order>, std::greater<Price>>;
using AskLevels = std::map<Price, std::deque<Order>>;

class OrderBook {
public:
    // Add a resting limit order to the book.
    void add(const Order& order);

    // Cancel by id. Returns true if the order was found and removed.
    bool cancel(OrderId id);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    BidLevels& bids() { return bids_; }
    AskLevels& asks() { return asks_; }
    const BidLevels& bids() const { return bids_; }
    const AskLevels& asks() const { return asks_; }

    // Called by the matching engine when a resting order is fully filled,
    // so the cancel index stays consistent.
    void on_resting_order_removed(OrderId id);

private:
    BidLevels bids_;
    AskLevels asks_;

    // order id -> (side, price) so cancels don't have to scan the book.
    struct Locator {
        Side side;
        Price price;
    };
    std::unordered_map<OrderId, Locator> index_;
};

} // namespace lob
