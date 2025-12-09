#include "orderbook/Network/WsServer.hpp"
#include <boost/asio/dispatch.hpp>

namespace orderbook {

//------------------------------------------------------------------------------

class WsSession : public std::enable_shared_from_this<WsSession> {
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    std::shared_ptr<WsServer> server_;

public:
    explicit WsSession(tcp::socket&& socket, std::shared_ptr<WsServer> server)
        : ws_(std::move(socket))
        , server_(server) {
    }

    void run() {
        // Set suggested timeout settings for the websocket
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

        // Set a decorator to change the Server of the handshake
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res) {
                res.set(http::field::server,
                    std::string(BOOST_BEAST_VERSION_STRING) + " OrderBook-WsServer");
            }));

        // Accept the websocket handshake
        ws_.async_accept(
            beast::bind_front_handler(
                &WsSession::on_accept,
                shared_from_this()));
    }

    void on_accept(beast::error_code ec) {
        if (ec) {
            std::cerr << "WsSession accept error: " << ec.message() << std::endl;
            return;
        }

        // Add to server's list of sessions
        server_->join(shared_from_this());

        // Read a message
        do_read();
    }

    void do_read() {
        // Read a message into our buffer
        ws_.async_read(
            buffer_,
            beast::bind_front_handler(
                &WsSession::on_read,
                shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);

        // This indicates that the session was closed
        if (ec == websocket::error::closed) {
            server_->leave(shared_from_this());
            return;
        }

        if (ec) {
            std::cerr << "WsSession read error: " << ec.message() << std::endl;
            server_->leave(shared_from_this());
            return;
        }

        // Echo the message back (optional, or just ignore)
        // ws_.text(ws_.got_text());
        // ws_.async_write(
        //     buffer_.data(),
        //     beast::bind_front_handler(
        //         &WsSession::on_write,
        //         shared_from_this()));
        
        // Clear the buffer
        buffer_.consume(buffer_.size());
        
        // Read another message
        do_read();
    }

    void send(std::shared_ptr<std::string const> const& ss) {
        // Post the work to the strand to ensure thread safety
        net::post(
            ws_.get_executor(),
            beast::bind_front_handler(
                &WsSession::on_send,
                shared_from_this(),
                ss));
    }

    void on_send(std::shared_ptr<std::string const> const& ss) {
        // Always write the message
        ws_.async_write(
            net::buffer(*ss),
            beast::bind_front_handler(
                &WsSession::on_write,
                shared_from_this()));
    }

    void on_write(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);

        if (ec) {
            std::cerr << "WsSession write error: " << ec.message() << std::endl;
            server_->leave(shared_from_this());
            return;
        }
    }
};

//------------------------------------------------------------------------------

WsServer::WsServer() 
    : ioc_(1) // Single threaded for now
    , running_(false) {
}

WsServer::~WsServer() {
    stop();
}

void WsServer::start(uint16_t port) {
    if (running_) return;

    try {
        auto const address = net::ip::make_address("0.0.0.0");
        auto endpoint = tcp::endpoint{address, port};

        acceptor_ = std::make_unique<tcp::acceptor>(ioc_);
        
        beast::error_code ec;
        acceptor_->open(endpoint.protocol(), ec);
        if (ec) {
            std::cerr << "Open error: " << ec.message() << std::endl;
            return;
        }

        acceptor_->set_option(net::socket_base::reuse_address(true), ec);
        if (ec) {
            std::cerr << "Set option error: " << ec.message() << std::endl;
            return;
        }

        acceptor_->bind(endpoint, ec);
        if (ec) {
            std::cerr << "Bind error: " << ec.message() << std::endl;
            return;
        }

        acceptor_->listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            std::cerr << "Listen error: " << ec.message() << std::endl;
            return;
        }

        running_ = true;
        stop_signal_ = false;
        do_accept();

        std::cout << "WebSocket Server listening on port " << port << std::endl;

        // Run the I/O context in a separate thread
        thread_ = std::make_unique<std::thread>([this] {
            run();
        });

        // Start broadcast loop
        broadcast_thread_ = std::make_unique<std::thread>([this] {
            broadcastLoop();
        });

    } catch (const std::exception& e) {
        std::cerr << "WsServer start error: " << e.what() << std::endl;
    }
}

void WsServer::stop() {
    if (!running_) return;
    
    running_ = false;
    stop_signal_ = true;
    
    // Stop the I/O context
    ioc_.stop();
    
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }

    if (broadcast_thread_ && broadcast_thread_->joinable()) {
        broadcast_thread_->join();
    }
    
    sessions_.clear();
}

void WsServer::run() {
    try {
        ioc_.run();
    } catch (const std::exception& e) {
        std::cerr << "WsServer run error: " << e.what() << std::endl;
    }
}

void WsServer::do_accept() {
    acceptor_->async_accept(
        net::make_strand(ioc_),
        beast::bind_front_handler(
            &WsServer::on_accept,
            shared_from_this()));
}

void WsServer::on_accept(beast::error_code ec, tcp::socket socket) {
    if (ec) {
        std::cerr << "Accept error: " << ec.message() << std::endl;
    } else {
        // Create the session and run it
        std::make_shared<WsSession>(
            std::move(socket),
            shared_from_this())->run();
    }

    // Accept another connection
    if (running_) {
        do_accept();
    }
}

void WsServer::join(std::shared_ptr<WsSession> session) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.insert(session);
}

void WsServer::leave(std::shared_ptr<WsSession> session) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(session);
}

void WsServer::broadcast(const std::string& msg) {
    auto const ss = std::make_shared<std::string const>(msg);
    
    std::lock_guard<std::mutex> lock(mutex_);
    for(auto const& session : sessions_) {
        session->send(ss);
    }
}

void WsServer::updateTopOfBook(double bid, double bidQty, double ask, double askQty) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    latest_state_.bid = bid;
    latest_state_.bidQty = bidQty;
    latest_state_.ask = ask;
    latest_state_.askQty = askQty;
    latest_state_.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void WsServer::updateLastTrade(double price, double qty) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    latest_state_.lastPrice = price;
    latest_state_.lastQty = qty;
    latest_state_.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void WsServer::broadcastLoop() {
    while (running_ && !stop_signal_) {
        // Sleep for ~16ms (60 FPS)
        std::this_thread::sleep_for(std::chrono::milliseconds(16));

        std::string msg;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            // Simple JSON serialization manually to avoid dependency in this file if possible, 
            // but we can use nlohmann/json if included. 
            // Given main.cpp uses nlohmann/json, we should probably use it here too or just string format for speed.
            // Let's use string formatting for zero-dependency in this file if we can, or just include json.
            // Actually, let's just format a simple JSON string.
            
            msg = "{\"type\":\"SNAPSHOT\",\"timestamp\":" + std::to_string(latest_state_.timestamp) + 
                  ",\"bid\":" + std::to_string(latest_state_.bid) + 
                  ",\"bid_qty\":" + std::to_string(latest_state_.bidQty) + 
                  ",\"ask\":" + std::to_string(latest_state_.ask) + 
                  ",\"ask_qty\":" + std::to_string(latest_state_.askQty) + 
                  ",\"last_price\":" + std::to_string(latest_state_.lastPrice) + 
                  ",\"last_qty\":" + std::to_string(latest_state_.lastQty) + "}";
        }
        
        broadcast(msg);
    }
}

} // namespace orderbook
