# Plan: Integrate Live Market Data Into Trading Platform (FIX + Modular Architecture)

## TL;DR
Refactor the design into **four clean layers**:
1. **Connector Layer** — FIX/WebSocket/UDP adapters (transport only)  
2. **Translation Layer** — feed-specific translators (e.g., FIX MD W/X)  
3. **Sequencing Layer** — gap detection (application-level), snapshot recovery, ordering  
4. **Publisher Layer** — normalized internal events for the rest of the system  
(Optional) sync to internal `OrderBook`.

Use **QuickFIX** for FIX MD in production.

---

## Parsing & Sequencing Refinements

- **Parsing**: Do **not** pass raw bytes or strings to the parser. The Connector (using QuickFIX) should pass typed FIX message objects (e.g., `const FIX44::MarketDataSnapshotFullRefresh&`, `const FIX44::MarketDataIncrementalRefresh&`) to the Translation Layer. The "Parser" role is strictly a **Translation Layer** that maps FIX field objects onto normalized internal event structs.
- **Sequencer**:
    - **Session-Level Sequencing** (Tag 34 MsgSeqNum): Handled automatically by QuickFIX. No custom gap detection or resend logic required in your code.
    - **Application-Level Sequencing** (Tag 83 RptSeq, Tag 1078 TotalNumReports): Your MarketDataSequencer tracks these fields within market data payloads to ensure correct ordering, gap detection, and snapshot/recovery for order book updates.

---

## Goals
- Connect to external market data feeds (FIX MD primarily).
- Normalize all feeds into one internal event format.
- Preserve application-level sequencing + correct order of updates.
- Allow pluggable MD connectors.
- Keep system modular, testable, and high-performance.

---

# High-Level Architecture

[ FIX / UDP / WS feed ] → [ Connector ] → [ Translation ] → [ Sequencer ] → [ MarketDataPublisher ] → (Optional) [ OrderBook ]

### Components
- `IMarketDataConnector` — starts/stops external feed sessions and emits **typed FIX messages**.
- `IMarketDataTranslator` — converts FIX messages → normalized MD events.
- `MarketDataSequencer` — enforces application-level ordering using market data fields, detects gaps in payload sequence, handles snapshot/recovery logic.
- `MarketDataPublisher` — publishes normalized updates to subscribers.
- `OrderBook::applyExternalMarketData` (optional) — sync external MD into matching engine.

---

# Detailed Steps

## 1) Add Connector Interface
**File:** `include/MarketData/IMarketDataConnector.hpp`  
Methods:
- `start()`, `stop()`, `isRunning()`
- `setMessageHandler(std::function<void(const FIX::Message&)>)`

**Responsibility:**  
Transport only (TCP/WebSocket/QuickFIX session). No parsing. Pass structured FIX message objects, not raw bytes.

---

## 2) Implement FIX Connector (QuickFIX Recommended)
**Files:**  
- `src/MarketData/Connectors/FixMDConnector.cpp/.hpp`  
- Config: `config/md_fix.cfg`

Features:
- QuickFIX session setup (SenderCompID, TargetCompID, host/port).
- Logon/logoff handling.
- Send MDRequest (MsgType V) for subscription.
- Receive FIX MD messages via QuickFIX callback (e.g., `fromApp`) → forward typed FIX message object to translator.

Add config keys:

md.host  
md.port  
md.sender  
md.target  
md.useQuickFix = true

---

## 3) Implement Translation Layer
**Files:** `src/MarketData/Translators/FixMDTranslator.cpp/.hpp`

Translate FIX messages:
- **W** — MarketDataSnapshotFullRefresh  
- **X** — MarketDataIncrementalRefresh  

Output normalized internal structs:

- NormalizedSnapshot
- NormalizedIncremental
- NormalizedTrade
- BookLevel
- UpdateAction

Translation does:
- Map FIX fields (MDEntryPx, MDEntrySize, MDEntryType, MDUpdateAction) from FIX44 objects
- Normalize multiple entries inside MDEntry repeating groups
- Populate uniform event objects

No raw byte parsing; always work with typed FIX messages.

---

## 4) Add MarketDataSequencer
**Files:**
- `src/MarketData/Sequencer/MarketDataSequencer.cpp/.hpp`

Responsibilities:
- Track expected application-level sequence numbers (Tag 83, Tag 1078, etc)
- Detect gaps & out-of-order application-level messages
- Trigger snapshot requests
- Merge snapshot + incrementals properly
- Forward clean, ordered events to `MarketDataPublisher`

Features:
- Gap threshold for payload sequencing
- Recovery counters
- Optional replay buffer
- Logging & metrics

**Note:** Do not implement session-level (Tag 34) logic; QuickFIX handles it.

---

## 5) Integrate With MarketDataPublisher
Normalize events and forward:

- publisher->publishBookUpdate()
- publisher->publishTrade()
- publisher->publishSnapshot()
- publisher->publishDepth()

Keep publisher simple — no sequencing or parsing logic.

---

## 6) Optional: Sync External MD Into OrderBook
Only implement if needed.

**OrderBook additions:**

- applyExternalSnapshot(const NormalizedSnapshot&)
- applyExternalIncremental(const NormalizedIncremental&)

Requires:
- verifying feed sequencing  
- depth truncation  
- conflict resolution with internal orders  

**Default:** disabled.

---

## 7) Monitoring & Metrics
Add metrics in:
- `MarketDataPublisher::PublishingStats`
- `FixMDConnector` stats
- `MarketDataSequencer::Stats`

Track:
- received messages
- translated messages
- dropped messages
- application-level gap detections
- snapshot requests
- latency (optionally)

---

## 8) Tests & Simulation
**Files:**
- `tests/MDReplaySimulator.cpp`
- `tests/MarketDataTranslatorTest.cpp`
- `tests/SequencerTest.cpp`
- `tests/MarketDataEndToEndTest.cpp`

Test scenarios:
- valid in-order messages
- out-of-order delivery (application-level)
- gaps + resync (application-level)
- snapshot + post-snapshot increments
- large depth updates
- performance test

---

# File Summary (Initial MR Scope)

### New
- `/include/MarketData/IMarketDataConnector.hpp`
- `/src/MarketData/Connectors/FixMDConnector.cpp/.hpp`
- `/src/MarketData/Translators/FixMDTranslator.cpp/.hpp`
- `/src/MarketData/Sequencer/MarketDataSequencer.cpp/.hpp`
- `/src/tests/MDReplaySimulator.cpp`
- `/src/tests/MarketDataIntegrationTest.cpp`

### Modified
- `MarketDataPublisher.cpp/.hpp` (stats/logging upgrade)
- `CMakeLists.txt` (QuickFIX dependency)
- Optional: `OrderBook.cpp/.hpp` (applyExternalMarketData)

---

# Questions to Confirm
1. Confirm FIX MD is the **primary** initial feed? (recommended)
2. Do you want OrderBook synchronization enabled? (default: no)
3. Should snapshot recovery be implemented at launch, or later?

---

# Roadmap & Timeline

| Task                                    | ETA      |
|------------------------------------------|----------|
| Connector interface + skeleton           | 0.5 day  |
| QuickFIX FIX MD connector                | 1 day    |
| FIX MD translator (W, X messages)        | 1 day    |
| MarketDataSequencer (gap logic + snapshot) | 1–2 days |
| Publisher integration                    | 0.5 day  |
| Tests (translator, sequencer, replay harness)| 1–2 days |
| Documentation                            | 0.5 day  |

---

# Notes
- QuickFIX is still the best production FIX MD choice.
- Keep translation (field mapping) independent from transport.
- Keep sequencing logic independent from translation.
- Handle **application-level** sequencing only; let QuickFIX manage session-level sequencing.
- Architecture makes it easy to add new feeds (WebSocket, REST, UDP multicast).

EOF
