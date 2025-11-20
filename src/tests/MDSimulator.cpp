#ifdef WITH_QUICKFIX
#include <quickfix/Application.h>
#include <quickfix/MessageCracker.h>
#include <quickfix/SocketAcceptor.h>
#include <quickfix/ThreadedSocketAcceptor.h>
#include <quickfix/FileStore.h>
#include <quickfix/FileLog.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/Log.h>
#include <quickfix/Values.h>
#include <quickfix/fix44/MarketDataSnapshotFullRefresh.h>
#include <quickfix/fix44/MarketDataIncrementalRefresh.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <random>

namespace FIX {
    USER_DEFINE_SEQNUM(MDSeqNum, 10001);
}

class MDSimulator : public FIX::Application, public FIX::MessageCracker {
public:
    MDSimulator() : running_(true), md_seq_counter_(0) {}
    void onCreate(const FIX::SessionID&) override {}
    void onLogon(const FIX::SessionID& sessionID) override { sessionId_ = sessionID; onLogonActions(); }
    void onLogout(const FIX::SessionID&) override {}
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message&, const FIX::SessionID&) override {}
    void fromApp(const FIX::Message& message, const FIX::SessionID& sessionID) override { crack(message, sessionID); }

    void onLogonActions() {
        // Send a full snapshot for a simple symbol
        std::cout << "MDSimulator: sending snapshot..." << std::endl;
        FIX44::MarketDataSnapshotFullRefresh snapshot;
        snapshot.setField(FIX::MDSeqNum(++md_seq_counter_));
        snapshot.set(FIX::Symbol("BTC/USD"));
        // Add two bid levels
        FIX44::MarketDataSnapshotFullRefresh::NoMDEntries bid1;
        bid1.set(FIX::MDEntryType(FIX::MDEntryType_BID));
        bid1.set(FIX::MDEntryPx(99.00));
        bid1.set(FIX::MDEntrySize(1000.0));
        bid1.set(FIX::NumberOfOrders(1));
        snapshot.addGroup(bid1);
        FIX44::MarketDataSnapshotFullRefresh::NoMDEntries ask1;
        ask1.set(FIX::MDEntryType(FIX::MDEntryType_OFFER));
        ask1.set(FIX::MDEntryPx(101.00));
        ask1.set(FIX::MDEntrySize(800.0));
        ask1.set(FIX::NumberOfOrders(1));
        snapshot.addGroup(ask1);

        try {
            FIX::Session::sendToTarget(snapshot, sessionId_);
        } catch (std::exception& ex) {
            std::cerr << "Failed to send snapshot: " << ex.what() << std::endl;
        }

        // Start sending incremental updates
        std::thread t([this]() { this->sendIncrementals(); });
        t.detach();
    }

    void sendIncrementals() {
        std::mt19937_64 rng(123);
        std::uniform_int_distribution<int> sideDist(0,1);
        std::uniform_real_distribution<double> priceDist(99.0, 101.0);
        std::uniform_int_distribution<int> qtyDist(50, 500);
        while (running_) {
            FIX44::MarketDataIncrementalRefresh inc;
            inc.setField(FIX::MDSeqNum(++md_seq_counter_));
            FIX44::MarketDataIncrementalRefresh::NoMDEntries entry;
            int side = sideDist(rng);
            entry.set(FIX::MDEntryType(side == 0 ? FIX::MDEntryType_BID : FIX::MDEntryType_OFFER));
            double price = priceDist(rng);
            entry.set(FIX::MDEntryPx(price));
            entry.set(FIX::MDEntrySize(static_cast<double>(qtyDist(rng))));
            entry.set(FIX::MDUpdateAction(FIX::MDUpdateAction_CHANGE));
            inc.addGroup(entry);
            try { FIX::Session::sendToTarget(inc, sessionId_); } catch(...) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void stop() { running_ = false; }

private:
    FIX::SessionID sessionId_;
    std::atomic<bool> running_;
    int md_seq_counter_;
};

int main(int argc, char* argv[]) {
    try {
        std::cout << "Loading QuickFIX config manually (bypass file)" << std::endl;
        FIX::SessionSettings settings;
        FIX::Dictionary defaults;
        defaults.setString("ConnectionType", "acceptor");
        defaults.setString("StartTime", "00:00:00");
        defaults.setString("EndTime", "00:00:00");
        defaults.setString("HeartBtInt", "30");
        defaults.setString("ReconnectInterval", "5");
        defaults.setString("FileStorePath", "store");
        defaults.setString("FileLogPath", "log");
        defaults.setString("UseDataDictionary", "Y");
        defaults.setString("DataDictionary", "third_party/fix/FIX44.xml");
        settings.set(defaults);

        FIX::SessionID sessionID("FIX.4.4", "MDPI", "ORDERBOOK");
        FIX::Dictionary sessionDict;
        sessionDict.setString("SocketAcceptPort", "9876");
        settings.set(sessionID, sessionDict);

        FIX::FileStoreFactory storeFactory(settings);
        FIX::FileLogFactory logFactory(settings);
        MDSimulator app;
        FIX::ThreadedSocketAcceptor acceptor(app, storeFactory, settings, logFactory);
        acceptor.start();
        std::cout << "MDSimulator running. Press Ctrl+C to stop." << std::endl;
        std::this_thread::sleep_for(std::chrono::hours(24));
        acceptor.stop();
    } catch (const std::exception& e) {
        std::cerr << "Error in MDSimulator: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
#else
int main() { return 0; }
#endif
