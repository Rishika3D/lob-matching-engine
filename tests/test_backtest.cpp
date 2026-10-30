// CSV event loading and the backtester: queue position, fills, P&L, and the
// ordering/safety guarantees a strategy relies on.

#include <gtest/gtest.h>

#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "lob/backtest.hpp"
#include "lob/event_io.hpp"
#include "test_util.hpp"

using namespace lob;
using namespace lob::testing;

namespace {

MarketEvent new_event(std::int64_t ts, const Order& o) {
    MarketEvent e;
    e.timestamp = ts;
    e.kind = EventKind::NewOrder;
    e.order = o;
    return e;
}

MarketEvent cancel_event(std::int64_t ts, OrderId id) {
    MarketEvent e;
    e.timestamp = ts;
    e.kind = EventKind::Cancel;
    e.order.id = id;
    return e;
}

// A strategy whose behaviour is a pair of lambdas, so each test reads as a script.
struct Scripted : Strategy {
    std::function<void(const MarketEvent&, Context&)> event_fn;
    std::function<void(const Fill&, Context&)> fill_fn;
    void on_event(const MarketEvent& e, Context& c) override { if (event_fn) event_fn(e, c); }
    void on_fill(const Fill& f, Context& c) override { if (fill_fn) fill_fn(f, c); }
};

ParseResult parse(const std::string& text) {
    std::istringstream in(text);
    return parse_events(in);
}

} // namespace

// --- CSV loading -------------------------------------------------------------------

TEST(EventCsv, ParsesEveryActionWithHeaderCommentsAndBlankLines) {
    const auto r = parse(
        "timestamp,action,id,side,type,tif,price,quantity\n"
        "# a comment\n"
        "\n"
        "1000,new,1,buy,limit,gtc,10000,50\n"
        "1001,NEW,2,SELL,Market,IOC,,20\n"
        "1002,modify,1,,,,9999,40\n"
        "1003,cancel,1,,,,,\n"
        "1004,new,3,b,limit,,10001,5\n");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.events.size(), 5u);

    EXPECT_EQ(r.events[0].kind, EventKind::NewOrder);
    EXPECT_EQ(r.events[0].order.id, 1u);
    EXPECT_EQ(r.events[0].order.side, Side::Buy);
    EXPECT_EQ(r.events[0].order.type, OrderType::Limit);
    EXPECT_EQ(r.events[0].order.price, 10000);
    EXPECT_EQ(r.events[0].order.quantity, 50u);

    EXPECT_EQ(r.events[1].order.side, Side::Sell);
    EXPECT_EQ(r.events[1].order.type, OrderType::Market);
    EXPECT_EQ(r.events[1].order.tif, TimeInForce::ImmediateOrCancel);

    EXPECT_EQ(r.events[2].kind, EventKind::Modify);
    EXPECT_EQ(r.events[2].order.price, 9999);
    EXPECT_EQ(r.events[2].order.quantity, 40u);

    EXPECT_EQ(r.events[3].kind, EventKind::Cancel);
    EXPECT_EQ(r.events[3].order.id, 1u);

    EXPECT_EQ(r.events[4].order.side, Side::Buy) << "'b' is accepted";
    EXPECT_EQ(r.events[4].order.tif, TimeInForce::GoodTilCancel) << "blank tif means gtc";
}

TEST(EventCsv, EmptyInputIsValidAndYieldsNoEvents) {
    const auto r = parse("");
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.events.empty());
}

TEST(EventCsv, FillOrKillIsRecognised) {
    const auto r = parse("1,new,1,buy,limit,fok,100,5\n");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.events[0].order.tif, TimeInForce::FillOrKill);
}

TEST(EventCsv, ReportsTheLineOfTheFirstProblem) {
    const auto r = parse("1,new,1,buy,limit,gtc,100,5\n2,new,2,buy,limit,gtc,100\n");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.error.find("line 2"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find("8"), std::string::npos) << "names the expected field count";
    EXPECT_TRUE(r.events.empty()) << "no partial result on failure";
}

TEST(EventCsv, RejectsBadFieldsWithAMessageNamingTheValue) {
    struct Case { const char* line; const char* mentions; };
    const Case cases[] = {
        {"x,new,1,buy,limit,gtc,100,5", "timestamp"},
        {"1,explode,1,buy,limit,gtc,100,5", "explode"},
        {"1,new,abc,buy,limit,gtc,100,5", "abc"},
        {"1,new,1,up,limit,gtc,100,5", "up"},
        {"1,new,1,buy,stop,gtc,100,5", "stop"},
        {"1,new,1,buy,limit,day,100,5", "day"},
        {"1,new,1,buy,limit,gtc,,5", "price"},
        {"1,new,1,buy,limit,gtc,100,-5", "-5"},
        {"1,new,1,buy,limit,gtc,100,5x", "5x"},
        {"1,modify,1,,,,,5", "price"},
        {"1,modify,1,,,,100,", "quantity"},
    };
    for (const auto& c : cases) {
        const auto r = parse(std::string(c.line) + "\n");
        EXPECT_FALSE(r.ok()) << "should reject: " << c.line;
        EXPECT_NE(r.error.find(c.mentions), std::string::npos)
            << "error for '" << c.line << "' should mention '" << c.mentions
            << "' but was: " << r.error;
    }
}

TEST(EventCsv, RejectsTimestampsThatGoBackwards) {
    const auto r = parse("5,new,1,buy,limit,gtc,100,5\n4,new,2,buy,limit,gtc,100,5\n");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.error.find("backwards"), std::string::npos) << r.error;
}

TEST(EventCsv, EqualTimestampsAreFine) {
    EXPECT_TRUE(parse("5,new,1,buy,limit,gtc,100,5\n5,new,2,buy,limit,gtc,100,5\n").ok());
}

TEST(EventCsv, ToleratesSpacesAroundFields) {
    const auto r = parse(" 1 , new , 1 , buy , limit , gtc , 100 , 5 \n");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.events[0].order.price, 100);
}

// --- queue position and fills ------------------------------------------------------

TEST(Backtest, StrategyOrderJoinsTheBackOfTheQueueAndFillsInPriorityOrder) {
    std::vector<MarketEvent> ev = {
        new_event(1, limit(1, Side::Buy, 100, 30)),
        new_event(2, limit(2, Side::Buy, 100, 20)),
        new_event(3, market(3, Side::Sell, 40)),   // eats #1 (30) and 10 of #2
        new_event(4, market(4, Side::Sell, 15)),   // rest of #2 (10), then 5 of ours
        new_event(5, market(5, Side::Sell, 10)),   // our last 5, plus nothing left
    };

    Scripted s;
    std::optional<OrderId> mine;
    std::vector<std::optional<Quantity>> ahead;
    s.event_fn = [&](const MarketEvent& e, Context& c) {
        if (e.timestamp == 2) mine = c.submit(Side::Buy, 100, 10);
        if (mine) ahead.push_back(c.queue_ahead(*mine));
    };

    Backtester bt;
    const auto r = bt.run(ev, s);
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_TRUE(mine.has_value());
    EXPECT_GE(*mine, kStrategyIdBase);

    ASSERT_EQ(ahead.size(), 4u);
    EXPECT_EQ(ahead[0], std::optional<Quantity>(50)) << "30 + 20 resting ahead of us";
    EXPECT_EQ(ahead[1], std::optional<Quantity>(10)) << "only #2's last 10 left ahead";
    EXPECT_EQ(ahead[2], std::optional<Quantity>(0)) << "at the front, partly filled";
    EXPECT_EQ(ahead[3], std::nullopt) << "fully filled: no longer resting";

    ASSERT_EQ(r.fills.size(), 2u) << "not filled at all until the queue ahead is gone";
    EXPECT_EQ(r.fills[0].timestamp, 4);
    EXPECT_EQ(r.fills[0].quantity, 5u);
    EXPECT_FALSE(r.fills[0].as_aggressor);
    EXPECT_EQ(r.fills[1].timestamp, 5);
    EXPECT_EQ(r.fills[1].quantity, 5u);
    EXPECT_EQ(r.position, 10);
    EXPECT_EQ(r.cash, -1000);
    ASSERT_TRUE(r.pnl.has_value());
    EXPECT_DOUBLE_EQ(*r.pnl, 0.0) << "bought at 100, marked at last trade 100";
}

TEST(Backtest, NotFilledWhileOrdersAheadOfItAreStillResting) {
    std::vector<MarketEvent> ev = {
        new_event(1, limit(1, Side::Buy, 100, 100)),
        new_event(2, market(2, Side::Sell, 99)),   // 1 share short of reaching us
    };
    Scripted s;
    s.event_fn = [&](const MarketEvent& e, Context& c) {
        if (e.timestamp == 1) c.submit(Side::Buy, 100, 10);
    };
    const auto r = Backtester().run(ev, s);
    EXPECT_TRUE(r.fills.empty());
    EXPECT_EQ(r.position, 0);
}

TEST(Backtest, AggressiveStrategyOrderFillsImmediatelyAsAggressor) {
    std::vector<MarketEvent> ev = {
        new_event(1, limit(1, Side::Sell, 100, 10)),
        new_event(2, limit(2, Side::Sell, 101, 10)),
    };
    Scripted s;
    std::vector<Fill> seen;
    s.event_fn = [&](const MarketEvent& e, Context& c) {
        if (e.timestamp == 2) c.submit_market(Side::Buy, 15);
    };
    s.fill_fn = [&](const Fill& f, Context&) { seen.push_back(f); };

    const auto r = Backtester().run(ev, s);
    ASSERT_EQ(r.fills.size(), 2u);
    EXPECT_TRUE(r.fills[0].as_aggressor);
    EXPECT_EQ(r.fills[0].price, 100);
    EXPECT_EQ(r.fills[0].quantity, 10u);
    EXPECT_EQ(r.fills[1].price, 101);
    EXPECT_EQ(r.fills[1].quantity, 5u);
    EXPECT_EQ(r.position, 15);
    EXPECT_EQ(r.cash, -(10 * 100 + 5 * 101));
    EXPECT_EQ(seen.size(), 2u) << "every fill is delivered to on_fill";
}

// --- P&L -----------------------------------------------------------------------------

TEST(Backtest, RoundTripProfitIsExact) {
    std::vector<MarketEvent> ev = {
        new_event(1, limit(1, Side::Sell, 100, 10)),
        new_event(2, limit(2, Side::Buy, 103, 10)),
    };
    Scripted s;
    s.event_fn = [&](const MarketEvent& e, Context& c) {
        if (e.timestamp == 1) c.submit_market(Side::Buy, 10);   // buy 10 @ 100
        if (e.timestamp == 2) c.submit_market(Side::Sell, 10);  // sell 10 @ 103
    };
    const auto r = Backtester().run(ev, s);
    EXPECT_EQ(r.position, 0);
    EXPECT_EQ(r.cash, 30);
    ASSERT_TRUE(r.pnl.has_value());
    EXPECT_DOUBLE_EQ(*r.pnl, 30.0);
    EXPECT_EQ(r.max_abs_position, 10);
}

TEST(Backtest, OpenShortPositionIsMarkedToMid) {
    std::vector<MarketEvent> ev = {
        new_event(1, limit(1, Side::Buy, 100, 10)),
        new_event(2, limit(2, Side::Buy, 99, 5)),
        new_event(3, limit(3, Side::Sell, 103, 5)),
    };
    Scripted s;
    s.event_fn = [&](const MarketEvent& e, Context& c) {
        if (e.timestamp == 1) c.submit_market(Side::Sell, 10);  // short 10 @ 100
    };
    const auto r = Backtester().run(ev, s);
    EXPECT_EQ(r.position, -10);
    EXPECT_EQ(r.cash, 1000);
    ASSERT_TRUE(r.mark.has_value());
    EXPECT_DOUBLE_EQ(*r.mark, 101.0) << "mid of 99 and 103";
    ASSERT_TRUE(r.pnl.has_value());
    EXPECT_DOUBLE_EQ(*r.pnl, 1000.0 + (-10) * 101.0) << "= -10";
}

TEST(Backtest, MarksToLastTradeWhenOneSideOfTheBookIsEmpty) {
    std::vector<MarketEvent> ev = {
        new_event(1, limit(1, Side::Sell, 100, 10)),
    };
    Scripted s;
    s.event_fn = [&](const MarketEvent&, Context& c) { c.submit_market(Side::Buy, 10); };
    const auto r = Backtester().run(ev, s);
    EXPECT_EQ(r.position, 10);
    ASSERT_TRUE(r.mark.has_value());
    EXPECT_DOUBLE_EQ(*r.mark, 100.0) << "no two-sided book, so the last trade";
    EXPECT_DOUBLE_EQ(*r.pnl, 0.0);
}

TEST(Backtest, NoActivityGivesZeroPnl) {
    Scripted s;
    const auto r = Backtester().run({new_event(1, limit(1, Side::Buy, 100, 10))}, s);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.position, 0);
    ASSERT_TRUE(r.pnl.has_value());
    EXPECT_DOUBLE_EQ(*r.pnl, 0.0);
    EXPECT_EQ(r.events, 1u);
}

// --- ordering and safety guarantees ----------------------------------------------------

TEST(Backtest, OnFillIsDeliveredBeforeOnEventForTheSameEvent) {
    std::vector<MarketEvent> ev = {
        new_event(1, limit(1, Side::Buy, 100, 10)),
        new_event(2, market(2, Side::Sell, 10)),
    };
    Scripted s;
    std::vector<std::string> log;
    s.event_fn = [&](const MarketEvent& e, Context& c) {
        if (e.timestamp == 1) c.submit(Side::Buy, 100, 5);  // behind the 10 already there
        log.push_back("event" + std::to_string(e.timestamp));
    };
    s.fill_fn = [&](const Fill&, Context&) { log.push_back("fill"); };

    // The market sell takes the 10 ahead of us, then nothing is left for us, so
    // to trigger a fill let it be larger.
    ev[1] = new_event(2, market(2, Side::Sell, 12));
    Backtester().run(ev, s);

    const std::vector<std::string> expected = {"event1", "fill", "event2"};
    EXPECT_EQ(log, expected);
}

TEST(Backtest, FillCallbacksAreNeverReentrant) {
    std::vector<MarketEvent> ev = {
        new_event(1, limit(1, Side::Sell, 100, 100)),
    };
    Scripted s;
    int depth_now = 0, max_depth = 0, fills = 0;
    s.event_fn = [&](const MarketEvent&, Context& c) { c.submit_market(Side::Buy, 10); };
    s.fill_fn = [&](const Fill&, Context& c) {
        ++depth_now;
        max_depth = std::max(max_depth, depth_now);
        ++fills;
        if (fills < 5) c.submit_market(Side::Buy, 10);  // fills again from inside on_fill
        --depth_now;
    };
    const auto r = Backtester().run(ev, s);
    EXPECT_EQ(max_depth, 1) << "a fill caused inside on_fill waits its turn";
    EXPECT_EQ(fills, 5);
    EXPECT_EQ(r.position, 50);
}

TEST(Backtest, StrategyCanOnlyCancelItsOwnOrders) {
    std::vector<MarketEvent> ev = {
        new_event(1, limit(1, Side::Buy, 100, 10)),
    };
    Scripted s;
    bool cancelled_historical = true, cancelled_own = false;
    s.event_fn = [&](const MarketEvent&, Context& c) {
        cancelled_historical = c.cancel(1);
        const auto mine = c.submit(Side::Buy, 99, 5);
        cancelled_own = mine && c.cancel(*mine);
    };
    const auto r = Backtester().run(ev, s);
    EXPECT_FALSE(cancelled_historical);
    EXPECT_TRUE(cancelled_own);
    EXPECT_TRUE(r.ok());
}

TEST(Backtest, ModifyKeepsOrLosesQueuePositionLikeTheEngine) {
    std::vector<MarketEvent> ev = {
        new_event(1, limit(1, Side::Buy, 100, 40)),
    };
    Scripted s;
    std::vector<Quantity> ahead;
    s.event_fn = [&](const MarketEvent&, Context& c) {
        const auto a = c.submit(Side::Buy, 100, 20);
        const auto b = c.submit(Side::Buy, 100, 20);
        ASSERT_TRUE(a && b);
        ahead.push_back(*c.queue_ahead(*a));            // 40
        c.modify(*a, 100, 10);                            // reduce: keeps place
        ahead.push_back(*c.queue_ahead(*a));            // 40
        c.modify(*a, 100, 30);                            // grow: goes to the back
        ahead.push_back(*c.queue_ahead(*a));            // 40 + 20 = 60
    };
    Backtester().run(ev, s);
    ASSERT_EQ(ahead.size(), 3u);
    EXPECT_EQ(ahead[0], 40u);
    EXPECT_EQ(ahead[1], 40u);
    EXPECT_EQ(ahead[2], 60u);
}

TEST(Backtest, StrategyIdsAreDistinctAndOutsideTheHistoricalRange) {
    std::vector<MarketEvent> ev = {new_event(1, limit(1, Side::Buy, 100, 10))};
    Scripted s;
    std::vector<OrderId> ids;
    s.event_fn = [&](const MarketEvent&, Context& c) {
        for (int i = 0; i < 5; ++i) ids.push_back(*c.submit(Side::Buy, 90, 1));
    };
    Backtester().run(ev, s);
    ASSERT_EQ(ids.size(), 5u);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        EXPECT_GE(ids[i], kStrategyIdBase);
        for (std::size_t j = i + 1; j < ids.size(); ++j) EXPECT_NE(ids[i], ids[j]);
    }
}

TEST(Backtest, HistoricalIdInTheStrategyRangeIsRefusedUpFront) {
    std::vector<MarketEvent> ev = {
        new_event(1, limit(1, Side::Buy, 100, 10)),
        new_event(2, limit(kStrategyIdBase + 1, Side::Buy, 100, 10)),
    };
    Scripted s;
    int calls = 0;
    s.event_fn = [&](const MarketEvent&, Context&) { ++calls; };
    const auto r = Backtester().run(ev, s);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.error.find("event 1"), std::string::npos) << r.error;
    EXPECT_EQ(calls, 0) << "nothing runs when the input is unusable";
    EXPECT_EQ(r.events, 0u);
}

TEST(Backtest, RejectedStrategyOrdersAreCountedNotFilled) {
    std::vector<MarketEvent> ev = {new_event(1, limit(1, Side::Buy, 100, 10))};
    Scripted s;
    std::optional<OrderId> id = OrderId{7};
    s.event_fn = [&](const MarketEvent&, Context& c) { id = c.submit(Side::Buy, 100, 0); };
    const auto r = Backtester().run(ev, s);
    EXPECT_FALSE(id.has_value());
    EXPECT_EQ(r.strategy_rejects, 1u);
    EXPECT_EQ(r.strategy_orders, 0u);
}

TEST(Backtest, HistoricalCancelOfAnOrderTheStrategyConsumedIsCountedAsAMiss) {
    std::vector<MarketEvent> ev = {
        new_event(1, limit(1, Side::Sell, 100, 10)),
        cancel_event(2, 1),                        // order 1 was already taken by us
    };
    Scripted s;
    s.event_fn = [&](const MarketEvent& e, Context& c) {
        if (e.timestamp == 1) c.submit_market(Side::Buy, 10);
    };
    const auto r = Backtester().run(ev, s);
    EXPECT_EQ(r.historical_cancel_misses, 1u);
    EXPECT_EQ(r.position, 10);
}

TEST(Backtest, CountsHistoricalRejectsAndTrades) {
    std::vector<MarketEvent> ev = {
        new_event(1, limit(1, Side::Sell, 100, 10)),
        new_event(2, limit(2, Side::Buy, 100, 0)),   // zero quantity: refused
        new_event(3, limit(1, Side::Buy, 90, 5)),    // id 1 is still live: refused
        new_event(4, market(3, Side::Buy, 4)),       // one trade
    };
    Scripted s;
    const auto r = Backtester().run(ev, s);
    EXPECT_EQ(r.historical_rejects, 2u);
    EXPECT_EQ(r.trades, 1u);
    EXPECT_EQ(r.events, 4u);
}

TEST(Backtest, RunningTwiceGivesIdenticalResults) {
    std::vector<MarketEvent> ev = {
        new_event(1, limit(1, Side::Buy, 100, 30)),
        new_event(2, limit(2, Side::Sell, 101, 30)),
        new_event(3, market(3, Side::Sell, 50)),
        new_event(4, market(4, Side::Buy, 20)),
    };
    struct Quoter : Strategy {
        void on_event(const MarketEvent& e, Context& c) override {
            if (e.timestamp == 2) { c.submit(Side::Buy, 100, 10); c.submit(Side::Sell, 101, 10); }
        }
    };
    Backtester bt;
    Quoter a, b;
    const auto r1 = bt.run(ev, a);
    const auto r2 = bt.run(ev, b);
    EXPECT_EQ(r1.position, r2.position);
    EXPECT_EQ(r1.cash, r2.cash);
    ASSERT_EQ(r1.fills.size(), r2.fills.size());
    for (std::size_t i = 0; i < r1.fills.size(); ++i) {
        EXPECT_EQ(r1.fills[i].order_id, r2.fills[i].order_id) << "ids restart every run";
        EXPECT_EQ(r1.fills[i].price, r2.fills[i].price);
        EXPECT_EQ(r1.fills[i].quantity, r2.fills[i].quantity);
    }
}

TEST(Backtest, ModifyAndCancelEventsFromTheFeedAreApplied) {
    MarketEvent mod;
    mod.timestamp = 2;
    mod.kind = EventKind::Modify;
    mod.order.id = 1;
    mod.order.price = 100;
    mod.order.quantity = 4;

    std::vector<MarketEvent> ev = {
        new_event(1, limit(1, Side::Sell, 100, 10)),
        mod,
        new_event(3, market(3, Side::Buy, 100)),
    };
    Scripted s;
    s.event_fn = [&](const MarketEvent& e, Context& c) {
        if (e.timestamp == 3) EXPECT_EQ(c.book().best_ask(), std::nullopt);
    };
    const auto r = Backtester().run(ev, s);
    EXPECT_EQ(r.trades, 1u) << "the modify shrank order 1 to 4, so only 4 could trade";
}
