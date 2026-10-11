#pragma once

// NASDAQ TotalView-ITCH 5.0 decoder and book replayer.
//
// ITCH is the feed NASDAQ publishes of every order event on its book: adds, executions,
// cancels, deletes, replaces. It is market-by-order, so replaying it rebuilds every
// stock's full order book exactly as NASDAQ saw it. The historical files on
// emi.nasdaq.com are "BinaryFILE" framing: each message is a 2-byte big-endian length
// followed by the message. All integers are big-endian; prices have 4 implied
// decimals ($12.3400 arrives as 123400), which maps directly onto integer ticks.

#include "vertexladder/order_book.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace vl::itch {

inline std::uint16_t be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
inline std::uint32_t be32(const std::uint8_t* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}
inline std::uint64_t be64(const std::uint8_t* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}
inline std::uint64_t be48(const std::uint8_t* p) noexcept {
    return (std::uint64_t{be16(p)} << 32) | be32(p + 2);
}

// Byte offsets from the ITCH 5.0 spec. Every message starts with:
//   type(1) stock_locate(2) tracking_number(2) timestamp(6, ns since midnight)
namespace off {
inline constexpr std::size_t kLocate = 1, kTimestamp = 5, kBody = 11;
}

// Total length of each message type we act on, used to reject truncated input.
inline constexpr std::size_t kLenAdd = 36, kLenAddMpid = 40, kLenExec = 31, kLenExecPx = 36,
                             kLenCancel = 23, kLenDelete = 19, kLenReplace = 35, kLenStock = 39;

struct Stats {
    std::uint64_t messages = 0;
    std::uint64_t by_type[256] = {};
    std::uint64_t book_events = 0;  // A F E C X D U
    std::uint64_t unknown_ref = 0;  // E/C/X/D/U naming an order we don't have
    std::uint64_t overfill = 0;     // executed/cancelled more than the open quantity
    std::uint64_t malformed = 0;    // a message shorter than its type requires
    // Validation (only with check_executions): an 'E' execution should hit the order at
    // the front of the queue at the best price, if our book matches NASDAQ's.
    std::uint64_t exec_checked = 0;
    std::uint64_t exec_at_best = 0;
    std::uint64_t exec_first_in_queue = 0;
};

// Rebuilds one OrderBook per stock (keyed by stock locate code) from ITCH messages.
class Replayer {
public:
    using Book = OrderBook<NullListener>;

    explicit Replayer(bool check_executions = false) : check_(check_executions) {}

    // Feed a buffer in BinaryFILE framing. Consumes only complete messages and returns
    // how many bytes it used; the caller carries the remainder into the next buffer.
    std::size_t feed(const std::uint8_t* data, std::size_t len) {
        std::size_t pos = 0;
        while (pos + 2 <= len) {
            const std::size_t n = be16(data + pos);
            if (pos + 2 + n > len) break;
            on_message(data + pos + 2, n);
            pos += 2 + n;
        }
        return pos;
    }

    void on_message(const std::uint8_t* m, std::size_t n) {
        if (n == 0) return;
        ++stats_.messages;
        ++stats_.by_type[m[0]];
        switch (m[0]) {
            case 'A':
            case 'F': {
                if (n < kLenAdd) return malformed();
                ++stats_.book_events;
                const Side side = m[19] == 'B' ? Side::Buy : Side::Sell;
                book(m).insert_resting(be64(m + 11), side, static_cast<Price>(be32(m + 32)), be32(m + 20));
                return;
            }
            case 'E':
            case 'C': {
                if (n < (m[0] == 'E' ? kLenExec : kLenExecPx)) return malformed();
                ++stats_.book_events;
                const OrderId ref = be64(m + 11);
                if (check_ && m[0] == 'E') check_execution(m, ref);
                return reduce(m, ref, be32(m + 19));
            }
            case 'X': {
                if (n < kLenCancel) return malformed();
                ++stats_.book_events;
                return reduce(m, be64(m + 11), be32(m + 19));
            }
            case 'D': {
                if (n < kLenDelete) return malformed();
                ++stats_.book_events;
                if (!book(m).erase(be64(m + 11))) ++stats_.unknown_ref;
                return;
            }
            case 'U': {
                // Replace = delete the old order and add a new one (new ref, same side,
                // no time priority kept).
                if (n < kLenReplace) return malformed();
                ++stats_.book_events;
                Book& b = book(m);
                const OrderId old_ref = be64(m + 11);
                const auto old = b.info(old_ref);
                if (!old) {
                    ++stats_.unknown_ref;
                    return;
                }
                b.erase(old_ref);
                b.insert_resting(be64(m + 19), old->side, static_cast<Price>(be32(m + 31)), be32(m + 27));
                return;
            }
            case 'R': {
                if (n < kLenStock) return malformed();
                const std::uint16_t loc = be16(m + off::kLocate);
                if (symbols_.size() <= loc) symbols_.resize(loc + 1u);
                symbols_[loc].assign(reinterpret_cast<const char*>(m + 11), 8);
                while (!symbols_[loc].empty() && symbols_[loc].back() == ' ') symbols_[loc].pop_back();
                return;
            }
            default:
                return;  // trades, system events, halts, imbalance messages: not book state
        }
    }

    const Stats& stats() const noexcept { return stats_; }
    std::size_t book_count() const noexcept { return book_count_; }
    const Book* find_book(std::uint16_t locate) const {
        return locate < books_.size() ? books_[locate].get() : nullptr;
    }
    std::string symbol(std::uint16_t locate) const {
        return locate < symbols_.size() ? symbols_[locate] : std::string();
    }
    std::size_t resting_orders() const {
        std::size_t n = 0;
        for (const auto& b : books_) if (b) n += b->order_count();
        return n;
    }
    std::uint64_t last_timestamp_ns() const noexcept { return last_ts_; }

private:
    Book& book(const std::uint8_t* m) {
        last_ts_ = be48(m + off::kTimestamp);
        const std::uint16_t loc = be16(m + off::kLocate);
        if (loc >= books_.size()) books_.resize(loc + 1u);
        if (!books_[loc]) {
            books_[loc] = std::make_unique<Book>(1024);
            ++book_count_;
        }
        return *books_[loc];
    }

    void reduce(const std::uint8_t* m, OrderId ref, Qty shares) {
        Book& b = book(m);
        const auto open = b.open_qty(ref);
        if (!open) {
            ++stats_.unknown_ref;
            return;
        }
        if (shares > *open) ++stats_.overfill;
        b.reduce(ref, shares);
    }

    void check_execution(const std::uint8_t* m, OrderId ref) {
        const auto inf = book(m).info(ref);
        if (!inf) return;
        ++stats_.exec_checked;
        stats_.exec_at_best += inf->at_best_price;
        stats_.exec_first_in_queue += inf->first_in_queue;
    }

    void malformed() { ++stats_.malformed; }

    std::vector<std::unique_ptr<Book>> books_;
    std::vector<std::string> symbols_;
    std::size_t book_count_ = 0;
    std::uint64_t last_ts_ = 0;
    Stats stats_;
    bool check_;
};

}  // namespace vl::itch
