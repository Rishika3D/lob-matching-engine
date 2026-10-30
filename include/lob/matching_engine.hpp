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
    // Time in force decides what happens to whatever does not trade at once:
    //
    //   GoodTilCancel      the unfilled remainder of a LIMIT order rests.
    //   ImmediateOrCancel  the remainder is discarded.
    //   FillOrKill         if the whole quantity cannot trade right now, the
    //                      order does nothing at all; otherwise it trades fully.
    //
    // Market orders never rest, whatever their time in force. A `true` return
    // means the order was processed according to its terms -- it may have
    // traded fully, partly or not at all (a killed fill-or-kill order returns
    // true with no trades). Only an invalid order returns false.
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

    // Change a resting order's price and/or remaining quantity. `new_quantity`
    // is the quantity you want left, not an amount to add or remove.
    //
    // Priority follows exchange convention:
    //   - same price and the quantity does not grow -> changed in place, the
    //     order keeps its place in the queue;
    //   - the price changes or the quantity grows -> the order loses priority:
    //     it is withdrawn and re-entered at the back of its (new) level, and if
    //     the new price crosses the book it trades immediately, appending to
    //     `out` exactly as submit() would.
    //
    // Returns false -- changing nothing -- if no such order is resting or
    // `new_quantity` is zero (cancel the order instead).
    bool modify(OrderId id, Price new_price, Quantity new_quantity,
                std::vector<Trade>& out);

    const OrderBook& book() const { return book_; }

private:
    template <typename Levels>
    void match_against(Order& incoming, Levels& levels, std::vector<Trade>& trades);

    // Could `incoming` be filled in full right now? Read-only; stops as soon as
    // enough opposite-side quantity has been seen.
    template <typename Levels>
    bool fully_fillable(const Order& incoming, const Levels& levels) const;

    OrderBook book_;
};

} // namespace lob
