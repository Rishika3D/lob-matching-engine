# LOB Matching Engine

A single-threaded limit order book and price-time priority matching engine in
C++20 — the core of an exchange, intended as the foundation for a tick-level
backtester.

## What it does

- **Price-time priority** — best price wins; ties are broken FIFO by arrival
- **Book walking** — an aggressive order consumes opposite-side levels best-price-first until filled
- **Limit and market orders** — an unfilled limit remainder rests; an unfilled market remainder is discarded
- **Price improvement** — trades execute at the *resting* order's price, not the aggressor's
- **Partial fills keep their place** — a partly filled resting order stays at the front of its queue
- **Cancel by order id**, O(1) in the size of the queue
- **Input validation** — zero-quantity orders and ids that are already live are rejected before they can touch the book

**Not implemented:** order modification (cancel/replace), IOC/FOK/stop orders,
self-trade prevention, timestamps/sequence numbers, market-data ingestion,
multi-threading, persistence. There is no backtester layer yet.

## Design

| Piece | Choice | Why |
|---|---|---|
| Prices | `int64_t` ticks (`$100.05` → `10005`) | Exact comparison; `0.1 + 0.2 != 0.3` would break crossing logic |
| Price levels | `std::map<Price, std::list<Order>>`, bids ordered by `std::greater` | `begin()` is the best price on both sides |
| Queue per level | `std::list` | Iterators stay valid across unrelated inserts/erases, which is what makes O(1) cancel possible |
| Order index | `unordered_map<OrderId, {side, price, list::iterator}>` | Cancel jumps straight to the order; also the O(1) duplicate-id check |
| Errors | `bool` returns, no exceptions | Matches `cancel()`; rejection happens before any state changes |

Source: [`order.hpp`](include/lob/order.hpp) (types),
[`order_book.hpp`](include/lob/order_book.hpp) (storage + index),
[`matching_engine.hpp`](include/lob/matching_engine.hpp) (matching policy).

### Complexity

L = price levels (about 40 in the benchmark), F = fills, K = levels consumed.

| Operation | Cost |
|---|---|
| Best bid / ask | O(1) |
| Duplicate-id check | O(1) average |
| Rest a limit order | O(log L) + 2 heap allocations (queue node, index node) |
| Match | O(F + K·log L) |
| Cancel | O(log L) level lookup + O(1) unlink |

## API notes

```cpp
lob::MatchingEngine engine;                 // or MatchingEngine engine(100'000);
std::vector<lob::Trade> trades;

bool accepted = engine.submit(order, trades);   // appends to `trades`
bool cancelled = engine.cancel(order_id);
```

**`submit(order, out&)` appends — it never clears `out`.** If you reuse a
buffer across orders and want only the current order's trades, call
`out.clear()` first (the benchmark does). Returns `false` if the order was
rejected, in which case `out` and the book are untouched.

**`submit(order)`** (convenience overload) returns a fresh vector holding only
that order's trades. It cannot tell you whether the order was rejected — a
rejection and an accepted order with no trades both return an empty vector. Use
the buffer overload when that distinction matters.

**Rejected:** `quantity == 0`, and any `id` that is currently resting in the
book. An id becomes reusable once its order is filled or cancelled. Market
orders never rest, so they are not tracked after submission.

**Capacity hint.** `MatchingEngine(n)` pre-sizes the id index for `n` live
orders. It never changes behaviour; it only avoids rehashing (see Benchmark).

**Move-only.** The index stores iterators into the book's own queues, so a
copied engine would alias the original and corrupt it. Copying does not
compile; moving is safe.

## Build and test

```bash
cmake -B build            # defaults to Release
cmake --build build
ctest --test-dir build --output-on-failure

./build/lob_demo          # worked example
./build/lob_bench         # benchmark
./build/lob_bench --reserve
```

Requires CMake 3.20+ and a C++20 compiler. GoogleTest is fetched automatically
at configure time. Options: `-DLOB_BUILD_TESTS=OFF`, `-DLOB_BUILD_BENCH=OFF`,
`-DLOB_BUILD_DEMO=OFF`, `-DLOB_SANITIZE=ON` (ASan + UBSan).

## Testing

54 GoogleTest cases in [`tests/`](tests/):

- **Behaviour** — resting, price-time priority, partial and full fills, price
  improvement, multi-level walks on both sides, market orders, all the cancel
  positions (front, middle, back, last-in-level), and both `submit` overloads
- **Validation** — duplicate ids (same level, other level, other side, after
  fill, after cancel, market orders) and zero quantity; each also checks that a
  rejection leaves the book, the index and the caller's buffer untouched
- **Integrity** — copying fails to compile, moving keeps the index valid, the
  capacity hint changes nothing, and a 20,000-step churn test checks that the
  index and the book never disagree
- **Differential** — [`test_differential.cpp`](tests/test_differential.cpp)
  runs 100 seeded sessions × 1,000 random operations against a deliberately
  naive reference model (flat vector, linear search) and requires identical
  accept/reject results, trades and book contents after every step

The tests were mutation-checked: 13 deliberate engine bugs (trade at the wrong
price, LIFO instead of FIFO, `>` instead of `>=`, dropped guards, stale index
entries, and others) were each applied in turn, and **every one was caught**.
One of them (an empty level left in the book) makes the engine loop forever, so
it surfaces as a hang rather than a failed assertion. The suite also runs
clean under AddressSanitizer and UBSan, and the build has no warnings under
`-Wall -Wextra -Wpedantic -Wshadow`.

## Benchmark

[`bench/benchmark.cpp`](bench/benchmark.cpp): one engine, 100,000 untimed
warm-up operations, then 1,000,000 timed ones. Mix: 80% limit orders, 10%
market orders, 10% cancels. RNG `mt19937_64`, seed 42, so every run does the
same work. Limit prices sit 5–25 ticks either side of a mid of 10,000; sizes
are 1–500. Each operation is timed individually with `steady_clock`.
Throughput is 1,000,000 ÷ the wall time of the whole loop.

Apple M2 (4 performance + 4 efficiency cores), Apple clang 16, Release
(`-O3 -DNDEBUG`; the benchmark file itself is `-O2`), no LTO, no core pinning.

### Results

Median of 10 interleaved runs per configuration. The three binaries alternate
within each round, so slow periods hit all of them.

| | Throughput | p50 | p99 | p99.9 |
|---|---|---|---|---|
| Baseline (`deque` queues, linear-scan cancel) | 1.06 M ops/s | 125 ns | 16,979 ns | 29,146 ns |
| Current | 3.89 M ops/s | 125 ns | 812 ns | 3,833 ns |
| **Improvement** | **3.7×** | — | **20.9×** | **7.6×** |

An earlier session on the same machine measured **4.9×** throughput and
**32.9×** p99 for the same change (median of 10 interleaved runs). The two
sessions disagree by about 30% on the throughput ratio, so the honest summary
is **roughly 4–5× throughput and 20–30× lower p99**.

Trade counts are identical across every run of every version (219,688 trades,
27,555,774 shares, 85,700 successful cancels), so the speedup changed cost, not
behaviour.

**What caused the improvement.** The baseline's cancel scanned the order's
whole price level. The benchmark leaves about 684,000 orders resting across 39
levels (average 17,500 deep), so each successful cancel walked thousands of
nodes. Storing a `list` iterator in the order index replaces that scan with a
direct unlink. A smaller part of the gain (about 1.1× throughput, measured
separately) came from reusing the trade buffer instead of allocating a result
vector on every call.

### Reading these numbers honestly

- **Absolute throughput is not stable.** The same code measured 5.8M ops/s in
  one session and about 4M in another. Low Power Mode was on and background load
  varied. Trust the ratios from interleaved runs, not the absolute figures.
- **p50 is limited by the timer.** `steady_clock` ticks in steps of about
  42 ns on this machine, and an empty timed region reads exactly 0 ns about 38%
  of the time. The 125 ns median is 3 ticks; treat it as "about 100 ns".
- **The median is the cost of resting an order, not of matching.** 80% of
  operations are limit orders that never cross (bids ≤ 9,995, asks ≥ 10,005), so
  none of them trade. Only the 10% market orders match, and 74% of those touch a
  single price level.
- **Throughput includes the harness**: RNG calls, two clock reads and a vector
  push per operation. The engine alone is faster than the figure shown.
- **About 22% of cancels target an order that is already gone**, because the
  benchmark picks from every id it ever generated.
- **Not benchmarked:** aggressive limit orders, deep multi-level walks, bursty
  or realistic order flow.

### Where the multi-millisecond spikes come from

A run's slowest single operation was 11–94 ms in the default configuration. The
slow samples land on the same iterations every run and match the sizes at which
the id index (`unordered_map`) rehashes — one insert has to move every entry.
Pre-sizing the index confirms it:

| | Median max latency | Median throughput |
|---|---|---|
| Default | 14.3 ms | 3.89 M ops/s |
| `--reserve` (index pre-sized) | **0.1 ms** | 4.51 M ops/s |

Pass an expected order count to the constructor if tail latency matters.

### Memory

Resting an order costs two heap allocations (a queue node and an index node);
there is no pooling or arena. `sizeof(Order)` is 32 bytes. The reusable trade
buffer removes the per-call result vector only — it does not make the engine
allocation-free.

## Known limitations

- Std node-based containers: pointer chasing and two allocations per resting
  order. A slab/arena with an intrusive list is the obvious next step.
- No sequence numbers or timestamps; priority is purely position in a list, so
  determinism depends on the order of calls.
- `OrderBook::bids()` / `asks()` are exposed mutably, and
  `on_resting_order_removed` must be called before an order leaves its queue, or
  the index dangles. Both are internal coupling the engine relies on.
- A single `false` for every rejection — the caller cannot tell why.
- Basic exception safety only: an out-of-memory error between inserting an
  order and indexing it would leave an uncancellable order.

## Layout

```
include/lob/   order.hpp  order_book.hpp  matching_engine.hpp
src/           order_book.cpp  matching_engine.cpp  main.cpp
bench/         benchmark.cpp
tests/         test_matching  test_validation  test_integrity  test_differential
```
