#pragma once

// Small blocking FIX client for tests and the demo client.

#include "vertexladder/fix.hpp"

#include <boost/asio.hpp>

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <thread>

namespace vl::test {

// A parsed message copied out of the receive buffer.
struct OwnedMessage {
    std::map<int, std::string> f;
    std::string type() const { return get(fix::tag::MsgType); }
    std::string get(int t) const {
        auto it = f.find(t);
        return it == f.end() ? std::string() : it->second;
    }
};

class FixTestClient {
public:
    FixTestClient(std::uint16_t port, std::string comp_id, const char* host = "127.0.0.1")
        : comp_(std::move(comp_id)), sock_(io_) {
        sock_.connect({boost::asio::ip::make_address(host), port});
        sock_.set_option(boost::asio::ip::tcp::no_delay(true));
        sock_.non_blocking(true);
    }

    template <class F>
    void send(std::string_view type, F&& fields, std::optional<std::uint64_t> seq_override = std::nullopt) {
        std::array<char, 32> ts;
        fix::Writer w(type, comp_, "VERTEXLADDER", seq_override ? *seq_override : seq_++, fix::utc_timestamp(ts));
        fields(w);
        send_raw(w.finish());
    }
    void send(std::string_view type) {
        send(type, [](fix::Writer&) {});
    }

    void send_raw(std::string_view bytes) {
        std::size_t done = 0;
        while (done < bytes.size()) {
            boost::system::error_code ec;
            done += sock_.write_some(boost::asio::buffer(bytes.data() + done, bytes.size() - done), ec);
            if (ec == boost::asio::error::would_block) std::this_thread::yield();
            else if (ec) return;
        }
    }

    void logon(int heartbeat = 30) {
        send("A", [&](fix::Writer& w) {
            w.add(fix::tag::EncryptMethod, std::uint64_t{0});
            w.add(fix::tag::HeartBtInt, static_cast<std::uint64_t>(heartbeat));
        });
    }

    // Next message, or nullopt on timeout / disconnect.
    std::optional<OwnedMessage> next(std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            fix::Message m;
            std::size_t used = 0;
            const auto st = fix::parse(buf_, m, used);
            if (st == fix::ParseStatus::Ok) {
                OwnedMessage out;
                for (std::size_t i = 0; i < m.size(); ++i) out.f[m[i].tag] = std::string(m[i].value);
                buf_.erase(0, used);
                return out;
            }
            if (st == fix::ParseStatus::Garbled) {
                buf_.erase(0, used);
                continue;
            }
            char tmp[4096];
            boost::system::error_code ec;
            const std::size_t n = sock_.read_some(boost::asio::buffer(tmp), ec);
            if (!ec) {
                buf_.append(tmp, n);
                continue;
            }
            if (ec != boost::asio::error::would_block) return std::nullopt;  // closed
            if (std::chrono::steady_clock::now() > deadline) return std::nullopt;
            // Spin rather than yield: on macOS yielding can give the core away for tens
            // of microseconds, which swamps a ~10 us round trip.
        }
    }

    // Skip heartbeats until a message of `type` arrives.
    std::optional<OwnedMessage> expect(std::string_view type,
                                       std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
        for (;;) {
            auto m = next(timeout);
            if (!m) return std::nullopt;
            if (m->type() == "0") continue;
            if (m->type() == type) return m;
            return m;  // let the test see the unexpected message
        }
    }

    void close() {
        boost::system::error_code ec;
        sock_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        sock_.close(ec);
    }

    std::uint64_t next_seq() const { return seq_; }

private:
    std::string comp_;
    boost::asio::io_context io_;
    boost::asio::ip::tcp::socket sock_;
    std::string buf_;
    std::uint64_t seq_ = 1;
};

}  // namespace vl::test
