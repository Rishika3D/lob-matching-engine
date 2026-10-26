// Differential fuzz test: the real engine versus a deliberately naive model.
//
// The model keeps every resting order in one flat vector and finds the best
// match by linear search over (price, arrival sequence). It is O(n) per match
// and has no cleverness to get wrong, so it serves as an executable
// specification of price-time priority. Random order flow is fed to both and
// they must agree on every accept/reject, every trade, and the full book
// contents after every operation.
//
// The flow is built to hit the awkward paths: a narrow price band so limit
// orders cross constantly, a small id range so duplicate and reused ids are
// common, and a few zero-quantity orders.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "lob/matching_engine.hpp"

using namespace lob;

namespace {

struct ModelOrder {
    OrderId id;
    Side side;
    Price price;
    Quantity qty;
    std::uint64_t seq;  // arrival order; lower = earlier = higher time priority
};

class Model {
public:
    bool submit(Order o, std::vector<Trade>& out) {
        if (o.quantity == 0) return false;
        if (live(o.id)) return false;

        while (o.quantity > 0) {
            ModelOrder* best = best_opposite(o);
            if (best == nullptr) break;
            const Quantity fill = std::min(o.quantity, best->qty);
            out.push_back(Trade{o.id, best->id, best->price, fill});
            o.quantity -= fill;
            best->qty -= fill;
            if (best->qty == 0) {
                resting_.erase(resting_.begin() + (best - resting_.data()));
            }
        }
        if (o.quantity > 0 && o.type == OrderType::Limit) {
            resting_.push_back({o.id, o.side, o.price, o.quantity, next_seq_++});
        }
        return true;
    }

    bool cancel(OrderId id) {
        for (auto it = resting_.begin(); it != resting_.end(); ++it) {
            if (it->id == id) {
                resting_.erase(it);
                return true;
            }
        }
        return false;
    }

    bool live(OrderId id) const {
        return std::any_of(resting_.begin(), resting_.end(),
                           [&](const ModelOrder& r) { return r.id == id; });
    }

    // Book contents in priority order: bids best-first, then asks best-first.
    std::vector<ModelOrder> sorted(Side side) const {
        std::vector<ModelOrder> v;
        for (const auto& r : resting_) {
            if (r.side == side) v.push_back(r);
        }
        std::sort(v.begin(), v.end(), [&](const ModelOrder& a, const ModelOrder& b) {
            if (a.price != b.price) {
                return side == Side::Buy ? a.price > b.price : a.price < b.price;
            }
            return a.seq < b.seq;
        });
        return v;
    }

private:
    // Best resting order on the opposite side that `o` is willing to trade
    // with: best price first, earliest arrival within a price.
    ModelOrder* best_opposite(const Order& o) {
        ModelOrder* best = nullptr;
        for (auto& r : resting_) {
            if (r.side == o.side) continue;
            if (o.type == OrderType::Limit) {
                const bool ok = (o.side == Side::Buy) ? (o.price >= r.price)
                                                      : (o.price <= r.price);
                if (!ok) continue;
            }
            if (best == nullptr) { best = &r; continue; }
            const bool better_price = (o.side == Side::Buy) ? (r.price < best->price)
                                                            : (r.price > best->price);
            if (better_price || (r.price == best->price && r.seq < best->seq)) {
                best = &r;
            }
        }
        return best;
    }

    std::vector<ModelOrder> resting_;
    std::uint64_t next_seq_ = 0;
};

// Flatten the engine's book into the same shape as Model::sorted().
template <typename Levels>
std::vector<ModelOrder> flatten(const Levels& levels, Side side) {
    std::vector<ModelOrder> v;
    for (const auto& [price, queue] : levels) {
        for (const auto& o : queue) v.push_back({o.id, side, o.price, o.quantity, 0});
    }
    return v;
}

bool same_book(const std::vector<ModelOrder>& a, const std::vector<ModelOrder>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].price != b[i].price || a[i].qty != b[i].qty) {
            return false;
        }
    }
    return true;
}

bool same_trades(const std::vector<Trade>& a, const std::vector<Trade>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].aggressor_id != b[i].aggressor_id || a[i].resting_id != b[i].resting_id ||
            a[i].price != b[i].price || a[i].quantity != b[i].quantity) {
            return false;
        }
    }
    return true;
}

// Runs one seeded session. Returns the step at which the engine and the model
// first disagreed, or -1 if they agreed throughout.
int run_session(std::uint64_t seed, int steps, const char** why) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<int> price_d(95, 105);   // narrow band: lots of crossing
    std::uniform_int_distribution<int> qty_d(1, 20);
    std::uniform_int_distribution<int> id_d(1, 250);       // small range: duplicates and reuse

    MatchingEngine engine;
    Model model;
    std::vector<Trade> got, want;

    for (int step = 0; step < steps; ++step) {
        const int r = pct(rng);
        if (r < 25) {
            const OrderId id = static_cast<OrderId>(id_d(rng));
            if (engine.cancel(id) != model.cancel(id)) { *why = "cancel result differs"; return step; }
        } else {
            Order o;
            o.id = static_cast<OrderId>(id_d(rng));
            o.side = (pct(rng) < 50) ? Side::Buy : Side::Sell;
            o.quantity = (r >= 97) ? 0 : static_cast<Quantity>(qty_d(rng));
            if (r >= 80 && r < 90) {
                o.type = OrderType::Market;
            } else {
                o.type = OrderType::Limit;
                o.price = price_d(rng);
            }
            got.clear();
            want.clear();
            const bool a = engine.submit(o, got);
            const bool b = model.submit(o, want);
            if (a != b) { *why = "accept/reject differs"; return step; }
            if (!same_trades(got, want)) { *why = "trades differ"; return step; }
        }

        if (!same_book(flatten(engine.book().bids(), Side::Buy), model.sorted(Side::Buy))) {
            *why = "bid book differs";
            return step;
        }
        if (!same_book(flatten(engine.book().asks(), Side::Sell), model.sorted(Side::Sell))) {
            *why = "ask book differs";
            return step;
        }
        const auto bids = model.sorted(Side::Buy);
        const auto asks = model.sorted(Side::Sell);

        // The index must agree with the book for every id in play.
        bool live[251] = {};
        for (const auto& resting : bids) live[resting.id] = true;
        for (const auto& resting : asks) live[resting.id] = true;
        for (OrderId id = 1; id <= 250; ++id) {
            if (engine.book().contains(id) != live[id]) {
                *why = "index disagrees with book";
                return step;
            }
        }
        // Best bid/ask accessors must agree with the book.
        if (engine.book().best_bid().has_value() != !bids.empty() ||
            (!bids.empty() && *engine.book().best_bid() != bids.front().price)) {
            *why = "best_bid differs";
            return step;
        }
        if (engine.book().best_ask().has_value() != !asks.empty() ||
            (!asks.empty() && *engine.book().best_ask() != asks.front().price)) {
            *why = "best_ask differs";
            return step;
        }
        // A crossed book means a trade was missed.
        if (!bids.empty() && !asks.empty() && bids.front().price >= asks.front().price) {
            *why = "book is crossed";
            return step;
        }
    }
    return -1;
}

} // namespace

// 100 seeded sessions x 1000 steps. Each seed is its own TEST_P-style check so
// a failure names the exact seed and step to replay.
TEST(Differential, AgreesWithTheReferenceModelOnRandomOrderFlow) {
    constexpr int kSeeds = 100;
    constexpr int kSteps = 1000;

    for (int s = 1; s <= kSeeds; ++s) {
        const char* why = "";
        const int at = run_session(static_cast<std::uint64_t>(s), kSteps, &why);
        ASSERT_EQ(at, -1) << "seed=" << s << " first disagreement at step " << at
                          << ": " << why;
    }
}
