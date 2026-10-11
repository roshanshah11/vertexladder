#pragma once

// The matching thread: one OrderBook per symbol, fed by an SPSC ring of commands from
// the gateway, publishing results on an SPSC ring of events back to it.
//
// Everything that touches a book runs on this one thread, in arrival order, so the
// output is a deterministic function of the input sequence. That is how real venues
// are built (one sequencer per book): no locks on the book, nothing to race, and a
// day can be replayed exactly from the input log. Throughput comes from keeping that
// thread's work small and cache-resident, not from adding threads.

#include "vertexladder/order_book.hpp"
#include "vertexladder/spsc_ring.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace vl {

struct Command {
    enum class Type : std::uint8_t { New, Cancel, Replace };
    Type type = Type::New;
    std::uint16_t book = 0;  // symbol index
    OrderId id = 0;          // engine order id (assigned by the gateway)
    Side side = Side::Buy;
    OrderType order_type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GTC;
    Price price = 0;
    Qty qty = 0;
};

struct EngineEvent {
    enum class Type : std::uint8_t { Accepted, Fill, Cancelled, Rejected, Replaced };
    Type type = Type::Accepted;
    Command::Type cause = Command::Type::New;  // which command produced it (for rejects)
    std::uint8_t reason = 0;                   // RejectReason / CancelReason
    std::uint16_t book = 0;
    OrderId id = 0;
    OrderId contra = 0;  // the other side of a fill
    Price price = 0;     // fill price, or new price for Replaced
    Qty qty = 0;         // fill qty, cancelled qty, or new qty for Replaced
    Qty leaves = 0;      // open qty left on `id` after a fill
    std::uint64_t match_id = 0;  // shared by both fills of one trade
};

class Engine {
public:
    using CommandRing = SpscRing<Command>;
    using EventRing = SpscRing<EngineEvent>;

    Engine(std::size_t n_books, CommandRing& in, EventRing& out, std::size_t orders_per_book = 1u << 16)
        : in_(in), out_(out) {
        books_.reserve(n_books);
        for (std::size_t i = 0; i < n_books; ++i)
            books_.push_back(std::make_unique<Book>(orders_per_book, Sink{this, static_cast<std::uint16_t>(i)}));
    }

    // Process one command if one is waiting. Returns false if the ring was empty.
    bool poll_once() {
        Command c;
        if (!in_.try_pop(c)) return false;
        process(c);
        return true;
    }

    // Busy-poll until stop is set, then drain what is left. Busy polling burns a core,
    // which is the normal trade on a trading box: waking a sleeping thread costs
    // microseconds, more than the matching itself.
    void run(const std::atomic<bool>& stop) {
        while (!stop.load(std::memory_order_relaxed)) {
            if (!poll_once()) cpu_relax();
        }
        while (poll_once()) {
        }
    }

    void process(const Command& c) {
        if (c.book >= books_.size()) {
            emit({EngineEvent::Type::Rejected, c.type, static_cast<std::uint8_t>(RejectReason::UnknownSymbol), c.book, c.id});
            return;
        }
        cause_ = c.type;
        Book& b = *books_[c.book];
        switch (c.type) {
            case Command::Type::New:
                b.submit(NewOrder{c.id, c.side, c.order_type, c.tif, c.price, c.qty});
                break;
            case Command::Type::Cancel: b.cancel(c.id); break;
            case Command::Type::Replace: b.replace(c.id, c.price, c.qty); break;
        }
    }

    std::size_t book_count() const noexcept { return books_.size(); }
    std::uint64_t events_published() const noexcept { return published_; }

private:
    struct Sink {
        Engine* e;
        std::uint16_t book;
        void on_accept(const NewOrder& o) {
            e->emit({EngineEvent::Type::Accepted, e->cause_, 0, book, o.id, 0, o.price, o.qty, o.qty});
        }
        void on_trade(const Trade& t) {
            const std::uint64_t m = ++e->match_seq_;
            e->emit({EngineEvent::Type::Fill, e->cause_, 0, book, t.taker_id, t.maker_id, t.price, t.qty,
                     t.taker_remaining, m});
            e->emit({EngineEvent::Type::Fill, e->cause_, 0, book, t.maker_id, t.taker_id, t.price, t.qty,
                     t.maker_remaining, m});
        }
        void on_cancel(OrderId id, Qty q, CancelReason r) {
            e->emit({EngineEvent::Type::Cancelled, e->cause_, static_cast<std::uint8_t>(r), book, id, 0, 0, q});
        }
        void on_reject(OrderId id, RejectReason r) {
            e->emit({EngineEvent::Type::Rejected, e->cause_, static_cast<std::uint8_t>(r), book, id});
        }
        void on_replace(OrderId id, Price p, Qty q) {
            e->emit({EngineEvent::Type::Replaced, e->cause_, 0, book, id, 0, p, q, q});
        }
    };
    using Book = OrderBook<Sink>;

    // Never drop an event: if the gateway falls behind, the engine waits for it.
    void emit(const EngineEvent& ev) {
        while (!out_.try_push(ev)) cpu_relax();
        ++published_;
    }

    static void cpu_relax() noexcept {
#if defined(__x86_64__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#else
        std::this_thread::yield();
#endif
    }

    CommandRing& in_;
    EventRing& out_;
    std::vector<std::unique_ptr<Book>> books_;
    Command::Type cause_ = Command::Type::New;
    std::uint64_t match_seq_ = 0;
    std::uint64_t published_ = 0;
};

}  // namespace vl
