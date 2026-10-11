#include "vertexladder/gateway.hpp"

#include "vertexladder/fix.hpp"

#include <boost/asio.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>

namespace vl {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

char fix_side(Side s) { return s == Side::Buy ? '1' : '2'; }

}  // namespace

class FixGateway::Impl {
public:
    Impl(GatewayConfig cfg, Engine::CommandRing& to_engine, Engine::EventRing& from_engine)
        : cfg_(std::move(cfg)),
          to_engine_(to_engine),
          from_engine_(from_engine),
          acceptor_(io_, tcp::endpoint(tcp::v4(), cfg_.port), /*reuse_addr=*/true) {
        for (std::size_t i = 0; i < cfg_.symbols.size(); ++i)
            symbol_index_[cfg_.symbols[i]] = static_cast<std::uint16_t>(i);
        acceptor_.non_blocking(true);
    }

    std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

    // The event loop busy-polls non-blocking sockets directly instead of running Asio's
    // reactor. Measured on an M3 Pro: io_context::poll() costs ~12 us per call even with
    // nothing ready (a kqueue syscall plus bookkeeping), and an order needs two passes
    // (read it, then write the ack), so the reactor alone added ~40 us to a round trip
    // that is ~7 us on bare loopback TCP. A non-blocking read_some is one recv() call.
    //
    // Hot path, every pass: read each session, drain engine events, flush writes.
    // Every millisecond: accept new connections, heartbeats, timeouts, reap sessions.
    void run(const std::atomic<bool>& stop) {
        using sc = std::chrono::steady_clock;
        auto last_work = sc::now();
        auto last_housekeeping = sc::time_point{};
        while (!stop.load(std::memory_order_relaxed)) {
            std::size_t work = 0;
            for (auto& sp : sessions_) work += sp->poll_read();
            work += drain_events();
            for (auto& sp : sessions_) sp->flush();
            const auto now = sc::now();
            if (now - last_housekeeping > std::chrono::milliseconds(1)) {
                work += accept_pending();
                for (auto& sp : sessions_) sp->housekeeping(now);
                reap();
                last_housekeeping = now;
            }
            if (work) {
                last_work = now;
            } else if (now - last_work > std::chrono::milliseconds(5)) {
                // Idle for 5 ms: nap so an idle gateway doesn't pin a core.
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
        acceptor_.close();
        for (auto& sp : sessions_) sp->close("server shutting down");
        for (auto& sp : sessions_) sp->flush();
        reap();
    }

    Stats stats() const { return stats_; }

private:
    // ---- Sessions -------------------------------------------------------------------
    class Session {
    public:
        using Clock = std::chrono::steady_clock;

        Session(Impl& gw, tcp::socket sock, std::uint64_t id) : gw_(gw), sock_(std::move(sock)), id_(id) {
            sock_.set_option(tcp::no_delay(true));  // never batch small order messages
            sock_.non_blocking(true);
            last_in_ = last_out_ = Clock::now();
        }

        std::uint64_t id() const { return id_; }
        bool gone() const { return gone_; }

        void send(std::string_view type, auto&& add_fields) {
            if (!open_) return;
            std::array<char, 32> ts;
            fix::Writer w(type, gw_.cfg_.comp_id, target_, out_seq_++, fix::utc_timestamp(ts));
            add_fields(w);
            const std::string_view msg = w.finish();
            if (msg.empty()) return;
            out_.append(msg);
            ++gw_.stats_.messages_out;
            last_out_ = Clock::now();
        }

        void close(std::string_view why) {
            if (!open_) return;
            if (logged_on_) send("5", [&](fix::Writer& w) { w.add(fix::tag::Text, why); });
            logged_on_ = false;
            open_ = false;
            closing_ = true;
            flush();
        }

        // One non-blocking recv. Returns 1 if bytes arrived.
        std::size_t poll_read() {
            if (!open_) return 0;
            boost::system::error_code ec;
            const std::size_t n = sock_.read_some(asio::buffer(buf_.data() + len_, buf_.size() - len_), ec);
            if (ec == asio::error::would_block) return 0;
            if (ec) {
                shutdown();
                return 1;
            }
            len_ += n;
            last_in_ = Clock::now();
            consume();
            return 1;
        }

        // Write whatever is queued without blocking; keep the rest for the next pass.
        void flush() {
            while (out_off_ < out_.size()) {
                boost::system::error_code ec;
                const std::size_t n =
                    sock_.write_some(asio::buffer(out_.data() + out_off_, out_.size() - out_off_), ec);
                if (ec == asio::error::would_block) return;
                if (ec) return shutdown();
                out_off_ += n;
            }
            out_.clear();
            out_off_ = 0;
            if (closing_) shutdown();
        }

        // Heartbeats, and a dead-peer check at twice the heartbeat interval.
        void housekeeping(Clock::time_point now) {
            if (!open_) return;
            if (logged_on_ && now - last_out_ >= std::chrono::seconds(heartbeat_s_)) send("0", [](fix::Writer&) {});
            if (now - last_in_ >= std::chrono::seconds(2 * heartbeat_s_ + 1)) close("heartbeat timeout");
        }

    private:
        void consume() {
            std::size_t pos = 0;
            fix::Message m;
            while (open_ && pos < len_) {
                std::size_t used = 0;
                const auto st = fix::parse({buf_.data() + pos, len_ - pos}, m, used);
                if (st == fix::ParseStatus::NeedMore) break;
                pos += used;
                if (st == fix::ParseStatus::Garbled) {
                    ++gw_.stats_.garbled;  // FIX: ignore garbled messages, don't reply
                    continue;
                }
                ++gw_.stats_.messages_in;
                on_message(m);
            }
            std::memmove(buf_.data(), buf_.data() + pos, len_ - pos);
            len_ -= pos;
            if (len_ == buf_.size()) close("message too large");
        }

        void on_message(const fix::Message& m) {
            const std::string_view type = m.msg_type();
            const auto seq = m.get_uint(fix::tag::MsgSeqNum);
            if (!seq) return close("missing MsgSeqNum");
            if (!logged_on_) {
                if (type != "A") return close("first message must be Logon");
                target_ = std::string(m.get(fix::tag::SenderCompID));
                if (target_.empty()) return close("missing SenderCompID");
                if (const auto hb = m.get_uint(fix::tag::HeartBtInt); hb && *hb > 0 && *hb < 3600)
                    heartbeat_s_ = static_cast<int>(*hb);
            }
            if (*seq < in_seq_) {
                if (m.get(43) == "Y") return;  // PossDupFlag: a duplicate we already processed
                return close("MsgSeqNum too low");
            }
            if (*seq > in_seq_) {
                char why[96];
                std::snprintf(why, sizeof why, "MsgSeqNum gap: expected %llu got %llu (resend not supported)",
                              static_cast<unsigned long long>(in_seq_), static_cast<unsigned long long>(*seq));
                return close(why);
            }
            ++in_seq_;

            if (type == "A") {
                if (logged_on_) return;
                logged_on_ = true;
                send("A", [&](fix::Writer& w) {
                    w.add(fix::tag::EncryptMethod, std::uint64_t{0});
                    w.add(fix::tag::HeartBtInt, static_cast<std::uint64_t>(heartbeat_s_));
                });
            } else if (type == "0") {
                // Heartbeat: nothing to do, last_in_ already updated.
            } else if (type == "1") {
                const std::string_view req = m.get(fix::tag::TestReqID);
                send("0", [&](fix::Writer& w) { w.add(fix::tag::TestReqID, req); });
            } else if (type == "5") {
                close("logout acknowledged");
            } else if (type == "D") {
                gw_.on_new_order(*this, m);
            } else if (type == "F") {
                gw_.on_cancel(*this, m);
            } else if (type == "G") {
                gw_.on_replace(*this, m);
            } else {
                send("3", [&](fix::Writer& w) {  // session-level Reject
                    w.add(fix::tag::RefSeqNum, *seq);
                    w.add(fix::tag::Text, "unsupported MsgType");
                });
            }
        }

        // Marks the session gone; the gateway reaps it (and cancels its orders) on the
        // next housekeeping pass, outside any loop over sessions_.
        void shutdown() {
            if (gone_) return;
            boost::system::error_code ignored;
            sock_.shutdown(tcp::socket::shutdown_both, ignored);
            sock_.close(ignored);
            open_ = false;
            gone_ = true;
        }

        Impl& gw_;
        tcp::socket sock_;
        std::uint64_t id_;
        std::array<char, 1u << 16> buf_{};
        std::size_t len_ = 0;
        std::string out_;  // bytes waiting to be written
        std::size_t out_off_ = 0;
        std::string target_;
        std::uint64_t in_seq_ = 1;
        std::uint64_t out_seq_ = 1;
        int heartbeat_s_ = 30;
        bool logged_on_ = false;
        bool open_ = true;
        bool closing_ = false;
        bool gone_ = false;
        Clock::time_point last_in_{}, last_out_{};
    };

    // ---- Order state (gateway side) -----------------------------------------------
    struct OrderState {
        std::uint64_t session;
        std::string clordid;
        std::string pending_clordid;  // ClOrdID of an in-flight cancel or replace
        std::string orig_clordid;
        std::uint16_t book;
        Side side;
        Qty order_qty;  // FIX OrderQty: total, including what has filled
        Qty cum = 0;
        Price price;
        long double notional = 0;  // for AvgPx
        Qty pending_qty = 0;       // replace in flight: applied only once the engine confirms
        Price pending_price = 0;
        // The order is done (filled or cancelled) but a cancel/replace for it is still in
        // flight; keep the state so that request still gets its OrderCancelReject.
        bool finished = false;
    };

    std::size_t accept_pending() {
        std::size_t n = 0;
        for (;;) {
            boost::system::error_code ec;
            tcp::socket sock = acceptor_.accept(ec);
            if (ec) return n;  // would_block: nobody waiting
            const std::uint64_t id = ++next_session_;
            sessions_.push_back(std::make_unique<Session>(*this, std::move(sock), id));
            by_session_id_[id] = sessions_.back().get();
            ++stats_.sessions;
            ++n;
        }
    }

    // Drop closed sessions and, if configured, cancel everything they left resting.
    void reap() {
        for (std::size_t i = 0; i < sessions_.size();) {
            if (!sessions_[i]->gone()) {
                ++i;
                continue;
            }
            const std::uint64_t id = sessions_[i]->id();
            by_session_id_.erase(id);
            sessions_[i] = std::move(sessions_.back());
            sessions_.pop_back();
            on_session_gone(id);
        }
    }

    std::string key(std::uint64_t session, std::string_view clordid) const {
        std::string k = std::to_string(session);
        k += '|';
        k += clordid;
        return k;
    }

    void push(const Command& c) {
        while (!to_engine_.try_push(c)) std::this_thread::yield();  // backpressure, never drop
    }

    void reject_order(Session& s, const fix::Message& m, std::string_view why, int reason_code) {
        s.send("8", [&](fix::Writer& w) {
            w.add(fix::tag::OrderID, "NONE");
            w.add(fix::tag::ClOrdID, m.get(fix::tag::ClOrdID));
            w.add(fix::tag::ExecID, ++exec_id_);
            w.add(fix::tag::ExecType, '8');
            w.add(fix::tag::OrdStatus, '8');
            w.add(fix::tag::Symbol, m.get(fix::tag::Symbol));
            w.add(fix::tag::Side, m.get(fix::tag::Side).empty() ? std::string_view("1") : m.get(fix::tag::Side));
            w.add(fix::tag::LeavesQty, std::uint64_t{0});
            w.add(fix::tag::CumQty, std::uint64_t{0});
            w.add(fix::tag::AvgPx, std::uint64_t{0});
            w.add(fix::tag::OrdRejReason, static_cast<std::uint64_t>(reason_code));
            w.add(fix::tag::Text, why);
        });
    }

    void cancel_reject(Session& s, std::string_view clordid, std::string_view orig, char response_to,
                       std::string_view why) {
        s.send("9", [&](fix::Writer& w) {
            w.add(fix::tag::OrderID, "NONE");
            w.add(fix::tag::ClOrdID, clordid);
            w.add(fix::tag::OrigClOrdID, orig);
            w.add(fix::tag::OrdStatus, '8');
            w.add(fix::tag::CxlRejResponseTo, response_to);
            w.add(fix::tag::Text, why);
        });
    }

    void on_new_order(Session& s, const fix::Message& m) {
        const std::string_view clordid = m.get(fix::tag::ClOrdID);
        if (clordid.empty()) return reject_order(s, m, "missing ClOrdID", 99);
        if (by_clordid_.count(key(s.id(), clordid))) return reject_order(s, m, "duplicate ClOrdID", 6);
        const auto sym = symbol_index_.find(std::string(m.get(fix::tag::Symbol)));
        if (sym == symbol_index_.end()) return reject_order(s, m, "unknown symbol", 1);
        const std::string_view side = m.get(fix::tag::Side);
        if (side != "1" && side != "2") return reject_order(s, m, "unsupported Side", 99);
        const auto qty = m.get_uint(fix::tag::OrderQty);
        if (!qty || *qty == 0 || *qty > UINT32_MAX) return reject_order(s, m, "bad OrderQty", 13);
        const std::string_view ord_type = m.get(fix::tag::OrdType);
        Command c;
        c.type = Command::Type::New;
        c.book = sym->second;
        c.side = side == "1" ? Side::Buy : Side::Sell;
        c.qty = static_cast<Qty>(*qty);
        if (ord_type == "1") {
            c.order_type = OrderType::Market;
            c.tif = TimeInForce::IOC;
        } else if (ord_type == "2") {
            const auto px = fix::parse_price(m.get(fix::tag::Price), cfg_.price_decimals);
            if (!px || *px <= 0) return reject_order(s, m, "bad Price", 99);
            c.price = *px;
            const std::string_view tif = m.get(fix::tag::TimeInForce);
            if (tif.empty() || tif == "0" || tif == "1") c.tif = TimeInForce::GTC;  // Day treated as GTC
            else if (tif == "3") c.tif = TimeInForce::IOC;
            else if (tif == "4") c.tif = TimeInForce::FOK;
            else return reject_order(s, m, "unsupported TimeInForce", 99);
        } else {
            return reject_order(s, m, "unsupported OrdType", 99);
        }
        c.id = ++next_order_;
        orders_[c.id] = OrderState{s.id(), std::string(clordid), {}, {}, c.book, c.side, c.qty, 0, c.price};
        by_clordid_[key(s.id(), clordid)] = c.id;
        push(c);
    }

    OrderState* find_live(Session& s, std::string_view orig, OrderId& id) {
        auto it = by_clordid_.find(key(s.id(), orig));
        if (it == by_clordid_.end()) return nullptr;
        auto o = orders_.find(it->second);
        if (o == orders_.end() || o->second.finished) return nullptr;
        id = it->second;
        return &o->second;
    }

    void on_cancel(Session& s, const fix::Message& m) {
        const std::string_view clordid = m.get(fix::tag::ClOrdID), orig = m.get(fix::tag::OrigClOrdID);
        OrderId id = 0;
        OrderState* o = find_live(s, orig, id);
        if (!o) return cancel_reject(s, clordid, orig, '1', "unknown order");
        if (!o->pending_clordid.empty()) return cancel_reject(s, clordid, orig, '1', "request already pending");
        o->pending_clordid = std::string(clordid);
        Command c;
        c.type = Command::Type::Cancel;
        c.book = o->book;
        c.id = id;
        push(c);
    }

    void on_replace(Session& s, const fix::Message& m) {
        const std::string_view clordid = m.get(fix::tag::ClOrdID), orig = m.get(fix::tag::OrigClOrdID);
        OrderId id = 0;
        OrderState* o = find_live(s, orig, id);
        if (!o) return cancel_reject(s, clordid, orig, '2', "unknown order");
        const auto qty = m.get_uint(fix::tag::OrderQty);
        const auto px = fix::parse_price(m.get(fix::tag::Price), cfg_.price_decimals);
        if (!qty || !px || *px <= 0) return cancel_reject(s, clordid, orig, '2', "bad OrderQty or Price");
        if (*qty <= o->cum || *qty > UINT32_MAX) return cancel_reject(s, clordid, orig, '2', "bad OrderQty");
        if (!o->pending_clordid.empty()) return cancel_reject(s, clordid, orig, '2', "request already pending");
        o->pending_clordid = std::string(clordid);
        o->pending_qty = static_cast<Qty>(*qty);
        o->pending_price = *px;
        Command c;
        c.type = Command::Type::Replace;
        c.book = o->book;
        c.id = id;
        c.price = *px;
        c.qty = static_cast<Qty>(*qty - o->cum);  // engine works in open quantity
        push(c);
    }

    void on_session_gone(std::uint64_t session) {
        if (!cfg_.cancel_on_disconnect) return;
        for (auto& [id, o] : orders_) {
            if (o.session != session || o.finished) continue;
            Command c;
            c.type = Command::Type::Cancel;
            c.book = o.book;
            c.id = id;
            push(c);
        }
    }

    // ---- Engine events -> ExecutionReports ----------------------------------------
    std::size_t drain_events() {
        std::size_t n = 0;
        EngineEvent ev;
        while (n < 4096 && from_engine_.try_pop(ev)) {
            ++n;
            on_event(ev);
        }
        return n;
    }

    void on_event(const EngineEvent& ev) {
        auto it = orders_.find(ev.id);
        if (it == orders_.end()) return;
        OrderState& o = it->second;
        auto sit = by_session_id_.find(o.session);
        Session* s = sit == by_session_id_.end() || sit->second->gone() ? nullptr : sit->second;
        bool done = false;

        switch (ev.type) {
            case EngineEvent::Type::Accepted:
                if (s) exec_report(*s, ev.id, o, '0', '0', ev.qty, 0, 0);
                break;
            case EngineEvent::Type::Fill: {
                o.cum += ev.qty;
                o.notional += static_cast<long double>(ev.price) * ev.qty;
                done = ev.leaves == 0;
                if (s) exec_report(*s, ev.id, o, 'F', done ? '2' : '1', ev.leaves, ev.qty, ev.price);
                break;
            }
            case EngineEvent::Type::Cancelled: {
                done = true;
                const bool requested = ev.reason == static_cast<std::uint8_t>(CancelReason::UserRequested);
                if (requested && !o.pending_clordid.empty()) {
                    o.orig_clordid = o.clordid;
                    o.clordid = o.pending_clordid;
                    o.pending_clordid.clear();
                }
                if (s) exec_report(*s, ev.id, o, '4', '4', 0, 0, 0);
                break;
            }
            case EngineEvent::Type::Rejected:
                if (ev.cause == Command::Type::New) {
                    done = true;
                    if (s) exec_report(*s, ev.id, o, '8', '8', 0, 0, 0);
                } else {
                    // Cancel/replace for an order that filled or was cancelled first.
                    if (s && !o.pending_clordid.empty())
                        cancel_reject(*s, o.pending_clordid, o.clordid, ev.cause == Command::Type::Cancel ? '1' : '2',
                                      "too late: order already filled or cancelled");
                    o.pending_clordid.clear();
                    done = o.finished;
                }
                break;
            case EngineEvent::Type::Replaced:
                by_clordid_.erase(key(o.session, o.clordid));
                o.orig_clordid = o.clordid;
                o.clordid = o.pending_clordid;
                o.pending_clordid.clear();
                o.order_qty = o.pending_qty;
                o.price = o.pending_price;
                by_clordid_[key(o.session, o.clordid)] = ev.id;
                if (s) exec_report(*s, ev.id, o, '5', o.cum ? '1' : '0', ev.qty, 0, 0);
                break;
        }
        if (done) {
            by_clordid_.erase(key(o.session, o.clordid));
            if (!o.orig_clordid.empty()) by_clordid_.erase(key(o.session, o.orig_clordid));
            if (!o.pending_clordid.empty() && ev.type != EngineEvent::Type::Rejected) {
                o.finished = true;  // wait for the in-flight request's reject
            } else {
                orders_.erase(it);
            }
        }
    }

    void exec_report(Session& s, OrderId id, const OrderState& o, char exec_type, char status, Qty leaves,
                     Qty last_qty, Price last_px) {
        const int d = cfg_.price_decimals;
        s.send("8", [&](fix::Writer& w) {
            w.add(fix::tag::OrderID, id);
            w.add(fix::tag::ClOrdID, o.clordid);
            if ((exec_type == '4' || exec_type == '5') && !o.orig_clordid.empty())
                w.add(fix::tag::OrigClOrdID, o.orig_clordid);
            w.add(fix::tag::ExecID, ++exec_id_);
            w.add(fix::tag::ExecType, exec_type);
            w.add(fix::tag::OrdStatus, status);
            w.add(fix::tag::Symbol, cfg_.symbols[o.book]);
            w.add(fix::tag::Side, fix_side(o.side));
            w.add(fix::tag::OrderQty, static_cast<std::uint64_t>(o.order_qty));
            if (o.price) w.add_price(fix::tag::Price, o.price, d);
            if (exec_type == 'F') {
                w.add(fix::tag::LastQty, static_cast<std::uint64_t>(last_qty));
                w.add_price(fix::tag::LastPx, last_px, d);
            }
            w.add(fix::tag::LeavesQty, static_cast<std::uint64_t>(leaves));
            w.add(fix::tag::CumQty, static_cast<std::uint64_t>(o.cum));
            const Price avg = o.cum ? static_cast<Price>(o.notional / o.cum + 0.5L) : 0;
            w.add_price(fix::tag::AvgPx, avg, d);
        });
    }

    GatewayConfig cfg_;
    Engine::CommandRing& to_engine_;
    Engine::EventRing& from_engine_;
    asio::io_context io_;
    tcp::acceptor acceptor_;
    std::unordered_map<std::string, std::uint16_t> symbol_index_;
    std::vector<std::unique_ptr<Session>> sessions_;
    std::unordered_map<std::uint64_t, Session*> by_session_id_;
    std::unordered_map<OrderId, OrderState> orders_;
    std::unordered_map<std::string, OrderId> by_clordid_;
    std::uint64_t next_session_ = 0;
    OrderId next_order_ = 0;
    std::uint64_t exec_id_ = 0;
    Stats stats_;
};

FixGateway::FixGateway(GatewayConfig cfg, Engine::CommandRing& to_engine, Engine::EventRing& from_engine)
    : impl_(std::make_unique<Impl>(std::move(cfg), to_engine, from_engine)) {}
FixGateway::~FixGateway() = default;
std::uint16_t FixGateway::port() const { return impl_->port(); }
void FixGateway::run(const std::atomic<bool>& stop) { impl_->run(stop); }
FixGateway::Stats FixGateway::stats() const { return impl_->stats(); }

}  // namespace vl
