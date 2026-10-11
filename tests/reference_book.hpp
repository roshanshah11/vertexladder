#pragma once

// A deliberately naive order book used as an oracle in the differential tests.
// std::map of price -> std::list, linear scans, no pools, no tricks. It is slow and
// obviously correct, which is the point: the fast book must produce exactly the same
// events and the same resting book for any sequence of operations.

#include "vertexladder/order_book.hpp"

#include <functional>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

namespace vl::test {

struct Event {
    enum class Kind { Accept, Trade, Cancel, Reject, Replace } kind;
    OrderId id = 0;
    OrderId other = 0;  // maker id for trades
    Price price = 0;
    Qty qty = 0;
    Qty rem_a = 0;  // trade: maker remaining
    Qty rem_b = 0;  // trade: taker remaining
    int reason = 0;
    bool operator==(const Event&) const = default;
};

struct RecordingListener {
    std::vector<Event> events;
    void on_accept(const NewOrder& o) {
        events.push_back({Event::Kind::Accept, o.id, 0, o.price, o.qty, 0, 0, 0});
    }
    void on_trade(const Trade& t) {
        events.push_back({Event::Kind::Trade, t.taker_id, t.maker_id, t.price, t.qty,
                          t.maker_remaining, t.taker_remaining, static_cast<int>(t.taker_side)});
    }
    void on_cancel(OrderId id, Qty q, CancelReason r) {
        events.push_back({Event::Kind::Cancel, id, 0, 0, q, 0, 0, static_cast<int>(r)});
    }
    void on_reject(OrderId id, RejectReason r) {
        events.push_back({Event::Kind::Reject, id, 0, 0, 0, 0, 0, static_cast<int>(r)});
    }
    void on_replace(OrderId id, Price p, Qty q) {
        events.push_back({Event::Kind::Replace, id, 0, p, q, 0, 0, 0});
    }
};

class ReferenceBook {
public:
    std::vector<Event> events;

    void submit(const NewOrder& o) {
        if (o.qty == 0) return reject(o.id, RejectReason::InvalidQuantity);
        if (o.id == 0 || where_.count(o.id)) return reject(o.id, RejectReason::DuplicateOrderId);
        const bool market = o.type == OrderType::Market;
        if (!market && o.price <= 0) return reject(o.id, RejectReason::InvalidPrice);
        events.push_back({Event::Kind::Accept, o.id, 0, o.price, o.qty, 0, 0, 0});
        const Price limit = market ? (o.side == Side::Buy ? kMarketBuyPrice : kMarketSellPrice) : o.price;
        if (o.tif == TimeInForce::FOK && available(o.side, limit) < o.qty)
            return cancel_event(o.id, o.qty, CancelReason::FokUnfilled);
        Qty left = match(o.id, o.side, limit, o.qty);
        if (left == 0) return;
        if (market || o.tif != TimeInForce::GTC) return cancel_event(o.id, left, CancelReason::IocRemainder);
        rest(o.id, o.side, o.price, left);
    }

    bool cancel(OrderId id) {
        auto it = where_.find(id);
        if (it == where_.end()) {
            reject(id, RejectReason::UnknownOrder);
            return false;
        }
        Qty q = find(id).qty;
        erase(id);
        cancel_event(id, q, CancelReason::UserRequested);
        return true;
    }

    bool replace(OrderId id, Price price, Qty qty) {
        auto it = where_.find(id);
        if (it == where_.end()) return reject(id, RejectReason::UnknownOrder), false;
        if (qty == 0) return reject(id, RejectReason::InvalidQuantity), false;
        if (price <= 0) return reject(id, RejectReason::InvalidPrice), false;
        auto [side, old_price] = it->second;
        RefOrder& o = find(id);
        if (price == old_price && qty <= o.qty) {
            o.qty = qty;
            events.push_back({Event::Kind::Replace, id, 0, price, qty, 0, 0, 0});
            return true;
        }
        erase(id);
        events.push_back({Event::Kind::Replace, id, 0, price, qty, 0, 0, 0});
        Qty left = match(id, side, price, qty);
        if (left) rest(id, side, price, left);
        return true;
    }

    // Levels best-first, same shape as OrderBook::depth.
    std::vector<LevelView> depth(Side s) const {
        std::vector<LevelView> out;
        auto emit = [&](const auto& kv) {
            std::uint64_t q = 0;
            for (const RefOrder& o : kv.second) q += o.qty;
            out.push_back({kv.first, q, static_cast<std::uint32_t>(kv.second.size())});
        };
        if (s == Side::Buy) for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) emit(*it);
        else for (const auto& kv : asks_) emit(kv);
        return out;
    }

    std::vector<OrderId> live_ids() const {
        std::vector<OrderId> ids;
        for (const auto& kv : where_) ids.push_back(kv.first);
        return ids;
    }
    std::optional<Price> price_of(OrderId id) const {
        auto it = where_.find(id);
        return it == where_.end() ? std::nullopt : std::optional<Price>(it->second.second);
    }
    std::optional<Qty> open_qty(OrderId id) const {
        auto it = where_.find(id);
        if (it == where_.end()) return std::nullopt;
        const auto& book = it->second.first == Side::Buy ? bids_ : asks_;
        for (const RefOrder& o : book.at(it->second.second))
            if (o.id == id) return o.qty;
        return std::nullopt;
    }

private:
    struct RefOrder {
        OrderId id;
        Qty qty;
    };
    using Queue = std::list<RefOrder>;
    std::map<Price, Queue> bids_, asks_;  // both ascending by price
    std::unordered_map<OrderId, std::pair<Side, Price>> where_;

    void reject(OrderId id, RejectReason r) {
        events.push_back({Event::Kind::Reject, id, 0, 0, 0, 0, 0, static_cast<int>(r)});
    }
    void cancel_event(OrderId id, Qty q, CancelReason r) {
        events.push_back({Event::Kind::Cancel, id, 0, 0, q, 0, 0, static_cast<int>(r)});
    }
    static bool crosses(Side s, Price limit, Price p) { return s == Side::Buy ? limit >= p : limit <= p; }

    std::uint64_t available(Side side, Price limit) const {
        std::uint64_t got = 0;
        auto add = [&](const auto& kv) {
            for (const RefOrder& o : kv.second) got += o.qty;
        };
        if (side == Side::Buy) {
            for (const auto& kv : asks_) { if (!crosses(side, limit, kv.first)) break; add(kv); }
        } else {
            for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) { if (!crosses(side, limit, it->first)) break; add(*it); }
        }
        return got;
    }

    Qty match(OrderId taker, Side side, Price limit, Qty qty) {
        auto& book = side == Side::Buy ? asks_ : bids_;
        while (qty > 0 && !book.empty()) {
            auto lvl = side == Side::Buy ? book.begin() : std::prev(book.end());
            if (!crosses(side, limit, lvl->first)) break;
            Queue& q = lvl->second;
            while (qty > 0 && !q.empty()) {
                RefOrder& m = q.front();
                Qty fill = std::min(qty, m.qty);
                m.qty -= fill;
                qty -= fill;
                events.push_back({Event::Kind::Trade, taker, m.id, lvl->first, fill, m.qty, qty,
                                  static_cast<int>(side)});
                if (m.qty == 0) {
                    where_.erase(m.id);
                    q.pop_front();
                }
            }
            if (q.empty()) book.erase(lvl);
        }
        return qty;
    }

    void rest(OrderId id, Side side, Price price, Qty qty) {
        (side == Side::Buy ? bids_ : asks_)[price].push_back({id, qty});
        where_[id] = {side, price};
    }

    RefOrder& find(OrderId id) {
        auto [side, price] = where_.at(id);
        for (RefOrder& o : (side == Side::Buy ? bids_ : asks_).at(price))
            if (o.id == id) return o;
        throw std::logic_error("missing order");
    }

    void erase(OrderId id) {
        auto [side, price] = where_.at(id);
        auto& book = side == Side::Buy ? bids_ : asks_;
        Queue& q = book.at(price);
        q.remove_if([&](const RefOrder& o) { return o.id == id; });
        if (q.empty()) book.erase(price);
        where_.erase(id);
    }
};

}  // namespace vl::test
