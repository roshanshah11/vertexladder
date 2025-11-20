#pragma once
#include <memory>
#include <string>
#include "orderbook/Core/Interfaces.hpp"

namespace orderbook {

class IMarketDataConnector {
public:
    virtual ~IMarketDataConnector() = default;

    // Start connector (connect to feed, start threads, etc.)
    virtual bool start() = 0;

    // Stop connector and cleanup
    virtual void stop() = 0;

    // Is connector running
    virtual bool isRunning() const = 0;

    // Human-readable name
    virtual std::string name() const = 0;
    // Optionally attach order book for applying external updates
    virtual void setOrderBook(class OrderBook* book) { (void)book; }
};

using MarketDataConnectorPtr = std::shared_ptr<IMarketDataConnector>;

}
