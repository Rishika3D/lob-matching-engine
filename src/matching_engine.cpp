#include "lob/matching_engine.hpp"

#include <algorithm>

namespace lob {

namespace {

// Is the incoming order willing to trade at this resting price level?
bool crosses(const Order& incoming, Price level_price) {
    if (incoming.type == OrderType::Market) {
        return true;
    }
    return incoming.side == Side::Buy ? incoming.price >= level_price
                                      : incoming.price <= level_price;
}

} // namespace

template <typename Levels>
void MatchingEngine::match_against(Order& incoming, Levels& levels,
                                   std::vector<Trade>& trades) {
    while (incoming.quantity > 0 && !levels.empty()) {
        auto level_it = levels.begin();  // best price on the opposite side
        if (!crosses(incoming, level_it->first)) {
            break;
        }

        auto& queue = level_it->second;
        while (incoming.quantity > 0 && !queue.empty()) {
            Order& resting = queue.front();
            const Quantity fill = std::min(incoming.quantity, resting.quantity);

            trades.push_back(Trade{incoming.id, resting.id, resting.price, fill});

            incoming.quantity -= fill;
            resting.quantity -= fill;

            if (resting.quantity == 0) {
                book_.on_resting_order_removed(resting.id);
                queue.pop_front();
            }
        }

        if (queue.empty()) {
            levels.erase(level_it);
        }
    }
}

template <typename Levels>
bool MatchingEngine::fully_fillable(const Order& incoming, const Levels& levels) const {
    Quantity needed = incoming.quantity;
    for (const auto& [price, queue] : levels) {
        if (!crosses(incoming, price)) {
            return false;  // levels only get worse from here
        }
        for (const Order& resting : queue) {
            if (resting.quantity >= needed) {
                return true;
            }
            needed -= resting.quantity;
        }
    }
    return false;
}

bool MatchingEngine::submit(Order order, std::vector<Trade>& out) {
    // Reject before matching: a rejected order must neither trade nor rest.
    if (order.quantity == 0) {
        return false;
    }
    if (book_.contains(order.id)) {
        return false;
    }

    // Fill-or-kill is all or nothing, so decide before touching the book.
    // A killed order is not an error: it was processed according to its terms.
    if (order.tif == TimeInForce::FillOrKill) {
        const bool fillable = (order.side == Side::Buy)
                                  ? fully_fillable(order, book_.asks())
                                  : fully_fillable(order, book_.bids());
        if (!fillable) {
            return true;
        }
    }

    if (order.side == Side::Buy) {
        match_against(order, book_.asks(), out);
    } else {
        match_against(order, book_.bids(), out);
    }

    // Only a good-til-cancel limit order rests. Market, immediate-or-cancel and
    // (already fully filled) fill-or-kill remainders are dropped.
    if (order.quantity > 0 && order.type == OrderType::Limit &&
        order.tif == TimeInForce::GoodTilCancel) {
        book_.add(order);
    }

    return true;
}

bool MatchingEngine::modify(OrderId id, Price new_price, Quantity new_quantity,
                            std::vector<Trade>& out) {
    const Order* current = book_.find(id);
    if (current == nullptr || new_quantity == 0) {
        return false;
    }

    // Same price, not larger: change in place and keep time priority.
    if (new_price == current->price && new_quantity <= current->quantity) {
        return book_.reduce_quantity(id, new_quantity);
    }

    // Otherwise the order forfeits its place: copy it, withdraw it, and send it
    // back through submit() so it can trade if the new price now crosses.
    Order replacement = *current;
    replacement.price = new_price;
    replacement.quantity = new_quantity;
    book_.cancel(id);
    return submit(replacement, out);
}

std::vector<Trade> MatchingEngine::submit(Order order) {
    std::vector<Trade> trades;
    submit(order, trades);
    return trades;
}

bool MatchingEngine::cancel(OrderId id) {
    return book_.cancel(id);
}

} // namespace lob
