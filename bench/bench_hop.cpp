// Engine hop latency: one thread pushes a Command, the engine thread (busy-polling) matches it
// and publishes its first EngineEvent, the first thread pops it. One command in flight.

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

    Histogram h;  // ns
    EngineEvent ev;
    constexpr int kWarm = 20'000, kN = 200'000;
    for (int i = 1; i <= kWarm + kN; ++i) {
        Command c;
        c.id = static_cast<OrderId>(i);
        c.side = (i & 1) ? Side::Buy : Side::Sell;  // alternate, so every other order trades
        c.price = 100;
        c.qty = 100;
        const auto t0 = std::chrono::steady_clock::now();
        while (!in.try_push(c)) {
        }
        while (!out.try_pop(ev)) {
        }
        const auto t1 = std::chrono::steady_clock::now();
        while (out.try_pop(ev)) {
        }
        if (i > kWarm) h.record(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    stop = true;
    t.join();
    std::printf("command -> first event across threads (ns, steady_clock): p50 %llu  p90 %llu  p99 %llu  p99.9 %llu\n",
                static_cast<unsigned long long>(h.quantile(0.5)), static_cast<unsigned long long>(h.quantile(0.9)),
                static_cast<unsigned long long>(h.quantile(0.99)), static_cast<unsigned long long>(h.quantile(0.999)));
}
