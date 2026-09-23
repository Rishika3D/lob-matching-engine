#pragma once

#include <vector>

#include "lob/order.hpp"
#include "lob/order_book.hpp"

namespace lob {

// Price-time priority matching engine.
//
// An incoming order is matched against the opposite side of the book,
// best price first, FIFO within each price level ("walking the book").
// Any unfilled remainder of a limit order rests in the book; the
// remainder of a market order is discarded.
class MatchingEngine {
public:
    // Submit an order, appending any resulting trades to `out`.
    //
    // Hot path: the caller owns the buffer and can reuse it across orders,
    // so a steady order flow does no per-order heap allocation.
    void submit(Order order, std::vector<Trade>& out);

    // Convenience overload for callers that don't care about allocation.
    std::vector<Trade> submit(Order order);

    // Cancel a resting order by id.
    bool cancel(OrderId id);

    const OrderBook& book() const { return book_; }

private:
    template <typename Levels>
    void match_against(Order& incoming, Levels& levels, std::vector<Trade>& trades);

    OrderBook book_;
};

} // namespace lob
