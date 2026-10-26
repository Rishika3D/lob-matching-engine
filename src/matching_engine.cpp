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

bool MatchingEngine::submit(Order order, std::vector<Trade>& out) {
    // Reject before matching: a rejected order must neither trade nor rest.
    if (order.quantity == 0) {
        return false;
    }
    if (book_.contains(order.id)) {
        return false;
    }

    if (order.side == Side::Buy) {
        match_against(order, book_.asks(), out);
    } else {
        match_against(order, book_.bids(), out);
    }

    // Unfilled limit remainder rests in the book; market remainder is dropped.
    if (order.quantity > 0 && order.type == OrderType::Limit) {
        book_.add(order);
    }

    return true;
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
