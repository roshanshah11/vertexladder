# Benchmarks

Machine: Apple M3 Pro (5 performance + 6 efficiency cores, 128-byte cache lines, 16 MB L2 on the
performance cluster), 18 GB, macOS 26.6, Apple clang 21, `-O3`, CMake Release, no `-march=native`.
It's a laptop doing other work during the runs (load average around 6), so treat single digits as
noise; ranges below are across repeated runs. macOS can't pin threads to cores.

Reproduce:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/bench_engine                                    # synthetic, ~20k resting orders
./build/bench_engine --resting 1000000 --ops 30000000 --warmup 10000000 --runs 3
scripts/get_itch_sample.sh data && ./build/bench_itch data/12302019.NASDAQ_ITCH50.gz --validate
./build/vl_server & ./build/vl_fix_client --orders 200000
./build/bench_tcp_baseline && ./build/bench_hop
```

## A note on the clock

On Apple Silicon the system counter behind `mach_absolute_time` and `steady_clock` ticks at
24 MHz: one tick is 41.67 ns. Reading it costs about 5 ns. An operation that takes 20 ns
can't be timed on its own: each sample comes out as 0 or 1 tick. So:

- **mean** is measured directly, as total time over operation count, with one timestamp before
  and one after the whole stream. This is the number to trust.
- **percentiles** come from timing every operation individually into a full histogram (every
  sample, no sampling). They are quantized to 41.7 ns and include one clock read, so read
  "p99 83" as "at most two ticks".

On x86 the code uses `rdtsc` instead, which resolves well under a nanosecond.

## 1. Matching engine, synthetic flow

`bench/bench_engine.cpp`. One thread calls `OrderBook::submit / cancel / replace` directly on
a pre-generated stream; generating the stream is not timed. The flow is shaped like a lit book:
the mid random-walks a tick at a time, passive orders sit a geometric number of ticks behind the
touch (never crossing), cancel pressure rises with book size so it settles at a target depth,
and 7% of orders are marketable IOCs that sweep one or more levels. 20M operations, the first
2M untimed as warm-up, median of 5 runs.

| | ~20k resting orders (fits in L2) | ~1M resting orders (doesn't) |
|---|---|---|
| op mix | 45.9% add, 42.1% cancel, 7.0% aggressive IOC, 5.0% replace | 47.5 / 40.5 / 7.0 / 5.0 |
| throughput | **53.2 M ops/s** (runs: 43.8 to 53.7) | **10.6 M ops/s** (runs: 9.4 to 13.4) |
| mean per op | **18.8 ns** | **95 ns** |
| p50 / p99 / p99.9 per op | < 42 / ≤ 83 / ≤ 167 ns | 83 / 417 / 3,958 ns |
| trades | 1.33 M | 1.93 M |
| book at end | 20,323 orders, 138 levels | 1,021,593 orders, 1,064 levels |

The two columns are the same code. With 20k orders the node pool (640 KB) and id map fit in
L2; at 1M orders the pool is 32 MB and the id map 64 MB, so a cancel of a random order costs
a couple of cache misses (cancel p50 goes from < 42 ns to 125 ns). Real books are closer to
the left column per symbol, but a whole market at once is closer to the right one (section 2).

Raw output, 20k column:

```
THROUGHPUT  (18000000 timed ops, median of 5 runs)
  53.24 M ops/s   mean 18.8 ns/op   (min run 43.76, max run 53.69 M ops/s)
  trades 1332337, book at end: 20323 orders, 48 bid / 90 ask levels

LATENCY PER OP (ns, includes one clock read; resolution 41.7 ns)
  op                    count     mean      p50      p90      p99      p99.9        max
  add (passive)       8251397     18.3        0       42       42        167       8750
  aggressive IOC      1260554     12.6        0       42       42        125      22875
  cancel              7589452     16.2        0       42       42        125      34583
  replace              898597     30.5       42       83       83        167      13375
  all                18000000     17.6        0       42       83        167      34583
  (empty timer)       1000000      4.7        0       42       42         42       1708
```

## 2. A full NASDAQ trading day (ITCH 5.0)

`bench/bench_itch.cpp` replays NASDAQ's own TotalView-ITCH feed for Monday, Dec 30, 2019
(`12302019.NASDAQ_ITCH50.gz` from emi.nasdaq.com, 3,524,013,057 bytes). It rebuilds every
stock's book order by order. The timed region is parsing plus book updates on one thread;
gzip decompression runs between timed chunks and is excluded.

| | |
|---|---|
| messages | 268,744,780 (8.25 GB decompressed) |
| book events | 263,241,937: 117.1M adds (A), 1.5M adds with MPID (F), 5.7M executions (E), 0.1M executions at a price (C), 2.8M partial cancels (X), 114.4M deletes (D), 21.6M replaces (U) |
| books | 8,892 symbols; peak 1.92M resting orders at once |
| replay time | **28.6 to 31.8 s** over three runs |
| throughput | **8.4 to 9.4 M messages/s, 106 to 118 ns per message** |
| per book event (individually timed) | mean 99 ns, p50 42, p99 583, p99.9 1,167 ns |
| integrity | 0 references to unknown orders, 0 over-fills, 0 malformed messages, every book empty at end of file |
| validation | of 5,722,824 'E' executions, **100.0000%** hit an order at our best price and **99.8154%** hit the exact order our book had first in line |

Per message this is about 5x the synthetic run: 1.9M live orders spread over 8,892 books, with
messages hopping between symbols, means most messages start with a cache miss. That puts it
next to the right-hand column above, not the left.

On the 0.18% of executions that weren't first in our queue: 9,052 of the 10,565 are between
9:00 and 10:00 ET (around the 9:30 opening cross), about 1,500 over the rest of the day, and
none are on orders created by a replace. The cause isn't pinned down; see DESIGN.md section 6.

AAPL's book a little after noon, as rebuilt (21,716 resting AAPL orders at the time):

```
    orders   shares        bid | ask        shares   orders
         3      300   290.7300 | 290.7500   719      5
         1      100   290.7200 | 290.7600   710      4
         4      303   290.7100 | 290.7700   200      2
         2      200   290.7000 | 290.7800   100      1
         3      300   290.6900 | 290.7900   200      2
```

## 3. End to end over FIX

`vl_fix_client` against `vl_server` on the same machine: one NewOrderSingle in flight at a time;
each sample is send to first ExecutionReport back (the ack). That covers client socket, kernel
loopback, gateway parse, ring to the engine thread, match, ring back, FIX encode and the write
back. 200,000 orders, first 10% discarded as warm-up.

| | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| order round trip (µs) | **10.2** | 12.9 | 26.7 | 57.8 |
| bare loopback TCP ping-pong, same machine, no FIX (µs, `bench_tcp_baseline`) | 7.0 to 7.6 | 10.7 to 18.9 | 25.8 to 47.5 | 53 to 131 |

So the gateway, both ring hops and the match add about 3 µs at the median over what the kernel
costs anyway. On the way there: Boost.Asio's reactor (`io_context::poll`) measured about 12 µs
per call on macOS even with nothing ready, which put the round trip at 53 µs. Busy-polling
non-blocking sockets brought it to 14.5 µs, and caching the SendingTime format brought it to
10.2 µs. DESIGN.md section 7 has the details.

Inside the process, the engine hop alone (`bench_hop`: push a command, get its first event back
from the engine thread) is 83 to 125 ns p50 and 250 to 500 ns p99 across runs.
