// Behavioral tests for matching, fills, cancellation and the submit() overloads.

#include <gtest/gtest.h>

#include <vector>

#include "test_util.hpp"

using namespace lob;
using namespace lob::testing;

// --- resting -----------------------------------------------------------------

TEST(Resting, NonCrossingLimitOrdersRestAndSetBestPrices) {
    MatchingEngine e;
    EXPECT_TRUE(submit(e, limit(1, Side::Buy, 100, 10)).empty());
    EXPECT_TRUE(submit(e, limit(2, Side::Buy, 101, 10)).empty());
    EXPECT_TRUE(submit(e, limit(3, Side::Sell, 105, 10)).empty());

    EXPECT_EQ(depth(e), 3u);
    EXPECT_EQ(e.book().best_bid(), std::optional<Price>(101));
    EXPECT_EQ(e.book().best_ask(), std::optional<Price>(105));
}

TEST(Resting, EmptyBookHasNoBestPrices) {
    MatchingEngine e;
    EXPECT_EQ(e.book().best_bid(), std::nullopt);
    EXPECT_EQ(e.book().best_ask(), std::nullopt);
}

TEST(Resting, SamePriceOrdersQueueInArrivalOrder) {
    MatchingEngine e;
    for (OrderId id = 1; id <= 4; ++id) submit(e, limit(id, Side::Buy, 100, 10));

    const auto q = level(e, Side::Buy, 100);
    ASSERT_EQ(q.size(), 4u);
    for (std::size_t i = 0; i < q.size(); ++i) EXPECT_EQ(q[i].id, i + 1);
}

// --- price-time priority -------------------------------------------------------

TEST(Priority, EarlierOrderAtSamePriceFillsFirst) {
    MatchingEngine e;
    submit(e, limit(1, Side::Buy, 100, 100));
    submit(e, limit(2, Side::Buy, 100, 50));

    const auto trades = submit(e, limit(3, Side::Sell, 100, 120));
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].resting_id, 1u);
    EXPECT_EQ(trades[0].quantity, 100u);
    EXPECT_EQ(trades[1].resting_id, 2u);
    EXPECT_EQ(trades[1].quantity, 20u);
}

TEST(Priority, BetterPriceFillsBeforeEarlierWorsePrice) {
    MatchingEngine e;
    submit(e, limit(1, Side::Buy, 100, 10));  // arrived first, worse price
    submit(e, limit(2, Side::Buy, 101, 10));  // arrived later, better price

    const auto trades = submit(e, limit(3, Side::Sell, 100, 10));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].resting_id, 2u) << "price priority beats time priority";
}

TEST(Priority, BestAskIsLowestForIncomingBuy) {
    MatchingEngine e;
    submit(e, limit(1, Side::Sell, 103, 10));
    submit(e, limit(2, Side::Sell, 101, 10));
    submit(e, limit(3, Side::Sell, 102, 10));

    const auto trades = submit(e, market(4, Side::Buy, 10));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].resting_id, 2u);
    EXPECT_EQ(trades[0].price, 101);
}

TEST(Priority, PartiallyFilledRestingOrderKeepsItsPlaceInQueue) {
    MatchingEngine e;
    submit(e, limit(1, Side::Buy, 100, 100));
    submit(e, limit(2, Side::Buy, 100, 100));

    submit(e, limit(3, Side::Sell, 100, 30));  // partially fills #1

    const auto q = level(e, Side::Buy, 100);
    ASSERT_EQ(q.size(), 2u);
    EXPECT_EQ(q[0].id, 1u) << "#1 must stay at the front after a partial fill";
    EXPECT_EQ(q[0].quantity, 70u);
    EXPECT_EQ(q[1].id, 2u);
}

TEST(Priority, AggressorRemainderJoinsTheBackOfItsLevel) {
    MatchingEngine e;
    submit(e, limit(1, Side::Buy, 100, 10));   // already resting at 100
    submit(e, limit(2, Side::Sell, 100, 5));   // fills 5 of #1, nothing left over
    submit(e, limit(3, Side::Sell, 99, 20));   // sells into #1 (5 left), 15 remainder rests

    const auto asks = level(e, Side::Sell, 99);
    ASSERT_EQ(asks.size(), 1u);
    EXPECT_EQ(asks[0].id, 3u);
    EXPECT_EQ(asks[0].quantity, 15u);
}

// --- fills ---------------------------------------------------------------------

TEST(Fills, CompleteFillRemovesBothOrdersAndEmptiesTheBook) {
    MatchingEngine e;
    submit(e, limit(1, Side::Buy, 100, 10));
    const auto trades = submit(e, limit(2, Side::Sell, 100, 10));

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].aggressor_id, 2u);
    EXPECT_EQ(trades[0].resting_id, 1u);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(depth(e), 0u);
    EXPECT_FALSE(e.book().contains(1));
    EXPECT_FALSE(e.book().contains(2));
    EXPECT_TRUE(e.book().bids().empty()) << "empty price level must be erased";
}

TEST(Fills, PartialFillLeavesRestingRemainder) {
    MatchingEngine e;
    submit(e, limit(1, Side::Buy, 100, 100));
    const auto trades = submit(e, limit(2, Side::Sell, 100, 30));

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 30u);
    const auto q = level(e, Side::Buy, 100);
    ASSERT_EQ(q.size(), 1u);
    EXPECT_EQ(q[0].quantity, 70u);
    EXPECT_TRUE(e.book().contains(1));
    EXPECT_FALSE(e.book().contains(2)) << "fully filled aggressor must not rest";
}

TEST(Fills, AggressorRemainderRestsAtItsOwnLimitPrice) {
    MatchingEngine e;
    submit(e, limit(1, Side::Sell, 100, 30));
    const auto trades = submit(e, limit(2, Side::Buy, 101, 100));

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 30u);
    const auto q = level(e, Side::Buy, 101);
    ASSERT_EQ(q.size(), 1u);
    EXPECT_EQ(q[0].id, 2u);
    EXPECT_EQ(q[0].quantity, 70u);
    EXPECT_TRUE(e.book().contains(2));
}

TEST(Fills, TradeExecutesAtRestingPriceNotAggressorPrice) {
    MatchingEngine e;
    submit(e, limit(1, Side::Sell, 100, 10));

    const auto buy = submit(e, limit(2, Side::Buy, 105, 10));
    ASSERT_EQ(buy.size(), 1u);
    EXPECT_EQ(buy[0].price, 100) << "buyer was willing to pay 105, pays 100";

    submit(e, limit(3, Side::Buy, 100, 10));
    const auto sell = submit(e, limit(4, Side::Sell, 95, 10));
    ASSERT_EQ(sell.size(), 1u);
    EXPECT_EQ(sell[0].price, 100) << "seller was willing to take 95, gets 100";
}

TEST(Fills, LimitOrderDoesNotTradeThroughItsLimit) {
    MatchingEngine e;
    submit(e, limit(1, Side::Sell, 100, 10));
    submit(e, limit(2, Side::Sell, 102, 10));

    const auto trades = submit(e, limit(3, Side::Buy, 101, 50));
    ASSERT_EQ(trades.size(), 1u) << "must stop before the 102 ask";
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_TRUE(e.book().asks().count(102) == 1);
}

TEST(Fills, LimitAtExactlyTheOppositePriceTrades) {
    MatchingEngine e;
    submit(e, limit(1, Side::Sell, 100, 10));
    EXPECT_EQ(submit(e, limit(2, Side::Buy, 100, 10)).size(), 1u);
}

// --- walking the book -----------------------------------------------------------

TEST(Walking, MarketOrderConsumesSeveralLevelsBestFirst) {
    MatchingEngine e;
    submit(e, limit(1, Side::Sell, 101, 10));
    submit(e, limit(2, Side::Sell, 102, 10));
    submit(e, limit(3, Side::Sell, 103, 10));

    const auto trades = submit(e, market(4, Side::Buy, 25));
    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0].price, 101);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(trades[1].price, 102);
    EXPECT_EQ(trades[1].quantity, 10u);
    EXPECT_EQ(trades[2].price, 103);
    EXPECT_EQ(trades[2].quantity, 5u);

    // Two levels gone, the third partially consumed.
    EXPECT_EQ(e.book().asks().size(), 1u);
    EXPECT_EQ(front_quantity(e, Side::Sell, 103), 5u);
    EXPECT_FALSE(e.book().contains(1));
    EXPECT_FALSE(e.book().contains(2));
    EXPECT_TRUE(e.book().contains(3));
}

TEST(Walking, SellWalksBidsFromHighestDown) {
    MatchingEngine e;
    submit(e, limit(1, Side::Buy, 99, 10));
    submit(e, limit(2, Side::Buy, 101, 10));
    submit(e, limit(3, Side::Buy, 100, 10));

    const auto trades = submit(e, market(4, Side::Sell, 30));
    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0].price, 101);
    EXPECT_EQ(trades[1].price, 100);
    EXPECT_EQ(trades[2].price, 99);
    EXPECT_EQ(depth(e), 0u);
}

TEST(Walking, MultiLevelWalkAlsoHonoursTimePriorityInsideEachLevel) {
    MatchingEngine e;
    submit(e, limit(1, Side::Sell, 100, 5));
    submit(e, limit(2, Side::Sell, 100, 5));
    submit(e, limit(3, Side::Sell, 101, 5));
    submit(e, limit(4, Side::Sell, 101, 5));

    const auto trades = submit(e, market(5, Side::Buy, 20));
    ASSERT_EQ(trades.size(), 4u);
    EXPECT_EQ(trades[0].resting_id, 1u);
    EXPECT_EQ(trades[1].resting_id, 2u);
    EXPECT_EQ(trades[2].resting_id, 3u);
    EXPECT_EQ(trades[3].resting_id, 4u);
}

// --- market orders ---------------------------------------------------------------

TEST(MarketOrders, IntoAnEmptyBookTradesNothingAndNeverRests) {
    MatchingEngine e;
    std::vector<Trade> out;
    EXPECT_TRUE(e.submit(market(1, Side::Buy, 50), out)) << "valid order is accepted";
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(depth(e), 0u);
    EXPECT_FALSE(e.book().contains(1));
    EXPECT_FALSE(e.cancel(1));
}

TEST(MarketOrders, UnfilledRemainderIsDiscardedNotRested) {
    MatchingEngine e;
    submit(e, limit(1, Side::Sell, 100, 10));

    const auto trades = submit(e, market(2, Side::Buy, 50));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(depth(e), 0u) << "the other 40 shares must vanish, not rest";
    EXPECT_FALSE(e.book().contains(2));
    EXPECT_EQ(e.book().best_bid(), std::nullopt);
}

TEST(MarketOrders, IgnoreTheirPriceField) {
    MatchingEngine e;
    submit(e, limit(1, Side::Sell, 100, 10));
    Order m = market(2, Side::Buy, 10);
    m.price = 1;  // would never cross a 100 ask if it were honoured
    EXPECT_EQ(submit(e, m).size(), 1u);
}

// --- cancellation ----------------------------------------------------------------

TEST(Cancel, RemovesARestingOrder) {
    MatchingEngine e;
    submit(e, limit(1, Side::Buy, 100, 10));
    EXPECT_TRUE(e.cancel(1));
    EXPECT_EQ(depth(e), 0u);
    EXPECT_FALSE(e.book().contains(1));
    EXPECT_TRUE(e.book().bids().empty()) << "empty level erased";
}

TEST(Cancel, UnknownIdReturnsFalse) {
    MatchingEngine e;
    EXPECT_FALSE(e.cancel(12345));
}

TEST(Cancel, SecondCancelOfTheSameIdFails) {
    MatchingEngine e;
    submit(e, limit(1, Side::Buy, 100, 10));
    EXPECT_TRUE(e.cancel(1));
    EXPECT_FALSE(e.cancel(1));
}

TEST(Cancel, AlreadyFilledOrderCannotBeCancelled) {
    MatchingEngine e;
    submit(e, limit(1, Side::Buy, 100, 10));
    submit(e, limit(2, Side::Sell, 100, 10));
    EXPECT_FALSE(e.cancel(1));
}

TEST(Cancel, PartiallyFilledOrderCancelsItsRemainder) {
    MatchingEngine e;
    submit(e, limit(1, Side::Buy, 100, 100));
    submit(e, limit(2, Side::Sell, 100, 30));
    EXPECT_TRUE(e.cancel(1));
    EXPECT_EQ(depth(e), 0u);
}

TEST(Cancel, MiddleOfQueueLeavesTheRestInOrder) {
    MatchingEngine e;
    for (OrderId id = 1; id <= 5; ++id) submit(e, limit(id, Side::Buy, 100, 10));
    EXPECT_TRUE(e.cancel(3));

    const auto trades = submit(e, limit(99, Side::Sell, 100, 40));
    ASSERT_EQ(trades.size(), 4u);
    EXPECT_EQ(trades[0].resting_id, 1u);
    EXPECT_EQ(trades[1].resting_id, 2u);
    EXPECT_EQ(trades[2].resting_id, 4u);
    EXPECT_EQ(trades[3].resting_id, 5u);
}

TEST(Cancel, FrontAndBackOfQueueAreHandled) {
    MatchingEngine e;
    for (OrderId id = 1; id <= 3; ++id) submit(e, limit(id, Side::Sell, 100, 10));
    EXPECT_TRUE(e.cancel(1));  // front
    EXPECT_TRUE(e.cancel(3));  // back

    const auto q = level(e, Side::Sell, 100);
    ASSERT_EQ(q.size(), 1u);
    EXPECT_EQ(q[0].id, 2u);
}

TEST(Cancel, LastOrderAtALevelRemovesTheLevel) {
    MatchingEngine e;
    submit(e, limit(1, Side::Buy, 100, 10));
    submit(e, limit(2, Side::Buy, 99, 10));
    EXPECT_TRUE(e.cancel(1));
    EXPECT_EQ(e.book().bids().count(100), 0u);
    EXPECT_EQ(e.book().best_bid(), std::optional<Price>(99));
}

TEST(Cancel, WorksOnBothSides) {
    MatchingEngine e;
    submit(e, limit(1, Side::Buy, 99, 10));
    submit(e, limit(2, Side::Sell, 101, 10));
    EXPECT_TRUE(e.cancel(2));
    EXPECT_TRUE(e.cancel(1));
    EXPECT_EQ(depth(e), 0u);
}

// --- the two submit() overloads ----------------------------------------------------

TEST(SubmitOverloads, BufferOverloadAppendsAndNeverClears) {
    MatchingEngine e;
    submit(e, limit(1, Side::Sell, 100, 50));

    std::vector<Trade> buf;
    buf.push_back(Trade{999, 999, 1, 1});  // caller's pre-existing content
    EXPECT_TRUE(e.submit(limit(2, Side::Buy, 100, 20), buf));

    ASSERT_EQ(buf.size(), 2u) << "must append to, not replace, the buffer";
    EXPECT_EQ(buf[0].aggressor_id, 999u) << "caller's entry preserved";
    EXPECT_EQ(buf[1].aggressor_id, 2u);
}

TEST(SubmitOverloads, ReusingABufferWithoutClearingAccumulatesTrades) {
    MatchingEngine e;
    submit(e, limit(1, Side::Sell, 100, 50));

    std::vector<Trade> buf;
    e.submit(limit(2, Side::Buy, 100, 10), buf);
    e.submit(limit(3, Side::Buy, 100, 10), buf);
    EXPECT_EQ(buf.size(), 2u);

    buf.clear();
    e.submit(limit(4, Side::Buy, 100, 10), buf);
    EXPECT_EQ(buf.size(), 1u) << "after clear() only this order's trades remain";
}

TEST(SubmitOverloads, ReturningOverloadGivesOnlyThisOrdersTrades) {
    MatchingEngine e;
    submit(e, limit(1, Side::Sell, 100, 50));
    EXPECT_EQ(e.submit(limit(2, Side::Buy, 100, 10)).size(), 1u);
    EXPECT_EQ(e.submit(limit(3, Side::Buy, 100, 10)).size(), 1u)
        << "a fresh vector each call: nothing accumulates";
}

TEST(SubmitOverloads, BothOverloadsProduceIdenticalResults) {
    MatchingEngine a, b;
    submit(a, limit(1, Side::Sell, 100, 50));
    submit(b, limit(1, Side::Sell, 100, 50));

    const auto ra = a.submit(limit(2, Side::Buy, 100, 30));
    std::vector<Trade> rb;
    b.submit(limit(2, Side::Buy, 100, 30), rb);

    ASSERT_EQ(ra.size(), rb.size());
    ASSERT_EQ(ra.size(), 1u);
    EXPECT_EQ(ra[0].quantity, rb[0].quantity);
    EXPECT_EQ(ra[0].price, rb[0].price);
    EXPECT_EQ(depth(a), depth(b));
}
