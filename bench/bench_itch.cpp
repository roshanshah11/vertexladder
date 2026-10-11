// Replay a full day of NASDAQ TotalView-ITCH 5.0 through VertexLadder's order book.
//
//   bench_itch <file.NASDAQ_ITCH50.gz> [--validate] [--latency] [--symbol AAPL]
//
// The file is decompressed in 64 MB chunks. Only feeding a decompressed chunk to the
// replayer is timed: gzip decoding and disk I/O are excluded, ITCH parsing and every
// book update are included. One thread, no batching tricks.
//
// --validate  For every 'E' (order executed) message, check that the order NASDAQ says
//             was executed is, in our book, at the best price and first in the queue.
//             If the book were wrong (bad priority, lost orders, wrong side), this is
//             where it would show.
// --latency   Also time each book event individually (adds a clock read per message).

#include "vertexladder/clock.hpp"
#include "vertexladder/histogram.hpp"
#include "vertexladder/itch.hpp"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace vl;

namespace {

struct TimedReplayer {
    itch::Replayer r;
    Histogram hist;
    explicit TimedReplayer(bool validate) : r(validate) {}
    std::size_t feed(const std::uint8_t* data, std::size_t len) {
        std::size_t pos = 0;
        while (pos + 2 <= len) {
            const std::size_t n = itch::be16(data + pos);
            if (pos + 2 + n > len) break;
            const std::uint64_t t0 = Clock::now();
            r.on_message(data + pos + 2, n);
            const std::uint64_t t1 = Clock::now();
            const char type = static_cast<char>(data[pos + 2]);
            if (std::strchr("AFECXDU", type)) hist.record(t1 - t0);
            pos += 2 + n;
        }
        return pos;
    }
};

void print_book(const itch::Replayer::Book& b, const char* sym, double secs_since_midnight) {
    const int h = static_cast<int>(secs_since_midnight / 3600);
    const int mi = static_cast<int>(secs_since_midnight / 60) % 60;
    const int s = static_cast<int>(secs_since_midnight) % 60;
    std::printf("\n%s book at %02d:%02d:%02d ET (%zu resting orders)\n", sym, h, mi, s, b.order_count());
    std::vector<LevelView> bids, asks;
    b.depth(Side::Buy, 5, bids);
    b.depth(Side::Sell, 5, asks);
    std::printf("  %8s %8s %10s | %-10s %-8s %-8s\n", "orders", "shares", "bid", "ask", "shares", "orders");
    for (std::size_t i = 0; i < 5; ++i) {
        if (i < bids.size())
            std::printf("  %8u %8llu %10.4f | ", bids[i].orders, static_cast<unsigned long long>(bids[i].qty),
                        static_cast<double>(bids[i].price) / 1e4);
        else
            std::printf("  %8s %8s %10s | ", "", "", "");
        if (i < asks.size())
            std::printf("%-10.4f %-8llu %-8u", static_cast<double>(asks[i].price) / 1e4,
                        static_cast<unsigned long long>(asks[i].qty), asks[i].orders);
        std::printf("\n");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.NASDAQ_ITCH50.gz> [--validate] [--latency] [--symbol SYM]\n", argv[0]);
        return 2;
    }
    bool validate = false, latency = false;
    std::string want_symbol = "AAPL";
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--validate")) validate = true;
        else if (!std::strcmp(argv[i], "--latency")) latency = true;
        else if (!std::strcmp(argv[i], "--symbol") && i + 1 < argc) want_symbol = argv[++i];
    }

    gzFile gz = gzopen(argv[1], "rb");
    if (!gz) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    gzbuffer(gz, 1u << 20);

    constexpr std::size_t kChunk = 64u << 20;
    std::vector<std::uint8_t> buf(kChunk + (1u << 16));
    std::size_t carry = 0;
    TimedReplayer tr(validate);
    itch::Replayer& rep = tr.r;
    std::uint64_t ticks = 0, bytes = 0;
    std::size_t peak_resting = 0;
    bool snapshot_done = false;
    int want_locate = -1;
    constexpr double kNoon = 12.0 * 3600.0;

    for (;;) {
        const int got = gzread(gz, buf.data() + carry, static_cast<unsigned>(kChunk));
        if (got < 0) {
            std::fprintf(stderr, "gzread failed\n");
            return 1;
        }
        const std::size_t avail = carry + static_cast<std::size_t>(got);
        if (avail == 0) break;

        const std::uint64_t t0 = Clock::now();
        const std::size_t used = latency ? tr.feed(buf.data(), avail) : rep.feed(buf.data(), avail);
        ticks += Clock::now() - t0;
        bytes += used;

        carry = avail - used;
        std::memmove(buf.data(), buf.data() + used, carry);
        const std::size_t resting = rep.resting_orders();
        if (resting > peak_resting) peak_resting = resting;

        if (want_locate < 0) {
            for (std::uint16_t l = 1; l < 20000 && want_locate < 0; ++l)
                if (rep.symbol(l) == want_symbol) want_locate = l;
        }
        const double t_sec = static_cast<double>(rep.last_timestamp_ns()) * 1e-9;
        if (!snapshot_done && want_locate > 0 && t_sec >= kNoon) {
            if (const auto* b = rep.find_book(static_cast<std::uint16_t>(want_locate)))
                print_book(*b, want_symbol.c_str(), t_sec);
            snapshot_done = true;
        }
        if (got == 0) break;
    }
    gzclose(gz);

    const itch::Stats& st = rep.stats();
    const double secs = static_cast<double>(ticks) * Clock::ns_per_tick() * 1e-9;
    std::printf("\nNASDAQ ITCH 5.0 replay: %s\n", argv[1]);
    std::printf("  messages        %llu (%.2f GB decompressed)\n",
                static_cast<unsigned long long>(st.messages), static_cast<double>(bytes) / 1e9);
    std::printf("  book events     %llu  (A %llu, F %llu, E %llu, C %llu, X %llu, D %llu, U %llu)\n",
                static_cast<unsigned long long>(st.book_events),
                static_cast<unsigned long long>(st.by_type['A']), static_cast<unsigned long long>(st.by_type['F']),
                static_cast<unsigned long long>(st.by_type['E']), static_cast<unsigned long long>(st.by_type['C']),
                static_cast<unsigned long long>(st.by_type['X']), static_cast<unsigned long long>(st.by_type['D']),
                static_cast<unsigned long long>(st.by_type['U']));
    std::printf("  books           %zu symbols, peak %zu resting orders (sampled per chunk), %zu at close of file\n",
                rep.book_count(), peak_resting, rep.resting_orders());
    std::printf("  replay time     %.2f s (parse + book updates, one thread, excludes gzip)\n", secs);
    std::printf("  throughput      %.1f M messages/s, %.1f ns/message\n",
                static_cast<double>(st.messages) / secs / 1e6, secs * 1e9 / static_cast<double>(st.messages));
    std::printf("  integrity       unknown order refs %llu, overfills %llu, malformed %llu\n",
                static_cast<unsigned long long>(st.unknown_ref), static_cast<unsigned long long>(st.overfill),
                static_cast<unsigned long long>(st.malformed));
    if (validate && st.exec_checked) {
        std::printf("  validation      %llu executions: %.4f%% hit our best price, %.4f%% hit our first-in-queue order\n",
                    static_cast<unsigned long long>(st.exec_checked),
                    100.0 * static_cast<double>(st.exec_at_best) / static_cast<double>(st.exec_checked),
                    100.0 * static_cast<double>(st.exec_first_in_queue) / static_cast<double>(st.exec_checked));
    }
    if (latency) {
        const double npt = Clock::ns_per_tick();
        const Histogram& h = tr.hist;
        std::printf("  per book event  mean %.1f ns, p50 %.0f, p99 %.0f, p99.9 %.0f, max %.0f ns (clock %s, %.1f ns/tick)\n",
                    h.mean() * npt, static_cast<double>(h.quantile(0.5)) * npt,
                    static_cast<double>(h.quantile(0.99)) * npt, static_cast<double>(h.quantile(0.999)) * npt,
                    static_cast<double>(h.max()) * npt, Clock::name(), npt);
    }
    return 0;
}
