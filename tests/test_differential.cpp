// Differential test: drive the real book and the naive reference book with the same
// random stream of submits, cancels and replaces, and require identical event streams
// and identical resting books. This is what catches the bugs unit tests don't think of.

#include "reference_book.hpp"
#include "vertexladder/order_book.hpp"

#include <gtest/gtest.h>

#include <random>

using namespace vl;
using vl::test::Event;
using vl::test::RecordingListener;
using vl::test::ReferenceBook;

namespace {

struct Params {
    std::uint64_t seed;
    int ops;
    Price mid;
    Price spread;  // prices drawn from [mid - spread, mid + spread]
    std::size_t pool_hint;
};

void compare_books(const OrderBook<RecordingListener>& fast, const ReferenceBook& ref) {
    std::vector<LevelView> got;
    for (Side s : {Side::Buy, Side::Sell}) {
        fast.depth(s, 1u << 20, got);
        auto want = ref.depth(s);
        ASSERT_EQ(got.size(), want.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            ASSERT_EQ(got[i].price, want[i].price);
            ASSERT_EQ(got[i].qty, want[i].qty);
            ASSERT_EQ(got[i].orders, want[i].orders);
        }
    }
    for (OrderId id : ref.live_ids()) ASSERT_EQ(fast.open_qty(id), ref.open_qty(id));
}

void run(const Params& p) {
    OrderBook<RecordingListener> fast(p.pool_hint);
    ReferenceBook ref;
    std::mt19937_64 rng(p.seed);
    std::uniform_int_distribution<int> op(0, 99);
    std::uniform_int_distribution<Price> px(p.mid - p.spread, p.mid + p.spread);
    std::uniform_int_distribution<Qty> qty(1, 100);
    std::vector<OrderId> ids;  // ids we have used, live or not
    OrderId next_id = 1;

    for (int i = 0; i < p.ops; ++i) {
        const int r = op(rng);
        if (r < 55 || ids.empty()) {
            NewOrder o;
            o.id = next_id++;
            o.side = (rng() & 1) ? Side::Buy : Side::Sell;
            o.qty = qty(rng);
            const int kind = static_cast<int>(rng() % 20);
            if (kind == 0) {
                o.type = OrderType::Market;
                o.tif = TimeInForce::IOC;
            } else {
                o.price = px(rng);
                o.tif = kind == 1 ? TimeInForce::IOC : kind == 2 ? TimeInForce::FOK : TimeInForce::GTC;
            }
            if (rng() % 500 == 0) o.qty = 0;                 // invalid
            if (rng() % 500 == 0 && !ids.empty()) o.id = ids[rng() % ids.size()];  // duplicate
            fast.submit(o);
            ref.submit(o);
            ids.push_back(o.id);
        } else if (r < 90) {
            OrderId id = ids[rng() % ids.size()];
            if (rng() % 50 == 0) id = next_id + 1000;  // unknown
            ASSERT_EQ(fast.cancel(id), ref.cancel(id));
        } else {
            OrderId id = ids[rng() % ids.size()];
            // Half the time keep the current price, to exercise the keep-priority path.
            const auto cur_px = ref.price_of(id);
            const Price np = (cur_px && (rng() & 1)) ? *cur_px : px(rng);
            const auto cur = ref.open_qty(id);
            const Qty nq = cur && (rng() & 1) ? std::max<Qty>(1, *cur / 2) : qty(rng);
            ASSERT_EQ(fast.replace(id, np, nq), ref.replace(id, np, nq));
        }

        ASSERT_EQ(fast.listener().events.size(), ref.events.size()) << "op " << i;
        if (i % 64 == 0 || i == p.ops - 1) {
            ASSERT_TRUE(fast.check_invariants()) << "op " << i;
            compare_books(fast, ref);
        }
    }
    ASSERT_EQ(fast.listener().events, ref.events);
}

}  // namespace

// Tight price range: lots of crossing, sweeps, partial fills, levels emptying.
TEST(Differential, TightBookManyTrades) {
    for (std::uint64_t seed = 1; seed <= 8; ++seed) run({seed, 50'000, 10'000, 8, 64});
}

// Wide price range: deep books, the binary-search path, long ladders.
TEST(Differential, WideBookDeepLadders) {
    for (std::uint64_t seed = 100; seed <= 104; ++seed) run({seed, 50'000, 10'000, 400, 64});
}

// Tiny pool hint so the pools and the hash map have to grow mid-run.
TEST(Differential, PoolGrowth) { run({7, 100'000, 5'000, 50, 4}); }
