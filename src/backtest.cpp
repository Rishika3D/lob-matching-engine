#include "lob/backtest.hpp"

#include <algorithm>
#include <cstdlib>

namespace lob {

// --- Context -----------------------------------------------------------------

const OrderBook& Context::book() const { return bt_.engine_.book(); }

std::int64_t Context::now() const { return bt_.now_; }

std::int64_t Context::position() const { return bt_.result_->position; }

std::optional<OrderId> Context::submit(Side side, Price price, Quantity quantity,
                                       TimeInForce tif) {
    Order o;
    o.side = side;
    o.type = OrderType::Limit;
    o.tif = tif;
    o.price = price;
    o.quantity = quantity;
    return bt_.strategy_submit(o);
}

std::optional<OrderId> Context::submit_market(Side side, Quantity quantity,
                                              TimeInForce tif) {
    Order o;
    o.side = side;
    o.type = OrderType::Market;
    o.tif = tif;
    o.quantity = quantity;
    return bt_.strategy_submit(o);
}

bool Context::cancel(OrderId id) {
    if (bt_.mine_.count(id) == 0) return false;
    return bt_.engine_.cancel(id);
}

bool Context::modify(OrderId id, Price new_price, Quantity new_quantity) {
    if (bt_.mine_.count(id) == 0) return false;
    const bool ok = bt_.engine_.modify(id, new_price, new_quantity, bt_.trade_buf_);
    bt_.absorb_trades();
    return ok;
}

std::optional<Quantity> Context::queue_ahead(OrderId id) const {
    const OrderBook& book = bt_.engine_.book();
    const Order* target = book.find(id);
    if (target == nullptr) return std::nullopt;

    auto sum_ahead = [&](const auto& levels) -> std::optional<Quantity> {
        auto level = levels.find(target->price);
        if (level == levels.end()) return std::nullopt;
        Quantity ahead = 0;
        for (const Order& o : level->second) {
            if (o.id == id) return ahead;
            ahead += o.quantity;
        }
        return std::nullopt;
    };
    return target->side == Side::Buy ? sum_ahead(book.bids()) : sum_ahead(book.asks());
}

// --- Backtester ----------------------------------------------------------------

void Backtester::record_fill(const Fill& f) {
    const std::int64_t sign = (f.side == Side::Buy) ? 1 : -1;
    const auto qty = static_cast<std::int64_t>(f.quantity);
    result_->position += sign * qty;
    result_->cash -= sign * f.price * qty;
    result_->max_abs_position =
        std::max(result_->max_abs_position, std::llabs(result_->position));
    result_->fills.push_back(f);
    pending_.push_back(f);
}

void Backtester::absorb_trades() {
    for (const Trade& t : trade_buf_) {
        ++result_->trades;
        last_trade_price_ = t.price;

        if (auto it = mine_.find(t.aggressor_id); it != mine_.end()) {
            record_fill(Fill{now_, t.aggressor_id, it->second, t.price, t.quantity, true});
        }
        if (auto it = mine_.find(t.resting_id); it != mine_.end()) {
            record_fill(Fill{now_, t.resting_id, it->second, t.price, t.quantity, false});
        }
    }
    trade_buf_.clear();
}

void Backtester::deliver_fills(Strategy& strategy) {
    Context ctx(*this);
    // on_fill may submit orders that fill, which appends to pending_; indexing
    // (not iterators) keeps this valid as the vector grows.
    while (delivered_ < pending_.size()) {
        const Fill f = pending_[delivered_++];
        strategy.on_fill(f, ctx);
    }
}

std::optional<OrderId> Backtester::strategy_submit(Order order) {
    order.id = next_strategy_id_++;
    mine_[order.id] = order.side;
    if (!engine_.submit(order, trade_buf_)) {
        mine_.erase(order.id);
        ++result_->strategy_rejects;
        return std::nullopt;
    }
    ++result_->strategy_orders;
    absorb_trades();
    return order.id;
}

bool Backtester::apply_historical(const MarketEvent& ev, BacktestResult& r) {
    switch (ev.kind) {
        case EventKind::NewOrder:
            if (!engine_.submit(ev.order, trade_buf_)) ++r.historical_rejects;
            break;
        case EventKind::Cancel:
            if (!engine_.cancel(ev.order.id)) ++r.historical_cancel_misses;
            break;
        case EventKind::Modify:
            if (!engine_.modify(ev.order.id, ev.order.price, ev.order.quantity, trade_buf_)) {
                ++r.historical_cancel_misses;
            }
            break;
    }
    absorb_trades();
    return true;
}

BacktestResult Backtester::run(const std::vector<MarketEvent>& events, Strategy& strategy) {
    BacktestResult r;

    // Historical ids must stay clear of the strategy's range.
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].order.id >= kStrategyIdBase) {
            r.error = "event " + std::to_string(i) + ": order id " +
                      std::to_string(events[i].order.id) +
                      " is in the range reserved for strategy orders";
            return r;
        }
    }

    // Fresh state for every run.
    engine_ = MatchingEngine(expected_);
    trade_buf_.clear();
    mine_.clear();
    pending_.clear();
    delivered_ = 0;
    next_strategy_id_ = kStrategyIdBase;
    now_ = 0;
    last_trade_price_.reset();
    result_ = &r;
    strategy_ = &strategy;

    Context ctx(*this);
    for (const MarketEvent& ev : events) {
        now_ = ev.timestamp;
        apply_historical(ev, r);
        deliver_fills(strategy);       // fills that this event caused
        strategy.on_event(ev, ctx);
        deliver_fills(strategy);       // fills from the strategy's own reaction
        ++r.events;
    }

    const OrderBook& book = engine_.book();
    if (book.best_bid() && book.best_ask()) {
        r.mark = (static_cast<double>(*book.best_bid()) + static_cast<double>(*book.best_ask())) / 2.0;
    } else if (last_trade_price_) {
        r.mark = static_cast<double>(*last_trade_price_);
    }
    if (r.mark) {
        r.pnl = static_cast<double>(r.cash) + static_cast<double>(r.position) * *r.mark;
    } else if (r.position == 0) {
        r.pnl = static_cast<double>(r.cash);
    }

    result_ = nullptr;
    strategy_ = nullptr;
    return r;
}

} // namespace lob
