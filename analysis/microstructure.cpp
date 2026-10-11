// Two market-microstructure questions answered on a full NASDAQ ITCH 5.0 day, using the
// same OrderBook that the matching engine uses, rebuilt order by order.
//
//   microstructure <file.NASDAQ_ITCH50.gz> [--csv out_prefix]
//
// 1. Queue imbalance. At every book update during regular hours, take the best bid and
//    best ask sizes and compute I = (bid - ask) / (bid + ask). Then wait for the mid price
//    to change and record whether it went up or down. P(up | I) shows how much the
//    top of the book says about the next price move. Split by whether the spread is one
//    tick ($0.01), because the effect is known to be strongest for large-tick stocks.
//
// 2. Queue position. For every order that joins the best price on its side, record how
//    many shares were already ahead of it. Then record whether it ever executes, and for
//    executions, the markout: the mid 1 s and 10 s after the fill minus the fill price,
//    from the resting order's point of view, in cents per share. A passive fill starts
//    half a spread in the money; a negative markout means the price moved against it by
//    more than that (adverse selection). Markouts use only fills made while the spread was
//    one tick, where the mid is a fair price; in a $1.00 / $5.00 market it isn't.
//
// Only regular trading hours (9:30 to 16:00 ET) are counted. Prices are ITCH's 4 implied
// decimals, so one cent is 100.

#include "vertexladder/itch.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

using namespace vl;

namespace {

constexpr std::uint64_t kOpen = 34'200'000'000'000ull;   // 9:30:00 ET, ns since midnight
constexpr std::uint64_t kClose = 57'600'000'000'000ull;  // 16:00:00 ET
constexpr Price kCent = 100;
constexpr int kImbBuckets = 10;

// Shares already resting at the price when an order joins it.
constexpr std::array<std::uint64_t, 5> kAheadEdges = {0, 1, 500, 2'000, 10'000};  // bucket lower bounds
constexpr int kAheadBuckets = static_cast<int>(kAheadEdges.size());
const char* kAheadNames[kAheadBuckets] = {"0 (new best level)", "1-499", "500-1,999", "2,000-9,999", "10,000+"};

int ahead_bucket(std::uint64_t shares) {
    int b = 0;
    for (int i = 0; i < kAheadBuckets; ++i)
        if (shares >= kAheadEdges[i]) b = i;
    return b;
}

constexpr std::array<std::uint64_t, 2> kHorizonsNs = {1'000'000'000ull, 10'000'000'000ull};

struct PendingMarkout {
    std::uint64_t due;  // timestamp at which to read the mid
    Price price;
    Side side;  // side of the resting (passive) order
    int bucket;
};

struct SymbolState {
    Price mid2 = 0;  // bid + ask (twice the mid, so it stays an integer)
    // Imbalance samples taken since the last mid change, per spread class and bucket.
    std::array<std::array<std::uint32_t, kImbBuckets>, 2> pending{};
    bool has_pending = false;
    std::array<std::deque<PendingMarkout>, kHorizonsNs.size()> markouts;
    std::uint64_t hits = 0, calls = 0;  // sign(I) called the next move, |I| > 0.6
    // Queue study, per symbol, so positions can be compared within the same stock.
    std::array<std::uint32_t, kAheadBuckets> joined{}, filled{};
    std::array<std::array<std::vector<float>, kAheadBuckets>, kHorizonsNs.size()> markouts_c;  // cents
};

struct OrderTrack {
    std::uint8_t bucket;
    bool filled;
};

class Study {
public:
    void on_message(const std::uint8_t* m, std::size_t n) {
        const char t = static_cast<char>(m[0]);
        const bool book_event = std::strchr("AFECXDU", t) != nullptr && t != '\0';
        if (!book_event) {
            rep_.on_message(m, n);
            return;
        }
        const std::uint16_t loc = itch::be16(m + 1);
        const std::uint64_t ts = itch::be48(m + 5);
        const bool rth = ts >= kOpen && ts < kClose;
        const auto* before = rep_.find_book(loc);

        // --- before the update -----------------------------------------------------
        OrderId tracked_ref = 0;  // the order this message touches, if we track it
        if ((t == 'A' || t == 'F') && rth) {
            const Side side = m[19] == 'B' ? Side::Buy : Side::Sell;
            const Price px = static_cast<Price>(itch::be32(m + 32));
            const auto top = before ? before->top(side) : std::nullopt;
            const bool better = !top || (side == Side::Buy ? px > top->price : px < top->price);
            const bool joins = top && px == top->price;
            if (better || joins) {
                const int b = better ? 0 : ahead_bucket(top->qty);
                tracks_.emplace(itch::be64(m + 11), OrderTrack{static_cast<std::uint8_t>(b), false});
                ++joined_[b];
                ++state(loc).joined[b];
            }
        } else if (t == 'E' || t == 'C' || t == 'X' || t == 'D' || t == 'U') {
            tracked_ref = itch::be64(m + 11);
            auto it = tracks_.find(tracked_ref);
            if (it == tracks_.end()) {
                tracked_ref = 0;
            } else if ((t == 'E' || t == 'C') && before) {
                if (const auto inf = before->info(tracked_ref)) {
                    if (!it->second.filled) {
                        it->second.filled = true;
                        ++filled_[it->second.bucket];
                        ++state(loc).filled[it->second.bucket];
                    }
                    // Markout from the resting order's side, at its price ('C' prints at its own price).
                    const Price px = t == 'C' ? static_cast<Price>(itch::be32(m + 32)) : inf->price;
                    const auto b0 = before->top(Side::Buy), a0 = before->top(Side::Sell);
                    const bool one_tick = b0 && a0 && a0->price - b0->price == kCent;
                    if (rth && one_tick)
                        for (std::size_t h = 0; h < kHorizonsNs.size(); ++h)
                            state(loc).markouts[h].push_back({ts + kHorizonsNs[h], px, inf->side, it->second.bucket});
                }
            }
        }

        rep_.on_message(m, n);

        // --- after the update ------------------------------------------------------
        const auto* book = rep_.find_book(loc);
        if (tracked_ref && book && !book->open_qty(tracked_ref)) tracks_.erase(tracked_ref);  // order is gone
        if (!book) return;
        const auto bid = book->top(Side::Buy);
        const auto ask = book->top(Side::Sell);
        if (!bid || !ask || ask->price <= bid->price) return;  // one-sided or locked/crossed
        SymbolState& st = state(loc);
        const Price mid2 = bid->price + ask->price;

        for (std::size_t h = 0; h < kHorizonsNs.size(); ++h) {
            auto& q = st.markouts[h];
            while (!q.empty() && q.front().due <= ts) {
                const PendingMarkout& p = q.front();
                const double mid = static_cast<double>(mid2) / 2.0;
                const double px = static_cast<double>(p.price);
                const double ticks = (p.side == Side::Buy ? mid - px : px - mid);  // 1/100 cent units
                st.markouts_c[h][p.bucket].push_back(static_cast<float>(ticks / 100.0));  // cents per share
                q.pop_front();
            }
        }

        if (!rth) {
            st.mid2 = mid2;
            st.pending = {};
            st.has_pending = false;
            return;
        }
        if (st.mid2 != 0 && mid2 != st.mid2 && st.has_pending) {
            const bool up = mid2 > st.mid2;
            for (int c = 0; c < 2; ++c)
                for (int b = 0; b < kImbBuckets; ++b) {
                    const std::uint32_t k = st.pending[c][b];
                    (up ? up_ : down_)[c][b] += k;
                    if (b < 2 || b >= kImbBuckets - 2) {  // |I| > 0.6 roughly: the two end buckets each side
                        st.calls += k;
                        if ((b >= kImbBuckets / 2) == up) st.hits += k;
                    }
                }
            st.pending = {};
            st.has_pending = false;
        }
        st.mid2 = mid2;

        const double qb = static_cast<double>(bid->qty), qa = static_cast<double>(ask->qty);
        const double imb = (qb - qa) / (qb + qa);
        int b = static_cast<int>((imb + 1.0) / 2.0 * kImbBuckets);
        b = std::clamp(b, 0, kImbBuckets - 1);
        const int cls = (ask->price - bid->price) == kCent ? 0 : 1;
        ++st.pending[cls][b];
        st.has_pending = true;
        ++samples_[cls];
    }

    void report(FILE* out, const char* csv_prefix) {
        std::fprintf(out, "\n1) QUEUE IMBALANCE -> NEXT MID MOVE  (regular hours, every book update is a sample)\n");
        const char* cls_name[2] = {"spread = 1 tick", "spread > 1 tick"};
        for (int c = 0; c < 2; ++c) {
            std::uint64_t n = 0;
            for (int b = 0; b < kImbBuckets; ++b) n += up_[c][b] + down_[c][b];
            std::fprintf(out, "\n  %s: %llu samples resolved by a later mid move (%llu taken)\n", cls_name[c],
                         static_cast<unsigned long long>(n), static_cast<unsigned long long>(samples_[c]));
            std::fprintf(out, "  %-16s %12s %10s\n", "imbalance", "samples", "P(up)");
            for (int b = 0; b < kImbBuckets; ++b) {
                const double lo = -1.0 + 2.0 * b / kImbBuckets, hi = lo + 2.0 / kImbBuckets;
                const std::uint64_t k = up_[c][b] + down_[c][b];
                std::fprintf(out, "  [%+.1f, %+.1f%s %12llu %9.1f%%\n", lo, hi, b == kImbBuckets - 1 ? "]" : ")",
                             static_cast<unsigned long long>(k),
                             k ? 100.0 * static_cast<double>(up_[c][b]) / static_cast<double>(k) : 0.0);
            }
            // Sign rule: predict up when I > 0, down when I < 0 (the middle buckets straddle 0
            // symmetrically, so bucket >= 5 means I >= 0).
            std::uint64_t right = 0, total = 0;
            for (int b = 0; b < kImbBuckets; ++b) {
                right += b >= kImbBuckets / 2 ? up_[c][b] : down_[c][b];
                total += up_[c][b] + down_[c][b];
            }
            std::fprintf(out, "  sign(I) calls the next move: %.1f%% of samples\n",
                         total ? 100.0 * static_cast<double>(right) / static_cast<double>(total) : 0.0);
        }

        std::fprintf(out, "\n  Per symbol, |I| > 0.6, spread any (sign(I) vs next move), most active and named:\n");
        std::vector<std::pair<std::uint64_t, std::uint16_t>> by_calls;
        for (std::size_t l = 0; l < states_.size(); ++l)
            if (states_[l].calls) by_calls.emplace_back(states_[l].calls, static_cast<std::uint16_t>(l));
        std::sort(by_calls.rbegin(), by_calls.rend());
        std::vector<std::uint16_t> show;
        for (std::size_t i = 0; i < by_calls.size() && i < 10; ++i) show.push_back(by_calls[i].second);
        for (const char* want : {"AAPL", "MSFT", "AMZN", "NVDA", "TSLA", "AMD", "INTC", "QQQ"})
            for (std::size_t l = 0; l < states_.size(); ++l)
                if (rep_.symbol(static_cast<std::uint16_t>(l)) == want &&
                    std::find(show.begin(), show.end(), l) == show.end())
                    show.push_back(static_cast<std::uint16_t>(l));
        for (std::uint16_t l : show) {
            const SymbolState& s = states_[l];
            std::fprintf(out, "  %-6s %12llu calls  %5.1f%% right\n", rep_.symbol(l).c_str(),
                         static_cast<unsigned long long>(s.calls),
                         s.calls ? 100.0 * static_cast<double>(s.hits) / static_cast<double>(s.calls) : 0.0);
        }

        std::fprintf(out, "\n2) QUEUE POSITION AT ENTRY -> FILL RATE AND MARKOUT  (orders joining the best price)\n");
        std::fprintf(out, "  Markout: mid N seconds after the fill minus the fill price, from the resting order's side,\n"
                          "  cents per share, fills made while the spread was 1 tick. A passive fill starts at +0.5c.\n");
        std::fprintf(out, "  %-20s %11s %9s %11s %11s %11s %11s\n", "shares ahead", "orders", "ever fill", "median 1s",
                     "median 10s", "under 10s", "fills");
        for (const SymbolState& st : states_)
            for (int b = 0; b < kAheadBuckets; ++b) {
                all1_[b].insert(all1_[b].end(), st.markouts_c[0][b].begin(), st.markouts_c[0][b].end());
                all10_[b].insert(all10_[b].end(), st.markouts_c[1][b].begin(), st.markouts_c[1][b].end());
            }
        for (int b = 0; b < kAheadBuckets; ++b) {
            std::fprintf(out, "  %-20s %11llu %8.2f%% %9.3f c %9.3f c %10.1f%% %11zu\n", kAheadNames[b],
                         static_cast<unsigned long long>(joined_[b]),
                         joined_[b] ? 100.0 * static_cast<double>(filled_[b]) / static_cast<double>(joined_[b]) : 0.0,
                         median(all1_[b]), median(all10_[b]), 100.0 * frac_below_zero(all10_[b]), all10_[b].size());
        }

        // Within-stock: the cross-section mixes $5 and $1,800 stocks, and huge queues only form
        // in some of them. Compare a short queue (1-499 shares ahead) with a long one (2,000+)
        // inside each stock that has enough of both.
        constexpr std::size_t kMinFills = 50;
        constexpr std::uint32_t kMinJoins = 200;
        std::vector<double> d_markout, d_fill;
        std::size_t worse_markout = 0, lower_fill = 0, n_mk = 0, n_fill = 0;
        for (const SymbolState& st : states_) {
            const std::uint32_t j_short = st.joined[1], j_long = st.joined[3] + st.joined[4];
            if (j_short >= kMinJoins && j_long >= kMinJoins) {
                const double f_short = static_cast<double>(st.filled[1]) / j_short;
                const double f_long = static_cast<double>(st.filled[3] + st.filled[4]) / j_long;
                d_fill.push_back(f_long - f_short);
                lower_fill += f_long < f_short;
                ++n_fill;
            }
            std::vector<float> lng = st.markouts_c[1][3];
            lng.insert(lng.end(), st.markouts_c[1][4].begin(), st.markouts_c[1][4].end());
            const std::vector<float>& shrt = st.markouts_c[1][1];
            if (shrt.size() >= kMinFills && lng.size() >= kMinFills) {
                const double d = mean_clipped(lng) - mean_clipped(shrt);
                d_markout.push_back(d);
                worse_markout += d < 0;
                ++n_mk;
            }
        }
        std::fprintf(out, "\n  Within the same stock, long queue (2,000+ ahead) vs short queue (1-499 ahead):\n");
        std::fprintf(out, "  fill rate:   %zu stocks with %u+ joins of each; long queue fills less often in %.1f%%,\n"
                          "               median difference %+.2f percentage points\n",
                     n_fill, kMinJoins, n_fill ? 100.0 * static_cast<double>(lower_fill) / static_cast<double>(n_fill) : 0.0,
                     100.0 * median_d(d_fill));
        std::fprintf(out, "  10s markout: %zu stocks with %zu+ one-tick fills of each; long queue's fills are worse in %.1f%%,\n"
                          "               median difference %+.3f c/share (means clipped to +/-5c)\n",
                     n_mk, kMinFills, n_mk ? 100.0 * static_cast<double>(worse_markout) / static_cast<double>(n_mk) : 0.0,
                     median_d(d_markout));

        if (csv_prefix) {
            std::string p = std::string(csv_prefix) + "_imbalance.csv";
            if (FILE* f = std::fopen(p.c_str(), "w")) {
                std::fprintf(f, "spread_class,bucket_lo,bucket_hi,samples,p_up\n");
                for (int c = 0; c < 2; ++c)
                    for (int b = 0; b < kImbBuckets; ++b) {
                        const double lo = -1.0 + 2.0 * b / kImbBuckets;
                        const std::uint64_t k = up_[c][b] + down_[c][b];
                        std::fprintf(f, "%s,%.1f,%.1f,%llu,%.6f\n", c == 0 ? "one_tick" : "wider", lo,
                                     lo + 2.0 / kImbBuckets, static_cast<unsigned long long>(k),
                                     k ? static_cast<double>(up_[c][b]) / static_cast<double>(k) : 0.0);
                    }
                std::fclose(f);
            }
            p = std::string(csv_prefix) + "_queue.csv";
            if (FILE* f = std::fopen(p.c_str(), "w")) {
                std::fprintf(f, "shares_ahead,orders,fill_rate,median_markout_1s_cents,median_markout_10s_cents,share_underwater_10s,fills_10s\n");
                for (int b = 0; b < kAheadBuckets; ++b)
                    std::fprintf(f, "\"%s\",%llu,%.6f,%.4f,%.4f,%.4f,%zu\n", kAheadNames[b],
                                 static_cast<unsigned long long>(joined_[b]),
                                 joined_[b] ? static_cast<double>(filled_[b]) / static_cast<double>(joined_[b]) : 0.0,
                                 median(all1_[b]), median(all10_[b]), frac_below_zero(all10_[b]), all10_[b].size());
                std::fclose(f);
            }
        }
    }

    const itch::Replayer& replayer() const { return rep_; }

private:
    SymbolState& state(std::uint16_t loc) {
        if (loc >= states_.size()) states_.resize(loc + 1u);
        return states_[loc];
    }
    static double median(std::vector<float> v) {
        if (v.empty()) return 0.0;
        auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
        std::nth_element(v.begin(), mid, v.end());
        return *mid;
    }
    static double median_d(std::vector<double> v) {
        if (v.empty()) return 0.0;
        auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
        std::nth_element(v.begin(), mid, v.end());
        return *mid;
    }
    static double frac_below_zero(const std::vector<float>& v) {
        if (v.empty()) return 0.0;
        std::size_t k = 0;
        for (float x : v) k += x < 0.0f;
        return static_cast<double>(k) / static_cast<double>(v.size());
    }
    // Mean with each value clipped to [-5c, +5c], so one blown-out book can't dominate.
    static double mean_clipped(const std::vector<float>& v) {
        if (v.empty()) return 0.0;
        double s = 0;
        for (float x : v) s += std::clamp(static_cast<double>(x), -5.0, 5.0);
        return s / static_cast<double>(v.size());
    }

    itch::Replayer rep_;
    std::vector<SymbolState> states_;
    std::unordered_map<OrderId, OrderTrack> tracks_;
    std::array<std::array<std::uint64_t, kImbBuckets>, 2> up_{}, down_{};
    std::array<std::uint64_t, 2> samples_{};
    std::array<std::uint64_t, kAheadBuckets> joined_{}, filled_{};
    std::array<std::vector<float>, kAheadBuckets> all1_, all10_;  // pooled at report time
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.NASDAQ_ITCH50.gz> [--csv out_prefix]\n", argv[0]);
        return 2;
    }
    const char* csv = nullptr;
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--csv")) csv = argv[i + 1];

    gzFile gz = gzopen(argv[1], "rb");
    if (!gz) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    gzbuffer(gz, 1u << 20);
    constexpr std::size_t kChunk = 64u << 20;
    std::vector<std::uint8_t> buf(kChunk + (1u << 16));
    std::size_t carry = 0;
    Study study;
    for (;;) {
        const int got = gzread(gz, buf.data() + carry, static_cast<unsigned>(kChunk));
        if (got < 0) return 1;
        const std::size_t avail = carry + static_cast<std::size_t>(got);
        std::size_t pos = 0;
        while (pos + 2 <= avail) {
            const std::size_t n = itch::be16(buf.data() + pos);
            if (pos + 2 + n > avail) break;
            if (n) study.on_message(buf.data() + pos + 2, n);
            pos += 2 + n;
        }
        carry = avail - pos;
        std::memmove(buf.data(), buf.data() + pos, carry);
        if (got == 0) break;
    }
    gzclose(gz);
    std::printf("NASDAQ ITCH 5.0: %s, %llu messages, %zu books\n", argv[1],
                static_cast<unsigned long long>(study.replayer().stats().messages), study.replayer().book_count());
    study.report(stdout, csv);
    return 0;
}
