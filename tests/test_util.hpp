#pragma once

// Small helpers shared by the test files.

#include <cstddef>
#include <optional>
#include <vector>

#include "lob/matching_engine.hpp"

namespace lob::testing {

inline Order limit(OrderId id, Side side, Price price, Quantity qty) {
    return Order{id, side, OrderType::Limit, price, qty};
}

inline Order market(OrderId id, Side side, Quantity qty) {
    return Order{id, side, OrderType::Market, 0, qty};
}

// Total resting orders across both sides of the book.
inline std::size_t depth(const OrderBook& book) {
    std::size_t n = 0;
    for (const auto& [price, queue] : book.bids()) n += queue.size();
    for (const auto& [price, queue] : book.asks()) n += queue.size();
    return n;
}

inline std::size_t depth(const MatchingEngine& e) { return depth(e.book()); }

// Resting orders at one price on one side, front (highest priority) first.
// Empty if the level does not exist.
inline std::vector<Order> level(const MatchingEngine& e, Side side, Price price) {
    std::vector<Order> out;
    if (side == Side::Buy) {
        auto it = e.book().bids().find(price);
        if (it != e.book().bids().end()) out.assign(it->second.begin(), it->second.end());
    } else {
        auto it = e.book().asks().find(price);
        if (it != e.book().asks().end()) out.assign(it->second.begin(), it->second.end());
    }
    return out;
}

// Submit through the buffer overload and return the trades for this order only.
inline std::vector<Trade> submit(MatchingEngine& e, const Order& o) {
    std::vector<Trade> out;
    e.submit(o, out);
    return out;
}

} // namespace lob::testing
