#pragma once

#include "vertexladder/flat_hash_map.hpp"
#include "vertexladder/types.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vl {

struct NewOrder {
    OrderId id = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GTC;
    Price price = 0;  // ignored for market orders
    Qty qty = 0;
};

struct Trade {
    OrderId taker_id;
    OrderId maker_id;
    Price price;  // always the maker's (resting) price
    Qty qty;
    Side taker_side;
    Qty maker_remaining;  // open qty left on the resting order after this fill
    Qty taker_remaining;  // open qty left on the incoming order after this fill
};

struct LevelView {
    Price price;
    std::uint64_t qty;
    std::uint32_t orders;
};

// Default listener: every callback is an empty inline function, so a book built with
// it compiles down to just the data-structure work. The engine and the tests plug in
// their own listener type. Callbacks run synchronously inside the book and must not
// call back into it.
struct NullListener {
    void on_accept(const NewOrder&) {}
    void on_trade(const Trade&) {}
    void on_cancel(OrderId, Qty /*cancelled*/, CancelReason) {}
    void on_reject(OrderId, RejectReason) {}
    void on_replace(OrderId, Price, Qty) {}
};

// A single-instrument limit order book with price-time priority matching.
//
// Layout
//   orders_  contiguous pool of 32-byte order nodes, addressed by 32-bit handle.
//            Each node sits in an intrusive doubly linked FIFO for its price level,
//            so time priority is just position in the list (no timestamps needed)
//            and removing any order is O(1).
//   levels_  pool of price levels: price, total open qty, order count, head/tail.
//   bids_/asks_ one sorted vector per side holding {price, level handle}, ordered
//            worst -> best so the best price is always back(). Matching only touches
//            back(), and new levels and removals are almost always near the top of
//            book, so the vector shifts only a few elements.
//   index_   flat hash map order id -> node handle, for O(1) cancel / replace.
//
// The book does no allocation once its pools have reached their working size.
// Not thread-safe by design: one book is owned by one thread (see DESIGN.md).
template <class Listener = NullListener>
class OrderBook {
public:
    explicit OrderBook(std::size_t expected_orders = 1u << 16, Listener listener = Listener{})
        : index_(expected_orders), listener_(std::move(listener)) {
        orders_.reserve(expected_orders);
        levels_.reserve(1024);
        bids_.reserve(1024);
        asks_.reserve(1024);
    }

    // ---- Matching engine API -------------------------------------------------------

    // Match an incoming order against the opposite side, then rest, cancel or expire
    // whatever is left according to its type and time in force.
    void submit(const NewOrder& o) {
        if (o.qty == 0) return listener_.on_reject(o.id, RejectReason::InvalidQuantity);
        if (o.id == 0 || index_.contains(o.id))
            return listener_.on_reject(o.id, RejectReason::DuplicateOrderId);
        const bool market = o.type == OrderType::Market;
        if (!market && o.price <= 0) return listener_.on_reject(o.id, RejectReason::InvalidPrice);

        listener_.on_accept(o);
        const Price limit = market ? (o.side == Side::Buy ? kMarketBuyPrice : kMarketSellPrice)
                                   : o.price;

        if (o.tif == TimeInForce::FOK && fillable_qty(o.side, limit, o.qty) < o.qty)
            return listener_.on_cancel(o.id, o.qty, CancelReason::FokUnfilled);

        const Qty left = match(o.id, o.side, limit, o.qty);
        if (left == 0) return;
        if (market || o.tif != TimeInForce::GTC)
            return listener_.on_cancel(o.id, left, CancelReason::IocRemainder);
        rest(o.id, o.side, o.price, left);
    }

    bool cancel(OrderId id) {
        const std::uint32_t* h = id ? index_.find(id) : nullptr;
        if (!h) {
            listener_.on_reject(id, RejectReason::UnknownOrder);
            return false;
        }
        const Qty open = orders_[*h].qty;
        remove(*h);
        listener_.on_cancel(id, open, CancelReason::UserRequested);
        return true;
    }

    // Cancel/replace. new_qty is the new open quantity. Cutting size at the same price
    // keeps the order's place in the queue; any other change loses time priority (the
    // order goes to the back, and can trade if the new price crosses).
    bool replace(OrderId id, Price new_price, Qty new_qty) {
        const std::uint32_t* h = id ? index_.find(id) : nullptr;
        if (!h) {
            listener_.on_reject(id, RejectReason::UnknownOrder);
            return false;
        }
        if (new_qty == 0) {
            listener_.on_reject(id, RejectReason::InvalidQuantity);
            return false;
        }
        if (new_price <= 0) {
            listener_.on_reject(id, RejectReason::InvalidPrice);
            return false;
        }
        const Handle handle = *h;
        OrderNode& n = orders_[handle];
        Level& lvl = levels_[n.level];
        if (new_price == lvl.price && new_qty <= n.qty) {
            lvl.total -= n.qty - new_qty;
            n.qty = new_qty;
            listener_.on_replace(id, new_price, new_qty);
            return true;
        }
        const Side side = n.side;
        remove(handle);
        listener_.on_replace(id, new_price, new_qty);
        const Qty left = match(id, side, new_price, new_qty);
        if (left) rest(id, side, new_price, left);
        return true;
    }

    // ---- Book-builder API (market data replay) -------------------------------------
    // When replaying a venue's feed (NASDAQ ITCH) the venue has already matched; we
    // only mirror its book. These skip matching and fire no listener events.

    bool insert_resting(OrderId id, Side side, Price price, Qty qty) {
        if (id == 0 || qty == 0 || index_.contains(id)) return false;
        rest(id, side, price, qty);
        return true;
    }

    // Take `by` shares off a resting order (partial cancel or a passive execution).
    // Removes the order if nothing is left.
    bool reduce(OrderId id, Qty by) {
        const std::uint32_t* h = id ? index_.find(id) : nullptr;
        if (!h) return false;
        OrderNode& n = orders_[*h];
        if (by >= n.qty) {
            remove(*h);
        } else {
            n.qty -= by;
            levels_[n.level].total -= by;
        }
        return true;
    }

    bool erase(OrderId id) {
        const std::uint32_t* h = id ? index_.find(id) : nullptr;
        if (!h) return false;
        remove(*h);
        return true;
    }

    // ---- Queries -------------------------------------------------------------------

    std::optional<Price> best_bid() const {
        return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.back().price);
    }
    std::optional<Price> best_ask() const {
        return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.back().price);
    }
    std::optional<LevelView> top(Side s) const {
        const auto& lad = ladder(s);
        if (lad.empty()) return std::nullopt;
        const Level& l = levels_[lad.back().level];
        return LevelView{l.price, l.total, l.count};
    }

    // Up to n levels from the best price outward.
    void depth(Side s, std::size_t n, std::vector<LevelView>& out) const {
        out.clear();
        const auto& lad = ladder(s);
        for (std::size_t i = lad.size(); i > 0 && out.size() < n; --i) {
            const Level& l = levels_[lad[i - 1].level];
            out.push_back(LevelView{l.price, l.total, l.count});
        }
    }

    // Open quantity of a resting order, or nullopt if it isn't in the book.
    std::optional<Qty> open_qty(OrderId id) const {
        const std::uint32_t* h = id ? index_.find(id) : nullptr;
        return h ? std::optional<Qty>(orders_[*h].qty) : std::nullopt;
    }

    struct OrderInfo {
        Side side;
        Price price;
        Qty qty;
        bool at_best_price;   // its level is the top of book on its side
        bool first_in_queue;  // and nothing is ahead of it at that level
    };
    std::optional<OrderInfo> info(OrderId id) const {
        const std::uint32_t* h = id ? index_.find(id) : nullptr;
        if (!h) return std::nullopt;
        const OrderNode& n = orders_[*h];
        const bool top = ladder(n.side).back().level == n.level;
        return OrderInfo{n.side, levels_[n.level].price, n.qty, top, top && n.prev == kNil};
    }

    std::size_t order_count() const noexcept { return index_.size(); }
    std::size_t level_count(Side s) const noexcept { return ladder(s).size(); }
    Listener& listener() noexcept { return listener_; }
    const Listener& listener() const noexcept { return listener_; }

    // Full structural check, O(book size). Used by the tests after every operation.
    bool check_invariants(bool allow_crossed = false) const {
        std::size_t total_orders = 0;
        for (Side s : {Side::Buy, Side::Sell}) {
            const auto& lad = ladder(s);
            for (std::size_t i = 0; i < lad.size(); ++i) {
                if (i > 0 && !better(s, lad[i].price, lad[i - 1].price)) return false;
                const Level& l = levels_[lad[i].level];
                if (l.price != lad[i].price || l.count == 0) return false;
                std::uint64_t sum = 0;
                std::uint32_t cnt = 0;
                Handle prev = kNil;
                for (Handle h = l.head; h != kNil; h = orders_[h].next) {
                    const OrderNode& n = orders_[h];
                    if (n.prev != prev || n.level != lad[i].level || n.side != s || n.qty == 0)
                        return false;
                    const std::uint32_t* idx = index_.find(n.id);
                    if (!idx || *idx != h) return false;
                    sum += n.qty;
                    ++cnt;
                    prev = h;
                }
                if (prev != l.tail || sum != l.total || cnt != l.count) return false;
                total_orders += cnt;
            }
        }
        if (total_orders != index_.size()) return false;
        if (!allow_crossed && !bids_.empty() && !asks_.empty() &&
            bids_.back().price >= asks_.back().price)
            return false;
        return true;
    }

private:
    struct OrderNode {  // 32 bytes: two per cache line
        OrderId id;
        Qty qty;  // open quantity
        Handle prev;
        Handle next;  // also the free-list link while the node is unused
        Handle level;
        Side side;
    };
    static_assert(sizeof(OrderNode) == 32);

    struct Level {
        Price price;
        std::uint64_t total;  // sum of open qty
        Handle head;          // also the free-list link while the level is unused
        Handle tail;
        std::uint32_t count;
    };

    struct LevelRef {
        Price price;
        Handle level;
    };
    using Ladder = std::vector<LevelRef>;

    // Linear scan from the top of book handles the common case (activity near the
    // inside) in a few predictable compares; past this many steps we binary search.
    static constexpr std::size_t kLinearScan = 16;

    static bool better(Side s, Price a, Price b) noexcept {
        return s == Side::Buy ? a > b : a < b;
    }
    static bool crosses(Side taker, Price limit, Price resting) noexcept {
        return taker == Side::Buy ? limit >= resting : limit <= resting;
    }
    Ladder& ladder(Side s) noexcept { return s == Side::Buy ? bids_ : asks_; }
    const Ladder& ladder(Side s) const noexcept { return s == Side::Buy ? bids_ : asks_; }

    // Index of the first entry in the ladder that is strictly better than `price`.
    // Everything before it is worse or equal.
    static std::size_t partition(Side s, const Ladder& lad, Price price) noexcept {
        std::size_t i = lad.size();
        for (std::size_t steps = 0; i > 0 && better(s, lad[i - 1].price, price); --i) {
            if (++steps == kLinearScan) {
                auto it = std::partition_point(lad.begin(), lad.begin() + static_cast<std::ptrdiff_t>(i),
                                               [&](const LevelRef& r) { return !better(s, r.price, price); });
                return static_cast<std::size_t>(it - lad.begin());
            }
        }
        return i;
    }

    // Sum of opposite-side quantity this order could take, stopping once it has `need`.
    std::uint64_t fillable_qty(Side side, Price limit, Qty need) const noexcept {
        const Ladder& lad = ladder(opposite(side));
        std::uint64_t got = 0;
        for (std::size_t i = lad.size(); i > 0 && got < need; --i) {
            if (!crosses(side, limit, lad[i - 1].price)) break;
            got += levels_[lad[i - 1].level].total;
        }
        return got;
    }

    // Walk the opposite side from the best price, filling FIFO within each level.
    // Returns the quantity still unfilled.
    Qty match(OrderId taker, Side side, Price limit, Qty qty) {
        Ladder& lad = ladder(opposite(side));
        while (qty > 0 && !lad.empty()) {
            const LevelRef best = lad.back();
            if (!crosses(side, limit, best.price)) break;
            Level& lvl = levels_[best.level];
            Handle h = lvl.head;
            while (qty > 0 && h != kNil) {
                OrderNode& maker = orders_[h];
                const Qty fill = std::min(qty, maker.qty);
                maker.qty -= fill;
                lvl.total -= fill;
                qty -= fill;
                const Handle next = maker.next;
                listener_.on_trade(Trade{taker, maker.id, best.price, fill, side, maker.qty, qty});
                if (maker.qty == 0) {  // maker is always the head here
                    index_.erase(maker.id);
                    lvl.head = next;
                    if (next != kNil) orders_[next].prev = kNil;
                    else lvl.tail = kNil;
                    --lvl.count;
                    free_order(h);
                }
                h = next;
            }
            if (lvl.count == 0) {
                lad.pop_back();
                free_level(best.level);
            }
        }
        return qty;
    }

    void rest(OrderId id, Side side, Price price, Qty qty) {
        const Handle li = find_or_add_level(side, price);
        const Handle h = alloc_order();
        Level& lvl = levels_[li];
        OrderNode& n = orders_[h];
        n = OrderNode{id, qty, lvl.tail, kNil, li, side};
        if (lvl.tail != kNil) orders_[lvl.tail].next = h;
        else lvl.head = h;
        lvl.tail = h;
        lvl.total += qty;
        ++lvl.count;
        index_.insert(id, h);
    }

    // Unlink a resting order, drop its level if now empty, recycle the node.
    void remove(Handle h) {
        OrderNode& n = orders_[h];
        Level& lvl = levels_[n.level];
        if (n.prev != kNil) orders_[n.prev].next = n.next;
        else lvl.head = n.next;
        if (n.next != kNil) orders_[n.next].prev = n.prev;
        else lvl.tail = n.prev;
        lvl.total -= n.qty;
        if (--lvl.count == 0) remove_level(n.side, n.level);
        index_.erase(n.id);
        free_order(h);
    }

    Handle find_or_add_level(Side side, Price price) {
        Ladder& lad = ladder(side);
        const std::size_t i = partition(side, lad, price);
        if (i > 0 && lad[i - 1].price == price) return lad[i - 1].level;
        const Handle li = alloc_level(price);
        lad.insert(lad.begin() + static_cast<std::ptrdiff_t>(i), LevelRef{price, li});
        return li;
    }

    void remove_level(Side side, Handle li) {
        Ladder& lad = ladder(side);
        const Price price = levels_[li].price;
        const std::size_t i = partition(side, lad, price);
        assert(i > 0 && lad[i - 1].level == li);
        lad.erase(lad.begin() + static_cast<std::ptrdiff_t>(i - 1));
        free_level(li);
    }

    Handle alloc_order() {
        if (order_free_ != kNil) {
            const Handle h = order_free_;
            order_free_ = orders_[h].next;
            return h;
        }
        orders_.emplace_back();
        return static_cast<Handle>(orders_.size() - 1);
    }
    void free_order(Handle h) noexcept {
        orders_[h].next = order_free_;
        order_free_ = h;
    }

    Handle alloc_level(Price price) {
        Handle h;
        if (level_free_ != kNil) {
            h = level_free_;
            level_free_ = levels_[h].head;
        } else {
            levels_.emplace_back();
            h = static_cast<Handle>(levels_.size() - 1);
        }
        levels_[h] = Level{price, 0, kNil, kNil, 0};
        return h;
    }
    void free_level(Handle h) noexcept {
        levels_[h].head = level_free_;
        level_free_ = h;
    }

    std::vector<OrderNode> orders_;
    std::vector<Level> levels_;
    Ladder bids_;
    Ladder asks_;
    FlatHashMap index_;
    Handle order_free_ = kNil;
    Handle level_free_ = kNil;
    Listener listener_;
};

}  // namespace vl
