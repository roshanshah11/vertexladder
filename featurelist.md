# Financial Order Book - Feature List

## Core Trading Engine
- **High-Performance Matching**: Price/Time priority matching engine capable of processing 1.8M+ orders/second.
- **Low Latency**: Sub-microsecond order processing latency (<1μs average).
- **Order Types**: Support for Limit orders (IOC, GTC support implied by architecture).
- **Memory Management**: Custom LIFO object pooling and pre-warming to ensure zero-allocation on the hot path.
- **Concurrency**: Lock-free SPSC (Single-Producer Single-Consumer) queues for thread-safe, high-throughput data ingestion.

## Connectivity & Networking
- **FIX Protocol Support**: Full FIX 4.4 compliance for order entry and market data.
  - **Initiator & Acceptor**: Capable of acting as both client and server.
  - **cTrader Integration**: Specific configurations for cTrader connectivity.
- **WebSocket Server**: Built-in WebSocket server for real-time market data broadcasting to frontends.
  - **Conflation**: Automatic throttling of updates to ~60 FPS to prevent frontend congestion.
  - **Snapshot Broadcasting**: Periodic broadcasting of Top-of-Book (ToB) and Last Trade data.

## Market Data
- **Real-time Feed**: Ingestion of market data via FIX or internal simulation.
- **Symbol Database**: Comprehensive list of 830+ trading pairs/symbols.
- **MDSimulator**: Standalone tool to simulate market data feeds for testing.

## Risk Management
- **Pre-Trade Checks**:
  - Maximum Order Size
  - Maximum Net Position
  - Price Band validation

## Infrastructure & Utilities
- **Configuration**: robust INI-based configuration system (`config/orderbook.cfg`).
- **Logging**: Asynchronous, high-performance logging system.
- **Performance Validation**: Dedicated tools (`OrderBookPerformanceValidation`) to benchmark throughput and latency.
- **Build System**: Modern CMake configuration with support for Release/Debug builds and sanitizers.
