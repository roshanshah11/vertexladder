#include "reference_book.hpp"
#include "vertexladder/order_book.hpp"

#include <gtest/gtest.h>

using namespace vl;
using vl::test::Event;
using vl::test::RecordingListener;

namespace {

using Book = OrderBook<RecordingListener>;

NewOrder limit(OrderId id, Side s, Price p, Qty q, TimeInForce tif = TimeInForce::GTC) {
    return NewOrder{id, s, OrderType::Limit, tif, p, q};
}
NewOrder market(OrderId id, Side s, Qty q) {
    return NewOrder{id, s, OrderType::Market, TimeInForce::IOC, 0, q};
}

std::vector<Event> trades(const Book& b) {
    std::vector<Event> out;
    for (const Event& e : b.listener().events)
        if (e.kind == Event::Kind::Trade) out.push_back(e);
    return out;
}

const Event& last(const Book& b) { return b.listener().events.back(); }

}  // namespace

TEST(OrderBook, EmptyBook) {
    Book b;
    EXPECT_FALSE(b.best_bid());
    EXPECT_FALSE(b.best_ask());
    EXPECT_EQ(b.order_count(), 0u);
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, RestsNonCrossingOrdersOnBothSides) {
    Book b;
    b.submit(limit(1, Side::Buy, 100, 10));
    b.submit(limit(2, Side::Buy, 101, 5));
    b.submit(limit(3, Side::Sell, 103, 7));
    b.submit(limit(4, Side::Sell, 102, 2));
    EXPECT_EQ(*b.best_bid(), 101);
    EXPECT_EQ(*b.best_ask(), 102);
    EXPECT_EQ(b.order_count(), 4u);
    EXPECT_EQ(b.level_count(Side::Buy), 2u);
    EXPECT_EQ(b.level_count(Side::Sell), 2u);
    EXPECT_TRUE(trades(b).empty());
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, SamePriceIsFifo) {
    Book b;
    b.submit(limit(1, Side::Sell, 100, 5));
    b.submit(limit(2, Side::Sell, 100, 5));
    b.submit(limit(3, Side::Sell, 100, 5));
    b.submit(limit(9, Side::Buy, 100, 12));
    auto t = trades(b);
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[0].other, 1u);
    EXPECT_EQ(t[1].other, 2u);
    EXPECT_EQ(t[2].other, 3u);
    EXPECT_EQ(t[2].qty, 2u);
    EXPECT_EQ(*b.open_qty(3), 3u);
    EXPECT_FALSE(b.open_qty(1));
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, BuyAtExactlyTheBestAskTrades) {
    // Regression guard: if bids and asks ever shared one price index, a buy at the best
    // ask's price would be queued on the ask side and could trade against itself.
    Book b;
    b.submit(limit(1, Side::Sell, 100, 10));
    b.submit(limit(2, Side::Buy, 100, 10));
    auto t = trades(b);
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].id, 2u);
    EXPECT_EQ(t[0].other, 1u);
    EXPECT_EQ(b.order_count(), 0u);
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, TradesAtMakerPriceAndSweepsLevelsBestFirst) {
    Book b;
    b.submit(limit(1, Side::Sell, 101, 5));
    b.submit(limit(2, Side::Sell, 103, 5));
    b.submit(limit(3, Side::Sell, 102, 5));
    b.submit(limit(9, Side::Buy, 105, 12));
    auto t = trades(b);
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[0].price, 101);
    EXPECT_EQ(t[1].price, 102);
    EXPECT_EQ(t[2].price, 103);
    EXPECT_EQ(t[2].qty, 2u);
    EXPECT_EQ(*b.best_ask(), 103);
    EXPECT_FALSE(b.best_bid());  // taker fully filled, nothing rests
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, StopsAtLimitAndRestsTheRemainder) {
    Book b;
    b.submit(limit(1, Side::Sell, 101, 5));
    b.submit(limit(2, Side::Sell, 103, 5));
    b.submit(limit(9, Side::Buy, 102, 8));
    EXPECT_EQ(trades(b).size(), 1u);
    EXPECT_EQ(*b.best_bid(), 102);
    EXPECT_EQ(*b.open_qty(9), 3u);
    EXPECT_EQ(*b.best_ask(), 103);
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, SellSideMirrorsBuySide) {
    Book b;
    b.submit(limit(1, Side::Buy, 99, 5));
    b.submit(limit(2, Side::Buy, 100, 5));
    b.submit(limit(9, Side::Sell, 99, 7));
    auto t = trades(b);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[0].price, 100);
    EXPECT_EQ(t[1].price, 99);
    EXPECT_EQ(*b.open_qty(1), 3u);
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, IocCancelsTheRemainder) {
    Book b;
    b.submit(limit(1, Side::Sell, 100, 4));
    b.submit(limit(9, Side::Buy, 100, 10, TimeInForce::IOC));
    EXPECT_EQ(trades(b).size(), 1u);
    EXPECT_EQ(last(b).kind, Event::Kind::Cancel);
    EXPECT_EQ(last(b).qty, 6u);
    EXPECT_EQ(last(b).reason, static_cast<int>(CancelReason::IocRemainder));
    EXPECT_FALSE(b.best_bid());
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, FokThatCannotFullyFillDoesNothing) {
    Book b;
    b.submit(limit(1, Side::Sell, 100, 4));
    b.submit(limit(2, Side::Sell, 101, 4));
    b.submit(limit(9, Side::Buy, 101, 10, TimeInForce::FOK));
    EXPECT_TRUE(trades(b).empty());
    EXPECT_EQ(last(b).reason, static_cast<int>(CancelReason::FokUnfilled));
    EXPECT_EQ(b.order_count(), 2u);
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, FokThatCanFillTradesEverything) {
    Book b;
    b.submit(limit(1, Side::Sell, 100, 4));
    b.submit(limit(2, Side::Sell, 101, 6));
    b.submit(limit(9, Side::Buy, 101, 10, TimeInForce::FOK));
    EXPECT_EQ(trades(b).size(), 2u);
    EXPECT_EQ(b.order_count(), 0u);
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, MarketOrderSweepsAndNeverRests) {
    Book b;
    b.submit(limit(1, Side::Sell, 100, 3));
    b.submit(limit(2, Side::Sell, 250, 3));
    b.submit(market(9, Side::Buy, 10));
    EXPECT_EQ(trades(b).size(), 2u);
    EXPECT_EQ(last(b).kind, Event::Kind::Cancel);
    EXPECT_EQ(last(b).qty, 4u);
    EXPECT_FALSE(b.best_bid());
    EXPECT_FALSE(b.best_ask());
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, MarketOrderIntoEmptyBookIsCancelled) {
    Book b;
    b.submit(market(9, Side::Sell, 10));
    EXPECT_EQ(last(b).kind, Event::Kind::Cancel);
    EXPECT_EQ(b.order_count(), 0u);
}

TEST(OrderBook, RejectsBadInput) {
    Book b;
    b.submit(limit(1, Side::Buy, 100, 0));
    EXPECT_EQ(last(b).reason, static_cast<int>(RejectReason::InvalidQuantity));
    b.submit(limit(0, Side::Buy, 100, 5));
    EXPECT_EQ(last(b).reason, static_cast<int>(RejectReason::DuplicateOrderId));
    b.submit(limit(2, Side::Buy, -5, 5));
    EXPECT_EQ(last(b).reason, static_cast<int>(RejectReason::InvalidPrice));
    b.submit(limit(3, Side::Buy, 100, 5));
    b.submit(limit(3, Side::Sell, 200, 5));
    EXPECT_EQ(last(b).reason, static_cast<int>(RejectReason::DuplicateOrderId));
    EXPECT_FALSE(b.cancel(42));
    EXPECT_EQ(last(b).reason, static_cast<int>(RejectReason::UnknownOrder));
    EXPECT_EQ(b.order_count(), 1u);
}

TEST(OrderBook, CancelRemovesOrderAndEmptyLevel) {
    Book b;
    b.submit(limit(1, Side::Buy, 100, 5));
    b.submit(limit(2, Side::Buy, 100, 5));
    b.submit(limit(3, Side::Buy, 99, 5));
    EXPECT_TRUE(b.cancel(1));
    EXPECT_EQ(b.top(Side::Buy)->orders, 1u);
    EXPECT_TRUE(b.cancel(2));
    EXPECT_EQ(*b.best_bid(), 99);
    EXPECT_FALSE(b.cancel(2));
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, CancelFromMiddleOfQueueKeepsFifo) {
    Book b;
    for (OrderId id = 1; id <= 5; ++id) b.submit(limit(id, Side::Sell, 100, 1));
    b.cancel(3);
    b.submit(limit(9, Side::Buy, 100, 4));
    auto t = trades(b);
    ASSERT_EQ(t.size(), 4u);
    EXPECT_EQ(t[0].other, 1u);
    EXPECT_EQ(t[1].other, 2u);
    EXPECT_EQ(t[2].other, 4u);
    EXPECT_EQ(t[3].other, 5u);
}

TEST(OrderBook, ReduceSizeKeepsQueuePriority) {
    Book b;
    b.submit(limit(1, Side::Sell, 100, 10));
    b.submit(limit(2, Side::Sell, 100, 10));
    EXPECT_TRUE(b.replace(1, 100, 4));
    b.submit(limit(9, Side::Buy, 100, 4));
    auto t = trades(b);
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].other, 1u);  // still first in line
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, IncreasingSizeLosesPriority) {
    Book b;
    b.submit(limit(1, Side::Sell, 100, 10));
    b.submit(limit(2, Side::Sell, 100, 10));
    EXPECT_TRUE(b.replace(1, 100, 11));
    b.submit(limit(9, Side::Buy, 100, 5));
    EXPECT_EQ(trades(b)[0].other, 2u);
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, ReplaceToCrossingPriceTrades) {
    Book b;
    b.submit(limit(1, Side::Sell, 102, 5));
    b.submit(limit(2, Side::Buy, 100, 5));
    EXPECT_TRUE(b.replace(2, 102, 5));
    ASSERT_EQ(trades(b).size(), 1u);
    EXPECT_EQ(b.order_count(), 0u);
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, BookBuilderApi) {
    OrderBook<> b;
    EXPECT_TRUE(b.insert_resting(10, Side::Buy, 100, 50));
    EXPECT_TRUE(b.insert_resting(11, Side::Sell, 101, 30));
    EXPECT_FALSE(b.insert_resting(10, Side::Buy, 99, 1));
    EXPECT_TRUE(b.reduce(10, 20));
    EXPECT_EQ(b.top(Side::Buy)->qty, 30u);
    EXPECT_TRUE(b.reduce(10, 30));
    EXPECT_FALSE(b.best_bid());
    EXPECT_TRUE(b.erase(11));
    EXPECT_FALSE(b.erase(11));
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, DeepBookUsesBinarySearchPath) {
    // More than kLinearScan levels between the insert and the top of book.
    OrderBook<> b;
    for (Price p = 1000; p > 900; --p) b.insert_resting(static_cast<OrderId>(p), Side::Buy, p, 1);
    b.insert_resting(5000, Side::Buy, 950, 7);  // joins an existing deep level
    b.insert_resting(5001, Side::Buy, 850, 1);  // new level at the very bottom
    EXPECT_EQ(b.level_count(Side::Buy), 101u);
    EXPECT_TRUE(b.erase(950));
    EXPECT_TRUE(b.erase(5000));  // level 950 now empty and removed
    EXPECT_EQ(b.level_count(Side::Buy), 100u);
    EXPECT_TRUE(b.check_invariants());
}

TEST(OrderBook, PoolsRecycleWithoutGrowing) {
    OrderBook<> b(64);
    for (int round = 0; round < 1000; ++round) {
        for (OrderId id = 1; id <= 50; ++id)
            b.submit(limit(id + static_cast<OrderId>(round) * 100, Side::Buy, 100 + static_cast<Price>(id), 1));
        for (OrderId id = 1; id <= 50; ++id) b.cancel(id + static_cast<OrderId>(round) * 100);
    }
    EXPECT_EQ(b.order_count(), 0u);
    EXPECT_TRUE(b.check_invariants());
}
