#pragma once
#include "Types.hpp"
#include "Order.hpp"
#include "../Utilities/ObjectPool.hpp"
#include "Interfaces.hpp"
#include "MarketData.hpp"
#include <boost/lockfree/spsc_queue.hpp>
#include <vector>
#include <unordered_map>
#include <unordered_set>
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

    // Read trade count without relying on market data. Used by performance tests.
    uint64_t getTradeCount() const;
    
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
        // For compatibility but we use OrderRequest queues for production
        std::unique_ptr<Order> order;
        OrderId order_id;
        Price price;
        Quantity quantity;
    };

    // OrderRequest used for lock-free per-thread SPSC ingestion
    struct OrderRequest {
        enum class Type { Add, Cancel, Modify } type;
        // Minimal POD fields to avoid allocations on producer hot-path
        OrderId id{0};
        Side side{Side::Buy};
        OrderType order_type{OrderType::Limit};
        Price price{0};
        Quantity quantity{0};
        // Fixed-size symbol/account buffers to avoid std::string allocations
        char symbol[32];
        char account[32];
        int tif{0};
    };

    // Sharded SPSC queues for orders to avoid global producer mutex contention
    static constexpr size_t OrderQueueCount = 4;
    static constexpr size_t OrderQueueCapacity = 100000;
    std::vector<std::unique_ptr<boost::lockfree::spsc_queue<OrderRequest, boost::lockfree::capacity<OrderQueueCapacity>>>> order_queues_;
    std::atomic<size_t> order_producer_rr_index_{0};
    std::atomic<bool> order_data_available_{false};
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread processing_thread_;
    std::atomic<bool> running_;

    void processLoop();
    void processAddOrder(Order* order);
    void processCancelOrder(OrderId id);
    void processModifyOrder(OrderId id, Price new_price, Quantity new_quantity);

    // Lock-free queue for market data updates (SPSC)
    // Increased capacity to absorb bursts during performance tests (tuned for memory)
    // Sharded SPSC queues for market updates to avoid multi-producer contention.
    static constexpr size_t MarketDataQueueCount = 4; // tuned for typical thread count
    static constexpr size_t MarketDataQueueCapacity = 100000;
    std::vector<std::unique_ptr<boost::lockfree::spsc_queue<MarketUpdate, boost::lockfree::capacity<MarketDataQueueCapacity>>>> market_queues_;
    // Simple round-robin index allocator for threads. Each producing thread gets a queue index
    // stored thread-locally to avoid hashing or heavy atomics on hot path.
    std::atomic<size_t> producer_rr_index_{0};
    // Per-OrderBook trade counter for benchmarking without a publisher
    std::atomic<uint64_t> trade_count_{0};
    // Simple single-threaded LIFO free-list for Orders (consumer-only)
    static constexpr size_t DefaultOrderPoolSize = 20000;
    std::vector<Order*> order_free_list_;
    // Track in-use pointers to detect double-release / misuse during development
    std::unordered_set<Order*> order_in_use_set_;
    // Deferred retirement list to avoid immediate reuse within same processing batch
    std::vector<Order*> order_retire_list_;
    // Debug counters for diagnostics
    std::atomic<uint64_t> debug_acquire_count_{0};
    std::atomic<uint64_t> debug_release_count_{0};
        // Acquire and release helpers (consumer single-threaded)
        Order* acquireOrder(const OrderRequest& req);
        void releaseOrder(Order* order);
    // Optimized storage: use heap-allocated PriceLevel for pointer stability
    std::vector<std::unique_ptr<PriceLevel>> bids_;
    std::vector<std::unique_ptr<PriceLevel>> asks_;
    std::unordered_map<Price, PriceLevel*> price_index_;
    std::unordered_map<OrderId, OrderLocation, OrderIdHash> order_index_;
    // Note: orders in the book are owned by price levels and managed manually
    
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

    // Accessor for performance harness to read trade count without relying on market_data
    // Implemented in header above as public method

    // Get a deterministic, thread-affine queue index for producers
    size_t getProducerQueueIndex();
    size_t getOrderQueueIndex();
    
    // Constants
    static constexpr size_t InitialCapacity = 1024;
    static constexpr Price MinPriceIncrement = 0.01;
};

}