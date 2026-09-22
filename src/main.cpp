#include <iostream>

#include "lob/matching_engine.hpp"

using namespace lob;

namespace {

void print_trades(const std::vector<Trade>& trades) {
    if (trades.empty()) {
        std::cout << "  no trades, order rested in the book\n";
        return;
    }
    for (const auto& t : trades) {
        std::cout << "  TRADE: aggressor #" << t.aggressor_id
                  << " x resting #" << t.resting_id
                  << "  " << t.quantity << " @ " << t.price << " ticks\n";
    }
}

void print_book(const OrderBook& book) {
    std::cout << "  book:";
    if (auto bid = book.best_bid()) {
        std::cout << " best bid " << *bid;
    } else {
        std::cout << " no bids";
    }
    if (auto ask = book.best_ask()) {
        std::cout << ", best ask " << *ask;
    } else {
        std::cout << ", no asks";
    }
    std::cout << "\n";
}

} // namespace

int main() {
    MatchingEngine engine;

    // Prices in ticks: with a $0.01 tick, 10002 == $100.02
    std::cout << "1. Resting limit orders build the book\n";
    print_trades(engine.submit({.id = 1, .side = Side::Buy,  .type = OrderType::Limit, .price = 10002, .quantity = 100}));
    print_trades(engine.submit({.id = 2, .side = Side::Buy,  .type = OrderType::Limit, .price = 10001, .quantity = 200}));
    print_trades(engine.submit({.id = 3, .side = Side::Buy,  .type = OrderType::Limit, .price = 10002, .quantity = 50}));
    print_trades(engine.submit({.id = 4, .side = Side::Sell, .type = OrderType::Limit, .price = 10005, .quantity = 300}));
    print_book(engine.book());

    std::cout << "\n2. Aggressive sell crosses the spread (price-time priority)\n";
    // 120 shares at 10002: order #1 (first in) fills 100, order #3 fills 20
    print_trades(engine.submit({.id = 5, .side = Side::Sell, .type = OrderType::Limit, .price = 10002, .quantity = 120}));
    print_book(engine.book());

    std::cout << "\n3. Market buy walks the book\n";
    print_trades(engine.submit({.id = 6, .side = Side::Buy, .type = OrderType::Market, .quantity = 150}));
    print_book(engine.book());

    std::cout << "\n4. Cancel a resting order\n";
    std::cout << "  cancel #2: " << (engine.cancel(2) ? "ok" : "not found") << "\n";
    std::cout << "  cancel #2 again: " << (engine.cancel(2) ? "ok" : "not found") << "\n";
    print_book(engine.book());

    return 0;
}
