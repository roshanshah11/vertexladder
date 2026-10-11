#pragma once

// FIX 4.4 order-entry gateway over TCP (Boost.Asio).
//
// Runs on its own thread. It owns the network and everything that is per-client
// (sessions, sequence numbers, ClOrdID strings, cumulative fill quantities), and turns
// FIX messages into fixed-size Commands for the engine thread. Engine results come back
// as EngineEvents and go out as ExecutionReports. The two threads share nothing but the
// two SPSC rings, so the matching thread never touches a socket, a string or a lock.
//
// Supported: Logon, Heartbeat, TestRequest, Logout, NewOrderSingle (limit/market,
// GTC/Day/IOC/FOK), OrderCancelRequest, OrderCancelReplaceRequest; ExecutionReport and
// OrderCancelReject out. Inbound sequence numbers are enforced. Not supported: resend
// requests / message replay (a gap ends the session), session persistence across
// restarts, and drop copy.

#include "vertexladder/engine.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vl {

struct GatewayConfig {
    std::uint16_t port = 9878;  // 0 = pick a free port (tests)
    std::string comp_id = "VERTEXLADDER";
    std::vector<std::string> symbols;  // index in this list = engine book id
    int price_decimals = 4;            // FIX "101.25" -> 1012500 ticks
    bool cancel_on_disconnect = true;
    Qty max_order_qty = 1'000'000;     // pre-trade fat-finger check
};

class FixGateway {
public:
    FixGateway(GatewayConfig cfg, Engine::CommandRing& to_engine, Engine::EventRing& from_engine);
    ~FixGateway();
    FixGateway(const FixGateway&) = delete;
    FixGateway& operator=(const FixGateway&) = delete;

    std::uint16_t port() const;  // the bound port

    // Event loop: polls the sockets and drains engine events until stop is set.
    void run(const std::atomic<bool>& stop);

    struct Stats {
        std::uint64_t sessions = 0;
        std::uint64_t messages_in = 0;
        std::uint64_t messages_out = 0;
        std::uint64_t garbled = 0;
    };
    Stats stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vl
