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
- **Time in force** — good-til-cancel, immediate-or-cancel (IOC) and fill-or-kill (FOK)
- **Cancel by order id**, O(1) in the size of the queue
- **Modify (cancel/replace)** with exchange-style priority: shrinking an order in place keeps its queue position; changing the price or growing it sends it to the back, and it trades at once if the new price crosses
- **Input validation** — zero-quantity orders and ids that are already live are rejected before they can touch the book
- **A replay backtester** on top of the engine (see below)

**Not implemented:** stop orders, self-trade prevention, timestamps/sequence
numbers, binary-feed parsing (ITCH), multi-threading, persistence.

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
[`matching_engine.hpp`](include/lob/matching_engine.hpp) (matching policy),
[`backtest.hpp`](include/lob/backtest.hpp) and
[`event_io.hpp`](include/lob/event_io.hpp) (the backtester and its CSV loader).

### Complexity

L = price levels (about 40 in the benchmark), F = fills, K = levels consumed.

| Operation | Cost |
|---|---|
| Best bid / ask | O(1) |
| Duplicate-id check | O(1) average |
| Rest a limit order | O(log L) + 2 heap allocations (queue node, index node) |
| Match | O(F + K·log L) |
| Cancel | O(log L) level lookup + O(1) unlink |
| Modify, quantity down at the same price | O(1) |
| Modify, price change or quantity up | cancel + submit |
| Fill-or-kill feasibility check | O(orders examined), stops as soon as enough quantity is seen; paid only by FOK orders |

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

**Time in force** (`Order::tif`) decides what happens to the part that does not
trade immediately: `GoodTilCancel` rests (limit orders only),
`ImmediateOrCancel` discards it, `FillOrKill` trades the whole quantity or does
nothing at all. Market orders never rest. A `true` return means the order was
processed according to its terms — a killed fill-or-kill order returns `true`
with no trades.

```cpp
engine.submit({.id = 7, .side = Side::Buy, .type = OrderType::Limit,
               .tif = TimeInForce::FillOrKill, .price = 10002, .quantity = 500}, trades);
```

**`modify(id, new_price, new_quantity, out)`** — `new_quantity` is the quantity
you want *left*, not a delta. Returns `false` (changing nothing) if the order is
not resting or `new_quantity` is zero.

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
./build/lob_backtest      # backtest demo on a synthetic feed
./build/lob_backtest events.csv
```

Requires CMake 3.20+ and a C++20 compiler. GoogleTest is fetched automatically
at configure time. Options: `-DLOB_BUILD_TESTS=OFF`, `-DLOB_BUILD_BENCH=OFF`,
`-DLOB_BUILD_DEMO=OFF`, `-DLOB_SANITIZE=ON` (ASan + UBSan).

## Testing

110 GoogleTest cases in [`tests/`](tests/):

- **Behaviour** — resting, price-time priority, partial and full fills, price
  improvement, multi-level walks on both sides, market orders, all the cancel
  positions (front, middle, back, last-in-level), and both `submit` overloads
- **Validation** — duplicate ids (same level, other level, other side, after
  fill, after cancel, market orders) and zero quantity; each also checks that a
  rejection leaves the book, the index and the caller's buffer untouched
- **Order types** — IOC, FOK (including one-share-short and
  liquidity-beyond-the-limit boundaries) and modify (priority kept or lost,
  crossing replacements, the "quantity means what is left" rule)
- **Backtester** — CSV loading (every action, every kind of bad input, line
  numbers in errors), queue position, fills as resting and as aggressor, exact
  P&L, mark-to-mid, callback ordering, non-re-entrant fills, id-range safety
- **Integrity** — copying fails to compile, moving keeps the index valid, the
  capacity hint changes nothing, and a 20,000-step churn test checks that the
  index and the book never disagree
- **Differential** — [`test_differential.cpp`](tests/test_differential.cpp)
  runs 100 seeded sessions × 1,000 random operations (new orders in all three
  time-in-force modes, cancels and modifies) against a deliberately naive
  reference model (flat vector, linear search, its own implementation of FOK and
  modify) and requires identical accept/reject results, trades and book contents
  after every step

The tests were mutation-checked: **34 deliberate bugs** were each applied in
turn — engine (trade at the wrong price, LIFO instead of FIFO, `>` instead of
`>=`, dropped guards, stale index entries), order types (FOK ignoring the limit
price, an off-by-one, IOC remainders resting, modify keeping or losing priority
wrongly), and the backtester (flipped P&L signs, dropped fills, wrong mark,
state leaking between runs, a CSV loader that accepts trailing junk) — and
**every one was caught**. One of them (an empty level left in the book) makes
the engine loop forever, so it surfaces as a hang rather than a failed
assertion; one is a copy-safety regression that fails to compile. The suite also runs
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

### Cost of the newer features

Adding time-in-force and modify did not slow the engine: against the previously
published version, 10 interleaved runs of this benchmark gave a median of
3.93 M ops/s versus 3.96 M ops/s (ratio 0.99, inside the run-to-run noise) with
identical trade counts. `Order` is still 32 bytes (checked by a `static_assert`).

### Memory

Resting an order costs two heap allocations (a queue node and an index node);
there is no pooling or arena. `sizeof(Order)` is 32 bytes. The reusable trade
buffer removes the per-call result vector only — it does not make the engine
allocation-free.

## Backtester

`Backtester` replays historical order flow through the real `MatchingEngine`
with a strategy's own orders sharing the book. Because strategy orders go
through the same matching logic, **queue position, partial fills and time
priority come from the engine itself** — a strategy order posted behind 50
shares only fills once 50 shares have traded ahead of it.

```cpp
struct MyStrategy : lob::Strategy {
    void on_event(const lob::MarketEvent& ev, lob::Context& ctx) override {
        if (auto bid = ctx.book().best_bid()) ctx.submit(lob::Side::Buy, *bid, 10);
    }
    void on_fill(const lob::Fill& f, lob::Context& ctx) override { /* ... */ }
};

auto parsed = lob::parse_events(csv_stream);          // see event_io.hpp for the format
lob::Backtester bt;
lob::BacktestResult r = bt.run(parsed.events, my_strategy);
// r.fills, r.position, r.cash, r.mark, r.pnl, and counters for rejects/misses
```

- **Input:** CSV (`timestamp,action,id,side,type,tif,price,quantity`; actions
  `new`, `cancel`, `modify`). Bad input is reported with its line number, and
  nothing partial is returned.
- **Context** gives a strategy the live book, its position, `submit`,
  `submit_market`, `cancel`, `modify` and `queue_ahead(id)`.
- **Callbacks:** `on_fill` for fills an event caused arrives before that
  event's `on_event`; fills caused by the strategy's own actions are delivered
  after the current callback returns, never re-entrantly.
- **Accounting:** position and cash are signed integers in ticks × shares; P&L
  is marked to the mid (or the last trade if one side of the book is empty).
- **Ids:** strategy order ids start at 2⁶², and historical ids at or above that
  are refused up front, so the two can never collide.

`lob_backtest` runs a toy quote-at-the-touch strategy on a synthetic random-walk
feed (about 6 M events/s in one run on the M2; not benchmarked rigorously). **Its
P&L means nothing as a trading result** — the feed is random and the strategy
is an example, there to show the pieces working together.

**What this model does not do.** Historical orders are replayed exactly as they
happened, so a strategy that moves the market does not change how other
participants would have reacted (no market-impact or reaction model), and a
historical cancel that targets an order the strategy's presence caused to trade
is counted in `historical_cancel_misses` rather than guessed at. There is no
latency model, no fees or rebates, and no position or risk limits beyond what a
strategy imposes on itself.

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
include/lob/   order.hpp  order_book.hpp  matching_engine.hpp  event_io.hpp  backtest.hpp
src/           order_book.cpp  matching_engine.cpp  event_io.cpp  backtest.cpp
               main.cpp  backtest_demo.cpp
bench/         benchmark.cpp
tests/         test_matching  test_validation  test_order_types  test_integrity
               test_differential  test_backtest
```
