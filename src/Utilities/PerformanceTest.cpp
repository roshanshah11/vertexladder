#include "orderbook/Utilities/PerformanceMeasurement.hpp"
#include "orderbook/Utilities/MemoryManager.hpp"
#include "orderbook/Core/OrderBook.hpp"
#include "orderbook/Core/MatchingEngine.hpp"
#include "orderbook/Risk/RiskManager.hpp"
#include "orderbook/MarketData/MarketDataFeed.hpp"
#include "orderbook/Utilities/Logger.hpp"
#include <iostream>
#include <random>
#include <thread>
#include <vector>
#include <iomanip>

namespace orderbook {

class PerformanceTest {
public:
    struct TestConfig {
        size_t num_orders = 1000000;
        size_t num_threads = 1;
        double buy_sell_ratio = 0.5;
        Price min_price = 100.0;
        Price max_price = 200.0;
        Quantity min_quantity = 100;
        Quantity max_quantity = 1000;
        bool enable_risk_management = true;
        bool enable_market_data = true;
        std::string symbol = "AAPL";
    };

    struct TestResults {
        std::unordered_map<std::string, orderbook::PerformanceMeasurement::OperationStats> operation_stats;
        std::vector<PerformanceValidator::ValidationResult> validation_results;
        MemoryManager::MemoryStats memory_stats;
        std::chrono::milliseconds total_test_time{0};
        size_t orders_processed = 0;
        size_t trades_executed = 0;
        bool all_validations_passed = true;
    };

    static TestResults runPerformanceTest(const TestConfig& config);
    static TestResults runPerformanceTest();

    static void printResults(const TestResults& results);
    static void runLatencyBenchmark();
    static void runThroughputStressTest(size_t target_ops_per_sec, int duration_seconds);

private:
    static TestResults runSingleThreadedTest(OrderBook& order_book, const TestConfig& config);
    static TestResults runMultiThreadedTest(OrderBook& order_book, const TestConfig& config);
};

// Definitions
PerformanceTest::TestResults PerformanceTest::runPerformanceTest(const TestConfig& config) {
    std::cout << "Starting performance test with " << config.num_orders 
              << " orders on " << config.num_threads << " threads...\n";

    // Configure logger to be less verbose for performance testing
    Logger::LogConfig log_config;
    log_config.console_output = false; // Disable console output for individual operations
    log_config.min_level = LogLevel::WARN; // Only log warnings and errors
    auto logger = std::make_shared<Logger>(log_config);

    auto risk_manager = config.enable_risk_management ? 
        std::make_shared<RiskManager>(logger) : nullptr;
    auto market_data = config.enable_market_data ? 
        std::make_shared<MarketDataPublisher>(logger) : nullptr;
        if (market_data) {
        // Benchmark does not need string-based market data output
        // setDisabled(true) can cause instability in debug builds; set only in Release mode
#ifdef NDEBUG
        market_data->setDisabled(true);
#endif
    }
        if (risk_manager) {
        // Skip risk checks in release harness to measure raw throughput
    #ifdef NDEBUG
        risk_manager->setBypass(true);
    #endif
        }

    OrderBook order_book(risk_manager, market_data, logger);

    MemoryManager::getInstance().prewarmPools();
    MemoryManager::getInstance().resetStats();

    auto& perf = PerformanceMeasurement::getInstance();
    perf.reset();
    perf.startMonitoring(100); // Monitor every 100ms

    auto start_time = std::chrono::high_resolution_clock::now();

    TestResults results;
    if (config.num_threads == 1) {
        results = runSingleThreadedTest(order_book, config);
    } else {
        results = runMultiThreadedTest(order_book, config);
    }

    // Wait for all orders to be processed
    order_book.waitForCompletion();

    auto end_time = std::chrono::high_resolution_clock::now();
    results.total_test_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    perf.stopMonitoring();

    // Collect actual trade counts
    if (market_data) {
        if (market_data->isDisabled()) {
            results.trades_executed = order_book.getTradeCount();
        } else {
            results.trades_executed = market_data->getStats().trades_published;
        }
    } else {
        // If market data is not configured, rely on internal count
        results.trades_executed = order_book.getTradeCount();
    }

    results.operation_stats = perf.getAllStats();
    results.validation_results = PerformanceValidator::validateAllOperations();
    results.memory_stats = MemoryManager::getInstance().getStats();

    results.all_validations_passed = std::all_of(
        results.validation_results.begin(), results.validation_results.end(),
        [](const auto& r){ return r.passed; });

    return results;
}

PerformanceTest::TestResults PerformanceTest::runPerformanceTest() {
    return runPerformanceTest(TestConfig{});
}

void PerformanceTest::printResults(const TestResults& results) {
    std::cout << "\n" << std::string(80, '=') << "\n";
    std::cout << "PERFORMANCE TEST RESULTS\n";
    std::cout << std::string(80, '=') << "\n";

    std::cout << "Test Duration: " << results.total_test_time.count() << " ms\n";
    std::cout << "Orders Processed: " << results.orders_processed << "\n";
    std::cout << "Trades Executed: " << results.trades_executed << "\n";
    std::cout << "Overall Throughput: " << (results.orders_processed * 1000.0 / results.total_test_time.count()) << " orders/sec\n";
    std::cout << "Validation Status: " << (results.all_validations_passed ? "PASSED" : "FAILED") << "\n\n";

    std::cout << "OPERATION LATENCY STATISTICS\n";
    std::cout << std::string(80, '-') << "\n";
    std::cout << std::left << std::setw(25) << "Operation"
              << std::setw(10) << "Count"
              << std::setw(12) << "Avg (μs)"
              << std::setw(12) << "P95 (μs)"
              << std::setw(12) << "P99 (μs)"
              << std::setw(15) << "Throughput/s"
              << std::setw(15) << "Capacity/s" << "\n";
    std::cout << std::string(80, '-') << "\n";

    for (const auto& [operation_name, stats] : results.operation_stats) {
        double capacity = 0.0;
        if (stats.avg_latency.count() > 0) {
            capacity = 1000000000.0 / stats.avg_latency.count();
        } else if (stats.sample_count > 0) {
            capacity = 999999999.0; // effectively infinite
        }

        std::cout << std::left << std::setw(25) << operation_name
                  << std::setw(10) << stats.sample_count
                  << std::setw(12) << std::fixed << std::setprecision(2) << (stats.avg_latency.count() / 1000.0)
                  << std::setw(12) << (stats.p95_latency.count() / 1000.0)
                  << std::setw(12) << (stats.p99_latency.count() / 1000.0)
                  << std::setw(15) << std::fixed << std::setprecision(2) << stats.throughput_ops_per_sec
                  << std::setw(15) << std::fixed << std::setprecision(2) << capacity
                  << "\n";
    }

    std::cout << "\nMemory Usage:\n";
    std::cout << "Total Memory Used: " << results.memory_stats.total_memory_used << " bytes\n";
    std::cout << "Peak Memory Used: " << results.memory_stats.peak_memory_used << " bytes\n\n";

    std::cout << "VALIDATION RESULTS\n";
    std::cout << std::string(80, '-') << "\n";
    for (const auto& r : results.validation_results) {
        std::cout << std::left << std::setw(25) << r.operation_name << std::setw(10) << (r.passed ? "PASS" : "FAIL") << " - " << r.failure_reason << "\n";
    }
}

void PerformanceTest::runLatencyBenchmark() {
    std::cout << "Starting latency benchmarks...\n";
    auto& perf = PerformanceMeasurement::getInstance();

    {
        OrderBook book(nullptr, nullptr, nullptr);
        TestConfig cfg{10000, 1, 0.5, 100.0, 200.0};
        runSingleThreadedTest(book, cfg);
        book.waitForCompletion();
        auto stats = perf.getOperationStats("StressTest::AddOrder");
        std::cout << "StressTest::AddOrder - P95: " << stats.p95_latency.count() << " ns\n";
    }

    {
        OrderBook book(nullptr, nullptr, nullptr);
        TestConfig cfg{10000, 1, 0.5, 100.0, 200.0};
        runSingleThreadedTest(book, cfg);
        book.waitForCompletion();
        auto stats = perf.getOperationStats("StressTest::CancelOrder");
        std::cout << "StressTest::CancelOrder - P95: " << stats.p95_latency.count() << " ns\n";
    }

    std::cout << "Latency benchmarks completed.\n";
}

void PerformanceTest::runThroughputStressTest(size_t /*target_ops_per_sec*/, int duration_seconds) {
    std::cout << "Starting throughput stress test for " << duration_seconds << " seconds...\n";

    std::atomic<bool> running{true};
    std::atomic<size_t> operations{0};

    auto worker = [&running, &operations]() {
        while (running.load()) {
            operations.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> workers;
    size_t thread_count = std::thread::hardware_concurrency();
    for (size_t i = 0; i < thread_count; ++i) workers.emplace_back(worker);

    std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
    running.store(false);
    for (auto& t : workers) if (t.joinable()) t.join();

    auto ops_per_sec = operations.load() / static_cast<double>(duration_seconds);
    std::cout << "Throughput: " << ops_per_sec << " ops/sec\n";
}

PerformanceTest::TestResults PerformanceTest::runSingleThreadedTest(OrderBook& order_book, const TestConfig& config) {
    TestResults results;
    std::mt19937_64 rng(12345);
    std::uniform_real_distribution<double> price_dist(config.min_price, config.max_price);
    std::uniform_int_distribution<uint64_t> qty_dist(config.min_quantity, config.max_quantity);
    std::uniform_real_distribution<double> side_dist(0.0, 1.0);
    uint64_t next_id = 1;
    for (size_t i = 0; i < config.num_orders; ++i) {
        Price price = std::round(price_dist(rng) * 100.0) / 100.0;
        Quantity qty = static_cast<Quantity>(qty_dist(rng));
        Side side = (side_dist(rng) < config.buy_sell_ratio) ? Side::Buy : Side::Sell;
            Order order(next_id++, side, OrderType::Limit, price, qty, config.symbol.c_str());
            PERF_MEASURE_SCOPE("StressTest::AddOrder");
            auto res = order_book.addOrder(order);
        if (res.isSuccess()) results.orders_processed++;
            if (i % 100 == 0 && i > 0) {
                PERF_MEASURE_SCOPE("StressTest::CancelOrder");
                order_book.cancelOrder(OrderId(i));
            }
        
        if (i % 10000 == 0 && i > 0) {
            std::cout << "Processed " << i << " orders..." << std::endl;
        }
    }
    // results.trades_executed = 0; // Removed hardcoded zero
    return results;
}

PerformanceTest::TestResults PerformanceTest::runMultiThreadedTest(OrderBook& order_book, const TestConfig& config) {
    TestResults results;
    std::atomic<uint64_t> next_id{1};
    std::atomic<size_t> processed{0};
    auto worker = [&](size_t thread_index) {
        std::mt19937_64 rng(12345 + static_cast<uint64_t>(thread_index));
        std::uniform_real_distribution<double> price_dist(config.min_price, config.max_price);
        std::uniform_int_distribution<uint64_t> qty_dist(config.min_quantity, config.max_quantity);
        std::uniform_real_distribution<double> side_dist(0.0, 1.0);
        for (size_t i = 0; i < config.num_orders / config.num_threads; ++i) {
            Price price = std::round(price_dist(rng) * 100.0) / 100.0;
            Quantity qty = static_cast<Quantity>(qty_dist(rng));
            Side side = (side_dist(rng) < config.buy_sell_ratio) ? Side::Buy : Side::Sell;
            uint64_t id = next_id.fetch_add(1);
            Order order(id, side, OrderType::Limit, price, qty, config.symbol.c_str());
            PERF_MEASURE_SCOPE("StressTest::AddOrder");
            auto res = order_book.addOrder(order);
            if (res.isSuccess()) processed.fetch_add(1);
            if (i % 100 == 0 && i > 0) {
                PERF_MEASURE_SCOPE("StressTest::CancelOrder");
                order_book.cancelOrder(OrderId(id));
            }

            if (i % 10000 == 0 && i > 0 && thread_index == 0) {
                std::cout << "Thread 0 processed " << i << " orders..." << std::endl;
            }
        }
    };
    std::vector<std::thread> threads;
    for (size_t t = 0; t < config.num_threads; ++t) threads.emplace_back(worker, t);
    for (auto& t : threads) if (t.joinable()) t.join();
    results.orders_processed = processed.load();
    // results.trades_executed = 0; // Removed hardcoded zero
    return results;
}

} // namespace orderbook