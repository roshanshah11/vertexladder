// FIX client that measures order round trip through the whole system:
// client socket -> gateway -> engine -> gateway -> ExecutionReport back.
//
//   vl_fix_client [--port 9878] [--orders 100000] [--symbol AAPL]
//
// Sends one NewOrderSingle at a time and waits for its first ExecutionReport (the
// ack), so each sample is one full round trip over loopback TCP. Orders alternate
// buy and sell at the same price, so half of them trade.

#include "fix_test_client.hpp"
#include "vertexladder/histogram.hpp"

#include <chrono>
#include <cstdio>
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
    vl::Histogram rtt;  // nanoseconds
    std::size_t warm = n / 10;
    for (std::size_t i = 0; i < n + warm; ++i) {
        const std::string id = "o" + std::to_string(i);
        const auto t0 = sc::now();
        c.send("D", [&](vl::fix::Writer& w) {
            w.add(vl::fix::tag::ClOrdID, id).add(vl::fix::tag::Symbol, symbol);
            w.add(vl::fix::tag::Side, (i & 1) ? '2' : '1').add(vl::fix::tag::OrderQty, std::uint64_t{100});
            w.add(vl::fix::tag::OrdType, '2').add(vl::fix::tag::Price, "100");
        });
        for (;;) {
            auto m = c.next();
            if (!m) {
                std::fprintf(stderr, "timed out waiting for %s\n", id.c_str());
                return 1;
            }
            if (m->type() == "8" && m->get(vl::fix::tag::ClOrdID) == id) break;
        }
        const auto t1 = sc::now();
        if (i >= warm) rtt.record(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    auto us = [](std::uint64_t ns) { return static_cast<double>(ns) / 1000.0; };
    std::printf("%zu orders, round trip over loopback TCP (us): mean %.1f  p50 %.1f  p90 %.1f  p99 %.1f  p99.9 %.1f  max %.1f\n",
                n, rtt.mean() / 1000.0, us(rtt.quantile(0.5)), us(rtt.quantile(0.9)), us(rtt.quantile(0.99)),
                us(rtt.quantile(0.999)), us(rtt.max()));
    c.send("5");
    return 0;
}
