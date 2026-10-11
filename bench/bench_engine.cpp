// Synthetic matching-engine benchmark.
//
// What is timed: OrderBook::submit / cancel / replace called directly on one thread,
// nothing else. The order stream is generated up front (not timed), so RNG and
// bookkeeping stay out of the measurement.
//
// The flow is shaped like a real lit book rather than uniform random prices:
//   - the mid price random-walks one tick at a time;
//   - passive orders land a geometric number of ticks behind the touch, so most
//     activity is in the top few levels;
//   - cancels outnumber trades several to one, as on real venues;
//   - a slice of orders are marketable IOCs that sweep one or more levels.
//
// Usage: bench_engine [--ops N] [--warmup N] [--runs N] [--seed S] [--resting N]

#include "vertexladder/clock.hpp"
#include "vertexladder/histogram.hpp"
#include "vertexladder/order_book.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using namespace vl;

namespace {

struct Op {
    enum Kind : std::uint8_t { Add, Aggress, Cancel, Replace, kKinds } kind;
    NewOrder order;  // Add / Aggress
    OrderId id;      // Cancel / Replace
    Price price;     // Replace
    Qty qty;         // Replace
};

const char* kind_name(int k) {
    static const char* n[] = {"add (passive)", "aggressive IOC", "cancel", "replace"};
    return n[k];
}

// Which generated orders are still resting, with side and price, so cancels and
// replaces in the stream target live orders the way real ones do.
struct LiveTracker {
    struct Info {
        Side side;
        Price price;
        std::size_t pos;
    };
    std::vector<OrderId> ids;
    std::unordered_map<OrderId, Info> info;

    void add(OrderId id, Side s, Price p) {
        info[id] = {s, p, ids.size()};
        ids.push_back(id);
    }
    void remove(OrderId id) {
        auto it = info.find(id);
        if (it == info.end()) return;
        const std::size_t i = it->second.pos;
        ids[i] = ids.back();
        info[ids[i]].pos = i;
        ids.pop_back();
        info.erase(it);
    }
};

struct GenListener : NullListener {
    LiveTracker* live = nullptr;
    void on_trade(const Trade& t) {
        if (t.maker_remaining == 0) live->remove(t.maker_id);
    }
};

// Size of the resting book the stream settles at (--resting). Pools and the id map are
// sized for this working set, not for the whole stream: sizing the hash table for 5M ids
// would spread 20k live keys over 256 MB and turn every lookup into a cache miss.
double kTargetResting = 20'000;
std::size_t kBookHint = 1u << 16;

std::vector<Op> generate(std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::geometric_distribution<int> depth(0.35);
    std::uniform_int_distribution<Qty> lots(1, 10);

    LiveTracker live;
    OrderBook<GenListener> shadow(kBookHint);
    shadow.listener().live = &live;

    std::vector<Op> ops;
    ops.reserve(n);
    Price mid = 1'000'000;
    OrderId next_id = 1;
    // Passive orders sit behind the touch and never cross: clamp against the opposite
    // side, since resting orders don't move when the mid drifts.
    auto passive_price = [&](Side s) {
        const Price behind = 1 + depth(rng);
        if (s == Side::Buy) {
            const Price p = mid - behind;
            const auto ask = shadow.best_ask();
            return ask ? std::min(p, *ask - 1) : p;
        }
        const Price p = mid + behind;
        const auto bid = shadow.best_bid();
        return bid ? std::max(p, *bid + 1) : p;
    };

    while (ops.size() < n) {
        if (u(rng) < 0.02) mid += (u(rng) < 0.5) ? -1 : 1;
        const double r = u(rng);
        // Cancel pressure rises with book size, so the book settles around
        // kTargetResting orders instead of draining or growing without bound.
        const double p_cancel = std::min(0.95, 0.47 * static_cast<double>(live.ids.size()) / kTargetResting);
        Op op{};
        if (r < 0.07) {
            // Marketable: priced at the opposite touch or through it, so it always trades.
            const Side s = u(rng) < 0.5 ? Side::Buy : Side::Sell;
            const Price through = depth(rng);  // 0 = take the touch only, more = sweep deeper
            const auto touch = s == Side::Buy ? shadow.best_ask() : shadow.best_bid();
            const Price ref = touch ? *touch : mid;
            op.kind = Op::Aggress;
            op.order = {next_id++, s, OrderType::Limit, TimeInForce::IOC,
                        s == Side::Buy ? ref + through : ref - through, 100 * lots(rng)};
            shadow.submit(op.order);
        } else if (r < 0.12 && !live.ids.empty()) {
            op.kind = Op::Replace;
            op.id = live.ids[rng() % live.ids.size()];
            const LiveTracker::Info inf = live.info.at(op.id);
            const Qty cur = *shadow.open_qty(op.id);
            if (u(rng) < 0.6 && cur > 100) {  // size down, keeps queue position
                op.price = inf.price;
                op.qty = cur - 100;
            } else {  // reprice, goes to the back of the new level
                op.price = passive_price(inf.side);
                op.qty = 100 * lots(rng);
            }
            shadow.replace(op.id, op.price, op.qty);
            live.remove(op.id);
            if (shadow.open_qty(op.id)) live.add(op.id, inf.side, op.price);
        } else if (!live.ids.empty() && u(rng) < p_cancel) {
            op.kind = Op::Cancel;
            op.id = live.ids[rng() % live.ids.size()];
            shadow.cancel(op.id);
            live.remove(op.id);
        } else {
            const Side s = u(rng) < 0.5 ? Side::Buy : Side::Sell;
            op.kind = Op::Add;
            op.order = {next_id++, s, OrderType::Limit, TimeInForce::GTC, passive_price(s), 100 * lots(rng)};
            shadow.submit(op.order);
            if (shadow.open_qty(op.order.id)) live.add(op.order.id, s, op.order.price);
        }
        ops.push_back(op);
    }
    return ops;
}

struct CountingListener : NullListener {
    std::uint64_t trades = 0;
    std::uint64_t volume = 0;
    std::uint64_t ops_that_traded = 0;
    OrderId last_taker = 0;
    void on_trade(const Trade& t) {
        ++trades;
        volume += t.qty;
        if (t.taker_id != last_taker) {
            ++ops_that_traded;
            last_taker = t.taker_id;
        }
    }
};

using Book = OrderBook<CountingListener>;

inline void apply(Book& b, const Op& op) {
    switch (op.kind) {
        case Op::Add:
        case Op::Aggress: b.submit(op.order); break;
        case Op::Cancel: b.cancel(op.id); break;
        case Op::Replace: b.replace(op.id, op.price, op.qty); break;
        default: break;
    }
}

std::size_t arg(int argc, char** argv, const char* name, std::size_t dflt) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return std::strtoull(argv[i + 1], nullptr, 10);
    return dflt;
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t n_ops = arg(argc, argv, "--ops", 20'000'000);
    const std::size_t warmup = arg(argc, argv, "--warmup", 2'000'000);
    const std::size_t runs = arg(argc, argv, "--runs", 5);
    const std::uint64_t seed = arg(argc, argv, "--seed", 42);
    kTargetResting = static_cast<double>(arg(argc, argv, "--resting", 20'000));
    kBookHint = static_cast<std::size_t>(kTargetResting) * 2;

    std::printf("generating %zu ops (seed %llu)...\n", n_ops, static_cast<unsigned long long>(seed));
    const std::vector<Op> ops = generate(n_ops, seed);
    std::size_t mix[Op::kKinds] = {};
    for (const Op& op : ops) ++mix[op.kind];

    const double ns_per_tick = Clock::ns_per_tick();
    std::printf("clock: %s, %.3f ns/tick\n", Clock::name(), ns_per_tick);
    std::printf("op mix (timed part): ");
    for (int k = 0; k < Op::kKinds; ++k)
        std::printf("%s %.1f%%  ", kind_name(k), 100.0 * static_cast<double>(mix[k]) / static_cast<double>(n_ops));
    std::printf("\n\n");

    // 1) Throughput: one timestamp before and after the timed stream.
    std::vector<double> rates;
    std::uint64_t trades = 0, traded_ops = 0;
    std::size_t resting = 0, bid_levels = 0, ask_levels = 0;
    for (std::size_t run = 0; run < runs; ++run) {
        Book book(kBookHint);
        for (std::size_t i = 0; i < warmup; ++i) apply(book, ops[i]);
        const std::uint64_t t0 = Clock::now();
        for (std::size_t i = warmup; i < n_ops; ++i) apply(book, ops[i]);
        const std::uint64_t t1 = Clock::now();
        const double secs = static_cast<double>(t1 - t0) * ns_per_tick * 1e-9;
        rates.push_back(static_cast<double>(n_ops - warmup) / secs);
        trades = book.listener().trades;
        traded_ops = book.listener().ops_that_traded;
        resting = book.order_count();
        bid_levels = book.level_count(Side::Buy);
        ask_levels = book.level_count(Side::Sell);
    }
    std::sort(rates.begin(), rates.end());
    const double median = rates[rates.size() / 2];
    std::printf("THROUGHPUT  (%zu timed ops, median of %zu runs)\n", n_ops - warmup, runs);
    std::printf("  %.2f M ops/s   mean %.1f ns/op   (min run %.2f, max run %.2f M ops/s)\n",
                median / 1e6, 1e9 / median, rates.front() / 1e6, rates.back() / 1e6);
    std::printf("  trades %llu (from %llu ops = %.1f%% of all ops), book at end: %zu orders, %zu bid / %zu ask levels\n\n",
                static_cast<unsigned long long>(trades), static_cast<unsigned long long>(traded_ops),
                100.0 * static_cast<double>(traded_ops) / static_cast<double>(n_ops),
                resting, bid_levels, ask_levels);

    // 2) Latency: timestamp around every single op. Adds the clock cost (~6 ns on
    // Apple Silicon, ~7 ns rdtsc) to each sample and is quantized to the clock tick.
    Histogram hist[Op::kKinds];
    Histogram all;
    {
        Book book(kBookHint);
        for (std::size_t i = 0; i < warmup; ++i) apply(book, ops[i]);
        for (std::size_t i = warmup; i < n_ops; ++i) {
            const std::uint64_t t0 = Clock::now();
            apply(book, ops[i]);
            const std::uint64_t t1 = Clock::now();
            hist[ops[i].kind].record(t1 - t0);
            all.record(t1 - t0);
        }
    }
    // Clock overhead: back-to-back reads with nothing in between.
    Histogram empty;
    for (int i = 0; i < 1'000'000; ++i) {
        const std::uint64_t t0 = Clock::now();
        const std::uint64_t t1 = Clock::now();
        empty.record(t1 - t0);
    }

    auto ns = [&](std::uint64_t ticks) { return static_cast<double>(ticks) * ns_per_tick; };
    std::printf("LATENCY PER OP (ns, includes one clock read; resolution %.1f ns)\n", ns_per_tick);
    std::printf("  %-16s %10s %8s %8s %8s %8s %10s %10s\n", "op", "count", "mean", "p50", "p90", "p99",
                "p99.9", "max");
    auto row = [&](const char* name, const Histogram& h) {
        std::printf("  %-16s %10llu %8.1f %8.0f %8.0f %8.0f %10.0f %10.0f\n", name,
                    static_cast<unsigned long long>(h.count()), h.mean() * ns_per_tick, ns(h.quantile(0.50)),
                    ns(h.quantile(0.90)), ns(h.quantile(0.99)), ns(h.quantile(0.999)), ns(h.max()));
    };
    for (int k = 0; k < Op::kKinds; ++k) row(kind_name(k), hist[k]);
    row("all", all);
    row("(empty timer)", empty);
    return 0;
}
