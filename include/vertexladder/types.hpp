#pragma once

#include <cstdint>
#include <limits>

namespace vl {

// Prices are integer ticks. A double price can't be used as a map key or compared
// for equality safely (0.1 + 0.2 != 0.3), and every venue publishes prices as fixed
// point anyway: NASDAQ ITCH sends 4 implied decimals, so $123.45 arrives as 1234500.
using Price = std::int64_t;
using Qty = std::uint32_t;      // ITCH share counts are 32-bit
using OrderId = std::uint64_t;  // 0 is reserved as "no order"

inline constexpr Price kMarketBuyPrice = std::numeric_limits<Price>::max();
inline constexpr Price kMarketSellPrice = std::numeric_limits<Price>::min();

enum class Side : std::uint8_t { Buy, Sell };

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

enum class OrderType : std::uint8_t { Limit, Market };

// GTC: rest whatever doesn't fill. IOC: fill what you can, cancel the rest.
// FOK: fill completely right now or do nothing.
enum class TimeInForce : std::uint8_t { GTC, IOC, FOK };

enum class RejectReason : std::uint8_t {
    None,
    DuplicateOrderId,
    UnknownOrder,
    InvalidQuantity,
    InvalidPrice,
    UnknownSymbol,
};

enum class CancelReason : std::uint8_t {
    UserRequested,  // explicit cancel
    IocRemainder,   // IOC / market order: the unfilled part is never rested
    FokUnfilled,    // FOK that could not fill completely, so nothing traded
};

// Index into the order pool / level pool. Indices survive vector reallocation,
// pointers don't, and they are half the size of a pointer.
using Handle = std::uint32_t;
inline constexpr Handle kNil = std::numeric_limits<Handle>::max();

}  // namespace vl
