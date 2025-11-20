#pragma once
#include "Types.hpp"
#include "Order.hpp"
#include "Interfaces.hpp"
#include "MarketData.hpp"
#include <boost/lockfree/spsc_queue.hpp>
#include <vector>
#include <unordered_map>
#include <optional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include <atomic>
// Note: Do not include the top-level OrderBook.hpp from within the core header to avoid circular includes

namespace orderbook {

// PriceLevel is defined in Order.hpp

struct MarketUpdate {
    enum class Type {
        Add,
        Modify,
        Remove,
        SnapshotStart, // Clear book
        SnapshotEnd
    };
    Type type;
    Side side;
    Price price;
    Quantity quantity;
    size_t order_count;
};

/**
 * @brief High-performance order book implementation
 */
class OrderBook {
public:
    // Constructor with dependency injection
    OrderBook(RiskManagerPtr risk_manager = nullptr,
              MarketDataPublisherPtr market_data = nullptr,
              LoggerPtr logger = nullptr);
    
    // Core operations
    OrderResult addOrder(const Order& order);
    CancelResult cancelOrder(OrderId id);
    ModifyResult modifyOrder(OrderId id, Price new_price, Quantity new_quantity);
    
    // Market data queries
    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    BestPrices getBestPrices() const;
    MarketDepth getDepth(size_t levels) const;
    
    // Statistics
    size_t getOrderCount() const;
    size_t getBidLevelCount() const;
    size_t getAskLevelCount() const;

    // Optional: apply external market data snapshot or incremental update to internal book state
    void applyExternalMarketData(const MarketDepth& depth);
    void applyExternalBookUpdate(const BookUpdate& update);
    void clearBook();

    // Process updates from the lock-free queue
    void poll();

    // Wait for all commands to be processed (for testing)
    void waitForCompletion();

    // Destructor
    ~OrderBook();

private:
    struct Command {
        enum class Type { Add, Cancel, Modify };
        Type type;
        std::unique_ptr<Order> order;
        OrderId order_id;
        Price price;
        Quantity quantity;
    };

    // Command queue for thread safety
    std::queue<Command> command_queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread processing_thread_;
    std::atomic<bool> running_;

    void processLoop();
    void processAddOrder(std::unique_ptr<Order> order);
    void processCancelOrder(OrderId id);
    void processModifyOrder(OrderId id, Price new_price, Quantity new_quantity);

    // Lock-free queue for market data updates (SPSC)
    boost::lockfree::spsc_queue<MarketUpdate, boost::lockfree::capacity<4096>> market_queue_;
    // Optimized storage: use heap-allocated PriceLevel for pointer stability
    std::vector<std::unique_ptr<PriceLevel>> bids_;
    std::vector<std::unique_ptr<PriceLevel>> asks_;
    std::unordered_map<Price, PriceLevel*> price_index_;
    std::unordered_map<OrderId, OrderLocation, OrderIdHash> order_index_;
    
    // Dependencies
    RiskManagerPtr risk_manager_;
    MarketDataPublisherPtr market_data_;
    LoggerPtr logger_;
    
    // Helper methods
    PriceLevel* findOrCreatePriceLevel(Price price, Side side);
    void removePriceLevel(PriceLevel* level, Side side);
    void maintainSortedOrder();
    void rebuildPriceIndex();
    void publishMarketDataUpdate();
    void publishBookUpdate(BookUpdate::Type type, Side side, Price price, 
                          Quantity quantity, size_t order_count);
    
    // Matching and trade execution
    void processMatching(Order& incoming_order);
    void executeMatching(Order& incoming_order, std::vector<std::unique_ptr<PriceLevel>>& opposite_levels);
    void matchAgainstPriceLevel(Order& incoming_order, PriceLevel& price_level);
    void executeTrade(Order& aggressive_order, Order& passive_order, 
                     Price trade_price, Quantity trade_quantity);
    
    // Constants
    static constexpr size_t InitialCapacity = 1024;
    static constexpr Price MinPriceIncrement = 0.01;
};

}