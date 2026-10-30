#include "lob/order_book.hpp"

#include <iterator>

namespace lob {

void OrderBook::add(const Order& order) {
    auto& queue = (order.side == Side::Buy) ? bids_[order.price]
                                            : asks_[order.price];
    queue.push_back(order);
    // std::prev(end()) is the just-inserted order; list iterators stay valid
    // as the level grows and shrinks around it.
    index_[order.id] = Locator{order.side, order.price, std::prev(queue.end())};
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
        queue.erase(loc.pos);  // O(1): no scan of the level
        if (queue.empty()) {
            levels.erase(level_it);
        }
        return true;
    };

    const bool removed =
        (loc.side == Side::Buy) ? remove_from(bids_) : remove_from(asks_);
    if (removed) {
        index_.erase(it);
    }
    return removed;
}

bool OrderBook::contains(OrderId id) const {
    return index_.find(id) != index_.end();
}

const Order* OrderBook::find(OrderId id) const {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &*it->second.pos;
}

bool OrderBook::reduce_quantity(OrderId id, Quantity new_quantity) {
    auto it = index_.find(id);
    if (it == index_.end()) {
        return false;
    }
    it->second.pos->quantity = new_quantity;
    return true;
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
