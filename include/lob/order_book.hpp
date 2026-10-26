#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>

#include "lob/order.hpp"

namespace lob {

// One side of the book: price level -> FIFO queue of resting orders.
// Bids are sorted descending (best bid first), asks ascending (best ask first).
//
// Levels hold a std::list rather than a deque so the order index can store a
// stable iterator to each resting order: list iterators survive insertions and
// unrelated erasures, which makes cancel O(1) instead of a scan of the level.
using OrderQueue = std::list<Order>;
using BidLevels = std::map<Price, OrderQueue, std::greater<Price>>;
using AskLevels = std::map<Price, OrderQueue>;

class OrderBook {
public:
    // `expected_live_orders` pre-sizes the id index. Without it the index
    // rehashes as it grows, and each rehash is a single multi-millisecond
    // insert (measured: ~2-4 ms at 100k-400k live orders). Purely a latency
    // hint -- behavior is identical for any value, including 0.
    explicit OrderBook(std::size_t expected_live_orders = 0) {
        index_.reserve(expected_live_orders);
    }

    // Not copyable: the id index stores iterators into this book's own queues,
    // so a copy would hold iterators into the *source*, and cancelling through
    // the copy would corrupt (and later dangle into) the original. Moving is
    // safe -- list and hash-map moves keep element iterators valid.
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = default;
    OrderBook& operator=(OrderBook&&) = default;

    // Add a resting limit order to the book.
    void add(const Order& order);

    // Cancel by id. Returns true if the order was found and removed.
    bool cancel(OrderId id);

    // Is an order with this id currently resting in the book? Order ids must
    // be unique among live orders: the index maps one id to one resting order,
    // so admitting a duplicate would orphan whichever order it displaced.
    // An id becomes reusable once its order is filled or cancelled.
    bool contains(OrderId id) const;

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

    // order id -> exactly where the order sits, so cancel is O(1): the price
    // level is a map lookup and the order itself is a stored list iterator.
    struct Locator {
        Side side;
        Price price;
        OrderQueue::iterator pos;
    };
    std::unordered_map<OrderId, Locator> index_;
};

} // namespace lob
