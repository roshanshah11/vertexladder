# What a NASDAQ day says about the top of the book

The engine is only half the point. With NASDAQ's full order-by-order feed rebuilt in the same
`OrderBook`, two questions a market maker cares about can be answered on real data:

1. Does the top of the book say which way the price moves next?
2. What does your place in the queue cost you?

Data: NASDAQ TotalView-ITCH 5.0 for Monday, Dec 30, 2019 (268.7M messages, 8,892 symbols), regular
hours only (9:30 to 16:00 ET). Code: [`analysis/microstructure.cpp`](../analysis/microstructure.cpp);
raw output: [`analysis/results/`](../analysis/results/).

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../analysis/results/2019-12-30-dark.png">
  <img alt="Three charts: P(next mid move up) rising with queue imbalance; fill rate falling and the share of fills underwater 10 s later rising with the number of shares ahead in the queue" src="../analysis/results/2019-12-30-light.png">
</picture>

## 1. Queue imbalance predicts the next tick

At every book update, take the size at the best bid and best ask and compute the imbalance
I = (bid - ask) / (bid + ask), from -1 (all ask) to +1 (all bid). Then wait for the mid price to
change, and record whether it went up or down. 255M samples resolved this way.

| imbalance | P(up), spread = 1 tick | P(up), spread > 1 tick |
|---|---|---|
| -1.0 to -0.8 | 21.2% | 40.7% |
| -0.6 to -0.4 | 35.0% | 40.7% |
| -0.2 to 0.0 | 45.7% | 45.6% |
| 0.0 to +0.2 | 51.3% | 50.8% |
| +0.4 to +0.6 | 63.2% | 59.4% |
| +0.8 to +1.0 | 78.4% | 58.9% |
| sign of I calls the next move | **63.1%** of 122M samples | **56.6%** of 133M samples |

(All ten buckets are in the raw output.)

When the spread is one tick, the next move goes the heavier side's way almost four times out of
five at the extremes. The mechanism: with the spread pinned at a cent, the mid only moves when a
queue at the best price runs out, and the thin side runs out first.

The split by spread is the large-tick vs small-tick effect. When the tick is large relative to the
price, queues are long and price moves are queue depletions, so imbalance carries a lot of
information. When the spread is wider, prices move by new orders stepping inside the spread, which
imbalance doesn't see. The same shows up stock by stock. With |I| > 0.6, sign(I) called the next
move for INTC 80.5% of the time, QQQ 77.0%, SPY 75.4% and MSFT 73.7%. For AMZN it was 52.9% and
for GOOGL 55.2%, both priced above $1,000 a share, where a cent is a tiny tick.

This is the result Gould and Bonart report for ten liquid NASDAQ stocks: a strong relationship for
large-tick stocks and a weaker one for small-tick stocks ("Queue Imbalance as a One-Tick-Ahead Price
Predictor in a Limit Order Book", [arXiv:1512.03492](https://arxiv.org/abs/1512.03492)).

Knowing the next tick 63% of the time is not a strategy. Taking liquidity costs the spread, which
for a one-tick stock is the whole move you were predicting. The value is to a passive trader: when
to stay in the queue, when to cancel, and which side to lean on.

## 2. Queue position: fewer fills, and worse ones

For every order that joins the best price on its side, record the shares already resting ahead of
it at that price. Then record whether it ever executes and, for fills, where the mid is 10 seconds
later. A passive fill starts half a spread in the money. It is "underwater" if the mid has moved past
the fill price against it. Markouts use fills made while the spread was one tick, where the mid is a
fair price.

| shares ahead when it joins | orders | ever fill | fills underwater 10 s later |
|---|---|---|---|
| 0 (it set a new best price) | 13.5M | 9.5% | 52.4% |
| 1-499 | 18.5M | 8.1% | 57.8% |
| 500-1,999 | 13.5M | 3.7% | 63.1% |
| 2,000-9,999 | 9.4M | 2.4% | 67.3% |
| 10,000+ | 4.6M | 1.8% | 72.8% |

The median passive fill, in every bucket, ends 10 seconds later at -0.5 cents a share against the
fill price: it started half a cent up, and the price ticked one cent through it. Getting filled
usually means the level is about to be taken out. That's adverse selection, and the further back in
the queue you were, the more of your fills look like that.

The table mixes very different stocks, and long queues only form in some of them, so the same
comparison is also done inside each stock: orders that joined behind 2,000+ shares against orders
that joined behind 1-499.

- **Fill rate**: in 914 stocks with at least 200 orders of each kind, the long-queue orders filled
  less often in 91.6% of them (median gap 5.6 percentage points).
- **Markout**: in 481 stocks with at least 50 one-tick fills of each kind, the long-queue fills had
  the worse 10-second markout in 69.9% of them (median gap 0.07 cents a share, with each markout
  clipped to ±5 cents so one blown-out book can't decide a stock).

Queue position is worth something twice: you fill more often, and the fills you get are less often
the ones right before the price runs you over. Moallemi and Yuan model exactly this: adverse
selection costs rise with queue position, and for large-tick stocks a good position can be worth
about as much as the spread (Moallemi and Yuan, "A Model for Queue Position Valuation in a Limit Order
Book", Columbia Business School, 2017, [PDF](https://business.columbia.edu/sites/default/files-efs/pubfiles/25461/queue-value-2016.pdf)).
It is also why venues' priority rules matter so much, and why the engine's cancel/replace keeps an
order's place only when it shrinks.

## Caveats

- **One day**, and a quiet one: the Monday between Christmas and New Year's.
- **Displayed orders only.** ITCH doesn't show hidden or reserve size, so "shares ahead" is the
  displayed queue.
- **Imbalance samples are taken at every book update**, so busy names count for more. The per-stock
  numbers above are the check that this isn't one symbol's story.
- **The 10-second mid** is read at the first book update at or after the 10 seconds, and fills made
  in the last seconds before 16:00 resolve after the close.
- **Orders created by a replace ('U')** aren't tracked as new joins.
- **The "0 ahead" bucket** is a different kind of order: it improved the price rather than joining
  a queue, which is why its fills behave differently.

## Reproduce

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j --target microstructure
scripts/get_itch_sample.sh data
./build/microstructure data/12302019.NASDAQ_ITCH50.gz --csv analysis/results/2019-12-30
python3 analysis/plot.py analysis/results/2019-12-30      # needs matplotlib
```

The full day takes about 90 seconds on an M3 Pro.
