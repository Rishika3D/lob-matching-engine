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
    // `expected_live_orders` is a latency hint forwarded to the book's id
    // index (see OrderBook). It never changes behavior. Like OrderBook, the
    // engine is move-only: copying would alias the book's internal iterators.
    explicit MatchingEngine(std::size_t expected_live_orders = 0)
        : book_(expected_live_orders) {}

    // Submit an order, appending any resulting trades to `out`.
    //
    // Returns false if the order was rejected, in which case `out` and the
    // book are both left untouched. An order is rejected when:
    //
    //   - its quantity is zero -- such an order can neither trade nor rest,
    //     so accepting it would discard it while reporting success; or
    //   - its id matches an order already resting in the book -- ids must be
    //     unique among live orders, or the new order would displace the
    //     existing one from the index and orphan it.
    //
    // Validation happens before matching, so a rejected order neither trades
    // nor rests, and a rejected id is not consumed.
    //
    // Appends to `out`; it never clears it. A caller who wants only this
    // order's trades must clear the buffer first. Reusing one buffer across
    // calls avoids allocating a result vector per order. (Resting an order
    // still allocates its queue node and index entry -- this only removes the
    // per-call vector.)
    bool submit(Order order, std::vector<Trade>& out);

    // Convenience overload: returns only this order's trades, in a fresh
    // vector (unlike the buffer overload, nothing accumulates across calls).
    // Note this overload discards the accept/reject signal: a rejected order
    // and an accepted order that happened to generate no trades both return an
    // empty vector. Use the buffer overload above when rejection matters.
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
