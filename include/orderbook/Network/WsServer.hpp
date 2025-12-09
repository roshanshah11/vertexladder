#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <set>

namespace orderbook {

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

// Forward declaration
class WsSession;

class WsServer : public std::enable_shared_from_this<WsServer> {
public:
    WsServer();
    ~WsServer();

    void start(uint16_t port);
    void stop();
    // void broadcast(const std::string& msg); // Removed direct broadcast

    // New API for Conflation
    void updateTopOfBook(double bid, double bidQty, double ask, double askQty);
    void updateLastTrade(double price, double qty);

    // Internal use
    void join(std::shared_ptr<WsSession> session);
    void leave(std::shared_ptr<WsSession> session);

private:
    void run();
    void do_accept();
    void on_accept(beast::error_code ec, tcp::socket socket);
    void broadcastLoop(); // New broadcast loop
    void broadcast(const std::string& msg); // Made private

    net::io_context ioc_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    std::unique_ptr<std::thread> thread_;
    std::mutex mutex_;
    std::set<std::shared_ptr<WsSession>> sessions_;
    bool running_;

    // Conflation State
    struct BookState {
        double bid = 0.0;
        double bidQty = 0.0;
        double ask = 0.0;
        double askQty = 0.0;
        double lastPrice = 0.0;
        double lastQty = 0.0;
        long long timestamp = 0;
    };

    std::mutex state_mutex_;
    BookState latest_state_;
    std::unique_ptr<std::thread> broadcast_thread_;
    std::atomic<bool> stop_signal_{false};
};

} // namespace orderbook
