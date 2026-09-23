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

**Sorted price levels.** `std::map<Price, std::list<Order>>`, with bids using
`std::greater` so `begin()` is the best price on both sides. Best bid/ask is
O(1); insert and level lookup are O(log n) over a small number of levels.

**O(1) cancel.** The order index is
`unordered_map<OrderId, {side, price, list::iterator}>`. Storing the list
iterator means a cancel erases the order directly rather than scanning its
price level — which matters, because real books build queues thousands of
orders deep at the touch. List iterators stay valid as the level grows and
shrinks around them, which a `deque` cannot promise.

**Allocation-free hot path.** `submit(order, out)` appends trades to a
caller-owned buffer, so a steady order flow does no per-order heap allocation.
A convenience overload returning a fresh vector is kept for non-hot callers.

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
| Throughput | **5.81 M ops/sec** |
| Latency p50 | **83 ns** |
| Latency p99 | **625 ns** |
| Latency p99.9 | 1,375 ns |
| Message throughput | 144 MB/s (at 26 B/msg) |
| Trades generated | 219,688 |
| Shares filled | 27,555,774 |

### How it got there

Two optimizations, each measured against the same fixed-seed workload. Trade
counts were byte-identical across all three runs, so the speedups changed cost,
not behavior.

| | Throughput | p50 | p99 | p99.9 |
|---|---|---|---|---|
| Baseline (`deque` levels, returned vector) | 1.15 M/s | 125 ns | 16,291 ns | 24,208 ns |
| + O(1) cancel via list iterators | 4.45 M/s | 83 ns | 875 ns | 2,125 ns |
| + allocation-free submit | **5.81 M/s** | **83 ns** | **625 ns** | **1,375 ns** |
| | **5.1×** | **1.5×** | **26×** | **17.6×** |

**The tail was the real story.** The baseline's p99 was 130× its p50 — not
noise, but the O(n) cancel scan. The benchmark rests ~900k orders across ~40
price levels, so each level's queue held roughly 22,000 orders and every cancel
walked it. Storing a list iterator in the order index removed the scan and cut
p99 by 26×. Removing the per-order vector allocation took another 29% off
throughput cost.

Reported `max` (~9 ms) is a scheduler artifact: single operations are timed on
a non-isolated core under a non-realtime OS, so the worst sample catches
preemption rather than engine work. p99.9 is the meaningful tail figure.

## Status

Working core engine with a reproducible benchmark. Not yet built:

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
