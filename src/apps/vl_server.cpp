// VertexLadder exchange: FIX 4.4 gateway thread + matching engine thread.
//
//   vl_server [--port 9878] [--symbols AAPL,MSFT,NVDA]

#include "vertexladder/gateway.hpp"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }
}  // namespace

int main(int argc, char** argv) {
    vl::GatewayConfig cfg;
    std::string symbols = "AAPL,MSFT,NVDA,AMZN,GOOGL";
    for (int i = 1; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--port")) cfg.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--symbols")) symbols = argv[++i];
    }
    std::stringstream ss(symbols);
    for (std::string s; std::getline(ss, s, ',');)
        if (!s.empty()) cfg.symbols.push_back(s);

    vl::Engine::CommandRing commands(1u << 16);
    vl::Engine::EventRing events(1u << 18);
    vl::Engine engine(cfg.symbols.size(), commands, events);
    vl::FixGateway gateway(cfg, commands, events);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::printf("VertexLadder listening on port %u (FIX 4.4, SenderCompID %s), %zu symbols: %s\n", gateway.port(),
                cfg.comp_id.c_str(), cfg.symbols.size(), symbols.c_str());

    std::thread matcher([&] { engine.run(g_stop); });
    gateway.run(g_stop);
    matcher.join();
    const auto st = gateway.stats();
    std::printf("\nshutdown: %llu sessions, %llu messages in, %llu out, %llu garbled, %llu engine events\n",
                static_cast<unsigned long long>(st.sessions), static_cast<unsigned long long>(st.messages_in),
                static_cast<unsigned long long>(st.messages_out), static_cast<unsigned long long>(st.garbled),
                static_cast<unsigned long long>(engine.events_published()));
    return 0;
}
