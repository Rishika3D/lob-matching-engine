#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "lob/event_io.hpp"
#include "lob/matching_engine.hpp"
#include "lob/order.hpp"

namespace lob {

// Strategy order ids start here. Historical ids must stay below it, so the two
// can never collide inside the shared book.
inline constexpr OrderId kStrategyIdBase = OrderId{1} << 62;

// One execution of a strategy order.
struct Fill {
    std::int64_t timestamp = 0;  // timestamp of the event that caused it
    OrderId order_id = 0;        // the strategy order
    Side side = Side::Buy;       // the strategy's side
    Price price = 0;
    Quantity quantity = 0;
    bool as_aggressor = false;   // true if the strategy order was the incoming one
};

struct BacktestResult {
    std::string error;  // non-empty if the run could not start (e.g. a bad id)
    bool ok() const { return error.empty(); }

    std::vector<Fill> fills;
    std::int64_t position = 0;           // signed: long > 0, short < 0
    std::int64_t cash = 0;               // ticks x shares; buys reduce it, sells increase it
    std::int64_t max_abs_position = 0;
    std::optional<double> mark;          // mid if both sides exist, else last trade price
    std::optional<double> pnl;           // cash + position * mark, in ticks x shares
                                         // (empty if a position is open but nothing to mark it to)

    std::uint64_t events = 0;            // historical events processed
    std::uint64_t trades = 0;            // trades in the whole book, strategy's or not
    std::uint64_t strategy_orders = 0;   // accepted strategy submissions
    std::uint64_t strategy_rejects = 0;  // strategy submissions the engine refused
    std::uint64_t historical_rejects = 0;  // historical new/modify the engine refused
    std::uint64_t historical_cancel_misses = 0;  // cancel/modify of an id no longer resting
};

class Backtester;

// What a strategy can see and do while handling an event.
class Context {
public:
    // The shared book: historical orders and the strategy's own, together.
    const OrderBook& book() const;

    // Timestamp of the event being handled.
    std::int64_t now() const;

    std::int64_t position() const;

    // Submit a limit order. Returns the id assigned, or nullopt if the engine
    // rejected it (zero quantity). Anything that can trade does so immediately.
    std::optional<OrderId> submit(Side side, Price price, Quantity quantity,
                                  TimeInForce tif = TimeInForce::GoodTilCancel);
    std::optional<OrderId> submit_market(Side side, Quantity quantity,
                                         TimeInForce tif = TimeInForce::ImmediateOrCancel);

    // Only the strategy's own orders can be cancelled or modified here.
    bool cancel(OrderId id);
    bool modify(OrderId id, Price new_price, Quantity new_quantity);

    // Shares resting in front of `id` at its price level (its queue position).
    // Empty if the order is not resting.
    std::optional<Quantity> queue_ahead(OrderId id) const;

private:
    friend class Backtester;
    explicit Context(Backtester& bt) : bt_(bt) {}
    Backtester& bt_;
};

class Strategy {
public:
    virtual ~Strategy() = default;

    // Called after each historical event has been applied to the book.
    virtual void on_event(const MarketEvent&, Context&) {}

    // Called for each fill of a strategy order, before the on_event of the
    // event that caused it. Fills caused by the strategy's own actions are
    // delivered after the current callback returns, never re-entrantly.
    virtual void on_fill(const Fill&, Context&) {}
};

// Replays historical order flow through a real MatchingEngine with a strategy's
// orders sharing the book. Because strategy orders go through the same matching
// logic, queue position, partial fills and time priority come from the engine
// itself rather than from a separate approximation.
//
// Honest limits of the model: the historical orders are replayed as submitted,
// so a strategy that moves the market does not change what the "other side"
// would have done in response (no reaction model), and historical cancels that
// target an order the strategy's presence caused to trade are counted in
// `historical_cancel_misses` rather than guessed at.
class Backtester {
public:
    explicit Backtester(std::size_t expected_live_orders = 0)
        : engine_(expected_live_orders), expected_(expected_live_orders) {}

    BacktestResult run(const std::vector<MarketEvent>& events, Strategy& strategy);

private:
    friend class Context;

    bool apply_historical(const MarketEvent& ev, BacktestResult& r);
    std::optional<OrderId> strategy_submit(Order order);
    void absorb_trades();
    void deliver_fills(Strategy& strategy);
    void record_fill(const Fill& f);

    MatchingEngine engine_;
    std::size_t expected_;

    // Per-run state.
    std::vector<Trade> trade_buf_;
    std::unordered_map<OrderId, Side> mine_;  // strategy order id -> its side
    std::vector<Fill> pending_;               // fills awaiting on_fill delivery
    std::size_t delivered_ = 0;
    OrderId next_strategy_id_ = kStrategyIdBase;
    std::int64_t now_ = 0;
    std::optional<Price> last_trade_price_;
    BacktestResult* result_ = nullptr;
    Strategy* strategy_ = nullptr;
};

} // namespace lob
