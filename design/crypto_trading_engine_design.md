# Crypto Trading Engine Design

## 1. Document Purpose

This document defines the requirements and high-level design for a C++ crypto trading engine. The system is intended to support real-time market data ingestion, strategy execution, risk checks, order execution, and operational observability for one or more crypto exchanges.

The first implementation target is a single-process engine with a clean modular architecture. The design should support later expansion into multi-exchange trading, backtesting, simulation, and distributed deployment.

The planned infrastructure stack uses Redis in Docker as the low-latency cache for real-time runtime state, PostgreSQL in Docker as the durable database for business and trading records, and Kafka in Docker as the transaction-ingress queue.

## 2. Goals

### 2.1 Primary Goals

- Build a low-latency, event-driven trading engine in C++20.
- Separate exchange-specific code from strategy and risk logic.
- Support both paper trading and live trading.
- Provide deterministic logging and replay for debugging and post-trade analysis.
- Make the codebase maintainable, testable, and extensible.
- Use Redis, PostgreSQL, and Kafka behind clear infrastructure abstractions.

### 2.2 Non-Goals for Version 1

- Multi-process or distributed deployment.
- Full high-frequency trading optimization.
- Cross-exchange arbitrage from day one.
- GUI dashboard.
- Portfolio margin or complex derivatives pricing.

## 3. Product Scope

Version 1 will support:

- One exchange adapter at a time, with the architecture prepared for multiple adapters.
- Spot trading first.
- Real-time order book and trade feed ingestion.
- Strategy execution based on normalized market events.
- Risk checks before order submission.
- Order lifecycle tracking: new, acknowledged, rejected, partially filled, filled, canceled.
- Redis-backed cache for real-time market and operational state.
- PostgreSQL-backed persistence for orders, fills, positions, balances, users, and transaction history.
- Kafka-backed transaction intake so the engine can consume and process transactions sequentially.
- Replay mode for debugging and simulation.

Future versions may support:

- Multiple exchanges.
- Futures and perpetual contracts.
- Smart order routing.
- Backtesting engine with historical data.
- Strategy sandboxing and plugin loading.

## 4. Requirements

### 4.1 Functional Requirements

#### 4.1.1 Market Data

- Connect to exchange WebSocket feeds.
- Subscribe to order book, trades, ticker, and optional candle streams.
- Parse exchange payloads and convert them into normalized internal events.
- Detect stale or disconnected feeds.
- Maintain an in-memory market state per instrument.

#### 4.1.2 Strategy Engine

- Consume normalized market events.
- Consume transaction requests from Kafka through a transaction-ingestion pipeline.
- Generate trading signals and order requests.
- Support multiple strategy implementations through a common interface.
- Keep strategies independent of exchange transport and raw JSON schemas.

#### 4.1.2A Transaction Intake

- Consume transactions from Kafka topics.
- Process transactions one by one in a deterministic order within the engine.
- Validate transaction schema and reject malformed records safely.
- Commit Kafka consumer offsets only after transaction handling reaches the intended processing checkpoint.

#### 4.1.3 Risk Management

- Enforce max position size per instrument.
- Enforce max order quantity and notional.
- Enforce per-strategy and global exposure limits.
- Block orders on stale market data.
- Provide a kill switch for manual or automatic shutdown.

#### 4.1.4 Execution

- Submit orders through exchange REST or WebSocket APIs as required.
- Generate unique client order IDs.
- Track order acknowledgements, rejections, fills, cancels, and cancel-replace operations.
- Reconcile local order state with exchange updates.

#### 4.1.5 Portfolio and PnL

- Track positions by instrument.
- Track realized and unrealized PnL.
- Track cash balance and inventory.
- Update position state from fills and exchange snapshots when available.

#### 4.1.6 Persistence and Replay

- Persist inbound market events, outbound order intents, exchange responses, and fills.
- Persist snapshots for positions and balances.
- Support replay of historical event logs into the engine.
- Cache the latest market snapshots and feed-health state in Redis.
- Store durable business entities and audit records in PostgreSQL.
- Store inbound transaction records and processing status for audit and recovery.

#### 4.1.7 Observability

- Structured logs for major state transitions.
- Metrics for message rates, latency, rejects, fills, and PnL.
- Health indicators for market data, execution connectivity, and risk status.

### 4.2 Non-Functional Requirements

- C++20 codebase.
- Modular architecture with clear interfaces.
- Deterministic event handling where possible.
- High reliability under disconnects and malformed payloads.
- Unit-testable core business logic.
- Portable build using CMake.
- Reasonable performance for retail or small institutional workloads.
- Docker-based local infrastructure for Redis, PostgreSQL, and Kafka.

## 5. Constraints and Assumptions

- Initial deployment is a single binary running on one host.
- Exchange APIs are external dependencies and may be inconsistent or unstable.
- Network latency is variable and must be tolerated.
- Exact-once semantics are not guaranteed by exchanges, so idempotency is required in execution handling.
- Redis, PostgreSQL, and Kafka will run in Docker for local development and test environments.
- PostgreSQL is the system of record for durable application data.
- Kafka is the ingestion transport for inbound transaction requests.

## 6. Architecture Overview

The engine will use an event-driven architecture centered on normalized internal messages.

### 6.1 Main Components

1. Market Data Gateway
- Connects to exchange feeds.
- Parses raw messages.
- Publishes normalized market events.

2. Market State Store
- Maintains best bid/ask, depth snapshot, last trade, and timestamps.
- Exposes a read-only view to strategy and risk components.

3. Strategy Engine
- Receives market events.
- Receives normalized transaction commands from Kafka ingestion.
- Produces order intents.

4. Transaction Ingestion
- Consumes transaction messages from Kafka.
- Validates and normalizes them into internal transaction commands.
- Feeds them into the engine for one-by-one processing.

5. Risk Engine
- Validates order intents against configured limits and current exposure.
- Approves or rejects intents.

6. Execution Engine
- Translates approved order intents into exchange API requests.
- Tracks order lifecycle and exchange acknowledgements.

7. Portfolio Service
- Maintains positions, balances, and PnL.
- Updates state from fills and balance snapshots.

8. Persistence Layer
- Writes low-latency runtime state to Redis.
- Writes durable records and snapshots to PostgreSQL.
- Supports replay and recovery.

9. Monitoring and Control
- Exposes metrics, logs, and operational controls such as kill switch and pause trading.

### 6.2 Data Flow

1. Exchange market data arrives.
2. Market Data Gateway normalizes payloads into internal events.
3. Market State Store updates the latest instrument state.
4. Kafka provides inbound transaction messages.
5. Transaction Ingestion consumes and normalizes one transaction at a time.
6. Strategy Engine processes events and transaction commands and emits order intents.
7. Risk Engine validates intents.
8. Approved intents go to Execution Engine.
9. Exchange responses and fills return into the engine.
10. Portfolio Service updates positions and PnL.
11. Persistence Layer records all significant events and maintains consistency between cache and durable storage.

## 7. Logical Module Design

Recommended project structure:

```text
trading-engine/
  CMakeLists.txt
  design/
  configs/
  include/
    core/
    market_data/
    strategy/
    risk/
    execution/
    ingestion/
    exchange/
    portfolio/
    storage/
    monitoring/
    infrastructure/
  src/
    core/
    market_data/
    strategy/
    risk/
    execution/
    ingestion/
    exchange/
    portfolio/
    storage/
    monitoring/
    infrastructure/
    app/
  tests/
```

### 7.1 Core Module

Responsibilities:

- Common types and enums.
- Event bus or dispatcher.
- Time abstraction and clock utilities.
- Configuration loading.
- Error handling primitives.

Representative types:

- `Instrument`
- `MarketEvent`
- `OrderRequest`
- `OrderUpdate`
- `FillEvent`
- `Position`
- `RiskDecision`

### 7.2 Market Data Module

Responsibilities:

- Connection management.
- Subscription management.
- Raw payload parsing.
- Sequence checking and gap detection.
- Publication of normalized events.

### 7.3 Strategy Module

Responsibilities:

- Strategy interface.
- Strategy context accessors.
- Strategy lifecycle hooks.
- Strategy registry and configuration loading.
- Translating validated transaction commands and market context into order intents where applicable.

Example interface:

```cpp
class IStrategy {
public:
    virtual ~IStrategy() = default;
    virtual void on_start() = 0;
    virtual std::vector<OrderRequest> on_market_event(const MarketEvent& event) = 0;
    virtual void on_order_update(const OrderUpdate& update) = 0;
    virtual void on_fill(const FillEvent& fill) = 0;
};
```

### 7.4 Ingestion Module

Responsibilities:

- Kafka consumer management.
- Transaction schema validation.
- Offset management.
- One-by-one transaction dispatch into the engine runtime.
- Dead-letter or reject handling for malformed transaction messages.

### 7.5 Risk Module

Responsibilities:

- Pre-trade checks.
- Runtime limits.
- Exposure and notional controls.
- Circuit breaker and kill switch.

Example interface:

```cpp
class IRiskManager {
public:
    virtual ~IRiskManager() = default;
    virtual RiskDecision check(const OrderRequest& request) = 0;
};
```

### 7.6 Execution Module

Responsibilities:

- Order request validation before exchange submission.
- Exchange command dispatch.
- Retry rules where safe.
- Order state machine.
- Reconciliation with exchange state.

Example interface:

```cpp
class IExchange {
public:
    virtual ~IExchange() = default;
    virtual void connect() = 0;
    virtual void submit_order(const OrderRequest& request) = 0;
    virtual void cancel_order(const std::string& client_order_id) = 0;
};
```

### 7.7 Portfolio Module

Responsibilities:

- Position accounting.
- Average price tracking.
- Realized and unrealized PnL.
- Balance and cash updates.

### 7.8 Storage Module

Responsibilities:

- Redis cache access for market state and hot runtime data.
- PostgreSQL repository access for durable entities.
- Persistence of inbound Kafka transaction records and processing status.
- Replay reader.
- Recovery routines.
- Persistence mapping between domain objects and storage records.

### 7.9 Infrastructure Module

Responsibilities:

- Docker-aware local runtime setup.
- Redis client bootstrap and connection health checks.
- PostgreSQL client or pool bootstrap and migration setup.
- Kafka producer and consumer bootstrap, topic settings, and health checks.
- Shared infrastructure settings used by storage and application startup.

### 7.10 Monitoring Module

Responsibilities:

- Metrics emission.
- Health status reporting.
- Log formatting.
- Alert hooks.

## 8. Event Model

The system should normalize external events into internal message types to decouple business logic from exchange schemas.

Core event categories:

- `BookSnapshotEvent`
- `BookDeltaEvent`
- `TradeEvent`
- `TickerEvent`
- `TimerEvent`
- `TransactionCommandEvent`
- `OrderIntentEvent`
- `OrderAckEvent`
- `OrderRejectEvent`
- `FillEvent`
- `CancelAckEvent`
- `BalanceUpdateEvent`
- `RiskRejectEvent`

Important event fields:

- `event_id`
- `source`
- `exchange`
- `instrument`
- `timestamp_exchange`
- `timestamp_receive`
- `timestamp_process`

This enables latency tracking and replay determinism.

Transaction command fields should also include:

- `transaction_id`
- `user_id`
- `account_id`
- `command_type`
- `command_payload`
- `kafka_topic`
- `kafka_partition`
- `kafka_offset`

## 9. State Management

### 9.1 Market State

For each instrument:

- Best bid and ask.
- Order book depth summary.
- Last trade price and size.
- Mid price.
- Last update timestamp.
- Feed health status.

Primary runtime location:

- Redis cache.

### 9.2 Order State

Each order should move through a defined state machine:

- Created
- PendingSubmit
- Acknowledged
- PartiallyFilled
- Filled
- PendingCancel
- Canceled
- Rejected
- Expired

Transitions must be driven by exchange responses and guarded against duplicate or out-of-order messages.

### 9.3 Portfolio State

Per instrument:

- Net position.
- Average entry price.
- Realized PnL.
- Unrealized PnL.
- Fees paid.

Global:

- Total equity.
- Available cash.
- Gross and net exposure.

Primary durable location:

- PostgreSQL database.

## 10. Concurrency Model

Version 1 should prefer simplicity over aggressive parallelism.

Recommended model:

- One main event loop for coordination.
- One Kafka consumer flow that feeds transactions into the main event loop one by one.
- Dedicated I/O handlers for market data and execution connectivity.
- Thread-safe queues between I/O boundaries and core event processing if needed.
- Business logic processed in a deterministic serialized pipeline.

Rationale:

- Easier debugging.
- Lower race-condition risk.
- Sufficient for initial throughput targets.

Future optimization options:

- Partition instruments across worker loops.
- Lock-free queues for high-volume event transfer.
- Separate processes for market data, strategy, and execution.

## 11. Error Handling and Resilience

The engine must handle external instability as a default case.

Failure scenarios:

- WebSocket disconnects.
- Kafka consumer disconnects or rebalance events.
- REST timeouts.
- Duplicate order acknowledgements.
- Missing or out-of-order market data sequence numbers.
- Partial fills without prior expected states.
- Malformed exchange payloads.
- Malformed Kafka transaction messages.

Resilience requirements:

- Automatic reconnect with backoff.
- Safe resubscription after reconnect.
- Safe Kafka consumer restart and offset handling.
- Detection of stale market data.
- Reject trading if market state is invalid.
- Persist enough data to reconstruct critical state after restart.

## 12. Security and Secrets

- API keys must not be hardcoded.
- Credentials should be loaded from environment variables or secure config sources.
- Logs must avoid leaking secrets.
- Redis and PostgreSQL credentials must not be committed to source control.
- Trading mode must be explicit: `simulation`, `paper`, or `live`.
- Live trading should require explicit configuration confirmation.

## 13. Configuration Design

Configuration should be file-based with environment overrides.

Key configuration groups:

- Exchange credentials and endpoints.
- Kafka topic and consumer settings.
- Instrument subscriptions.
- Strategy parameters.
- Risk limits.
- Logging and metrics settings.
- Redis connection settings.
- PostgreSQL connection settings.
- Docker service settings for local development.
- Trading mode and feature flags.

Example:

```yaml
mode: paper
exchange:
  name: binance
  websocket_url: wss://example
  rest_url: https://example
instruments:
  - BTCUSDT
risk:
  max_position_usd: 10000
  max_order_usd: 1000
  kill_switch_enabled: true
strategy:
  name: market_maker
redis:
  host: redis
  port: 6379
postgres:
  host: postgres
  port: 5432
  database: trading
kafka:
  brokers:
    - kafka:9092
  transaction_topic: trading-transactions
  consumer_group: trading-engine
```

## 14. Technology Choices

Recommended initial stack:

- Language: C++20
- Build: CMake
- Networking: Boost.Asio or standalone Asio
- WebSocket/HTTP: Boost.Beast or exchange SDK where appropriate
- JSON: simdjson for speed or nlohmann/json for simplicity
- Logging: spdlog
- Formatting: fmt
- Testing: Catch2 or GoogleTest
- Cache: Redis
- Durable database: PostgreSQL
- Transaction queue: Kafka
- Local infrastructure: Docker / Docker Compose
- Serialization: plain structs initially, protobuf later if needed

Selection principle:

- Favor straightforward, proven libraries with good portability.
- Keep third-party dependencies limited in version 1.

## 15. Testing Strategy

### 15.1 Unit Tests

- Order state transitions.
- Risk checks.
- PnL calculations.
- Strategy logic using synthetic events.
- Exchange payload normalization.

### 15.2 Integration Tests

- Simulated market data stream into strategy and execution pipeline.
- Mock exchange order submission and fill handling.
- Replay of recorded event logs.
- Redis cache boundary tests with isolated test data.
- PostgreSQL repository tests with fixture data.
- Kafka consumer tests with captured transaction fixtures and deterministic offsets.

### 15.3 Failure Tests

- Reconnect handling.
- Duplicate and out-of-order messages.
- Stale market data block.
- Exchange reject paths.

## 16. Delivery Plan

### Phase 1: Foundation

- Create project structure.
- Define core domain models and interfaces.
- Add logging, config loading, and test framework.
- Add Redis, PostgreSQL, and Kafka configuration plumbing.

### Phase 2: Simulation Engine

- Build internal event loop.
- Build Kafka-backed transaction ingestion flow.
- Implement simulated exchange.
- Implement portfolio and risk basics.
- Add one sample strategy.
- Introduce storage abstractions for Redis cache and PostgreSQL persistence.

### Phase 3: Live Exchange Adapter

- Add one real exchange connector.
- Add market data subscriptions and order submission.
- Add reconciliation and recovery behavior.
- Back durable state with PostgreSQL and runtime cache with Redis.
- Consume live transaction requests from Kafka and process them sequentially.

### Phase 4: Operations Hardening

- Add metrics.
- Add replay tools.
- Improve restart recovery.
- Expand test coverage.

## 17. Open Design Questions

- Which exchange should be the first live target?
- Is spot trading sufficient for the first production milestone?
- Should all normalized market events be persisted in PostgreSQL, or only a selected subset plus audit checkpoints?
- Should Redis be used only as a cache, or also for selected internal pub/sub flows?
- What Kafka offset-commit strategy should be used for transaction processing guarantees?
- Is one strategy instance enough initially, or should the engine support multiple strategies in version 1?
- What latency target is required for the intended use case?

## 18. Summary

The proposed system is a modular, event-driven C++ trading engine with clear separation between market data, ingestion, strategy, risk, execution, portfolio, and storage. Redis will serve as the real-time cache, PostgreSQL will serve as the durable system of record, and Kafka will serve as the transaction-ingress queue, all running in Docker for local development and controlled deployment. The first version should optimize for correctness, observability, and extensibility rather than maximum speed. That approach will make it practical to move from simulation to paper trading and then to controlled live trading with lower implementation risk.
