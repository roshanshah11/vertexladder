#pragma once
#include "orderbook/MarketData/IMarketDataConnector.hpp"
#include "orderbook/Core/Interfaces.hpp"
#include "orderbook/Utilities/Logger.hpp"
#include "orderbook/Utilities/Config.hpp"

#ifdef WITH_QUICKFIX
#include <quickfix/Application.h>
#include <quickfix/MessageCracker.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/SocketInitiator.h>
#include <quickfix/FileStore.h>
#include <quickfix/Log.h>
#include <quickfix/fix44/MarketDataSnapshotFullRefresh.h>
#include <quickfix/fix44/MarketDataIncrementalRefresh.h>
#include <quickfix/fix44/MarketDataRequest.h>
#include <quickfix/fix44/SecurityList.h>
#include <quickfix/fix44/SecurityListRequest.h>
#include <memory>
#include <string>

namespace orderbook {

class QuickFixConnector : public IMarketDataConnector,
                         public FIX::Application,
                         public FIX::MessageCracker {
public:
    QuickFixConnector(MarketDataPublisherPtr publisher,
                     std::shared_ptr<Config> cfg,
                     LoggerPtr logger,
                     class OrderBook* book = nullptr);
    ~QuickFixConnector();

    bool start() override;
    void stop() override;
    bool isRunning() const override;
    std::string name() const override { return "QuickFixConnector"; }
    struct Stats { uint64_t messagesProcessed; uint64_t gapsDetected; };
    Stats getStats() const { return { messages_processed_.load(), gaps_detected_.load() }; }

    // FIX::Application callbacks
    void onCreate(const FIX::SessionID& sessionID) override;
    void onLogon(const FIX::SessionID& sessionID) override;
    void onLogout(const FIX::SessionID& sessionID) override;
    void toAdmin(FIX::Message&, const FIX::SessionID&) override;
    void toApp(FIX::Message&, const FIX::SessionID&) override;
    void fromAdmin(const FIX::Message&, const FIX::SessionID&) override;
    void fromApp(const FIX::Message& message, const FIX::SessionID& sessionID) override;

    // MessageCracker handlers for MD messages
    void onMessage(const FIX44::MarketDataSnapshotFullRefresh& message, const FIX::SessionID& sessionID) override;
    void onMessage(const FIX44::MarketDataIncrementalRefresh& message, const FIX::SessionID& sessionID) override;
    void onMessage(const FIX44::SequenceReset& message, const FIX::SessionID& sessionID) override;
    void onMessage(const FIX44::MarketDataRequestReject& message, const FIX::SessionID& sessionID) override;
    void onMessage(const FIX44::SecurityList& message, const FIX::SessionID& sessionID) override;

private:
    MarketDataPublisherPtr publisher_;
    std::shared_ptr<Config> config_;
    LoggerPtr logger_;
    class OrderBook* order_book_ = nullptr;
    std::unique_ptr<FIX::SocketInitiator> initiator_;
    bool running_ = false;
    std::string session_file_;
    std::string quickfix_config_path_;
    std::unique_ptr<FIX::MessageStoreFactory> store_factory_;
    std::unique_ptr<FIX::LogFactory> log_factory_;
    std::atomic<long> last_msg_seq_{0};
    std::atomic<uint64_t> messages_processed_{0};
    std::atomic<uint64_t> gaps_detected_{0};
    FIX::SessionSettings settings_;
    void requestSnapshot(const FIX::SessionID& sessionID, const std::string& symbol);
};

} // namespace orderbook
#endif // WITH_QUICKFIX
