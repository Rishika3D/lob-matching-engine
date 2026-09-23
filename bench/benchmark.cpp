// Throughput and latency benchmark for the matching engine.
//
// Measures per-order submit latency over a synthetic order flow that keeps a
// realistic book shape: mostly resting limit orders near the touch, with a
// minority of aggressive orders that cross the spread and generate trades.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

#include "lob/matching_engine.hpp"

using namespace lob;
using Clock = std::chrono::steady_clock;

namespace {

struct Stats {
    double p50_ns = 0;
    double p99_ns = 0;
    double p999_ns = 0;
    double max_ns = 0;
};

Stats percentiles(std::vector<double>& samples) {
    std::sort(samples.begin(), samples.end());
    auto at = [&](double q) {
        const auto idx = static_cast<std::size_t>(q * (samples.size() - 1));
        return samples[idx];
    };
    return Stats{at(0.50), at(0.99), at(0.999), samples.back()};
}

} // namespace

int main() {
    constexpr int kWarmup = 100'000;
    constexpr int kOrders = 1'000'000;
    constexpr Price kMidPrice = 10000;   // $100.00 in ticks
    constexpr Price kHalfSpread = 5;     // levels spread around the mid

    std::mt19937_64 rng(42);  // fixed seed: runs are reproducible
    std::uniform_int_distribution<int> level_dist(0, 20);
    std::uniform_int_distribution<int> qty_dist(1, 500);
    std::uniform_int_distribution<int> action_dist(0, 99);

    MatchingEngine engine;
    OrderId next_id = 1;
    std::vector<OrderId> resting;  // ids we may later cancel
    resting.reserve(kOrders);

    std::uint64_t total_trades = 0;
    std::uint64_t total_filled_qty = 0;
    std::uint64_t cancels_ok = 0;

    std::vector<double> latencies;
    latencies.reserve(kOrders);

    std::vector<Trade> trade_buf;  // reused across every submit
    trade_buf.reserve(64);

    auto step = [&](bool record) {
        const int action = action_dist(rng);

        // 10% cancels of a previously resting order.
        if (action < 10 && !resting.empty()) {
            std::uniform_int_distribution<std::size_t> pick(0, resting.size() - 1);
            const OrderId id = resting[pick(rng)];
            const auto t0 = Clock::now();
            const bool ok = engine.cancel(id);
            const auto t1 = Clock::now();
            if (record) {
                latencies.push_back(
                    std::chrono::duration<double, std::nano>(t1 - t0).count());
            }
            cancels_ok += ok ? 1 : 0;
            return;
        }

        Order order;
        order.id = next_id++;
        order.side = (action % 2 == 0) ? Side::Buy : Side::Sell;
        order.quantity = static_cast<Quantity>(qty_dist(rng));

        if (action >= 90) {
            // 10% marketable orders that cross and generate trades.
            order.type = OrderType::Market;
        } else {
            order.type = OrderType::Limit;
            const Price offset = kHalfSpread + level_dist(rng);
            order.price = (order.side == Side::Buy) ? kMidPrice - offset
                                                    : kMidPrice + offset;
            resting.push_back(order.id);
        }

        trade_buf.clear();  // reuse the buffer: no per-order allocation
        const auto t0 = Clock::now();
        engine.submit(order, trade_buf);
        const auto t1 = Clock::now();
        if (record) {
            latencies.push_back(
                std::chrono::duration<double, std::nano>(t1 - t0).count());
        }

        total_trades += trade_buf.size();
        for (const auto& t : trade_buf) {
            total_filled_qty += t.quantity;
        }
    };

    for (int i = 0; i < kWarmup; ++i) {
        step(/*record=*/false);
    }

    const auto wall_start = Clock::now();
    for (int i = 0; i < kOrders; ++i) {
        step(/*record=*/true);
    }
    const auto wall_end = Clock::now();

    const double wall_sec =
        std::chrono::duration<double>(wall_end - wall_start).count();
    const Stats s = percentiles(latencies);

    // Wire-size equivalent: each order/cancel message carries id, side, type,
    // price and quantity -- 26 bytes in a packed binary encoding.
    constexpr double kMsgBytes = 26.0;
    const double mb_per_sec = (kOrders * kMsgBytes) / wall_sec / (1024 * 1024);

    std::size_t bid_levels = engine.book().bids().size();
    std::size_t ask_levels = engine.book().asks().size();

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "=== LOB Matching Engine Benchmark ===\n";
    std::cout << "operations         : " << kOrders << " (90% new orders, 10% cancels)\n";
    std::cout << "wall time          : " << wall_sec << " s\n";
    std::cout << "throughput         : " << (kOrders / wall_sec / 1e6)
              << " M ops/sec\n";
    std::cout << "message throughput : " << mb_per_sec << " MB/s (at "
              << kMsgBytes << " B/msg)\n";
    std::cout << "\n--- latency per operation (ns) ---\n";
    std::cout << "p50                : " << s.p50_ns << "\n";
    std::cout << "p99                : " << s.p99_ns << "\n";
    std::cout << "p99.9              : " << s.p999_ns << "\n";
    std::cout << "max                : " << s.max_ns << "\n";
    std::cout << "\n--- matching activity ---\n";
    std::cout << "trades generated   : " << total_trades << "\n";
    std::cout << "shares filled      : " << total_filled_qty << "\n";
    std::cout << "cancels succeeded  : " << cancels_ok << "\n";
    std::cout << "final book depth   : " << bid_levels << " bid levels, "
              << ask_levels << " ask levels\n";

    return 0;
}
