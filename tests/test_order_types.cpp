// Immediate-or-cancel, fill-or-kill and modify (cancel/replace).

#include <gtest/gtest.h>

#include <vector>

#include "test_util.hpp"

using namespace lob;
using namespace lob::testing;

namespace {

// Seeds a book with asks 10 @ 100, 10 @ 101, 10 @ 102 (ids 1, 2, 3).
void seed_asks(MatchingEngine& e) {
    submit(e, limit(1, Side::Sell, 100, 10));
    submit(e, limit(2, Side::Sell, 101, 10));
    submit(e, limit(3, Side::Sell, 102, 10));
}

} // namespace

// --- immediate-or-cancel -------------------------------------------------------

TEST(Ioc, FillsWhatIsAvailableAndDiscardsTheRest) {
    MatchingEngine e;
    submit(e, limit(1, Side::Sell, 100, 10));

    const auto trades = submit(e, ioc(2, Side::Buy, 100, 25));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(depth(e), 0u) << "the unfilled 15 must not rest";
    EXPECT_FALSE(e.book().contains(2));
    EXPECT_FALSE(e.cancel(2));
}

TEST(Ioc, WithNothingToTradeAgainstDoesNothingAndDoesNotRest) {
    MatchingEngine e;
    std::vector<Trade> out;
    EXPECT_TRUE(e.submit(ioc(1, Side::Buy, 100, 10), out)) << "valid order: accepted";
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(depth(e), 0u);
}

TEST(Ioc, DoesNotRestEvenWhenItsPriceIsNotMarketable) {
    MatchingEngine e;
    submit(e, limit(1, Side::Sell, 105, 10));
    EXPECT_TRUE(submit(e, ioc(2, Side::Buy, 100, 10)).empty());
    EXPECT_EQ(depth(e), 1u) << "only the original ask remains";
    EXPECT_EQ(e.book().best_bid(), std::nullopt);
}

TEST(Ioc, RespectsItsLimitPrice) {
    MatchingEngine e;
    seed_asks(e);
    const auto trades = submit(e, ioc(9, Side::Buy, 101, 100));
    ASSERT_EQ(trades.size(), 2u) << "may take 100 and 101 but not 102";
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[1].price, 101);
    EXPECT_EQ(e.book().asks().size(), 1u);
}

TEST(Ioc, SellSideWorksToo) {
    MatchingEngine e;
    submit(e, limit(1, Side::Buy, 100, 10));
    const auto trades = submit(e, ioc(2, Side::Sell, 100, 25));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(depth(e), 0u);
}

TEST(Ioc, ZeroQuantityAndLiveIdAreStillRejected) {
    MatchingEngine e;
    std::vector<Trade> out;
    submit(e, limit(1, Side::Buy, 90, 10));
    EXPECT_FALSE(e.submit(ioc(2, Side::Sell, 95, 0), out));
    EXPECT_FALSE(e.submit(ioc(1, Side::Sell, 95, 5), out));
    EXPECT_TRUE(out.empty());
}

TEST(Ioc, MarketOrderWithIocBehavesLikeAPlainMarketOrder) {
    MatchingEngine a, b;
    seed_asks(a);
    seed_asks(b);
    const auto ra = submit(a, market(9, Side::Buy, 25));
    const auto rb = submit(b, market(9, Side::Buy, 25, TimeInForce::ImmediateOrCancel));
    ASSERT_EQ(ra.size(), rb.size());
    for (std::size_t i = 0; i < ra.size(); ++i) {
        EXPECT_EQ(ra[i].price, rb[i].price);
        EXPECT_EQ(ra[i].quantity, rb[i].quantity);
    }
    EXPECT_EQ(depth(a), depth(b));
}

// --- fill-or-kill ---------------------------------------------------------------

TEST(Fok, FillsCompletelyAcrossSeveralOrdersAndLevels) {
    MatchingEngine e;
    seed_asks(e);
    const auto trades = submit(e, fok(9, Side::Buy, 102, 25));
    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(trades[1].quantity, 10u);
    EXPECT_EQ(trades[2].quantity, 5u);
    EXPECT_FALSE(e.book().contains(9)) << "a filled FOK never rests";
    EXPECT_EQ(front_quantity(e, Side::Sell, 102), 5u);
}

TEST(Fok, KilledWhenLiquidityIsInsufficientAndTheBookIsUntouched) {
    MatchingEngine e;
    seed_asks(e);
    std::vector<Trade> out;
    EXPECT_TRUE(e.submit(fok(9, Side::Buy, 102, 31), out)) << "killed is still accepted";
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(depth(e), 3u);
    EXPECT_EQ(front_quantity(e, Side::Sell, 100), 10u);
    EXPECT_EQ(front_quantity(e, Side::Sell, 101), 10u);
    EXPECT_EQ(front_quantity(e, Side::Sell, 102), 10u);
    EXPECT_FALSE(e.book().contains(9));
}

TEST(Fok, OneShareShortIsKilledAndExactlyEnoughIsFilled) {
    MatchingEngine e;
    seed_asks(e);
    EXPECT_TRUE(submit(e, fok(8, Side::Buy, 102, 31)).empty()) << "31 > 30 available";
    EXPECT_EQ(submit(e, fok(9, Side::Buy, 102, 30)).size(), 3u) << "exactly 30 fills";
    EXPECT_EQ(depth(e), 0u);
}

TEST(Fok, LiquidityBeyondTheLimitPriceDoesNotCount) {
    MatchingEngine e;
    seed_asks(e);
    // 30 shares exist in total, but only 20 at or below 101.
    EXPECT_TRUE(submit(e, fok(9, Side::Buy, 101, 25)).empty());
    EXPECT_EQ(depth(e), 3u);
    EXPECT_EQ(submit(e, fok(10, Side::Buy, 101, 20)).size(), 2u);
}

TEST(Fok, KilledOrderDoesNotConsumeItsId) {
    MatchingEngine e;
    seed_asks(e);
    EXPECT_TRUE(submit(e, fok(9, Side::Buy, 102, 100)).empty());
    EXPECT_FALSE(e.book().contains(9));
    EXPECT_EQ(submit(e, limit(9, Side::Buy, 90, 5)).size(), 0u);
    EXPECT_TRUE(e.book().contains(9)) << "id 9 is free to use";
}

TEST(Fok, SellSideWorksToo) {
    MatchingEngine e;
    submit(e, limit(1, Side::Buy, 100, 10));
    submit(e, limit(2, Side::Buy, 99, 10));
    EXPECT_TRUE(submit(e, fok(8, Side::Sell, 99, 21)).empty());
    EXPECT_EQ(submit(e, fok(9, Side::Sell, 99, 20)).size(), 2u);
}

TEST(Fok, MarketOrderCanBeFillOrKill) {
    MatchingEngine e;
    seed_asks(e);
    EXPECT_TRUE(submit(e, market(8, Side::Buy, 31, TimeInForce::FillOrKill)).empty());
    EXPECT_EQ(depth(e), 3u);
    EXPECT_EQ(submit(e, market(9, Side::Buy, 30, TimeInForce::FillOrKill)).size(), 3u);
    EXPECT_EQ(depth(e), 0u);
}

TEST(Fok, SeesPartiallyFilledRestingQuantities) {
    MatchingEngine e;
    submit(e, limit(1, Side::Sell, 100, 10));
    submit(e, limit(2, Side::Buy, 100, 4));  // takes 4 of the 10
    // 6 remain.
    EXPECT_TRUE(submit(e, fok(8, Side::Buy, 100, 7)).empty());
    EXPECT_EQ(submit(e, fok(9, Side::Buy, 100, 6)).size(), 1u);
}

TEST(Fok, ZeroQuantityAndLiveIdAreStillRejected) {
    MatchingEngine e;
    std::vector<Trade> out;
    submit(e, limit(1, Side::Sell, 100, 10));
    EXPECT_FALSE(e.submit(fok(2, Side::Buy, 100, 0), out));
    EXPECT_FALSE(e.submit(fok(1, Side::Buy, 100, 5), out));
    EXPECT_TRUE(out.empty());
}

TEST(Fok, DoesNotMutateTheBookWhileDecidingToKill) {
    // The feasibility check is read-only: after a kill the queue order and
    // quantities are exactly as before.
    MatchingEngine e;
    submit(e, limit(1, Side::Sell, 100, 5));
    submit(e, limit(2, Side::Sell, 100, 5));
    submit(e, limit(3, Side::Sell, 100, 5));
    EXPECT_TRUE(submit(e, fok(9, Side::Buy, 100, 16)).empty());
    const auto q = level(e, Side::Sell, 100);
    ASSERT_EQ(q.size(), 3u);
    EXPECT_EQ(q[0].id, 1u);
    EXPECT_EQ(q[1].id, 2u);
    EXPECT_EQ(q[2].id, 3u);
    for (const auto& o : q) EXPECT_EQ(o.quantity, 5u);
}

// --- modify ---------------------------------------------------------------------

TEST(Modify, UnknownIdIsRejected) {
    MatchingEngine e;
    std::vector<Trade> out;
    EXPECT_FALSE(e.modify(42, 100, 10, out));
    EXPECT_TRUE(out.empty());
}

TEST(Modify, ZeroQuantityIsRejectedAndTheOrderIsUntouched) {
    MatchingEngine e;
    std::vector<Trade> out;
    submit(e, limit(1, Side::Buy, 100, 10));
    EXPECT_FALSE(e.modify(1, 100, 0, out));
    EXPECT_FALSE(e.modify(1, 105, 0, out));
    ASSERT_EQ(level(e, Side::Buy, 100).size(), 1u);
    EXPECT_EQ(front_quantity(e, Side::Buy, 100), 10u);
    EXPECT_TRUE(e.book().contains(1));
}

TEST(Modify, ReducingQuantityKeepsPriority) {
    MatchingEngine e;
    std::vector<Trade> out;
    submit(e, limit(1, Side::Buy, 100, 50));
    submit(e, limit(2, Side::Buy, 100, 50));
    submit(e, limit(3, Side::Buy, 100, 50));

    ASSERT_TRUE(e.modify(1, 100, 20, out));
    EXPECT_TRUE(out.empty());

    const auto q = level(e, Side::Buy, 100);
    ASSERT_EQ(q.size(), 3u);
    EXPECT_EQ(q[0].id, 1u) << "reducing must not move the order";
    EXPECT_EQ(q[0].quantity, 20u);

    const auto trades = submit(e, limit(9, Side::Sell, 100, 30));
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].resting_id, 1u) << "order 1 still trades first";
    EXPECT_EQ(trades[0].quantity, 20u);
    EXPECT_EQ(trades[1].resting_id, 2u);
}

TEST(Modify, SamePriceAndQuantityIsAnAcceptedNoOpThatKeepsPriority) {
    MatchingEngine e;
    std::vector<Trade> out;
    submit(e, limit(1, Side::Buy, 100, 10));
    submit(e, limit(2, Side::Buy, 100, 10));
    EXPECT_TRUE(e.modify(1, 100, 10, out));
    EXPECT_EQ(front_id(e, Side::Buy, 100), 1u);
}

TEST(Modify, IncreasingQuantityLosesPriority) {
    MatchingEngine e;
    std::vector<Trade> out;
    submit(e, limit(1, Side::Buy, 100, 10));
    submit(e, limit(2, Side::Buy, 100, 10));

    ASSERT_TRUE(e.modify(1, 100, 15, out));
    const auto q = level(e, Side::Buy, 100);
    ASSERT_EQ(q.size(), 2u);
    EXPECT_EQ(q[0].id, 2u) << "the grown order goes to the back";
    EXPECT_EQ(q[1].id, 1u);
    EXPECT_EQ(q[1].quantity, 15u);
}

TEST(Modify, ChangingPriceMovesTheOrderAndLosesPriority) {
    MatchingEngine e;
    std::vector<Trade> out;
    submit(e, limit(1, Side::Buy, 100, 10));
    submit(e, limit(2, Side::Buy, 99, 10));
    submit(e, limit(3, Side::Buy, 99, 10));

    ASSERT_TRUE(e.modify(1, 99, 10, out));
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(e.book().bids().count(100), 0u) << "the emptied level is gone";
    const auto q = level(e, Side::Buy, 99);
    ASSERT_EQ(q.size(), 3u);
    EXPECT_EQ(q[0].id, 2u);
    EXPECT_EQ(q[1].id, 3u);
    EXPECT_EQ(q[2].id, 1u) << "joins the back of the new level";
    EXPECT_EQ(e.book().best_bid(), std::optional<Price>(99));
}

TEST(Modify, PriceChangeToACrossingPriceTradesImmediately) {
    MatchingEngine e;
    std::vector<Trade> out;
    submit(e, limit(1, Side::Sell, 101, 10));
    submit(e, limit(2, Side::Buy, 99, 25));

    ASSERT_TRUE(e.modify(2, 101, 25, out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].aggressor_id, 2u);
    EXPECT_EQ(out[0].resting_id, 1u);
    EXPECT_EQ(out[0].price, 101) << "executes at the resting price";
    EXPECT_EQ(out[0].quantity, 10u);

    // The 15 left over rests at the new price; the old level is gone.
    EXPECT_EQ(e.book().bids().count(99), 0u);
    ASSERT_EQ(level(e, Side::Buy, 101).size(), 1u);
    EXPECT_EQ(front_quantity(e, Side::Buy, 101), 15u);
    EXPECT_TRUE(e.book().contains(2));
    EXPECT_FALSE(e.book().contains(1));
}

TEST(Modify, ReplacementThatFullyFillsLeavesNothingBehind) {
    MatchingEngine e;
    std::vector<Trade> out;
    submit(e, limit(1, Side::Sell, 101, 50));
    submit(e, limit(2, Side::Buy, 99, 20));

    ASSERT_TRUE(e.modify(2, 101, 20, out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].quantity, 20u);
    EXPECT_FALSE(e.book().contains(2));
    EXPECT_FALSE(e.cancel(2));
    EXPECT_EQ(depth(e), 1u);
}

TEST(Modify, WorksOnTheSellSide) {
    MatchingEngine e;
    std::vector<Trade> out;
    submit(e, limit(1, Side::Buy, 100, 10));
    submit(e, limit(2, Side::Sell, 105, 10));

    ASSERT_TRUE(e.modify(2, 100, 10, out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].price, 100);
    EXPECT_EQ(depth(e), 0u);
}

TEST(Modify, QuantityMeansWhatIsLeftNotTheOriginalSize) {
    MatchingEngine e;
    std::vector<Trade> out;
    submit(e, limit(1, Side::Buy, 100, 100));
    submit(e, limit(2, Side::Sell, 100, 60));  // 40 left on order 1

    // Asking for 50 is an increase relative to the 40 that remain.
    submit(e, limit(3, Side::Buy, 100, 5));
    ASSERT_TRUE(e.modify(1, 100, 50, out));
    const auto q = level(e, Side::Buy, 100);
    ASSERT_EQ(q.size(), 2u);
    EXPECT_EQ(q[0].id, 3u) << "growing from 40 to 50 forfeits priority";
    EXPECT_EQ(q[1].id, 1u);
    EXPECT_EQ(q[1].quantity, 50u);
}

TEST(Modify, KeepsTheIdLiveAndCancellable) {
    MatchingEngine e;
    std::vector<Trade> out;
    submit(e, limit(1, Side::Buy, 100, 10));
    ASSERT_TRUE(e.modify(1, 98, 12, out));
    EXPECT_TRUE(e.book().contains(1));
    std::vector<Trade> dup;
    EXPECT_FALSE(e.submit(limit(1, Side::Buy, 97, 5), dup)) << "id is still taken";
    EXPECT_TRUE(e.cancel(1));
    EXPECT_EQ(depth(e), 0u);
    EXPECT_FALSE(e.book().contains(1));
}

TEST(Modify, FailedModifyLeavesTheOutputBufferAlone) {
    MatchingEngine e;
    std::vector<Trade> out{Trade{999, 999, 1, 1}};
    EXPECT_FALSE(e.modify(7, 100, 10, out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].aggressor_id, 999u);
}

TEST(Modify, CannotModifyAnOrderThatAlreadyFilled) {
    MatchingEngine e;
    std::vector<Trade> out;
    submit(e, limit(1, Side::Buy, 100, 10));
    submit(e, limit(2, Side::Sell, 100, 10));
    EXPECT_FALSE(e.modify(1, 100, 5, out));
}
