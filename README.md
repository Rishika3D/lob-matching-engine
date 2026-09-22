# LOB Matching Engine

A limit order book and price-time priority matching engine in C++20 — the core
of an exchange, and the foundation for a tick-level backtester.

## What it does

Models how an exchange like NASDAQ actually matches orders:

- **Price-time priority** — best price wins; ties broken FIFO by arrival order
- **Book walking** — an aggressive order consumes opposite-side levels best-price-first until filled
- **Limit and market orders** — unfilled limit remainder rests in the book, market remainder is discarded
- **Price improvement** — trades execute at the *resting* order's price, not the aggressor's
- **Cancellation** by order ID

## Design notes

**Integer tick prices.** Prices are `int64_t` ticks, not doubles. With a $0.01
tick, $100.05 is stored as `10005`. Floating point equality on prices is a
correctness bug waiting to happen — `0.1 + 0.2 != 0.3` breaks price matching.

**Sorted price levels.** `std::map<Price, std::deque<Order>>`, with bids using
`std::greater` so `begin()` is the best price on both sides. Best bid/ask is
O(1); insert and level lookup are O(log n) over a small number of levels.

**Order index.** A `std::unordered_map<OrderId, {side, price}>` so cancels jump
straight to the right price level instead of scanning the book.

## Build

```bash
cmake -DCMAKE_BUILD_TYPE=Release -B build -S .
cmake --build build
./build/lob_demo    # worked example
./build/lob_bench   # throughput + latency benchmark
```

Requires CMake 3.20+ and a C++20 compiler.

## Benchmark

1M operations (90% new orders, 10% cancels), fixed RNG seed, Apple M2,
Release `-O2`:

| Metric | Value |
|---|---|
| Throughput | 1.15 M ops/sec |
| Latency p50 | 125 ns |
| Latency p99 | 16,291 ns |
| Latency p99.9 | 24,208 ns |
| Trades generated | 219,688 |
| Shares filled | 27,555,774 |

### Known issue: p99 is 130× p50

This is a real structural limitation, not measurement noise.

The benchmark rests ~900k orders across ~40 price levels, so each level's
`deque` holds roughly 22,000 orders. `OrderBook::cancel` finds the price level
in O(1) via the index, but then **linearly scans that level's queue** to find
the order — O(n) in the queue depth. Deep queues make cancels the tail latency.

The fix is to hold each level in a `std::list` and store a
`std::list::iterator` in the order index, making cancel genuinely O(1). Real
exchange engines do exactly this. Tracked as the next change.

## Status

Working core engine with a reproducible benchmark. Not yet built:

- [ ] O(1) cancel via stored list iterators (see above)
- [ ] Unit tests (GoogleTest via `FetchContent`)
- [ ] IOC / FOK / stop order types
- [ ] Market data ingestion (ITCH parsing or CSV replay)
- [ ] Backtester layer — strategy interface, queue-position fill simulation, P&L

## Layout

```
include/lob/     order.hpp, order_book.hpp, matching_engine.hpp
src/             order_book.cpp, matching_engine.cpp, main.cpp
bench/           benchmark.cpp
```
