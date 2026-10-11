// Engine hop latency, one command in flight. Orders alternate buy and sell at one price, so
// every sell trades against the buy before it. Two numbers:
//   ack:  push a command -> its Accepted event comes back (emitted before matching)
//   fill: push a crossing sell -> its own Fill event comes back (includes the match)

#include "vertexladder/engine.hpp"
#include "vertexladder/histogram.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace vl;

int main() {
    Engine::CommandRing in(1u << 12);
    Engine::EventRing out(1u << 14);
    Engine engine(1, in, out);
    std::atomic<bool> stop{false};
    std::thread t([&] { engine.run(stop); });

    Histogram ack, fill;  // ns
    EngineEvent ev;
    constexpr int kWarm = 20'000, kN = 200'000;
    for (int i = 1; i <= kWarm + kN; ++i) {
        Command c;
        c.id = static_cast<OrderId>(i);
        c.side = (i & 1) ? Side::Buy : Side::Sell;
        c.price = 100;
        c.qty = 100;
        const auto t0 = std::chrono::steady_clock::now();
        while (!in.try_push(c)) {
        }
        bool acked = false, filled = c.side == Side::Buy;  // buys only rest
        while (!acked || !filled) {
            if (!out.try_pop(ev)) continue;
            const auto t1 = std::chrono::steady_clock::now();
            const auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            if (ev.id != c.id) continue;
            if (ev.type == EngineEvent::Type::Accepted && !acked) {
                acked = true;
                if (i > kWarm) ack.record(ns);
            } else if (ev.type == EngineEvent::Type::Fill && !filled) {
                filled = true;
                if (i > kWarm) fill.record(ns);
            }
        }
        while (out.try_pop(ev)) {
        }
    }
    stop = true;
    t.join();
    auto row = [](const char* name, const Histogram& h) {
        std::printf("%-34s p50 %llu  p90 %llu  p99 %llu  p99.9 %llu\n", name,
                    static_cast<unsigned long long>(h.quantile(0.5)), static_cast<unsigned long long>(h.quantile(0.9)),
                    static_cast<unsigned long long>(h.quantile(0.99)), static_cast<unsigned long long>(h.quantile(0.999)));
    };
    std::printf("engine thread hop (ns, steady_clock, 41.7 ns resolution on Apple Silicon)\n");
    row("  command -> Accepted", ack);
    row("  crossing order -> its Fill", fill);
}
