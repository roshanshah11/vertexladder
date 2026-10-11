// FIX client that measures order round trip through the whole system:
// client socket -> gateway -> engine -> gateway -> ExecutionReport back.
//
//   vl_fix_client [--port 9878] [--orders 100000] [--symbol AAPL]
//
// Sends one NewOrderSingle at a time and waits for its reports, so each sample is one full
// round trip over loopback TCP. Orders alternate buy and sell at the same price, so every
// sell trades against the buy before it. Closed loop: with one order in flight there is no
// queueing, so this is service time on an idle system, not latency under load.

#include "fix_test_client.hpp"
#include "vertexladder/histogram.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    std::uint16_t port = 9878;
    std::size_t n = 100'000;
    std::string symbol = "AAPL";
    for (int i = 1; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--port")) port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--orders")) n = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--symbol")) symbol = argv[++i];
    }

    vl::test::FixTestClient c(port, "BENCH");
    c.logon();
    if (!c.expect("A")) {
        std::fprintf(stderr, "logon failed\n");
        return 1;
    }

    using sc = std::chrono::steady_clock;
    // ack: send -> the order's first ExecutionReport (150=0), every order.
    // fill: send -> the order's own fill report (150=F), for the sells, which cross the
    // resting buy before them; this one includes the match.
    // Recorded in 10 ns units: the histogram is exact below 65,536 units, so up to 655 us.
    vl::Histogram ack, fill;
    auto ns_since = [](sc::time_point t0) {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(sc::now() - t0).count()) / 10;
    };
    const std::size_t warm = n / 10;
    for (std::size_t i = 0; i < n + warm; ++i) {
        const std::string id = "o" + std::to_string(i);
        const bool sell = i & 1;
        const auto t0 = sc::now();
        c.send("D", [&](vl::fix::Writer& w) {
            w.add(vl::fix::tag::ClOrdID, id).add(vl::fix::tag::Symbol, symbol);
            w.add(vl::fix::tag::Side, sell ? '2' : '1').add(vl::fix::tag::OrderQty, std::uint64_t{100});
            w.add(vl::fix::tag::OrdType, '2').add(vl::fix::tag::Price, "100");
        });
        bool acked = false, filled = !sell;
        while (!acked || !filled) {
            auto m = c.next();
            if (!m) {
                std::fprintf(stderr, "timed out waiting for %s\n", id.c_str());
                return 1;
            }
            if (m->type() != "8" || m->get(vl::fix::tag::ClOrdID) != id) continue;
            const std::string et = m->get(vl::fix::tag::ExecType);
            if (et == "0" && !acked) {
                acked = true;
                if (i >= warm) ack.record(ns_since(t0));
            } else if (et == "F" && !filled) {
                filled = true;
                if (i >= warm) fill.record(ns_since(t0));
            }
        }
    }
    auto row = [](const char* name, const vl::Histogram& h) {
        auto us = [](std::uint64_t tens_of_ns) { return static_cast<double>(tens_of_ns) / 100.0; };
        std::printf("%-30s mean %.1f  p50 %.1f  p90 %.1f  p99 %.1f  p99.9 %.1f  max %.1f\n", name, h.mean() / 100.0,
                    us(h.quantile(0.5)), us(h.quantile(0.9)), us(h.quantile(0.99)), us(h.quantile(0.999)), us(h.max()));
    };
    std::printf("%zu orders, one in flight (closed loop), loopback TCP, microseconds:\n", n);
    row("  order -> ack (150=0)", ack);
    row("  crossing order -> fill (150=F)", fill);
    c.send("5");
    return 0;
}
