#ifdef WITH_QUICKFIX
#include "orderbook/MarketData/QuickFixConnector.hpp"
#include "orderbook/MarketData/MarketDataFeed.hpp"
#include "orderbook/OrderBook.hpp"
#include <quickfix/fix44/MarketDataIncrementalRefresh.h>
#include <quickfix/fix44/SequenceReset.h>
#include <quickfix/fix44/MarketDataRequestReject.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/FileStore.h>
#include <quickfix/SocketInitiator.h>
#include <quickfix/Log.h>
#include <quickfix/FileLog.h>
#include <quickfix/MessageStore.h>
#include <quickfix/Session.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <algorithm>

namespace FIX {
    USER_DEFINE_SEQNUM(MDSeqNum, 10001);
}

static inline void trim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch){ return !std::isspace(ch); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch){ return !std::isspace(ch); }).base(), s.end());
}

namespace orderbook {

QuickFixConnector::QuickFixConnector(MarketDataPublisherPtr publisher, std::shared_ptr<Config> cfg, LoggerPtr logger, OrderBook* book)
    : publisher_(publisher), config_(cfg), logger_(logger) {
    quickfix_config_path_ = config_->getString("marketdata", "quickfix_config", "config/quickfix/quickfix.cfg");
    order_book_ = book;
}

QuickFixConnector::~QuickFixConnector() {
    stop();
}

bool QuickFixConnector::start() {
    try {
        if (logger_) logger_->info("Loading QuickFIX config manually (bypass file)", "QuickFixConnector::start");
        
        FIX::SessionSettings settings;
        FIX::Dictionary defaults;
        defaults.setString("ConnectionType", "initiator");
        defaults.setString("StartTime", "00:00:00");
        defaults.setString("EndTime", "00:00:00");
        defaults.setString("HeartBtInt", "30");
        defaults.setString("ReconnectInterval", "5");
        defaults.setString("FileStorePath", "store");
        defaults.setString("FileLogPath", "log");
        defaults.setString("UseDataDictionary", "Y");
        defaults.setString("DataDictionary", "third_party/fix/FIX44.xml");
        settings.set(defaults);

        FIX::SessionID sessionID("FIX.4.4", "ORDERBOOK", "MDPI");
        FIX::Dictionary sessionDict;
        sessionDict.setString("SocketConnectHost", "127.0.0.1");
        sessionDict.setString("SocketConnectPort", "9876");
        settings.set(sessionID, sessionDict);

        store_factory_.reset(new FIX::FileStoreFactory(settings));
        log_factory_.reset(new FIX::FileLogFactory(settings));
        initiator_.reset(new FIX::SocketInitiator(*this, *store_factory_, settings, *log_factory_));
        initiator_->start();
        running_ = true;
        if (logger_) logger_->info("QuickFIX connector started", "QuickFixConnector::start");
        return true;
    } catch (const std::exception& e) {
        if (logger_) logger_->error(std::string("Failed to start QuickFIX connector: ") + e.what(), "QuickFixConnector::start");
        return false;
    }
}

void QuickFixConnector::stop() {
    if (initiator_) {
        try {
            initiator_->stop();
        } catch (...) {}
        initiator_.reset();
    }
    running_ = false;
}

bool QuickFixConnector::isRunning() const { return running_; }

// FIX::Application callbacks
void QuickFixConnector::onCreate(const FIX::SessionID& sessionID) {
    if (logger_) logger_->info("QuickFIX session created: " + sessionID.toString(), "QuickFixConnector::onCreate");
}

void QuickFixConnector::onLogon(const FIX::SessionID& sessionID) {
    if (logger_) logger_->info("QuickFIX session logged on: " + sessionID.toString(), "QuickFixConnector::onLogon");
    // On logon, subscribe for MD for all configured symbols
    std::string symbols = config_->getString("marketdata", "symbols", "");
    if (!symbols.empty()) {
        std::istringstream ss(symbols);
        std::string symbol;
        while (std::getline(ss, symbol, ',')) {
            trim(symbol); // we'll implement trim inline; but to keep code compiling, use quick workaround
            // Build MDRequest
            FIX44::MarketDataRequest mdReq;
            FIX::MDReqID reqId("MD-" + symbol);
            mdReq.set(reqId);
            mdReq.set(FIX::SubscriptionRequestType(FIX::SubscriptionRequestType_SNAPSHOT_AND_UPDATES));
            mdReq.set(FIX::MarketDepth(1));
            // No related symbols
            FIX44::MarketDataRequest::NoRelatedSym relSym;
            relSym.set(FIX::Symbol(symbol));
            mdReq.addGroup(relSym);
            try {
                FIX::Session::sendToTarget(mdReq, sessionID);
                if (logger_) logger_->info("Sent MarketDataRequest for: " + symbol, "QuickFixConnector::onLogon");
            } catch (const std::exception& e) {
                if (logger_) logger_->error(std::string("Failed to send MarketDataRequest: ") + e.what(), "QuickFixConnector::onLogon");
            }
        }
    }
}

void QuickFixConnector::onLogout(const FIX::SessionID& sessionID) {
    if (logger_) logger_->info("QuickFIX session logged out: " + sessionID.toString(), "QuickFixConnector::onLogout");
}

void QuickFixConnector::toAdmin(FIX::Message&, const FIX::SessionID&) {}
void QuickFixConnector::toApp(FIX::Message&, const FIX::SessionID&) {}

void QuickFixConnector::fromAdmin(const FIX::Message&, const FIX::SessionID&) {}

void QuickFixConnector::fromApp(const FIX::Message& message, const FIX::SessionID& sessionID) {
    // Check message sequence for gaps. If gap detected, request resend
    FIX::MsgSeqNum msgSeqNum;
    try {
        if (message.getHeader().isSetField(msgSeqNum)) {
            message.getHeader().getField(msgSeqNum);
            long seq = msgSeqNum.getValue();
            long expected = last_msg_seq_.load() + 1;
            if (seq > expected) {
                gaps_detected_.fetch_add(1, std::memory_order_relaxed);
                if (logger_) logger_->warn("Gap detected in FIX messages: expected=" + std::to_string(expected) + " got=" + std::to_string(seq), "QuickFixConnector::fromApp");
                // Snapshot reset approach: clear internal book and request snapshot
                if (order_book_) {
                    order_book_->clearBook();
                }
                // Request snapshots for all configured symbols
                std::string symbols = config_->getString("marketdata", "symbols", "");
                if (!symbols.empty()) {
                    std::istringstream ss(symbols);
                    std::string symbol;
                    while (std::getline(ss, symbol, ',')) {
                        trim(symbol);
                        requestSnapshot(sessionID, symbol);
                    }
                }
            }
            last_msg_seq_.store(seq);
            messages_processed_.fetch_add(1, std::memory_order_relaxed);
        }
    } catch(...) {}
    crack(message, sessionID);
}

// QuickFIX message handlers for MD types
void QuickFixConnector::onMessage(const FIX44::MarketDataSnapshotFullRefresh& message, const FIX::SessionID& sessionID) {
    // Sequence checking for MDSeqNum
    FIX::MDSeqNum mdSeq;
    long seq = 0;
    try {
        if (message.isSetField(mdSeq)) {
            message.getField(mdSeq);
            seq = mdSeq.getValue();
            long last = last_msg_seq_.load();
            if (seq <= last) {
                if (logger_) logger_->debug("Ignoring duplicate/old MD snapshot seq=" + std::to_string(seq), "QuickFixConnector::onMessage");
                return;
            }
            long expected = last + 1;
            if (seq > expected) {
                gaps_detected_.fetch_add(1, std::memory_order_relaxed);
                if (logger_) logger_->warn("Gap detected in MD snapshot: expected=" + std::to_string(expected) + " got=" + std::to_string(seq), "QuickFixConnector::onMessage");
                try { /* resends disabled in favor of snapshot request */ } catch (...) {}
            }
        }
    } catch(...) {}

    // Build MarketDepth object
    MarketDepth depth;
    depth.timestamp = std::chrono::system_clock::now();

    FIX::NoMDEntries noEntriesField;
    if (message.isSetField(noEntriesField)) {
        message.get(noEntriesField);
        int noEntries = noEntriesField.getValue();
        FIX44::MarketDataSnapshotFullRefresh::NoMDEntries group;
        for (int i = 1; i <= noEntries; ++i) {
            message.getGroup(i, group);
            char mdEntryType = '0';
            FIX::MDEntryType mdType;
            FIX::MDEntryPx mdPx;
            FIX::MDEntrySize mdSize;
            FIX::NumberOfOrders mdOrders;
            if (group.isSetField(mdType)) group.get(mdType);
            if (group.isSetField(mdPx)) group.get(mdPx);
            if (group.isSetField(mdSize)) group.get(mdSize);
            if (group.isSetField(mdOrders)) group.get(mdOrders);
            mdEntryType = mdType.getValue();
            double px = mdPx.getValue();
            double size = mdSize.getValue();
            int orderCount = mdOrders.getValue();

        MarketDepth::Level level{static_cast<Price>(px), static_cast<Quantity>(size), static_cast<size_t>(orderCount)};
        if (mdEntryType == FIX::MDEntryType_BID) depth.bids.push_back(level);
        else depth.asks.push_back(level);
    }
    }

    publisher_->publishDepth(depth);
    if (seq > 0) last_msg_seq_.store(seq);
    if (order_book_ && config_->getBool("marketdata", "apply_to_book", false)) {
        order_book_->applyExternalMarketData(depth);
    }
}

void QuickFixConnector::onMessage(const FIX44::MarketDataIncrementalRefresh& message, const FIX::SessionID& sessionID) {
    // Sequence checking for MDSeqNum
    FIX::MDSeqNum mdSeq;
    long seq = 0;
    try {
        if (message.isSetField(mdSeq)) {
            message.getField(mdSeq);
            seq = mdSeq.getValue();
            long last = last_msg_seq_.load();
            if (seq <= last) {
                if (logger_) logger_->debug("Ignoring duplicate/old MD incremental seq=" + std::to_string(seq), "QuickFixConnector::onMessage");
                return;
            }
            long expected = last + 1;
            if (seq > expected) {
                gaps_detected_.fetch_add(1, std::memory_order_relaxed);
                if (logger_) logger_->warn("Gap detected in MD incremental: expected=" + std::to_string(expected) + " got=" + std::to_string(seq), "QuickFixConnector::onMessage");
                if (order_book_) {
                    order_book_->clearBook();
                }
                std::string symbols = config_->getString("marketdata", "symbols", "");
                if (!symbols.empty()) {
                    std::istringstream ss(symbols);
                    std::string symbol;
                    while (std::getline(ss, symbol, ',')) { trim(symbol); requestSnapshot(sessionID, symbol); }
                }
            }
        }
    } catch(...) {}

    FIX::NoMDEntries noEntriesInc;
    if (message.isSetField(noEntriesInc)) {
        message.get(noEntriesInc);
        int noEntries = noEntriesInc.getValue();
        FIX44::MarketDataIncrementalRefresh::NoMDEntries group;
        for (int i = 1; i <= noEntries; ++i) {
            message.getGroup(i, group);
        FIX::MDEntryType mdType;
        FIX::MDUpdateAction mdAction;
        FIX::MDEntryPx mdPx;
        FIX::MDEntrySize mdSize;
        FIX::NumberOfOrders mdOrders;
        if (group.isSetField(mdType)) group.get(mdType);
        if (group.isSetField(mdAction)) group.get(mdAction);
        if (group.isSetField(mdPx)) group.get(mdPx);
        if (group.isSetField(mdSize)) group.get(mdSize);
        if (group.isSetField(mdOrders)) group.get(mdOrders);
        char mdEntryType = mdType.getValue();
        char mdUpdateAction = mdAction.getValue();
        double px = mdPx.getValue();
        double size = mdSize.getValue();
        int orderCount = mdOrders.getValue();

        BookUpdate::Type type = BookUpdate::Type::Modify;
        if (mdUpdateAction == '0') type = BookUpdate::Type::Add;
        else if (mdUpdateAction == '1') type = BookUpdate::Type::Modify;
        else if (mdUpdateAction == '2') type = BookUpdate::Type::Remove;

        SequenceNumber seq = 0;
        BookUpdate upd(type,
                   (mdEntryType == FIX::MDEntryType_BID) ? Side::Buy : Side::Sell,
                   static_cast<Price>(px), static_cast<Quantity>(size), static_cast<size_t>(orderCount), seq);
        publisher_->publishBookUpdate(upd);
        if (order_book_ && config_->getBool("marketdata", "apply_to_book", false)) {
            order_book_->applyExternalBookUpdate(upd);
        }

        if (mdEntryType == FIX::MDEntryType_TRADE) {
            Trade trade;
            // Trivial mapping: no buy/sell order ids from MD; set zeros
            trade.price = static_cast<Price>(px);
            trade.quantity = static_cast<Quantity>(size);
            trade.id = TradeId(0);
            trade.buy_order_id = OrderId(0);
            trade.sell_order_id = OrderId(0);
            FIX::Symbol symbol;
            if (message.isSetField(symbol)) {
                message.getField(symbol);
                trade.symbol = symbol.getValue();
            }
            trade.timestamp = std::chrono::system_clock::now();
            publisher_->publishTrade(trade);
        }
    }
    }
    if (seq > 0) last_msg_seq_.store(seq);
}

void QuickFixConnector::requestSnapshot(const FIX::SessionID& sessionID, const std::string& symbol) {
    try {
        FIX44::MarketDataRequest mdReq;
        std::string reqId = std::string("SS-") + symbol + "-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        mdReq.set(FIX::MDReqID(reqId));
        mdReq.set(FIX::SubscriptionRequestType('0')); // Snapshot
        mdReq.set(FIX::MarketDepth(0));

        FIX44::MarketDataRequest::NoRelatedSym relSym;
        relSym.set(FIX::Symbol(symbol));
        mdReq.addGroup(relSym);

        FIX::Session::sendToTarget(mdReq, sessionID);
        if (logger_) logger_->info("Requested snapshot for symbol: " + symbol, "QuickFixConnector::requestSnapshot");
    } catch (const std::exception& e) {
        if (logger_) logger_->error(std::string("Failed to request snapshot for symbol: ") + symbol + " - " + e.what(), "QuickFixConnector::requestSnapshot");
    }
}

void QuickFixConnector::onMessage(const FIX44::SequenceReset& message, const FIX::SessionID& sessionID) {
    // SequenceReset can indicate a new sequence number; update last_msg_seq_
    FIX::NewSeqNo newSeq;
    try {
        if (message.isSetField(newSeq)) {
            message.get(newSeq);
            long newSeqVal = newSeq.getValue();
            last_msg_seq_.store(newSeqVal - 1); // Next expected is newSeqVal
            if (logger_) logger_->info("SequenceReset received. New sequence: " + std::to_string(newSeqVal), "QuickFixConnector::onMessage");
        }
    } catch (...) {}
}

void QuickFixConnector::onMessage(const FIX44::MarketDataRequestReject& message, const FIX::SessionID& sessionID) {
    try {
        FIX::MDReqID mdReq;
        FIX::Text reason;
        if (message.isSetField(mdReq)) {
            message.get(mdReq);
        }
        if (message.isSetField(reason)) {
            message.get(reason);
        }
        if (logger_) logger_->warn("MarketDataRequestReject for " + mdReq.getValue() + " reason: " + reason.getValue(), "QuickFixConnector::onMessage");
    } catch (...) {}
}

// Expose stats through getters (small wrapper implementation)
// getStats implemented inline via header getStats method

} // namespace orderbook
#endif // WITH_QUICKFIX
