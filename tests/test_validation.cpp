// Input validation: duplicate live ids and zero-quantity orders are rejected
// before they can touch the book, and a rejection leaves everything intact.

#include <gtest/gtest.h>

#include <vector>

#include "test_util.hpp"

using namespace lob;
using namespace lob::testing;

// --- duplicate order ids -------------------------------------------------------

TEST(DuplicateId, IsRejectedAndTheOriginalSurvivesIntact) {
    MatchingEngine e;
    std::vector<Trade> out;

    ASSERT_TRUE(e.submit(limit(7, Side::Buy, 100, 10), out));
    EXPECT_FALSE(e.submit(limit(7, Side::Buy, 200, 999), out));

    EXPECT_TRUE(out.empty());
    EXPECT_EQ(depth(e), 1u) << "the duplicate must not rest";
    EXPECT_EQ(e.book().bids().count(200), 0u) << "no level created at the rejected price";

    const auto q = level(e, Side::Buy, 100);
    ASSERT_EQ(q.size(), 1u);
    EXPECT_EQ(q[0].quantity, 10u) << "original quantity unchanged";

    EXPECT_TRUE(e.book().contains(7));
    EXPECT_TRUE(e.cancel(7)) << "original stays cancellable";
    EXPECT_EQ(depth(e), 0u);
    EXPECT_FALSE(e.book().contains(7)) << "no stale index entry left behind";
}

TEST(DuplicateId, AtAnotherPriceLevelDoesNotOrphanTheOriginal) {
    MatchingEngine e;
    std::vector<Trade> out;
    e.submit(limit(7, Side::Buy, 100, 10), out);
    EXPECT_FALSE(e.submit(limit(7, Side::Buy, 200, 10), out));

    EXPECT_TRUE(e.cancel(7));
    EXPECT_FALSE(e.cancel(7)) << "there is no second order behind it";
    EXPECT_EQ(depth(e), 0u) << "nothing stranded in the book";
}

TEST(DuplicateId, AcrossSidesIsStillRejected) {
    MatchingEngine e;
    std::vector<Trade> out;
    e.submit(limit(7, Side::Buy, 100, 10), out);
    EXPECT_FALSE(e.submit(limit(7, Side::Sell, 105, 10), out));
    EXPECT_EQ(depth(e), 1u);
}

TEST(DuplicateId, FillingTheOriginalDoesNotStrandAnything) {
    MatchingEngine e;
    std::vector<Trade> out;
    e.submit(limit(7, Side::Buy, 200, 10), out);
    EXPECT_FALSE(e.submit(limit(7, Side::Buy, 100, 10), out));

    out.clear();
    e.submit(limit(8, Side::Sell, 200, 10), out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].quantity, 10u);

    EXPECT_EQ(depth(e), 0u);
    EXPECT_FALSE(e.book().contains(7));
    EXPECT_FALSE(e.cancel(7));
}

TEST(DuplicateId, IsReusableOnceTheOriginalIsCancelled) {
    MatchingEngine e;
    std::vector<Trade> out;
    e.submit(limit(7, Side::Buy, 100, 10), out);
    ASSERT_TRUE(e.cancel(7));
    EXPECT_TRUE(e.submit(limit(7, Side::Buy, 100, 10), out));
    EXPECT_EQ(depth(e), 1u);
}

TEST(DuplicateId, IsReusableOnceTheOriginalIsFilled) {
    MatchingEngine e;
    std::vector<Trade> out;
    e.submit(limit(7, Side::Buy, 100, 10), out);
    e.submit(limit(8, Side::Sell, 100, 10), out);  // fills #7
    ASSERT_FALSE(e.book().contains(7));
    EXPECT_TRUE(e.submit(limit(7, Side::Buy, 100, 5), out));
    EXPECT_EQ(depth(e), 1u);
}

TEST(DuplicateId, MarketOrderMayNotReuseALiveId) {
    MatchingEngine e;
    std::vector<Trade> out;
    e.submit(limit(1, Side::Sell, 100, 50), out);
    e.submit(limit(7, Side::Buy, 90, 10), out);

    out.clear();
    EXPECT_FALSE(e.submit(market(7, Side::Buy, 10), out));
    EXPECT_TRUE(out.empty()) << "a rejected market order must not trade";
    EXPECT_EQ(front_quantity(e, Side::Sell, 100), 50u);
}

TEST(DuplicateId, RejectedCrossingOrderDoesNotTrade) {
    MatchingEngine e;
    std::vector<Trade> out;
    e.submit(limit(1, Side::Buy, 100, 10), out);
    e.submit(limit(2, Side::Sell, 105, 10), out);

    // Same id as a live order AND it would cross: validation runs first.
    EXPECT_FALSE(e.submit(limit(1, Side::Sell, 100, 10), out));
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(depth(e), 2u);
}

TEST(DuplicateId, RejectionLeavesTheOutputBufferUntouched) {
    MatchingEngine e;
    std::vector<Trade> out;
    e.submit(limit(1, Side::Buy, 100, 10), out);

    out.push_back(Trade{999, 999, 1, 1});
    EXPECT_FALSE(e.submit(limit(1, Side::Buy, 100, 10), out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].aggressor_id, 999u);
}

TEST(DuplicateId, ReturningOverloadCannotDistinguishRejectionFromNoTrades) {
    // Documented limitation of the convenience overload: both outcomes are an
    // empty vector. Callers who need the signal use the buffer overload.
    MatchingEngine e;
    e.submit(limit(1, Side::Buy, 100, 10));
    EXPECT_TRUE(e.submit(limit(1, Side::Buy, 100, 10)).empty());
    EXPECT_EQ(depth(e), 1u);
}

// --- zero quantity ---------------------------------------------------------------

TEST(ZeroQuantity, LimitOrderIsRejectedNotSilentlySwallowed) {
    MatchingEngine e;
    std::vector<Trade> out;

    EXPECT_FALSE(e.submit(limit(1, Side::Buy, 100, 0), out));
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(depth(e), 0u);
    EXPECT_TRUE(e.book().bids().empty()) << "no level created";
    EXPECT_FALSE(e.book().contains(1)) << "no index entry created";
    EXPECT_FALSE(e.cancel(1));
}

TEST(ZeroQuantity, MarketOrderIsRejected) {
    MatchingEngine e;
    std::vector<Trade> out;
    e.submit(limit(1, Side::Sell, 100, 50), out);

    EXPECT_FALSE(e.submit(market(2, Side::Buy, 0), out));
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(depth(e), 1u);
}

TEST(ZeroQuantity, CrossingPriceDoesNotDisturbTheBook) {
    MatchingEngine e;
    std::vector<Trade> out;
    e.submit(limit(1, Side::Sell, 100, 50), out);

    EXPECT_FALSE(e.submit(limit(2, Side::Buy, 100, 0), out));
    EXPECT_TRUE(out.empty());
    const auto q = level(e, Side::Sell, 100);
    ASSERT_EQ(q.size(), 1u);
    EXPECT_EQ(q[0].quantity, 50u);
    EXPECT_TRUE(e.book().contains(1));
}

TEST(ZeroQuantity, RejectionDoesNotConsumeTheId) {
    MatchingEngine e;
    std::vector<Trade> out;
    EXPECT_FALSE(e.submit(limit(7, Side::Buy, 100, 0), out));
    EXPECT_TRUE(e.submit(limit(7, Side::Buy, 100, 10), out));
    EXPECT_TRUE(e.cancel(7));
}

TEST(ZeroQuantity, RejectionLeavesTheOutputBufferUntouched) {
    MatchingEngine e;
    std::vector<Trade> out;
    out.push_back(Trade{999, 999, 1, 1});
    EXPECT_FALSE(e.submit(limit(1, Side::Buy, 100, 0), out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].aggressor_id, 999u);
}

// --- minimal valid quantity is still accepted ------------------------------------

TEST(ZeroQuantity, QuantityOfOneIsAccepted) {
    MatchingEngine e;
    std::vector<Trade> out;
    EXPECT_TRUE(e.submit(limit(1, Side::Buy, 100, 1), out));
    EXPECT_EQ(depth(e), 1u);
}
