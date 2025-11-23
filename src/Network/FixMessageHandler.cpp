#include "orderbook/Network/FixMessageHandler.hpp"
#include "orderbook/Network/FixConstants.hpp"
#include <sstream>
#include <iomanip>
#include <mutex>
#include <optional>

namespace orderbook {

using namespace fix;

FixMessageHandler::FixMessageHandler(std::shared_ptr<OrderManager> orderManager,
                                   std::shared_ptr<RiskManager> riskManager)
    : orderManager_(std::move(orderManager)), riskManager_(std::move(riskManager)) {
}

// Mapping helpers - thread-safe
void FixMessageHandler::addClOrdIdMapping(const std::string& clOrdId, OrderId orderId) {
    std::lock_guard<std::mutex> lock(mapping_mutex_);
    clOrdIdToOrderId_[clOrdId] = orderId;
    orderIdToClOrdId_[orderId] = clOrdId;
}

void FixMessageHandler::removeClOrdIdMapping(const std::string& clOrdId) {
    std::lock_guard<std::mutex> lock(mapping_mutex_);
    auto it = clOrdIdToOrderId_.find(clOrdId);
    if (it != clOrdIdToOrderId_.end()) {
        OrderId oid = it->second;
        clOrdIdToOrderId_.erase(it);
        orderIdToClOrdId_.erase(oid);
    }
}

void FixMessageHandler::removeOrderIdMapping(OrderId orderId) {
    std::lock_guard<std::mutex> lock(mapping_mutex_);
    auto it = orderIdToClOrdId_.find(orderId);
    if (it != orderIdToClOrdId_.end()) {
        std::string clOrd = it->second;
        orderIdToClOrdId_.erase(it);
        clOrdIdToOrderId_.erase(clOrd);
    }
}

std::optional<OrderId> FixMessageHandler::getOrderIdForClOrdId(const std::string& clOrdId) {
    std::lock_guard<std::mutex> lock(mapping_mutex_);
    auto it = clOrdIdToOrderId_.find(clOrdId);
    if (it != clOrdIdToOrderId_.end()) return it->second;
    return std::nullopt;
}

std::optional<std::string> FixMessageHandler::getClOrdIdForOrderId(OrderId orderId) {
    std::lock_guard<std::mutex> lock(mapping_mutex_);
    auto it = orderIdToClOrdId_.find(orderId);
    if (it != orderIdToClOrdId_.end()) return it->second;
    return std::nullopt;
}

void FixMessageHandler::setFixSession(std::shared_ptr<FixSession> session) {
    fixSession_ = std::move(session);
}

void FixMessageHandler::handleNewOrderSingle(const FixMessageParser::NewOrderSingle& newOrder) {
    ++ordersProcessed_;
    
    if (!newOrder.isValid) {
        sendRejectionReport(newOrder.clOrdId, newOrder.symbol, newOrder.side, 
                          "Invalid order format: " + newOrder.errorMessage);
        ++ordersRejected_;
        return;
    }
    
    // Convert to internal order
    auto order = convertToInternalOrder(newOrder);
    if (!order) {
        sendRejectionReport(newOrder.clOrdId, newOrder.symbol, newOrder.side, 
                          "Failed to create internal order");
        ++ordersRejected_;
        return;
    }
    
    // Risk validation
    if (riskManager_) {
        // Create a simple portfolio for risk checking (in production, would be more sophisticated)
        Portfolio portfolio(newOrder.account.empty() ? "DEFAULT" : newOrder.account);
        auto riskCheck = riskManager_->validateOrder(*order, portfolio);
        
        if (riskCheck.result == RiskResult::Rejected) {
            sendRejectionReport(newOrder.clOrdId, newOrder.symbol, newOrder.side, 
                              "Risk check failed: " + riskCheck.reason);
            ++ordersRejected_;
            return;
        }
    }
    
    // Submit order to order manager
    auto result = orderManager_->addOrder(std::move(order));
    
    if (result.isError()) {
        // Order was rejected by order manager
        sendRejectionReport(newOrder.clOrdId, newOrder.symbol, newOrder.side, 
                          "Order rejected: " + result.error());
        // Clean up mappings using client order id (safe)
        ++ordersRejected_;
    } else {
        // Add client order ID mapping after successful add
        addClOrdIdMapping(newOrder.clOrdId, result.value());
        // Retrieve stored order and send acknowledgment (New execution report) with client ClOrdID
        Order* storedOrder = orderManager_->getOrder(result.value());
        if (storedOrder) {
            sendExecutionReport(*storedOrder, newOrder.clOrdId, EXEC_TYPE_NEW);
        }
    }
}

void FixMessageHandler::handleOrderCancelReplaceRequest(const FixMessageParser::OrderCancelReplaceRequest& cancelReplace) {
    if (!cancelReplace.isValid) {
        // Send reject for invalid message
        if (fixSession_) {
            fixSession_->sendReject(0, "Invalid cancel replace request: " + cancelReplace.errorMessage);
        }
        return;
    }
    
    // Find original order (thread-safe)
    auto maybeOrderId = getOrderIdForClOrdId(cancelReplace.origClOrdId);
    if (!maybeOrderId.has_value()) {
        sendRejectionReport(cancelReplace.clOrdId, cancelReplace.symbol, cancelReplace.side, 
                          "Original order not found: " + cancelReplace.origClOrdId);
        return;
    }
    
    OrderId originalOrderId = maybeOrderId.value();
    
    // Modify the order
    auto result = orderManager_->modifyOrder(originalOrderId, cancelReplace.price, cancelReplace.quantity);
    
    if (result.isSuccess()) {
        // Update client order ID mapping (thread-safe)
        removeClOrdIdMapping(cancelReplace.origClOrdId);
        addClOrdIdMapping(cancelReplace.clOrdId, originalOrderId);
        
        // Find the modified order and send execution report
        Order* modifiedOrder = findOrderByClOrdId(cancelReplace.clOrdId);
        if (modifiedOrder) {
            sendExecutionReport(*modifiedOrder, EXEC_TYPE_NEW); // Modified order treated as new
        }
    } else {
        sendRejectionReport(cancelReplace.clOrdId, cancelReplace.symbol, cancelReplace.side, 
                          "Modify failed: " + result.error());
    }
}

void FixMessageHandler::handleOrderCancelRequest(const FixMessageParser::OrderCancelRequest& cancelRequest) {
    if (!cancelRequest.isValid) {
        // Send reject for invalid message
        if (fixSession_) {
            fixSession_->sendReject(0, "Invalid cancel request: " + cancelRequest.errorMessage);
        }
        return;
    }
    
    // Find original order (thread-safe)
    auto maybeOrderId = getOrderIdForClOrdId(cancelRequest.origClOrdId);
    if (!maybeOrderId.has_value()) {
        sendRejectionReport(cancelRequest.clOrdId, cancelRequest.symbol, cancelRequest.side, 
                          "Original order not found: " + cancelRequest.origClOrdId);
        return;
    }
    OrderId originalOrderId = maybeOrderId.value();
    
    // Cancel the order
    auto result = orderManager_->cancelOrder(originalOrderId);
    
    if (result.isSuccess()) {
        // Update client order ID mapping for the cancel confirmation (thread-safe)
        removeClOrdIdMapping(cancelRequest.origClOrdId);
        addClOrdIdMapping(cancelRequest.clOrdId, originalOrderId);
        
        // Find the cancelled order and send execution report
        Order* cancelledOrder = findOrderByClOrdId(cancelRequest.clOrdId);
        if (cancelledOrder) {
            sendExecutionReport(*cancelledOrder, EXEC_TYPE_CANCELLED);
        }
    } else {
        sendRejectionReport(cancelRequest.clOrdId, cancelRequest.symbol, cancelRequest.side, 
                          "Cancel failed: " + result.error());
    }
}

void FixMessageHandler::handleTradeExecution(const Trade& trade) {
    ++tradesReported_;
    
    // Send execution reports for both buy and sell orders
    Order* buyOrder = nullptr;
    Order* sellOrder = nullptr;
    
    // Find orders by their IDs
    auto maybeBuyClOrd = getClOrdIdForOrderId(trade.buy_order_id);
    auto maybeSellClOrd = getClOrdIdForOrderId(trade.sell_order_id);

    if (maybeBuyClOrd.has_value()) {
        buyOrder = findOrderByClOrdId(maybeBuyClOrd.value());
    }

    if (maybeSellClOrd.has_value()) {
        sellOrder = findOrderByClOrdId(maybeSellClOrd.value());
    }
    
    // Send trade execution reports
    if (buyOrder) {
        sendTradeExecutionReport(trade, *buyOrder);
    }
    
    if (sellOrder) {
        sendTradeExecutionReport(trade, *sellOrder);
    }
}

void FixMessageHandler::handleOrderStatusChange(const Order& order) {
    // Send execution report for status changes (e.g., cancelled, rejected)
    char execType = EXEC_TYPE_NEW;
    
    switch (order.status) {
        case OrderStatus::Cancelled:
            execType = EXEC_TYPE_CANCELLED;
            break;
        case OrderStatus::Rejected:
            execType = EXEC_TYPE_REJECTED;
            break;
        case OrderStatus::Filled:
            execType = EXEC_TYPE_FILL;
            break;
        case OrderStatus::PartiallyFilled:
            execType = EXEC_TYPE_PARTIAL_FILL;
            break;
        default:
            execType = EXEC_TYPE_NEW;
            break;
    }
    
    sendExecutionReport(order, execType);
}

std::string FixMessageHandler::generateExecutionId() {
    std::ostringstream oss;
    oss << "EXEC" << std::setfill('0') << std::setw(10) << executionIdCounter_++;
    return oss.str();
}

std::unique_ptr<Order> FixMessageHandler::convertToInternalOrder(const FixMessageParser::NewOrderSingle& newOrder) {
    try {
        // Generate internal order ID (in production, would use a proper ID generator)
        static std::atomic<uint64_t> orderIdCounter{1};
        uint64_t orderId = orderIdCounter++;
        
        auto order = std::make_unique<Order>(
            orderId,
            newOrder.side,
            newOrder.orderType,
            newOrder.timeInForce,
            newOrder.price,
            newOrder.quantity,
            newOrder.symbol.c_str(),
            newOrder.account.c_str()
        );
        
        return order;
        
    } catch (const std::exception&) {
        return nullptr;
    }
}

void FixMessageHandler::sendExecutionReport(const Order& order, char execType, 
                                          Quantity lastQty, Price lastPx) {
    if (!fixSession_ || !fixSession_->isLoggedIn()) {
        return;
    }
    
    // Thread-safe lookup of client order ID
    auto maybeClOrdId = getClOrdIdForOrderId(order.id);
    if (!maybeClOrdId.has_value()) {
        return; // No client order ID found
    }
    
    FixMessageParser::ExecutionReport execReport;
    execReport.orderId = std::to_string(order.id.value);
    execReport.clOrdId = maybeClOrdId.value();
    execReport.execId = generateExecutionId();
    execReport.execType = execType;
    execReport.ordStatus = orderStatusToFixChar(order.status);
    execReport.symbol = order.symbol;
    execReport.side = order.side;
    execReport.orderQty = order.quantity;
    execReport.price = order.price;
    execReport.lastQty = lastQty;
    execReport.lastPx = lastPx;
    execReport.leavesQty = order.remainingQuantity();
    execReport.cumQty = order.filled_quantity;
    execReport.avgPx = order.price; // Simplified - in production would calculate actual average
    execReport.transactTime = std::chrono::system_clock::now();
    
    fixSession_->sendExecutionReport(execReport);
}

// Overload: send execution report using explicit clOrdId (no lookup)
void FixMessageHandler::sendExecutionReport(const Order& order, const std::string& clOrdId, char execType, 
                                          Quantity lastQty, Price lastPx) {
    if (!fixSession_ || !fixSession_->isLoggedIn()) {
        return;
    }

    FixMessageParser::ExecutionReport execReport;
    execReport.orderId = std::to_string(order.id.value);
    execReport.clOrdId = clOrdId;
    execReport.execId = generateExecutionId();
    execReport.execType = execType;
    execReport.ordStatus = orderStatusToFixChar(order.status);
    execReport.symbol = order.symbol;
    execReport.side = order.side;
    execReport.orderQty = order.quantity;
    execReport.price = order.price;
    execReport.lastQty = lastQty;
    execReport.lastPx = lastPx;
    execReport.leavesQty = order.remainingQuantity();
    execReport.cumQty = order.filled_quantity;
    execReport.avgPx = order.price; // Simplified
    execReport.transactTime = std::chrono::system_clock::now();

    fixSession_->sendExecutionReport(execReport);
}

void FixMessageHandler::sendTradeExecutionReport(const Trade& trade, const Order& order) {
    char execType = order.isFullyFilled() ? EXEC_TYPE_FILL : EXEC_TYPE_PARTIAL_FILL;
    sendExecutionReport(order, execType, trade.quantity, trade.price);
}

void FixMessageHandler::sendRejectionReport(const std::string& clOrdId, const std::string& symbol, 
                                          Side side, const std::string& reason) {
    if (!fixSession_ || !fixSession_->isLoggedIn()) {
        return;
    }
    
    FixMessageParser::ExecutionReport execReport;
    execReport.orderId = "0"; // No internal order ID for rejected orders
    execReport.clOrdId = clOrdId;
    execReport.execId = generateExecutionId();
    execReport.execType = EXEC_TYPE_REJECTED;
    execReport.ordStatus = ORD_STATUS_REJECTED;
    execReport.symbol = symbol;
    execReport.side = side;
    execReport.orderQty = 0;
    execReport.price = 0.0;
    execReport.lastQty = 0;
    execReport.lastPx = 0.0;
    execReport.leavesQty = 0;
    execReport.cumQty = 0;
    execReport.avgPx = 0.0;
    execReport.transactTime = std::chrono::system_clock::now();
    
    fixSession_->sendExecutionReport(execReport);
}

Order* FixMessageHandler::findOrderByClOrdId(const std::string& clOrdId) {
    OrderId oid;
    {
        std::lock_guard<std::mutex> lock(mapping_mutex_);
        auto it = clOrdIdToOrderId_.find(clOrdId);
        if (it == clOrdIdToOrderId_.end()) {
            return nullptr;
        }
        oid = it->second;
    }

    // Get order from order manager (do not hold mapping mutex while handing control out)
    return orderManager_->getOrder(oid);
}

// Helper function to convert OrderStatus to FIX character
char FixMessageHandler::orderStatusToFixChar(OrderStatus status) {
    switch (status) {
        case OrderStatus::New: return ORD_STATUS_NEW;
        case OrderStatus::PartiallyFilled: return ORD_STATUS_PARTIALLY_FILLED;
        case OrderStatus::Filled: return ORD_STATUS_FILLED;
        case OrderStatus::Cancelled: return ORD_STATUS_CANCELLED;
        case OrderStatus::Rejected: return ORD_STATUS_REJECTED;
        default: return ORD_STATUS_NEW;
    }
}

}