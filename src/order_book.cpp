#include "lob/order_book.hpp"

namespace lob {

void OrderBook::add(const Order& order) {
    if (order.side == Side::Buy) {
        bids_[order.price].push_back(order);
    } else {
        asks_[order.price].push_back(order);
    }
    index_[order.id] = Locator{order.side, order.price};
}

bool OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) {
        return false;
    }
    const Locator loc = it->second;

    auto remove_from = [&](auto& levels) {
        auto level_it = levels.find(loc.price);
        if (level_it == levels.end()) {
            return false;
        }
        auto& queue = level_it->second;
        for (auto order_it = queue.begin(); order_it != queue.end(); ++order_it) {
            if (order_it->id == id) {
                queue.erase(order_it);
                if (queue.empty()) {
                    levels.erase(level_it);
                }
                return true;
            }
        }
        return false;
    };

    const bool removed =
        (loc.side == Side::Buy) ? remove_from(bids_) : remove_from(asks_);
    if (removed) {
        index_.erase(it);
    }
    return removed;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

void OrderBook::on_resting_order_removed(OrderId id) {
    index_.erase(id);
}

} // namespace lob
