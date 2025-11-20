#include "orderbook/Core/OrderBook.hpp"
#include "orderbook/Utilities/PerformanceTimer.hpp"
#include "orderbook/Utilities/MemoryManager.hpp"
#include "orderbook/Utilities/PerformanceMeasurement.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>

namespace orderbook {

// Constructor with dependency injection
OrderBook::OrderBook(RiskManagerPtr risk_manager,
                     MarketDataPublisherPtr market_data,
                     LoggerPtr logger)
    : risk_manager_(risk_manager), market_data_(market_data), logger_(logger), running_(true) {
    
    // Reserve capacity for typical number of price levels
    bids_.reserve(InitialCapacity);
    asks_.reserve(InitialCapacity);
    
    if (logger_) {
        logger_->info("OrderBook initialized", "OrderBook::Constructor");
    }

    // Start processing thread
    processing_thread_ = std::thread(&OrderBook::processLoop, this);
}

OrderBook::~OrderBook() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        running_ = false;
    }
    queue_cv_.notify_one();
    if (processing_thread_.joinable()) {
        processing_thread_.join();
    }
}

void OrderBook::waitForCompletion() {
    int wait_count = 0;
    while (true) {
        size_t queue_size = 0;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (command_queue_.empty()) {
                break;
            }
            queue_size = command_queue_.size();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wait_count++;
        if (wait_count % 10 == 0) {
            std::cout << "Waiting for completion... Queue size: " << queue_size << std::endl;
        }
    }
    // Give a little time for the last item to be processed
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void OrderBook::processLoop() {
    size_t processed_count = 0;
    std::cout << "Consumer thread started" << std::endl;
    while (running_) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return !command_queue_.empty() || !running_; });

        if (!running_ && command_queue_.empty()) {
            std::cout << "Consumer thread stopping" << std::endl;
            return;
        }

        while (!command_queue_.empty()) {
            Command cmd = std::move(command_queue_.front());
            command_queue_.pop();
            lock.unlock();

            switch (cmd.type) {
                case Command::Type::Add:
                    if (cmd.order) {
                        processAddOrder(std::move(cmd.order));
                    }
                    break;
                case Command::Type::Cancel:
                    processCancelOrder(cmd.order_id);
                    break;
                case Command::Type::Modify:
                    processModifyOrder(cmd.order_id, cmd.price, cmd.quantity);
                    break;
            }
            
            processed_count++;
            if (processed_count % 10000 == 0) {
                std::cout << "Consumer processed " << processed_count << " commands" << std::endl;
            }

            lock.lock();
        }
    }
    std::cout << "Consumer thread exited loop" << std::endl;
}

void OrderBook::applyExternalMarketData(const MarketDepth& depth) {
    // Push clear message first
    MarketUpdate clear_msg;
    clear_msg.type = MarketUpdate::Type::SnapshotStart;
    market_queue_.push(clear_msg);

    // Push all bids
    for (const auto& level : depth.bids) {
        MarketUpdate update;
        update.type = MarketUpdate::Type::Add;
        update.side = Side::Buy;
        update.price = level.price;
        update.quantity = level.quantity;
        update.order_count = level.order_count;
        market_queue_.push(update);
    }

    // Push all asks
    for (const auto& level : depth.asks) {
        MarketUpdate update;
        update.type = MarketUpdate::Type::Add;
        update.side = Side::Sell;
        update.price = level.price;
        update.quantity = level.quantity;
        update.order_count = level.order_count;
        market_queue_.push(update);
    }
}

void OrderBook::applyExternalBookUpdate(const BookUpdate& update) {
    MarketUpdate mu;
    if (update.type == BookUpdate::Type::Add) mu.type = MarketUpdate::Type::Add;
    else if (update.type == BookUpdate::Type::Modify) mu.type = MarketUpdate::Type::Modify;
    else mu.type = MarketUpdate::Type::Remove;
    
    mu.side = update.side;
    mu.price = update.price;
    mu.quantity = update.quantity;
    mu.order_count = update.order_count;
    
    market_queue_.push(mu);
}

void OrderBook::clearBook() {
    MarketUpdate mu;
    mu.type = MarketUpdate::Type::SnapshotStart;
    market_queue_.push(mu);
}

// Core operations
OrderResult OrderBook::addOrder(const Order& order) {
    // Create a copy of the order for processing
    auto order_copy = std::make_unique<Order>(order.id.value, order.side, order.type, order.price, order.quantity, order.symbol);
    order_copy->timestamp = std::chrono::system_clock::now();
    order_copy->account = order.account;
    order_copy->tif = order.tif;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        Command cmd;
        cmd.type = Command::Type::Add;
        cmd.order = std::move(order_copy);
        command_queue_.push(std::move(cmd));
    }
    queue_cv_.notify_one();

    return OrderResult::success(order.id);
}

void OrderBook::processAddOrder(std::unique_ptr<Order> order_ptr) {
    // PERF_TIMER("OrderBook::processAddOrder", logger_);
    // PERF_MEASURE("OrderBook::processAddOrder");
    
    if (logger_) {
        logger_->debug("Processing Add Order ID: " + std::to_string(order_ptr->id.value) + 
                      " Side: " + (order_ptr->side == Side::Buy ? "Buy" : "Sell") +
                      " Price: " + std::to_string(order_ptr->price) +
                      " Quantity: " + std::to_string(order_ptr->quantity), "OrderBook::processAddOrder");
    }
    
    // Risk validation if risk manager is available
    if (risk_manager_) {
        // Associate order with account
        std::string account = order_ptr->account.empty() ? "default" : order_ptr->account;
        risk_manager_->associateOrderWithAccount(order_ptr->id, account);
        
        // Get the portfolio for the order's account
        const Portfolio& portfolio = risk_manager_->getPortfolio(account);
        auto risk_check = risk_manager_->validateOrder(*order_ptr, portfolio);
        if (risk_check.isRejected()) {
            if (logger_) {
                logger_->error("Order rejected by risk manager: " + risk_check.reason, 
                              "OrderBook::processAddOrder - OrderID: " + std::to_string(order_ptr->id.value) + 
                              " Account: " + account);
            }
            return;
        }
        
        if (logger_) {
            logger_->debug("Order passed risk validation: " + risk_check.reason, 
                          "OrderBook::processAddOrder - OrderID: " + std::to_string(order_ptr->id.value) + 
                          " Account: " + account);
        }
    }
    
    // Check if order already exists
    if (order_index_.find(order_ptr->id) != order_index_.end()) {
        if (logger_) {
            logger_->error("Duplicate order ID: " + std::to_string(order_ptr->id.value), 
                          "OrderBook::processAddOrder");
        }
        return;
    }
    
    // Find or create price level
    PriceLevel* price_level = findOrCreatePriceLevel(order_ptr->price, order_ptr->side);
    if (!price_level) {
        if (logger_) {
            logger_->error("Failed to create price level for price: " + std::to_string(order_ptr->price) +
                          " side: " + (order_ptr->side == Side::Buy ? "Buy" : "Sell"), 
                          "OrderBook::processAddOrder - OrderID: " + std::to_string(order_ptr->id.value));
        }
        return;
    }
    
    // Add order to price level
    price_level->addOrder(order_ptr.get());
    
    // Add to order index for fast lookup
    OrderLocation location(order_ptr.get(), price_level, order_ptr->side);
    order_index_[order_ptr->id] = location;
    
    // Release ownership - order is now managed by price level
    Order* order_raw = order_ptr.release();
    
    // Maintain sorted order - optimization: findOrCreatePriceLevel already maintains order
    // maintainSortedOrder();
    
    // Publish book update for order addition
    publishBookUpdate(BookUpdate::Type::Add, order_raw->side, order_raw->price, 
                     order_raw->remainingQuantity(), price_level->order_count);
    
    // Check for immediate matching opportunities
    processMatching(*order_raw);
    
    // Handle aggressive order updates after matching
    if (order_raw->filled_quantity > 0) {
        // Update price level total quantity to reflect the fill
        // Note: We must be careful not to double-count if we remove the order later
        // The safest way is to update the price level's quantity to match the order's new state
        // But PriceLevel::updateQuantity is designed for this
        
        price_level->updateQuantity(order_raw, order_raw->filled_quantity);
        
        // If fully filled, updateQuantity already removed it from the list?
        // Let's check PriceLevel::updateQuantity implementation
        // void updateQuantity(Order* order, Quantity filled_quantity) {
        //     if (filled_quantity <= total_quantity) total_quantity -= filled_quantity;
        //     if (order->isFullyFilled()) removeOrder(order);
        // }
        
        // So updateQuantity handles removal from the linked list!
        // But we still need to handle OrderIndex and Memory cleanup.
        
        if (order_raw->isFullyFilled()) {
            // Publish book update for removal
            publishBookUpdate(BookUpdate::Type::Remove, order_raw->side, order_raw->price, 
                             0, price_level->order_count); // order_count is already updated by removeOrder
            
            // Clean up empty price level
            if (price_level->isEmpty()) {
                removePriceLevel(price_level, order_raw->side);
            }
            
            // Remove from order index
            auto it = order_index_.find(order_raw->id);
            if (it != order_index_.end()) {
                order_index_.erase(it);
            }
            
            // Delete the order
            delete order_raw;
            
            // Set order_raw to null to avoid using it
            order_raw = nullptr;
        } else {
            // Partially filled
             publishBookUpdate(BookUpdate::Type::Modify, order_raw->side, order_raw->price, 
                              order_raw->remainingQuantity(), price_level->order_count);
        }
    }
    
    // Publish market data update (best prices and depth)
    publishMarketDataUpdate();
    
    /*
    if (logger_ && order_raw) { // Check order_raw validity
        logger_->info("Order added successfully ID: " + std::to_string(order_raw->id.value), 
                     "OrderBook::processAddOrder");
    }
    */
}

CancelResult OrderBook::cancelOrder(OrderId id) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        Command cmd;
        cmd.type = Command::Type::Cancel;
        cmd.order_id = id;
        command_queue_.push(std::move(cmd));
    }
    queue_cv_.notify_one();
    return CancelResult::success(true);
}

void OrderBook::processCancelOrder(OrderId id) {
    // PERF_TIMER("OrderBook::processCancelOrder", logger_);
    // PERF_MEASURE("OrderBook::processCancelOrder");
    
    if (logger_) {
        logger_->debug("Processing Cancel Order ID: " + std::to_string(id.value), "OrderBook::processCancelOrder");
    }
    
    // Find order in index
    auto it = order_index_.find(id);
    if (it == order_index_.end()) {
        if (logger_) {
            logger_->warn("Cancel requested for non-existent order ID: " + std::to_string(id.value), 
                         "OrderBook::processCancelOrder");
        }
        return;
    }
    
    const OrderLocation& location = it->second;
    if (!location.isValid()) {
        return;
    }
    
    // Store order details before removal for book update
    Side order_side = location.order->side;
    Price order_price = location.order->price;
    Quantity remaining_qty = location.order->remainingQuantity();
    
    // Remove from price level
    location.price_level->removeOrder(location.order);
    
    // Publish book update for order removal
    publishBookUpdate(BookUpdate::Type::Remove, order_side, order_price, 
                     remaining_qty, location.price_level->order_count);
    
    // Clean up empty price level
    if (location.price_level->isEmpty()) {
        removePriceLevel(location.price_level, location.side);
    }
    
    // Remove from order index
    order_index_.erase(it);
    
    // Delete the order
    delete location.order;
    
    // Publish market data update (best prices and depth)
    publishMarketDataUpdate();
    
    if (logger_) {
        logger_->info("Order canceled successfully ID: " + std::to_string(id.value), 
                     "OrderBook::processCancelOrder");
    }
}

ModifyResult OrderBook::modifyOrder(OrderId id, Price new_price, Quantity new_quantity) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        Command cmd;
        cmd.type = Command::Type::Modify;
        cmd.order_id = id;
        cmd.price = new_price;
        cmd.quantity = new_quantity;
        command_queue_.push(std::move(cmd));
    }
    queue_cv_.notify_one();
    return ModifyResult::success(true);
}

void OrderBook::processModifyOrder(OrderId id, Price new_price, Quantity new_quantity) {
    // PERF_TIMER("OrderBook::processModifyOrder", logger_);
    // PERF_MEASURE("OrderBook::processModifyOrder");
    
    if (logger_) {
        logger_->debug("Processing Modify Order ID: " + std::to_string(id.value) +
                      " New Price: " + std::to_string(new_price) +
                      " New Quantity: " + std::to_string(new_quantity), "OrderBook::processModifyOrder");
    }
    
    // Find order in index
    auto it = order_index_.find(id);
    if (it == order_index_.end()) {
        return;
    }
    
    const OrderLocation& location = it->second;
    if (!location.isValid()) {
        return;
    }
    
    Order* order = location.order;
    
    // If price is changing, we need to move the order to a different price level
    if (new_price > 0 && std::abs(order->price - new_price) > MinPriceIncrement) {
        // Store old details for book update
        Price old_price = order->price;
        Quantity old_remaining = order->remainingQuantity();
        
        // Remove from current price level
        location.price_level->removeOrder(order);
        
        // Publish book update for removal from old price level
        publishBookUpdate(BookUpdate::Type::Remove, order->side, old_price, 
                         old_remaining, location.price_level->order_count);
        
        // Clean up empty price level
        if (location.price_level->isEmpty()) {
            removePriceLevel(location.price_level, location.side);
        }
        
        // Update order price
        order->price = new_price;
        
        // Find or create new price level
        PriceLevel* new_price_level = findOrCreatePriceLevel(new_price, order->side);
        if (!new_price_level) {
            // Restore order to original price level if new level creation fails
            order->price = old_price;
            location.price_level->addOrder(order);
            return;
        }
        
        // Add to new price level
        new_price_level->addOrder(order);
        
        // Publish book update for addition to new price level
        publishBookUpdate(BookUpdate::Type::Add, order->side, new_price, 
                         order->remainingQuantity(), new_price_level->order_count);
        
        // Update order index
        order_index_[id] = OrderLocation(order, new_price_level, order->side);
    }
    
    // Update quantity if specified
    if (new_quantity > 0) {
        // Store old quantity for book update
        Quantity old_remaining = order->remainingQuantity();
        
        // Update price level quantity
        PriceLevel* current_level = order_index_[id].price_level;
        current_level->total_quantity -= old_remaining;
        
        order->quantity = new_quantity;
        // Reset filled quantity if new quantity is smaller
        if (order->filled_quantity > new_quantity) {
            order->filled_quantity = new_quantity;
        }
        
        current_level->total_quantity += order->remainingQuantity();
        
        // Publish book update for quantity modification
        publishBookUpdate(BookUpdate::Type::Modify, order->side, order->price, 
                         order->remainingQuantity(), current_level->order_count);
    }
    
    // Publish market data update
    publishMarketDataUpdate();
    
    if (logger_) {
        logger_->info("Order modified successfully ID: " + std::to_string(id.value), 
                     "OrderBook::processModifyOrder");
    }
}

// Market data queries with O(1) performance
std::optional<Price> OrderBook::bestBid() const {
    // PERF_MEASURE("OrderBook::bestBid");
    if (bids_.empty()) {
        return std::nullopt;
    }
    // Best bid is at the end (highest price)
    return bids_.back()->price;
}

std::optional<Price> OrderBook::bestAsk() const {
    // PERF_MEASURE("OrderBook::bestAsk");
    if (asks_.empty()) {
        return std::nullopt;
    }
    // Best ask is at the end (lowest price)
    return asks_.back()->price;
}

BestPrices OrderBook::getBestPrices() const {
    BestPrices prices;
    prices.timestamp = std::chrono::system_clock::now();
    
    if (!bids_.empty()) {
        const auto& best_bid_level = bids_.back();
        prices.bid = best_bid_level->price;
        prices.bid_size = best_bid_level->total_quantity;
    }
    
    if (!asks_.empty()) {
        const auto& best_ask_level = asks_.back();
        prices.ask = best_ask_level->price;
        prices.ask_size = best_ask_level->total_quantity;
    }
    
    return prices;
}

MarketDepth OrderBook::getDepth(size_t levels) const {
    MarketDepth depth;
    depth.timestamp = std::chrono::system_clock::now();
    
    // Get bid levels (highest to lowest)
    size_t bid_count = std::min(levels, bids_.size());
    depth.bids.reserve(bid_count);
    
    for (size_t i = 0; i < bid_count; ++i) {
        const auto& level = bids_[bids_.size() - 1 - i]; // Start from best (end)
        depth.bids.emplace_back(MarketDepth::Level{
            level->price, 
            level->total_quantity, 
            level->order_count
        });
    }
    
    // Get ask levels (lowest to highest)
    size_t ask_count = std::min(levels, asks_.size());
    depth.asks.reserve(ask_count);
    
    for (size_t i = 0; i < ask_count; ++i) {
        const auto& level = asks_[asks_.size() - 1 - i]; // Start from best (end)
        depth.asks.emplace_back(MarketDepth::Level{
            level->price, 
            level->total_quantity, 
            level->order_count
        });
    }
    
    return depth;
}

// Statistics
size_t OrderBook::getOrderCount() const {
    return order_index_.size();
}

size_t OrderBook::getBidLevelCount() const {
    return bids_.size();
}

size_t OrderBook::getAskLevelCount() const {
    return asks_.size();
}

// Helper methods for price level management
PriceLevel* OrderBook::findOrCreatePriceLevel(Price price, Side side) {
    // Check if price level already exists in index
    auto it = price_index_.find(price);
    if (it != price_index_.end()) {
        // Verify the price level is still valid
        PriceLevel* level = it->second;
        if (level && std::abs(level->price - price) < MinPriceIncrement) {
            return level;
        }
        // Remove invalid entry
        price_index_.erase(it);
    }
    
    // Create new price level
    auto& levels = (side == Side::Buy) ? bids_ : asks_;
    
    // Find insertion point to maintain sorted order
    auto insert_pos = levels.end();
    if (side == Side::Buy) {
        // For bids: insert in ascending order (best bid at end)
        insert_pos = std::lower_bound(levels.begin(), levels.end(), price,
            [](const std::unique_ptr<PriceLevel>& level, Price p) {
                return level->price < p;
            });
    } else {
        // For asks: insert in descending order (best ask at end)
        insert_pos = std::lower_bound(levels.begin(), levels.end(), price,
            [](const std::unique_ptr<PriceLevel>& level, Price p) {
                return level->price > p;
            });
    }
    
    // Insert new price level at correct position (heap-allocated for stable pointers)
    auto inserted_it = levels.emplace(insert_pos, std::make_unique<PriceLevel>(price));
    PriceLevel* new_level = inserted_it->get();
    
    // Add to price index for O(1) lookup
    price_index_[price] = new_level;
    
    if (logger_) {
        logger_->debug("Created new price level at " + std::to_string(price) + 
                      " for " + (side == Side::Buy ? "bids" : "asks"), 
                      "OrderBook::findOrCreatePriceLevel");
    }
    
    return new_level;
}

void OrderBook::removePriceLevel(PriceLevel* level, Side side) {
    if (!level) {
        if (logger_) {
            logger_->warn("Attempted to remove null price level", "OrderBook::removePriceLevel");
        }
        return;
    }
    
    // Verify the level is actually empty before removing
    if (!level->isEmpty()) {
        if (logger_) {
            logger_->warn("Attempted to remove non-empty price level at " + 
                         std::to_string(level->price), "OrderBook::removePriceLevel");
        }
        return;
    }
    
    Price price = level->price;
    
    // Remove from price index first
    auto index_it = price_index_.find(price);
    if (index_it != price_index_.end()) {
        price_index_.erase(index_it);
    }
    
    // Remove from appropriate vector
    auto& levels = (side == Side::Buy) ? bids_ : asks_;
    
    // Find and remove the level efficiently
    auto it = std::find_if(levels.begin(), levels.end(),
        [level](const std::unique_ptr<PriceLevel>& pl) { return pl.get() == level; });
    
    if (it != levels.end()) {
        levels.erase(it);
        
        if (logger_) {
            logger_->debug("Removed empty price level at " + std::to_string(price) + 
                          " from " + (side == Side::Buy ? "bids" : "asks"), 
                          "OrderBook::removePriceLevel");
        }
    } else {
        if (logger_) {
            logger_->warn("Price level not found in vector during removal at " + 
                         std::to_string(price), "OrderBook::removePriceLevel");
        }
    }
    // No need to rebuild the entire price index: other PriceLevel pointers are stable
    // as PriceLevel objects are heap-allocated. We only removed the specific price.
}

void OrderBook::maintainSortedOrder() {
    // Check if vectors are already sorted to avoid unnecessary work
    bool bids_sorted = std::is_sorted(bids_.begin(), bids_.end(),
        [](const std::unique_ptr<PriceLevel>& a, const std::unique_ptr<PriceLevel>& b) {
            return a->price < b->price;
        });
    
    bool asks_sorted = std::is_sorted(asks_.begin(), asks_.end(),
        [](const std::unique_ptr<PriceLevel>& a, const std::unique_ptr<PriceLevel>& b) {
            return a->price > b->price;
        });
    
    // Only sort if necessary
    if (!bids_sorted) {
        std::sort(bids_.begin(), bids_.end(),
            [](const std::unique_ptr<PriceLevel>& a, const std::unique_ptr<PriceLevel>& b) {
                return a->price < b->price;
            });
        
        if (logger_) {
            logger_->debug("Sorted bid levels", "OrderBook::maintainSortedOrder");
        }
    }
    
    if (!asks_sorted) {
        std::sort(asks_.begin(), asks_.end(),
            [](const std::unique_ptr<PriceLevel>& a, const std::unique_ptr<PriceLevel>& b) {
                return a->price > b->price;
            });
        
        if (logger_) {
            logger_->debug("Sorted ask levels", "OrderBook::maintainSortedOrder");
        }
    }
    
    // Update price index to ensure consistency after sorting
    rebuildPriceIndex();
}

void OrderBook::rebuildPriceIndex() {
    price_index_.clear();
    
    // Add all bid levels to index
    for (auto& level : bids_) {
        price_index_[level->price] = level.get();
    }
    
    // Add all ask levels to index
    for (auto& level : asks_) {
        price_index_[level->price] = level.get();
    }
    
    if (logger_) {
        logger_->debug("Rebuilt price index with " + std::to_string(price_index_.size()) + 
                      " levels", "OrderBook::rebuildPriceIndex");
    }
}

void OrderBook::publishMarketDataUpdate() {
    if (market_data_) {
        // Publish best prices with sub-10μs latency requirement
        BestPrices prices = getBestPrices();
        market_data_->publishBestPrices(prices);
        
        // Publish market depth (top 5 levels) for full book updates
        MarketDepth depth = getDepth(5);
        market_data_->publishDepth(depth);
    }
}

void OrderBook::publishBookUpdate(BookUpdate::Type type, Side side, Price price, 
                                 Quantity quantity, size_t order_count) {
    if (market_data_) {
        // Create sequence number for gap detection
        static std::atomic<SequenceNumber> book_sequence{0};
        SequenceNumber seq = ++book_sequence;
        
        BookUpdate update(type, side, price, quantity, order_count, seq);
        market_data_->publishBookUpdate(update);
    }
}

void OrderBook::processMatching(Order& incoming_order) {
    // Simple matching logic - check if we can match against opposite side
    auto& opposite_levels = (incoming_order.side == Side::Buy) ? asks_ : bids_;
    
    if (opposite_levels.empty()) {
        return; // No opposite side orders to match against
    }
    
    // Find best opposite price
    const auto& best_level = opposite_levels.back(); // unique_ptr - Best price is at the end
    
    bool can_match = false;
    if (incoming_order.side == Side::Buy) {
        // Buy order can match if buy price >= ask price
        can_match = incoming_order.price >= best_level->price;
    } else {
        // Sell order can match if sell price <= bid price
        can_match = incoming_order.price <= best_level->price;
    }
    
    if (can_match && !best_level->isEmpty()) {
        // Execute trades
        executeMatching(incoming_order, opposite_levels);
    }
}

void OrderBook::executeMatching(Order& incoming_order, std::vector<std::unique_ptr<PriceLevel>>& opposite_levels) {
    // Levels are sorted such that the "Best" price is always at the end (back).
    // Bids: Ascending (Low -> High). Best Bid is back().
    // Asks: Descending (High -> Low). Best Ask is back().
    
    // So for both Buy and Sell orders, we match against the "Best" price first,
    // which means iterating from back to front (Reverse).
    
    for (auto it = opposite_levels.rbegin(); it != opposite_levels.rend(); ++it) {
        auto& levelPtr = *it; // unique_ptr<PriceLevel>&

        if (levelPtr->isEmpty() || incoming_order.isFullyFilled()) break;
        
        // Check price condition
        bool can_match = false;
        if (incoming_order.side == Side::Buy) {
            // Buy matches if BidPrice >= AskPrice (level.price)
            can_match = incoming_order.price >= levelPtr->price;
        } else {
            // Sell matches if SellPrice <= BidPrice (level.price)
            can_match = incoming_order.price <= levelPtr->price;
        }
        
        if (!can_match) break; // Prices no longer cross
        
        matchAgainstPriceLevel(incoming_order, *levelPtr);
    }
}

void OrderBook::matchAgainstPriceLevel(Order& incoming_order, PriceLevel& price_level) {
    Order* current_order = price_level.getFirstOrder();
    
    while (current_order && !incoming_order.isFullyFilled()) {
        // Save next pointer before potential removal
        Order* next_order = current_order->next;

        // Calculate trade quantity
        Quantity trade_quantity = std::min(incoming_order.remainingQuantity(),
                                          current_order->remainingQuantity());
        
        if (trade_quantity == 0) {
             // Handle ghost orders (fully filled but not removed)
             if (current_order->remainingQuantity() == 0) {
                 price_level.removeOrder(current_order);
                 auto it = order_index_.find(current_order->id);
                 if (it != order_index_.end()) order_index_.erase(it);
                 delete current_order;
                 current_order = next_order;
                 continue;
             }
             break;
        }
        
        // Execute the trade
        executeTrade(incoming_order, *current_order, price_level.price, trade_quantity);
        
        // Update price level quantity
        price_level.updateQuantity(current_order, trade_quantity);
        
        // Publish book update for the passive order modification/removal
        if (current_order->isFullyFilled()) {
            // Order fully filled - publish removal
            publishBookUpdate(BookUpdate::Type::Remove, current_order->side, 
                             current_order->price, 0, price_level.order_count);
        } else {
            // Order partially filled - publish modification
            publishBookUpdate(BookUpdate::Type::Modify, current_order->side, 
                             current_order->price, current_order->remainingQuantity(), 
                             price_level.order_count);
        }
        
        // Move to next order if current is fully filled
        if (current_order->isFullyFilled()) {
            // Remove filled order from order index
            auto it = order_index_.find(current_order->id);
            if (it != order_index_.end()) {
                order_index_.erase(it);
            }
            
            // Delete the filled order
            delete current_order;
            current_order = next_order;
        }
    }
    
    // Clean up empty price level
    if (price_level.isEmpty()) {
        Side opposite_side = (incoming_order.side == Side::Buy) ? Side::Sell : Side::Buy;
        removePriceLevel(&price_level, opposite_side);
    }
}

void OrderBook::executeTrade(Order& aggressive_order, Order& passive_order, 
                            Price trade_price, Quantity trade_quantity) {
    // Fill both orders
    aggressive_order.fill(trade_quantity);
    passive_order.fill(trade_quantity);
    
    // Create trade record
    static uint64_t trade_counter = 1;
    TradeId trade_id(trade_counter++);
    
    // Determine buy/sell order IDs
    OrderId buy_order_id = aggressive_order.isBuy() ? aggressive_order.id : passive_order.id;
    OrderId sell_order_id = aggressive_order.isSell() ? aggressive_order.id : passive_order.id;
    
    Trade trade(trade_id.value, buy_order_id, sell_order_id, 
               trade_price, trade_quantity, aggressive_order.symbol);
    
    // Update positions through risk manager
    if (risk_manager_) {
        risk_manager_->updatePosition(trade);
        
        if (logger_) {
            logger_->debug("Updated positions for trade ID: " + std::to_string(trade.id.value), 
                          "OrderBook::executeTrade");
        }
    }
    
    // Publish trade to market data
    if (market_data_) {
        market_data_->publishTrade(trade);
    }
    
    // Log trade execution with risk context
    if (logger_) {
        std::string buy_account = risk_manager_ ? risk_manager_->getAccountForOrder(trade.buy_order_id) : "unknown";
        std::string sell_account = risk_manager_ ? risk_manager_->getAccountForOrder(trade.sell_order_id) : "unknown";
        
        logger_->info("Trade executed: ID=" + std::to_string(trade.id.value) + 
                     " Buy=" + std::to_string(trade.buy_order_id.value) + " (Account: " + buy_account + ")" +
                     " Sell=" + std::to_string(trade.sell_order_id.value) + " (Account: " + sell_account + ")" +
                     " Price=" + std::to_string(trade.price) + 
                     " Qty=" + std::to_string(trade.quantity) + 
                     " Symbol=" + trade.symbol,
                     "OrderBook::executeTrade");
        
        // Log position updates
        if (risk_manager_) {
            const Portfolio& buy_portfolio = risk_manager_->getPortfolio(buy_account);
            const Portfolio& sell_portfolio = risk_manager_->getPortfolio(sell_account);
            
            logger_->debug("Position updates - Buy account " + buy_account + 
                          " new position: " + std::to_string(buy_portfolio.getPosition(trade.symbol)) +
                          ", Sell account " + sell_account + 
                          " new position: " + std::to_string(sell_portfolio.getPosition(trade.symbol)),
                          "OrderBook::executeTrade");
        }
    }
}

void OrderBook::poll() {
    MarketUpdate update;
    while (market_queue_.pop(update)) {
        if (update.type == MarketUpdate::Type::SnapshotStart) {
            // Clear book logic
            for (auto& level : bids_) {
                Order* cur = level->getFirstOrder();
                while (cur) {
                    Order* next = cur->next;
                    delete cur;
                    cur = next;
                }
            }
            for (auto& level : asks_) {
                Order* cur = level->getFirstOrder();
                while (cur) {
                    Order* next = cur->next;
                    delete cur;
                    cur = next;
                }
            }
            bids_.clear();
            asks_.clear();
            price_index_.clear();
            order_index_.clear();
            if (logger_) logger_->info("OrderBook cleared via queue", "OrderBook::poll");
            continue;
        }

        // Handle Add/Modify/Remove
        PriceLevel* level = nullptr;
        auto it = price_index_.find(update.price);
        if (it != price_index_.end()) {
            level = it->second;
        }

        if (update.type == MarketUpdate::Type::Add || update.type == MarketUpdate::Type::Modify) {
             if (!level) {
                level = findOrCreatePriceLevel(update.price, update.side);
            }
            level->total_quantity = update.quantity;
            level->order_count = update.order_count;
        } else if (update.type == MarketUpdate::Type::Remove) {
            if (level) {
                if (level->getOrderCount() == 0) {
                    removePriceLevel(level, update.side);
                } else {
                    level->total_quantity = 0;
                    level->order_count = 0;
                }
            }
        }
        
        // No sort required; maintainSortedOrder() can be expensive under heavy update load
        publishMarketDataUpdate();
    }
}

} // namespace orderbook
