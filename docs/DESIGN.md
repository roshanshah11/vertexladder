# VertexLadder design notes

What each part does, the alternatives, and why this one. Numbers are from an Apple M3 Pro
(see [BENCHMARKS.md](BENCHMARKS.md)).

## 1. Threading: one thread owns the book

```
 FIX clients ──TCP──▶ gateway thread ──SPSC ring──▶ engine thread ──SPSC ring──▶ gateway thread ──TCP──▶ clients
                      (sockets, FIX,      Command        (all books,      EngineEvent     (ExecutionReports)
                       ClOrdIDs, seq #s)  40 bytes        matching)        48 bytes
```

Every book lives on one thread, and that thread does nothing but match. The gateway thread
owns everything that is per-client and slow: sockets, FIX parsing, sequence numbers, ClOrdID
strings, average prices. The two threads share exactly two lock-free single-producer
single-consumer rings and nothing else.

Why not lock the book and let many threads in? Matching is a sequential problem. Two orders
for the same book have to be applied in some order, and price-time priority depends on that
order. With a lock the threads would mostly wait on each other, the cache lines holding the
book would bounce between cores, and the output would depend on scheduling. One owner gives:

- no locks or atomics inside the book;
- the book's working set stays in one core's cache;
- determinism: the same input sequence always produces the same trades, so a day can be
  replayed exactly from the input log (this is also what makes the differential test
  possible);
- the same shape real venues use (one sequencer per book; LMAX's published design is the
  well-known example).

Scaling past one core happens across symbols, not within one: shard symbols across engine
threads, each with its own rings. The engine already holds many books; a second engine
thread would take a disjoint set of symbols.

## 2. The order book

### Prices are integers

`Price` is `int64_t` ticks. Doubles can't be compared for equality or used as map keys safely
(0.1 + 0.2 != 0.3), and every venue sends fixed point anyway: ITCH prices have 4 implied
decimals, so $290.73 arrives as 2907300. The FIX gateway converts "290.73" to ticks exactly with
integer arithmetic and rejects prices finer than the tick.

### Layout

```
bids_ (vector, worst ... best)            levels_ pool                     orders_ pool (32 B nodes)
┌────────┬────────┬────────┐             ┌──────────────────────────┐
│ 99.97  │ 99.98  │ 99.99  │──level idx──▶│ price 99.99, total 700,  │──head──▶ [id 17, 300] ⇄ [id 42, 400]
└────────┴────────┴────────┘   back() =   │ count 2, head, tail      │                ▲ tail
                                best bid  └──────────────────────────┘
index_: flat hash map  order id ──▶ node handle
```

- **Orders** are 32-byte nodes in one contiguous pool, aligned to 32 so none straddles a cache line
  (two per 64-byte x86 line, four per 128-byte Apple line). A node holds id, open quantity, filled
  quantity, prev/next links, its level and its side. No price (the level has it) and no
  timestamp: time priority is simply the order's position in its level's FIFO.
- **Handles, not pointers.** Nodes are addressed by 32-bit indices. Indices survive the pool's
  vector growing (pointers would dangle), and they're half the size.
- **Levels** are pooled the same way: price, total open quantity, order count, head and tail.
- **Each side** is a `std::vector<{price, level handle}>` sorted worst to best, so the best price
  is `back()`. Matching only ever touches the back. Adding a level or removing an empty one
  shifts only the entries better than it, which on a real book are a handful because activity
  is concentrated at the inside. This is the "reversed vector" idea from David Gross's CppCon
  2024 talk on Optiver's book.
- **Finding a level** scans from the top for up to 16 levels, then switches to binary search.
  The linear part is a few predictable compares on one or two cache lines; the binary part
  keeps an order 500 levels deep from costing 500 steps.
- **Order id lookup** is an open-addressing hash map (section 4).

### Complexity

| Operation | Cost |
|---|---|
| Add at an existing level | O(1) to append, plus O(k) to find the level, k = levels between it and the top (k ≤ 16 linear, then log) |
| Add at a new level | the same, plus a vector insert that moves the k better levels |
| Cancel | O(1) unlink; if the level empties, O(k) to remove it from the side vector |
| Match | O(1) per fill; each emptied level is a `pop_back` |
| Best bid / ask | O(1), `back()` |

Alternatives considered:

- **`std::map<Price, Level>`**: O(log n) everything, a heap node per level, pointer chasing on
  every lookup. The differential test's reference book is built this way on purpose: it is
  the obviously correct, slow version.
- **Array indexed by price tick** (one slot per possible price): O(1) level lookup, but it needs
  a bounded price range. A stock that moves 10% intraday is thousands of ticks, mostly empty,
  and finding the next non-empty level after the best one empties is a scan unless you add a
  bitmap. It's the right choice when the range is known and small; a general engine can't
  assume that.
- **Removing levels lazily (marking empty)**: turns every best-price query into a scan past
  dead levels, and the vector grows forever. Removing an empty level immediately is
  cheap with the best-at-back layout, so there's nothing to defer.

### Matching rules

- Price-time priority. An incoming order walks the opposite side from the best price, FIFO
  within each level, and every fill prints at the resting order's price (so a buy at 101 against
  an ask at 100 trades at 100).
- **GTC limit**: whatever doesn't fill rests at its limit.
- **IOC**: whatever doesn't fill is cancelled, never rests.
- **FOK**: before touching anything, add up the quantity available at prices it could take; if
  that's short, cancel the whole order without trading.
- **Market**: a limit at +/- infinity with IOC semantics.
- **Cancel/replace**: the new quantity is the order's new *total*, including what has already
  filled, as in FIX. The book tracks each order's filled amount and computes the new open
  quantity itself (total minus filled), and rejects a total at or below what has filled. This
  matters because the book is the only place that knows the filled amount for certain: a fill can
  be on its way back to the gateway while the client's replace is on its way in. Cutting size at
  the same price keeps the order's place in line; any other change (new price, more size) takes it
  out and re-enters it at the back, where it can trade if the new price crosses. This is how most
  venues treat priority.
- Rejects: zero quantity, non-positive limit price, duplicate id, unknown id.
- Not implemented: self-trade prevention, iceberg/hidden orders, auctions, stop orders, pegs.

### Events

The book takes a `Listener` type as a template parameter and calls it for accepts, trades,
cancels, rejects and replaces. With the default `NullListener`, every callback is an empty
inline function and compiles away. That's static dispatch: no virtual call and no
`std::function` on the hot path. The engine's listener writes fixed-size events into the
outbound ring. The test listener records them into a vector.

### Memory

The book never calls the allocator once its pools have grown to the working size: nodes and
levels are recycled through free lists threaded through the nodes themselves, and the hash
map is sized up front. If a pool does have to grow, it grows (a latency spike, not a failure).
Sizing matters more than it looks. Sizing the id map for the whole stream instead of the live
working set spread 20k live keys over 256 MB and made every lookup a cache miss. That cost
about 2x in the benchmark until I fixed it.

## 3. Correctness

- **Differential testing.** `tests/reference_book.hpp` is a deliberately naive book (std::map
  of std::list). The test drives it and the real book with the same random stream of limit,
  IOC, FOK and market orders, cancels, replaces, duplicates and invalid inputs. It requires the
  two event streams to be identical, every live order's open quantity to match, and the full
  depth on both sides to match, checking every 64 operations. It runs 750k operations over
  three regimes: tight prices (constant crossing), wide prices (deep ladders, the
  binary-search path), and tiny initial pools (growth mid-run).
- **The tests catch real bugs.** I checked by planting three: trades at the taker's price instead
  of the maker's, keeping priority on any replace, and an inverted binary-search predicate. The
  differential test caught all three.
- **Structural invariants** (`check_invariants`): sorted ladders, level totals equal the sum
  of their orders, links consistent both ways, every order in the index, book not crossed.
- **Against real data**: the ITCH replay (section 6) checks the book against NASDAQ's own
  record of which order each execution hit.
- CI runs the suite under AddressSanitizer + UBSan and ThreadSanitizer on Linux.

## 4. Flat hash map

`FlatHashMap` maps a 64-bit order id to a 32-bit node handle.

- One flat array of `{key, value}` slots, power-of-two size, load factor at most 1/2, linear
  probing. A lookup is a multiply, a shift and almost always one cache line.
- **Fibonacci hashing**: order ids are usually sequential, which would cluster under a plain
  mask, so the key is multiplied by 2^64/phi and the high bits taken.
- **Backward-shift deletion** instead of tombstones. After erasing a slot, later entries in
  the same probe run whose home slot is at or before the hole get moved back into it. Probe
  chains never fill up with dead markers, so a full day of adds and cancels doesn't slowly
  degrade lookups. This is the subtle part; its test compares against `std::unordered_map`
  over 1M random operations on a small key space to force long runs and wraparound.
- Key 0 means empty, so order id 0 is reserved.

## 5. SPSC ring and the engine loop

`SpscRing<T>` is a bounded single-producer single-consumer queue.

- One writer per index: the producer only writes `tail_`, the consumer only writes `head_`. No
  CAS loops; the common case is one relaxed load, one store of the slot, one release store.
- **Memory ordering**: the producer writes the slot, then stores `tail_` with release. The
  consumer loads `tail_` with acquire before reading the slot. Release/acquire on the same
  variable creates a happens-before edge, so the slot contents are visible. The same pairing
  on `head_` stops the producer from overwriting a slot the consumer is still copying out.
- **False sharing**: `head_` and `tail_` sit on separate 128-byte-aligned lines (Apple Silicon's
  line size; on x86 the adjacent-line prefetcher makes 128 the safe padding too).
- **Cached indices**: each side keeps a private copy of the other side's index and only reloads
  the shared one when its copy says full or empty. In steady state neither thread touches the
  other's cache line.
- **Full means full.** `try_push` returns false and the caller decides. The engine spins (it
  must never drop an event); the gateway applies backpressure. A queue that drops on full
  loses orders silently under exactly the load you're benchmarking.
- **Testing it honestly**: the two-thread stress test checks every message arrives once, in
  order, intact, and CI runs it under ThreadSanitizer. When I weakened every ordering to relaxed,
  the stress test still passed, and ThreadSanitizer stayed quiet in six of my runs on the M3
  (-O0 to -O2); a reviewer running the same experiment did get a TSan report. A check that only
  sometimes notices a missing barrier isn't proof, so the correctness argument for the orderings
  is the happens-before reasoning above, not the test.

The engine thread busy-polls its input ring. Waking a sleeping thread costs microseconds,
more than matching an order, so trading systems spin on a dedicated core.
In-process, a command's ack comes back from the engine thread about 170 ns (p50) after it's pushed,
and a crossing order's own fill about 250 ns (`bench_hop`, on a loaded machine).

## 6. NASDAQ ITCH 5.0 replay

ITCH is NASDAQ's market-by-order feed: every add, execution, cancel, delete and replace on its
book. Replaying a day rebuilds every stock's full book as NASDAQ had it. The replayer uses the
book-builder half of the API (`insert_resting`, `reduce`, `erase`), because NASDAQ has already
done the matching.

The check that the book is right: an ITCH 'E' message says "order X was executed". If our book
has the same orders in the same priority, X should be at our best price and at the front of its
queue. Over the Dec 30, 2019 file:

- 5,722,824 executions: 100.0000% at our best price, 99.8154% first in our queue;
- 0 references to orders we don't have, 0 executions or cancels larger than the open quantity,
  and every book empty at the end of the file;
- the 10,565 executions (0.18%) that hit an order with something ahead of it in our queue
  cluster at the open: 9,052 between 9:00 and 10:00 ET, about 1,500 the rest of the day.
  None were on orders created by a replace. I haven't pinned down the cause. My leading guess
  is NASDAQ order types whose queue priority isn't fully visible in ITCH, which would matter
  most around the opening cross.

## 7. FIX 4.4 gateway

- **Codec** (`src/fix/fix_codec.cpp`): zero-copy parse into (tag, string_view) pairs over the
  receive buffer. It validates framing the way a counterparty would: BeginString, then
  BodyLength (must equal the actual byte count), then MsgType, and CheckSum (sum of bytes mod
  256) last. Garbled input is skipped and the parser resynchronises at the next `8=FIX.4.4`,
  because FIX says garbled messages are ignored, not answered. Writing goes into a fixed stack
  buffer with room reserved for the header, so BodyLength and CheckSum are filled in at the
  end without moving the body.
- **Session**: Logon first or disconnect, MsgSeqNum enforced (a gap ends the session with a
  Logout explaining why; resend is not implemented), TestRequest answered, Heartbeats sent and
  a dead peer dropped at twice the interval.
- **Orders**: NewOrderSingle (limit/market; Day/GTC/IOC/FOK), OrderCancelRequest,
  OrderCancelReplaceRequest. ExecutionReports for new, partial and full fills (LastQty, LastPx,
  CumQty, LeavesQty, AvgPx), cancels, replaces and rejects; OrderCancelReject when a cancel or
  replace arrives too late. ClOrdID chains follow FIX (the cancel's report carries the cancel's
  ClOrdID and the original as OrigClOrdID). Cancel-on-disconnect is on by default.
- **The races worth knowing about**: the engine's output and the client's input cross in
  flight.
  - A client replaces an order while a fill for it is still in the engine's output ring. If the
    gateway computed the new open quantity from its own CumQty, which is stale by that fill, the
    order would overfill. So the gateway forwards the FIX total and the engine, which knows the
    true filled amount, computes open = total - filled. `GatewayRace` reproduces this
    deterministically by holding the engine back until all three commands are queued.
  - A client cancels while the fill that completes the order is in flight. The gateway sees the
    fill first and the order is done, but the cancel still needs an answer. The gateway keeps the
    order's state until the engine's reject for that cancel arrives and answers it with an
    OrderCancelReject.
- **Every request gets exactly one answer, or the session ends.** ClOrdIDs are capped at 64
  bytes and must be unique for the session's whole day (reusing one would let two orders share a
  key), so every report fits the writer's buffer; a sequence number is consumed only by a message
  that was actually written; a client that stops reading is cut off once 8 MB is queued, and a
  closing session that won't take its Logout is dropped after a second, so cancel-on-disconnect
  still runs.
- **Backpressure without deadlock**: the gateway never drops a command, so if the engine's input
  ring is full it waits, but while it waits it keeps draining the engine's output. Otherwise a
  mass cancel-on-disconnect could fill both rings, leaving the gateway waiting on the engine and
  the engine waiting on the gateway. `GatewayBackpressure` runs this with 64-slot rings.
- **Pre-trade check**: orders above a configurable size (default 1,000,000) are rejected.
- **I/O loop**: the gateway busy-polls non-blocking Boost.Asio sockets itself instead of
  running Asio's reactor. I measured `io_context::poll()` at about 12 µs per call on macOS even
  with nothing ready (a kqueue syscall plus bookkeeping), and an order needs two passes (read,
  then write the ack). With the reactor, the order round trip was 53 µs at the median. After
  switching to direct non-blocking `read_some`/`write_some` it was 14.5 µs, and 10.2 µs after
  caching the SendingTime formatting (gmtime + strftime cost about a microsecond per message),
  against 7.0 µs for bare loopback TCP ping-pong measured the same way. BENCHMARKS.md has the
  current numbers next to a same-window baseline.

## 8. What I'd do next

- Kernel-bypass networking (or at least `SO_BUSY_POLL` on Linux), core pinning and isolated
  cores. All of these need Linux; macOS can't pin threads.
- Self-trade prevention, iceberg orders, an opening/closing auction.
- An input journal so the engine can replay its own day after a crash (the determinism is
  already there; it needs the log).
- A market-data publisher (MBO and top-of-book) on its own thread off a third ring.
- Shard symbols across several engine threads.
