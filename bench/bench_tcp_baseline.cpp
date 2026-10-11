// Baseline for the FIX round trip: bare loopback TCP ping-pong of 200-byte messages, both ends
// busy-polling non-blocking sockets with TCP_NODELAY. No FIX, no engine. Whatever the FIX
// round trip costs above this is VertexLadder's own overhead.

#include "vertexladder/histogram.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace {
void make_fast(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}
void read_all(int fd, char* b, int n) {
    for (int got = 0; got < n;) {
        const auto r = read(fd, b + got, static_cast<std::size_t>(n - got));
        if (r > 0) got += static_cast<int>(r);
    }
}
void write_all(int fd, const char* b, int n) {
    for (int sent = 0; sent < n;) {
        const auto r = write(fd, b + sent, static_cast<std::size_t>(n - sent));
        if (r > 0) sent += static_cast<int>(r);
    }
}
}  // namespace

int main() {
    constexpr int kWarm = 20'000, kN = 200'000, kSize = 200;
    const int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    socklen_t len = sizeof addr;
    getsockname(ls, reinterpret_cast<sockaddr*>(&addr), &len);
    listen(ls, 1);

    std::thread echo([&] {
        const int c = accept(ls, nullptr, nullptr);
        make_fast(c);
        char b[kSize];
        for (int i = 0; i < kWarm + kN; ++i) {
            read_all(c, b, kSize);
            write_all(c, b, kSize);
        }
        close(c);
    });
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    make_fast(s);
    char b[kSize] = {};
    vl::Histogram h;
    for (int i = 0; i < kWarm + kN; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        write_all(s, b, kSize);
        read_all(s, b, kSize);
        const auto t1 = std::chrono::steady_clock::now();
        // 10 ns units: exact up to 655 us.
        if (i >= kWarm) h.record(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / 10);
    }
    echo.join();
    close(s);
    close(ls);
    auto us = [](std::uint64_t tens_of_ns) { return static_cast<double>(tens_of_ns) / 100.0; };
    std::printf("bare loopback TCP round trip (us): p50 %.1f  p90 %.1f  p99 %.1f  p99.9 %.1f\n", us(h.quantile(0.5)),
                us(h.quantile(0.9)), us(h.quantile(0.99)), us(h.quantile(0.999)));
}
