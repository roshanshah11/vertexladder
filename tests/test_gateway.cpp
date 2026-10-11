// End to end over real TCP on localhost: FIX client -> gateway thread -> SPSC ring ->
// engine thread -> SPSC ring -> gateway -> ExecutionReports back to both clients.

#include "fix_test_client.hpp"
#include "vertexladder/gateway.hpp"

#include <gtest/gtest.h>

#include <thread>

using namespace vl;
using vl::test::FixTestClient;
using vl::test::OwnedMessage;
namespace tag = vl::fix::tag;

namespace {

class GatewayTest : public ::testing::Test {
protected:
    Engine::CommandRing cmds{1u << 12};
    Engine::EventRing events{1u << 14};
    Engine engine{2, cmds, events};
    std::unique_ptr<FixGateway> gw;
    std::atomic<bool> stop{false};
    std::thread engine_thread, gw_thread;

    void SetUp() override {
        GatewayConfig cfg;
        cfg.port = 0;
        cfg.symbols = {"AAPL", "MSFT"};
        gw = std::make_unique<FixGateway>(cfg, cmds, events);
        engine_thread = std::thread([this] { engine.run(stop); });
        gw_thread = std::thread([this] { gw->run(stop); });
    }
    void TearDown() override {
        stop = true;
        gw_thread.join();
        engine_thread.join();
    }

    std::unique_ptr<FixTestClient> connect(const char* comp) {
        auto c = std::make_unique<FixTestClient>(gw->port(), comp);
        c->logon();
        auto ack = c->expect("A");
        EXPECT_TRUE(ack && ack->type() == "A");
        return c;
    }

    static void order(FixTestClient& c, const char* clordid, const char* side, std::uint64_t qty, const char* px,
                      const char* sym = "AAPL", char tif = '1') {
        c.send("D", [&](fix::Writer& w) {
            w.add(tag::ClOrdID, clordid).add(tag::Symbol, sym).add(tag::Side, side);
            w.add(tag::OrderQty, qty).add(tag::OrdType, '2').add(tag::Price, px).add(tag::TimeInForce, tif);
        });
    }

    static OwnedMessage exec(FixTestClient& c, const char* exec_type) {
        auto m = c.expect("8");
        EXPECT_TRUE(m) << "no ExecutionReport";
        if (!m) return {};
        EXPECT_EQ(m->get(tag::ExecType), exec_type) << "ClOrdID " << m->get(tag::ClOrdID) << " text " << m->get(tag::Text);
        return *m;
    }
};

}  // namespace

TEST_F(GatewayTest, LogonAndTestRequest) {
    auto a = connect("ALICE");
    a->send("1", [](fix::Writer& w) { w.add(tag::TestReqID, "ping-1"); });
    auto hb = a->next();
    ASSERT_TRUE(hb);
    EXPECT_EQ(hb->type(), "0");
    EXPECT_EQ(hb->get(tag::TestReqID), "ping-1");
}

TEST_F(GatewayTest, OrdersMatchAcrossSessions) {
    auto a = connect("ALICE");
    auto b = connect("BOB");
    order(*a, "a1", "2", 100, "10.25");
    exec(*a, "0");
    order(*b, "b1", "1", 60, "10.25");
    exec(*b, "0");
    auto bf = exec(*b, "F");
    EXPECT_EQ(bf.get(tag::LastQty), "60");
    EXPECT_EQ(bf.get(tag::LastPx), "10.25");
    EXPECT_EQ(bf.get(tag::OrdStatus), "2");
    EXPECT_EQ(bf.get(tag::LeavesQty), "0");
    auto af = exec(*a, "F");
    EXPECT_EQ(af.get(tag::ClOrdID), "a1");
    EXPECT_EQ(af.get(tag::OrdStatus), "1");
    EXPECT_EQ(af.get(tag::LeavesQty), "40");
    EXPECT_EQ(af.get(tag::CumQty), "60");
    EXPECT_EQ(af.get(tag::AvgPx), "10.25");

    // Cancel the rest: the report carries the cancel's ClOrdID and the original's.
    a->send("F", [](fix::Writer& w) {
        w.add(tag::ClOrdID, "a1-cxl").add(tag::OrigClOrdID, "a1").add(tag::Symbol, "AAPL").add(tag::Side, '2');
    });
    auto ac = exec(*a, "4");
    EXPECT_EQ(ac.get(tag::ClOrdID), "a1-cxl");
    EXPECT_EQ(ac.get(tag::OrigClOrdID), "a1");
}

TEST_F(GatewayTest, BooksAreSeparatePerSymbol) {
    auto a = connect("ALICE");
    order(*a, "a1", "2", 100, "10", "MSFT");
    exec(*a, "0");
    order(*a, "a2", "1", 100, "10", "AAPL");
    exec(*a, "0");
    // Nothing traded: different books.
    auto m = a->next(std::chrono::milliseconds(200));
    EXPECT_FALSE(m && m->type() == "8");
}

TEST_F(GatewayTest, ReplaceDownKeepsPriorityAndIocCancelsRemainder) {
    auto a = connect("ALICE");
    auto b = connect("BOB");
    order(*a, "a1", "2", 100, "11");
    exec(*a, "0");
    a->send("G", [](fix::Writer& w) {
        w.add(tag::ClOrdID, "a1-r").add(tag::OrigClOrdID, "a1").add(tag::Symbol, "AAPL").add(tag::Side, '2');
        w.add(tag::OrderQty, std::uint64_t{50}).add(tag::OrdType, '2').add(tag::Price, "11");
    });
    auto r = exec(*a, "5");
    EXPECT_EQ(r.get(tag::ClOrdID), "a1-r");
    EXPECT_EQ(r.get(tag::LeavesQty), "50");
    order(*b, "b1", "1", 100, "11", "AAPL", '3');  // IOC
    exec(*b, "0");
    EXPECT_EQ(exec(*b, "F").get(tag::LastQty), "50");
    auto rem = exec(*b, "4");
    EXPECT_EQ(rem.get(tag::LeavesQty), "0");
    EXPECT_EQ(exec(*a, "F").get(tag::ClOrdID), "a1-r");
}

TEST_F(GatewayTest, RejectsUnknownSymbolAndUnknownCancel) {
    auto a = connect("ALICE");
    order(*a, "x1", "1", 10, "5", "NOPE");
    auto rej = exec(*a, "8");
    EXPECT_EQ(rej.get(tag::OrdRejReason), "1");
    a->send("F", [](fix::Writer& w) { w.add(tag::ClOrdID, "c9").add(tag::OrigClOrdID, "never-sent"); });
    auto cr = a->expect("9");
    ASSERT_TRUE(cr);
    EXPECT_EQ(cr->get(tag::CxlRejResponseTo), "1");
}

TEST_F(GatewayTest, IgnoresGarbledMessages) {
    auto a = connect("ALICE");
    a->send_raw("8=FIX.4.4\x01" "9=5\x01" "35=D\x01" "10=000\x01");  // wrong length and checksum
    order(*a, "a1", "1", 10, "5");
    exec(*a, "0");
}

TEST_F(GatewayTest, SequenceGapLogsOut) {
    auto a = connect("ALICE");
    a->send("0", [](fix::Writer&) {}, a->next_seq() + 5);
    auto lo = a->expect("5");
    ASSERT_TRUE(lo);
    EXPECT_NE(lo->get(tag::Text).find("gap"), std::string::npos);
}

TEST_F(GatewayTest, CancelsOrdersWhenClientDisconnects) {
    {
        auto a = connect("ALICE");
        order(*a, "a1", "2", 100, "10");
        exec(*a, "0");
        a->close();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto b = connect("BOB");
    order(*b, "b1", "1", 100, "10", "AAPL", '3');
    exec(*b, "0");
    exec(*b, "4");  // nothing to trade against: ALICE's order was pulled
}

// ---- Regressions from the code review ------------------------------------------------

// Owns the threads for the standalone tests below, so a failed ASSERT (which returns
// early) still stops and joins them instead of terminating the test binary.
struct Threads {
    std::atomic<bool>& stop;
    std::vector<std::thread> ts;
    ~Threads() {
        stop = true;
        for (auto& t : ts)
            if (t.joinable()) t.join();
    }
};

TEST_F(GatewayTest, ReplaceAfterPartialFillUsesTheEngineFillCount) {
    auto a = connect("ALICE");
    auto b = connect("BOB");
    order(*a, "a1", "2", 100, "10");
    exec(*a, "0");
    order(*b, "b1", "1", 60, "10", "AAPL", '3');
    exec(*b, "0");
    exec(*b, "F");
    exec(*a, "F");  // a1: 60 filled, 40 open
    a->send("G", [](fix::Writer& w) {
        w.add(tag::ClOrdID, "a1-r").add(tag::OrigClOrdID, "a1").add(tag::Symbol, "AAPL").add(tag::Side, '2');
        w.add(tag::OrderQty, std::uint64_t{80}).add(tag::OrdType, '2').add(tag::Price, "10");
    });
    auto r = exec(*a, "5");
    EXPECT_EQ(r.get(tag::OrderQty), "80");
    EXPECT_EQ(r.get(tag::CumQty), "60");
    EXPECT_EQ(r.get(tag::LeavesQty), "20");  // OrderQty = CumQty + LeavesQty
}

TEST_F(GatewayTest, RejectsOverlongAndReusedClOrdIDs) {
    auto a = connect("ALICE");
    const std::string longid(200, 'x');
    a->send("D", [&](fix::Writer& w) {
        w.add(tag::ClOrdID, longid).add(tag::Symbol, "AAPL").add(tag::Side, '1');
        w.add(tag::OrderQty, std::uint64_t{10}).add(tag::OrdType, '2').add(tag::Price, "5");
    });
    exec(*a, "8");
    order(*a, "dup", "1", 10, "5");
    exec(*a, "0");
    a->send("F", [](fix::Writer& w) { w.add(tag::ClOrdID, "dup-c").add(tag::OrigClOrdID, "dup"); });
    exec(*a, "4");
    order(*a, "dup", "1", 10, "5");  // reused after the first one is gone: still a duplicate
    EXPECT_EQ(exec(*a, "8").get(tag::OrdRejReason), "6");
}

TEST_F(GatewayTest, MarketFokIsAllOrNothing) {
    auto a = connect("ALICE");
    auto b = connect("BOB");
    order(*a, "a1", "2", 50, "10");
    exec(*a, "0");
    b->send("D", [](fix::Writer& w) {
        w.add(tag::ClOrdID, "b1").add(tag::Symbol, "AAPL").add(tag::Side, '1');
        w.add(tag::OrderQty, std::uint64_t{100}).add(tag::OrdType, '1').add(tag::TimeInForce, '4');
    });
    exec(*b, "0");
    auto c = exec(*b, "4");  // cancelled whole, no fill
    EXPECT_EQ(c.get(tag::CumQty), "0");
}

TEST_F(GatewayTest, FatFingerQuantityRejected) {
    auto a = connect("ALICE");
    order(*a, "big", "1", 5'000'000, "10");
    EXPECT_EQ(exec(*a, "8").get(tag::OrdRejReason), "13");
}

// Tiny rings: a disconnect that cancels hundreds of orders must not deadlock the gateway
// (it waits on a full command ring while the engine waits on a full event ring).
TEST(GatewayBackpressure, MassCancelOnDisconnectDoesNotDeadlock) {
    Engine::CommandRing cmds{64};
    Engine::EventRing events{64};
    Engine engine{1, cmds, events};
    GatewayConfig cfg;
    cfg.port = 0;
    cfg.symbols = {"AAPL"};
    FixGateway gw(cfg, cmds, events);
    std::atomic<bool> stop{false};
    Threads threads{stop, {}};
    threads.ts.emplace_back([&] { engine.run(stop); });
    threads.ts.emplace_back([&] { gw.run(stop); });
    {
        FixTestClient a(gw.port(), "ALICE");
        a.logon();
        ASSERT_TRUE(a.expect("A"));
        for (int i = 0; i < 600; ++i) {
            const std::string id = "o" + std::to_string(i);
            a.send("D", [&](fix::Writer& w) {
                w.add(tag::ClOrdID, id).add(tag::Symbol, "AAPL").add(tag::Side, '2');
                w.add(tag::OrderQty, std::uint64_t{1}).add(tag::OrdType, '2').add(tag::Price, "10");
            });
        }
        for (int i = 0; i < 600; ++i) ASSERT_TRUE(a.expect("8")) << i;
        a.close();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    FixTestClient b(gw.port(), "BOB");
    b.logon();
    ASSERT_TRUE(b.expect("A", std::chrono::milliseconds(3000)));  // still alive
    b.send("D", [](fix::Writer& w) {
        w.add(tag::ClOrdID, "b1").add(tag::Symbol, "AAPL").add(tag::Side, '1');
        w.add(tag::OrderQty, std::uint64_t{600}).add(tag::OrdType, '2').add(tag::Price, "10").add(tag::TimeInForce, '3');
    });
    ASSERT_TRUE(b.expect("8"));
    auto rest = b.expect("8");
    ASSERT_TRUE(rest);
    EXPECT_EQ(rest->get(tag::ExecType), "4");  // all 600 were cancelled: nothing to hit
}

// The replace/fill race, made deterministic: the engine thread starts only after the
// gateway has queued all three commands, so when the gateway handles the replace it has
// not yet heard about the fill (its CumQty is still 0). The engine must reject a new total
// of 50 because 60 already filled. Sending "total - gateway CumQty" here would rest 50
// more shares and overfill the order to 110.
TEST(GatewayRace, ReplaceCrossingAnInFlightFillCannotOverfill) {
    Engine::CommandRing cmds{1024};
    Engine::EventRing events{1024};
    Engine engine{1, cmds, events};
    GatewayConfig cfg;
    cfg.port = 0;
    cfg.symbols = {"AAPL"};
    FixGateway gw(cfg, cmds, events);
    std::atomic<bool> stop{false};
    Threads threads{stop, {}};
    threads.ts.emplace_back([&] { gw.run(stop); });

    FixTestClient a(gw.port(), "ALICE"), b(gw.port(), "BOB");
    a.logon();
    b.logon();
    ASSERT_TRUE(a.expect("A"));
    ASSERT_TRUE(b.expect("A"));
    auto lim = [](FixTestClient& c, const char* id, char side, std::uint64_t q) {
        c.send("D", [&](fix::Writer& w) {
            w.add(tag::ClOrdID, id).add(tag::Symbol, "AAPL").add(tag::Side, side);
            w.add(tag::OrderQty, q).add(tag::OrdType, '2').add(tag::Price, "10");
        });
    };
    lim(a, "a1", '2', 100);
    while (cmds.size_approx() < 1) std::this_thread::yield();
    lim(b, "b1", '1', 60);
    while (cmds.size_approx() < 2) std::this_thread::yield();
    a.send("G", [](fix::Writer& w) {
        w.add(tag::ClOrdID, "a1-r").add(tag::OrigClOrdID, "a1").add(tag::Symbol, "AAPL").add(tag::Side, '2');
        w.add(tag::OrderQty, std::uint64_t{50}).add(tag::OrdType, '2').add(tag::Price, "10");
    });
    while (cmds.size_approx() < 3) std::this_thread::yield();

    threads.ts.emplace_back([&] { engine.run(stop); });  // now let the engine see all three
    auto ack = a.expect("8");
    ASSERT_TRUE(ack);
    EXPECT_EQ(ack->get(tag::ExecType), "0");
    auto fill = a.expect("8");
    ASSERT_TRUE(fill);
    EXPECT_EQ(fill->get(tag::ExecType), "F");
    EXPECT_EQ(fill->get(tag::LeavesQty), "40");
    auto rej = a.expect("9");
    ASSERT_TRUE(rej);
    EXPECT_EQ(rej->type(), "9");  // OrderCancelReject: 50 is below the 60 already filled
}
