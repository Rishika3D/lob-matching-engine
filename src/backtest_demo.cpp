// Runs the backtester on a CSV order-flow file, or on a synthetic feed, with a
// small example strategy that quotes both sides at the touch.
//
//   lob_backtest events.csv
//   lob_backtest                       # 200,000 synthetic events
//   lob_backtest --synthetic 1000000

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "lob/backtest.hpp"
#include "lob/event_io.hpp"

using namespace lob;

namespace {

// Keeps one bid and one ask resting at the current best prices, re-quoting when
// the touch moves, and stops quoting a side once the position limit is reached.
class TouchQuoter : public Strategy {
public:
    TouchQuoter(Quantity size, std::int64_t max_position)
        : size_(size), max_position_(max_position) {}

    void on_event(const MarketEvent&, Context& ctx) override {
        const auto bid = ctx.book().best_bid();
        const auto ask = ctx.book().best_ask();
        if (!bid || !ask) return;
        requote(bid_, Side::Buy, *bid, ctx.position() < max_position_, ctx);
        requote(ask_, Side::Sell, *ask, ctx.position() > -max_position_, ctx);
    }

private:
    void requote(std::optional<OrderId>& slot, Side side, Price want, bool allowed,
                 Context& ctx) {
        if (slot) {
            const Order* resting = ctx.book().find(*slot);
            if (resting != nullptr && resting->price == want && allowed) return;  // still good
            if (resting != nullptr) ctx.cancel(*slot);
            slot.reset();
        }
        if (allowed) slot = ctx.submit(side, want, size_);
    }

    Quantity size_;
    std::int64_t max_position_;
    std::optional<OrderId> bid_, ask_;
};

// A random-walk order flow: passive quotes around a drifting mid, some market
// orders, some marketable limit orders, and cancels of recently placed orders.
std::vector<MarketEvent> synthesize(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> pct(0, 99), away(1, 6), qty(1, 50);
    std::vector<MarketEvent> events;
    events.reserve(count);

    std::vector<OrderId> recent;
    Price mid = 10000;
    OrderId next_id = 1;

    for (std::size_t i = 0; i < count; ++i) {
        if (pct(rng) < 8) mid += (pct(rng) < 50) ? 1 : -1;

        MarketEvent ev;
        ev.timestamp = static_cast<std::int64_t>(i) * 1000;
        const int r = pct(rng);
        if (r < 20 && !recent.empty()) {
            ev.kind = EventKind::Cancel;
            ev.order.id = recent[static_cast<std::size_t>(rng() % recent.size())];
        } else {
            ev.kind = EventKind::NewOrder;
            ev.order.id = next_id++;
            ev.order.side = (pct(rng) < 50) ? Side::Buy : Side::Sell;
            ev.order.quantity = static_cast<Quantity>(qty(rng));
            if (r < 32) {
                ev.order.type = OrderType::Market;
            } else if (r < 42) {  // marketable limit: crosses the mid by a tick or two
                ev.order.type = OrderType::Limit;
                ev.order.price = ev.order.side == Side::Buy ? mid + 1 : mid - 1;
            } else {              // passive quote
                ev.order.type = OrderType::Limit;
                const Price d = away(rng);
                ev.order.price = ev.order.side == Side::Buy ? mid - d : mid + d;
                recent.push_back(ev.order.id);
                if (recent.size() > 2000) recent.erase(recent.begin());
            }
        }
        events.push_back(ev);
    }
    return events;
}

void print(const BacktestResult& r, double seconds) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "events replayed      : " << r.events << "  ("
              << static_cast<double>(r.events) / seconds / 1e6 << " M/s)\n";
    std::cout << "trades in the book   : " << r.trades << "\n";
    std::cout << "strategy orders      : " << r.strategy_orders << " accepted, "
              << r.strategy_rejects << " rejected\n";
    std::cout << "strategy fills       : " << r.fills.size() << "\n";
    std::cout << "position (shares)    : " << r.position << "   max |position| "
              << r.max_abs_position << "\n";
    std::cout << "cash (ticks x shares): " << r.cash << "\n";
    if (r.mark) std::cout << "mark price           : " << *r.mark << " ticks\n";
    if (r.pnl) std::cout << "P&L (ticks x shares) : " << *r.pnl << "\n";
    else std::cout << "P&L                  : n/a (open position with nothing to mark it to)\n";
    std::cout << "historical rejects   : " << r.historical_rejects << "\n";
    std::cout << "historical cancel misses: " << r.historical_cancel_misses
              << "  (cancels of orders no longer resting)\n";
}

} // namespace

int main(int argc, char** argv) {
    std::vector<MarketEvent> events;
    std::string source;

    if (argc >= 2 && std::string(argv[1]) != "--synthetic") {
        std::ifstream in(argv[1]);
        if (!in) {
            std::cerr << "cannot open " << argv[1] << "\n";
            return 1;
        }
        ParseResult parsed = parse_events(in);
        if (!parsed.ok()) {
            std::cerr << argv[1] << ": " << parsed.error << "\n";
            return 1;
        }
        events = std::move(parsed.events);
        source = argv[1];
    } else {
        std::size_t n = 200'000;
        if (argc >= 3) n = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
        events = synthesize(n, 42);
        source = "synthetic feed (seed 42)";
    }

    std::cout << "source: " << source << " -- " << events.size() << " events\n\n";

    TouchQuoter strategy(/*size=*/10, /*max_position=*/200);
    Backtester backtester(events.size());

    const auto start = std::chrono::steady_clock::now();
    const BacktestResult result = backtester.run(events, strategy);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    if (!result.ok()) {
        std::cerr << "backtest failed: " << result.error << "\n";
        return 1;
    }
    print(result, seconds);
    return 0;
}
