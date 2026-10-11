# Benchmarks

Machine: Apple M3 Pro (5 performance + 6 efficiency cores, 128-byte cache lines, 16 MB L2 on the
performance cluster), 18 GB, macOS 26.6, Apple clang 21, `-O3`, CMake Release, no `-march=native`.

This is a laptop that was doing other work during the runs (iOS Simulator tests, load average 6 to
16), and macOS can't pin threads to cores. Background load only ever slows a run down, so for
throughput I report the median and the range across runs, and every latency table sits next to a
baseline measured in the same window. Re-running on a quiet machine gives faster numbers, not
slower ones.

Reproduce:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/bench_engine --runs 9                            # synthetic, ~20k resting orders
./build/bench_engine --resting 1000000 --ops 30000000 --warmup 10000000 --runs 3
scripts/get_itch_sample.sh data && ./build/bench_itch data/12302019.NASDAQ_ITCH50.gz --validate
./build/bench_hop
./build/bench_tcp_baseline
./build/vl_server & ./build/vl_fix_client --orders 200000
```

## A note on the clock

On Apple Silicon the counter behind `mach_absolute_time` and `steady_clock` ticks at 24 MHz: one
tick is 41.67 ns, and reading it costs about 5 ns. An operation that takes 20 ns can't be timed on
its own. So:

- **The mean** comes from one timestamp before and one after the whole stream, divided by the
  operation count. This is the number to trust.
- **Percentiles** come from timing every operation individually into a histogram (every sample,
  no sampling), so they are quantized. A reading of k ticks means the true time was below
  (k + 1) ticks: "p99 83 ns" (2 ticks) means p99 is under 125 ns. They also include one clock
  read.

On x86 the code uses `rdtsc` fenced with `lfence`, which resolves well under a nanosecond.

## 1. Matching engine, synthetic flow

`bench/bench_engine.cpp`. One thread calls `OrderBook::submit / cancel / replace` directly on a
pre-generated stream, and generating the stream is not timed. The flow is shaped like a lit book:

- the mid random-walks a tick at a time;
- passive orders sit a geometric number of ticks behind the touch and never cross;
- cancel pressure rises with book size, so the book settles at a target depth;
- 7% of operations are IOCs priced at or through the opposite touch, so every one of them trades,
  some sweeping several levels.

20M operations, the first 2M untimed as warm-up. These are book operations, not orders that all
trade: 7.0% of operations produce a fill (2.55M fills in all), the rest are adds, cancels and
replaces, which is also what most of a real feed is.

| | ~20k resting orders (fits in L2) | ~1M resting orders (doesn't) |
|---|---|---|
| op mix | 47.6% add, 40.4% cancel, 7.0% aggressive IOC, 5.0% replace | 49.2 / 38.9 / 7.0 / 5.0 |
| throughput | **50.5 M ops/s**, median of 9 (46.1 to 51.2) | **8.8 M ops/s**, median of 3 (7.3 to 12.3) |
| mean per op | **19.8 ns** | **114 ns** |
| per-op readings p50 / p99 / p99.9 | 0 / 83 / 167 ns (true p99 < 125 ns) | 83 / 500 / 4,833 ns |
| book at end | 19,361 orders, 154 levels | 979,844 orders, 1,220 levels |

Same code in both columns. With 20k orders the node pool (620 KB) and the id map fit in L2. At 1M
orders the pool is 31 MB and the id map 64 MB, so cancelling a random order costs a couple of cache
misses (cancel p50 reading goes from under one tick to 167 ns).

Per-op readings, 20k column:

```
  op                    count     mean      p50      p90      p99      p99.9        max
  add (passive)       8552369     21.6        0       42       83        167     612625
  aggressive IOC      1260687     20.5        0       42       83        167     371000
  cancel              7288410     19.9        0       42       42        167     631500
  replace              898534     34.3       42       83       83        167     139250
  all                18000000     21.5        0       42       83        167     631500
  (empty timer)       1000000      4.7        0       42       42         42       3208
```

## 2. A full NASDAQ trading day (ITCH 5.0)

`bench/bench_itch.cpp` replays NASDAQ's own TotalView-ITCH feed for Monday, Dec 30, 2019
(`12302019.NASDAQ_ITCH50.gz` from emi.nasdaq.com, 3,524,013,057 bytes) and rebuilds every stock's
book order by order. This uses the book-builder half of the API (`insert_resting`, `reduce`,
`erase`): NASDAQ already did the matching, so `match()` doesn't run here. The timed region is
parsing plus book updates on one thread; gzip decompression runs between timed chunks and is
excluded.

| | |
|---|---|
| messages | 268,744,780 (8.25 GB decompressed) |
| book events | 263,241,937: 117.1M adds (A), 1.5M adds with MPID (F), 5.7M executions (E), 0.1M executions at a price (C), 2.8M partial cancels (X), 114.4M deletes (D), 21.6M replaces (U) |
| books | 8,892 symbols; at least 1.92M orders resting at once (sampled once per 64 MB chunk) |
| replay time | **28.6 to 34.8 s** over four runs |
| throughput | **7.7 to 9.4 M messages/s, 106 to 129 ns per message** |
| per book event, individually timed | mean 99 ns; readings p50 42, p99 583, p99.9 1,167 ns |
| integrity | 0 references to unknown orders, 0 over-fills, 0 malformed messages, every book empty at end of file |
| validation | of 5,722,824 'E' executions, **100.0000%** hit an order at our best price and **99.8154%** hit the exact order our book had first in line (identical in both validation runs) |

Per message this is about 5x the synthetic run: 1.9M live orders spread over 8,892 books, with
messages hopping between symbols, means most messages start with a cache miss. That lines up with
the right-hand column above, not the left.

The 'E' check is the strict one. 'C' executions (99,917 of them, executed at a price other than the
displayed one) aren't checked, because they include crosses and other prints where queue position
isn't the point. On the 0.18% of 'E' executions that weren't first in our queue: 9,052 of the 10,565
are between 9:00 and 10:00 ET (around the 9:30 opening cross), about 1,500 over the rest of the day,
and none are on orders created by a replace. The cause isn't pinned down; see DESIGN.md section 6.

AAPL's book a little after noon, as rebuilt (21,716 resting AAPL orders at the time):

```
    orders   shares        bid | ask        shares   orders
         3      300   290.7300 | 290.7500   719      5
         1      100   290.7200 | 290.7600   710      4
         4      303   290.7100 | 290.7700   200      2
         2      200   290.7000 | 290.7800   100      1
         3      300   290.6900 | 290.7900   200      2
```

## 3. Across threads and over FIX

**Engine hop** (`bench_hop`): one command in flight, pushed from one thread to the busy-polling
engine thread, results read back on the first thread. Orders alternate buy and sell at one price, so
every sell trades.

| ns (41.7 ns resolution) | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| command → its Accepted event (sent before matching) | 167 | 291 | 375 | 666 |
| crossing order → its Fill event (includes the match) | 250 | 333 | 458 | 833 |

**FIX round trip** (`vl_fix_client` against `vl_server`, same machine): one NewOrderSingle in
flight at a time, 200,000 orders, first 10% discarded. Each sample covers the client socket, kernel
loopback, gateway parse, the ring to the engine thread, the engine, the ring back, FIX encoding and
the write back. Two clocks: to the ack (150=0, which the engine sends before matching) and, for the
orders that cross, to their own fill report (150=F, which includes the match). This is a closed loop,
so with no queueing it measures service time on an otherwise idle system, not latency under load
(no coordinated-omission correction). Bare TCP in the same window is the baseline.

| µs, same window, load average ~10 | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| order → ack | **15.1** | 34.2 | 79.9 | 223.5 |
| crossing order → fill | **16.8** | 36.8 | 82.2 | 240.9 |
| bare loopback TCP ping-pong, no FIX (`bench_tcp_baseline`) | 9.2 | 23.3 | 52.0 | 106.3 |

So the gateway, both ring hops, the engine and FIX encoding add about 6 µs at the median on a busy
machine. On a quieter run earlier the same day (load ~6, gateway code from before the review fixes)
it was 10.2 µs against 7.0 µs bare, about 3 µs.

How it got there: with Boost.Asio's reactor driving the sockets, the round trip was 53 µs.
`io_context::poll()` measured about 12 µs per call on macOS even with nothing ready, and an order
needs two passes (read it, then write the ack). Busy-polling non-blocking sockets directly brought
it to 14.5 µs, and caching the SendingTime format (gmtime + strftime cost about a microsecond per
message) to 10.2 µs. DESIGN.md section 7 has the details.
