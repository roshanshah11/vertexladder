#pragma once

// Minimal FIX 4.4 tag=value codec.
//
// Parsing is zero-copy: a parsed message is an array of (tag, string_view) pairs
// pointing into the caller's receive buffer. The parser validates framing the way a
// counterparty would: BeginString first, BodyLength second and equal to the byte count
// up to the CheckSum field, MsgType third, CheckSum last and correct.
//
// Writing goes into a fixed stack buffer; BodyLength and CheckSum are filled in at the
// end. No std::string, no heap.

#include "vertexladder/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace vl::fix {

inline constexpr char kSoh = '\x01';

namespace tag {
inline constexpr int BeginString = 8, BodyLength = 9, MsgType = 35, SenderCompID = 49, TargetCompID = 56,
                     MsgSeqNum = 34, SendingTime = 52, CheckSum = 10, TestReqID = 112, HeartBtInt = 108,
                     EncryptMethod = 98, Text = 58, BeginSeqNo = 7, EndSeqNo = 16, RefSeqNum = 45,
                     ClOrdID = 11, OrigClOrdID = 41, OrderID = 37, ExecID = 17, Symbol = 55, Side = 54,
                     OrderQty = 38, OrdType = 40, Price = 44, TimeInForce = 59, ExecType = 150,
                     OrdStatus = 39, LeavesQty = 151, CumQty = 14, AvgPx = 6, LastQty = 32, LastPx = 31,
                     OrdRejReason = 103, CxlRejResponseTo = 434, CxlRejReason = 102, TransactTime = 60;
}

enum class ParseStatus { Ok, NeedMore, Garbled };

struct Field {
    int tag;
    std::string_view value;
};

class Message {
public:
    static constexpr std::size_t kMaxFields = 64;

    std::string_view get(int t) const noexcept {
        for (std::size_t i = 0; i < count_; ++i)
            if (fields_[i].tag == t) return fields_[i].value;
        return {};
    }
    bool has(int t) const noexcept { return !get(t).empty(); }
    std::string_view msg_type() const noexcept { return count_ > 2 ? fields_[2].value : std::string_view{}; }
    std::optional<std::uint64_t> get_uint(int t) const noexcept;
    std::size_t size() const noexcept { return count_; }
    const Field& operator[](std::size_t i) const noexcept { return fields_[i]; }

private:
    friend ParseStatus parse(std::string_view, Message&, std::size_t&);
    std::array<Field, kMaxFields> fields_{};
    std::size_t count_ = 0;
};

// Try to parse one message from the front of `buf`.
//   Ok:       `out` holds it, `consumed` is its length in bytes.
//   NeedMore: no complete message yet; read more bytes.
//   Garbled:  the front of the buffer is not valid FIX; `consumed` is how many bytes to
//             drop to resynchronise (FIX says garbled messages are ignored, not answered).
ParseStatus parse(std::string_view buf, Message& out, std::size_t& consumed);

// Decimal string -> integer ticks with `decimals` implied places, exactly (no double).
// "101.25" with decimals = 4 -> 1012500. Rejects more precision than the tick allows.
std::optional<Price> parse_price(std::string_view s, int decimals) noexcept;

// Integer ticks -> decimal string. Returns the number of chars written.
std::size_t format_price(Price p, int decimals, char* out) noexcept;

std::uint8_t checksum(std::string_view bytes) noexcept;

class Writer {
public:
    static constexpr std::size_t kCapacity = 1024;

    // Starts a message: header fields in the order FIX requires.
    Writer(std::string_view msg_type, std::string_view sender, std::string_view target, std::uint64_t seq,
           std::string_view sending_time);

    Writer& add(int t, std::string_view v);
    Writer& add(int t, std::uint64_t v);
    Writer& add(int t, char v);
    Writer& add_price(int t, Price p, int decimals);

    // Writes 8= and 9= in front, appends 10=, returns the full message.
    std::string_view finish();

private:
    void put(std::string_view s);
    void put_tag(int t);
    // Room at the front for "8=FIX.4.4|9=NNNNN|" so the body never has to move.
    static constexpr std::size_t kHeadRoom = 32;
    std::array<char, kCapacity> buf_;
    std::size_t len_ = kHeadRoom;
    std::size_t start_ = 0;
    bool overflow_ = false;
};

// Current UTC time as FIX UTCTimestamp, YYYYMMDD-HH:MM:SS.sss
std::string_view utc_timestamp(std::array<char, 32>& out);

}  // namespace vl::fix
