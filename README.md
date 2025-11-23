# Order Book System

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

A high-performance trading system with FIX protocol support and configurable runtime parameters.

# VertexLadder - High-Performance Order Book System

A low-latency order book implementation optimized for electronic trading, featuring FIX protocol support and real-time matching.

## Key Features
- **High-Frequency Trading Optimized**: **1.8 Million+** orders/sec throughput
- **Ultra-Low Latency**: **<1μs** order processing (Avg ~0.4μs)
- **Zero Allocation Hot-Path**: Custom LIFO Object Pooling & Pre-warming
- **Memory Efficient**: Raw pointers & fixed-size buffers (no `std::string` or `std::shared_ptr` overhead)
- **FIX 4.4 Compliant**: Async TCP networking via Boost.Asio
- **Lock-Free Concurrency**: Sharded SPSC queues for massive parallelism

## Performance Characteristics

### Throughput Benchmarks (Apple M3 Pro / 4 Threads)
| Operation          | Capacity (ops/sec)   | Latency (Avg μs) | Latency (P99 μs) |
|--------------------|----------------------|------------------|------------------|
| Order Add          | **~2,500,000**       | **0.41**         | 0.67             |
| Order Cancel       | **~2,600,000**       | **0.39**         | 0.67             |
| Overall System     | **~1,870,000**       | -                | -                |

### Optimization Techniques
1. **Memory Management**
   - **Object Pooling**: Custom LIFO pool with pre-warming (1,000,000 objects) to ensure O(1) allocation.
   - **Raw Pointers**: Replaced `std::shared_ptr` to eliminate atomic reference counting overhead.
   - **Zero-Copy Strings**: Replaced `std::string` with fixed-size `char` arrays to prevent heap fragmentation.

2. **Algorithmic Improvements**
   - **Lazy Deletion**: Price levels are marked empty rather than erased from vectors, making cancellation O(1) (avoiding O(N) shifts).
   - **Deferred Deallocation**: "Retire List" strategy to safely recycle objects after batch processing (preventing Use-After-Free).

3. **Concurrency**
   - **Sharded SPSC Queues**: Lock-free ingestion using `boost::lockfree::spsc_queue` to minimize contention.
   - **Thread-Local Indexing**: Producer threads map to queues via thread-local storage.

## Configuration

Edit `config/orderbook.cfg`:

```ini
[orderbook]
symbol = BTC/USD       # Trading instrument pair
max_orders = 1000000   # Maximum active orders

[network]
port = 5000            # FIX protocol listening port
max_connections = 1000 # Maximum concurrent clients

[risk]
max_order_size = 10000 # Maximum quantity per order
max_position = 100000  # Maximum net position allowed
max_price = 1000000.0  # Maximum acceptable price

[logging]
level = info           # Log verbosity (debug|info|warn|error)
file = orderbook.log   # Log file path
```

**To Modify Configuration**:
1. Edit `config/orderbook.cfg`
2. Restart the application
3. Changes take effect immediately

## File Architecture

```
orderbook/
├── CMakeLists.txt
├── config/
│   └── orderbook.cfg       # Runtime configuration
├── include/
│   └── orderbook/
│       ├── Core/           # Matching engine core
│       ├── Network/        # FIX protocol implementation
│       ├── MarketData/     # Real-time data distribution
│       ├── Risk/           # Risk controls
│       └── Utilities/      # Support components
├── src/
│   ├── Core/               # Business logic
│   ├── Network/            # Protocol handling
│   ├── MarketData/         # Data publishing
│   ├── Risk/               # Risk checks
│   ├── Utilities/          # Infrastructure
│   └── main.cpp            # Entry point
└── third_party/
    └── fix/                # Protocol specifications
```

## Build & Run

The project uses CMake for build configuration.

```bash
# Create build directory (Defaults to Release build)
cmake -S . -B build

# Build with parallel jobs
cmake --build build -- -j4

# Run Performance Validation
./build/OrderBookPerformanceValidation --orders 1000000 --threads 4

# Run Main Application
./build/OrderBook
```

### Recent Updates (November 2025)

- **Symbol Database**: Added `docs/symbols.csv` and `docs/symbols.json` containing 830+ symbols discovered from cTrader.
- **cTrader Integration**: 
  - Configured `QuickFixConnector` to support cTrader's specific FIX requirements (Bid/Offer entry types).
  - Validated connectivity and data subscription for Apple (ID: 21508).
- **Project Cleanup**: 
  - Consolidated build artifacts into a single `build/` directory (Release mode by default).
  - Removed redundant QuickFIX source files and duplicate configurations.
  - Streamlined `third_party/` and `config/` directories.

### QuickFIX Market Data Integration (Optional)

To enable FIX-based market-data ingestion using QuickFIX, install QuickFIX and rebuild with CMake flag `-DWITH_QUICKFIX=ON`.

1. Install QuickFIX (macOS example):
```bash
brew install quickfix
```

2. Configure `config/quickfix/quickfix.cfg` for the feed endpoint and credentials.

3. Enable QuickFIX in `config/orderbook.cfg` and set symbols:
```ini
[marketdata]
use_quickfix = true
quickfix_config = config/quickfix/quickfix.cfg
symbols = BTC/USD,EUR/USD
apply_to_book = false
```

4. Rebuild with QuickFIX enabled:
```bash
mkdir -p build && cd build
cmake -DWITH_QUICKFIX=ON ..
make -j
```

The QuickFIX connector will initiate an application-level session, log on to the provider, then subscribe to `symbols` via `MarketDataRequest`. The connector handles message sequencing (MDSeqNum), requests retransmissions on gaps, and exposes lightweight stats for monitoring.

### MDSimulator (QuickFIX acceptor)

The project includes `MDSimulator` — a small QuickFIX-based acceptor that sends a market-data snapshot and random incrementals. Use it to validate the QuickFIX connector and `OrderBook`:

1. Build with QuickFIX enabled:
```bash
mkdir -p build && cd build
cmake -DWITH_QUICKFIX=ON ..
make -j MDSimulator
```
2. Run MDSimulator (acceptor) using acceptor config:
```bash
./MDSimulator config/quickfix/quickfix_acceptor.cfg
```
3. Run the `OrderBook` (initiator) with QuickFIX enabled and pointing to the client config in `config/quickfix/quickfix.cfg`.

When MDSimulator detects a connection, it will send a snapshot and then random incremental updates.

## License

MIT License - See [LICENSE](LICENSE) for details.

---

This implementation provides a complete configuration system with:
1. INI-file parsing
2. Type-safe accessors
3. Default value handling
4. Runtime reconfiguration capability
5. Clear documentation

The system will automatically load configuration at startup and use it for all operational parameters.