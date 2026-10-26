// Structural invariants: copy/move safety and index consistency under churn.

#include <gtest/gtest.h>

#include <random>
#include <type_traits>
#include <utility>
#include <vector>

#include "test_util.hpp"

using namespace lob;
using namespace lob::testing;

// The id index stores iterators into this book's own queues, so a copy would
// alias the source's nodes (cancelling through the copy corrupts the original
// and leaves it with dangling iterators). Copying must not compile.
static_assert(!std::is_copy_constructible_v<OrderBook>);
static_assert(!std::is_copy_assignable_v<OrderBook>);
static_assert(!std::is_copy_constructible_v<MatchingEngine>);
static_assert(!std::is_copy_assignable_v<MatchingEngine>);
static_assert(std::is_move_constructible_v<OrderBook>);
static_assert(std::is_move_constructible_v<MatchingEngine>);

TEST(Move, MovedEngineKeepsWorkingIndexAndBook) {
    MatchingEngine a;
    for (OrderId id = 1; id <= 5; ++id) submit(a, limit(id, Side::Buy, 100, 10));

    MatchingEngine b = std::move(a);
    EXPECT_EQ(depth(b), 5u);
    EXPECT_TRUE(b.book().contains(3));
    EXPECT_TRUE(b.cancel(3)) << "stored iterators must still be valid after a move";
    EXPECT_EQ(depth(b), 4u);

    const auto trades = submit(b, limit(99, Side::Sell, 100, 40));
    EXPECT_EQ(trades.size(), 4u);
    EXPECT_EQ(depth(b), 0u);
}

TEST(Move, MoveAssignmentKeepsWorkingIndexAndBook) {
    MatchingEngine a;
    submit(a, limit(1, Side::Sell, 100, 10));
    submit(a, limit(2, Side::Sell, 100, 10));

    MatchingEngine b;
    submit(b, limit(50, Side::Buy, 10, 1));
    b = std::move(a);

    EXPECT_FALSE(b.book().contains(50)) << "old contents replaced";
    EXPECT_TRUE(b.cancel(1));
    EXPECT_TRUE(b.cancel(2));
    EXPECT_EQ(depth(b), 0u);
}

TEST(CapacityHint, DoesNotChangeBehaviour) {
    MatchingEngine plain;
    MatchingEngine hinted(100'000);

    for (OrderId id = 1; id <= 50; ++id) {
        const Order o = limit(id, id % 2 ? Side::Buy : Side::Sell,
                              id % 2 ? 100 - static_cast<Price>(id % 5)
                                     : 100 + static_cast<Price>(id % 5), 7);
        EXPECT_EQ(plain.submit(o).size(), hinted.submit(o).size());
    }
    EXPECT_EQ(depth(plain), depth(hinted));
    EXPECT_EQ(plain.book().best_bid(), hinted.book().best_bid());
    EXPECT_EQ(plain.book().best_ask(), hinted.book().best_ask());
}

// The index and the book must never disagree, however orders enter and leave.
TEST(IndexConsistency, ContainsMatchesTheBookAfterHeavyChurn) {
    MatchingEngine e;
    std::mt19937_64 rng(7);
    std::uniform_int_distribution<int> pct(0, 99), px(98, 102), qty(1, 9), ids(1, 120);

    for (int step = 0; step < 20'000; ++step) {
        const int r = pct(rng);
        const auto id = static_cast<OrderId>(ids(rng));
        if (r < 30) {
            e.cancel(id);
        } else if (r < 45) {
            e.submit(market(id, pct(rng) < 50 ? Side::Buy : Side::Sell,
                            static_cast<Quantity>(qty(rng))));
        } else {
            e.submit(limit(id, pct(rng) < 50 ? Side::Buy : Side::Sell, px(rng),
                           static_cast<Quantity>(qty(rng))));
        }

        // Every order in the book must be indexed, and nothing else may be.
        std::size_t in_book = 0;
        for (const auto& [p, q] : e.book().bids()) {
            EXPECT_FALSE(q.empty()) << "empty bid level left behind";
            for (const auto& o : q) { ++in_book; ASSERT_TRUE(e.book().contains(o.id)); }
        }
        for (const auto& [p, q] : e.book().asks()) {
            EXPECT_FALSE(q.empty()) << "empty ask level left behind";
            for (const auto& o : q) { ++in_book; ASSERT_TRUE(e.book().contains(o.id)); }
        }
        std::size_t indexed = 0;
        for (OrderId i = 1; i <= 120; ++i) indexed += e.book().contains(i) ? 1 : 0;
        ASSERT_EQ(indexed, in_book) << "stale index entry at step " << step;
    }
}
