# Crypto Trading Engine Development Tasks and Priorities

## 1. Purpose

This document breaks the project into implementation tasks and assigns priorities so development can proceed in a controlled order.

Priority levels:

- `P0`: required to make the engine viable
- `P1`: important for a safe and usable first release
- `P2`: valuable after the first release foundation is stable

## 2. Delivery Principles

- Build the core event model before any exchange-specific logic.
- Get a simulation path working before live trading.
- Add risk and observability early, not at the end.
- Keep live exchange integration behind a stable internal interface.
- Favor thin vertical slices that can be tested end to end.
- Use Redis for real-time cache responsibilities, PostgreSQL for durable transactional data, and Kafka for inbound transaction ingestion.
- For every implementation task, add unit tests in the same step using mock data or test doubles.
- No task is considered complete unless its core behavior is verified by automated tests.

## 2.1 Test-First Verification Rule

Each task below must include unit-test coverage with representative mock inputs.

Required test expectations for every step:

- validate the happy path
- validate at least one failure or rejection path
- use deterministic mock data
- avoid live network or exchange dependencies
- keep fixtures small and readable

Mock-data guidelines:

- Use synthetic market events for market data tests.
- Use fake order requests, fills, and balances for portfolio and risk tests.
- Use stub exchange adapters for execution tests.
- Use temporary local files only when storage behavior must be verified.
- Use Redis, PostgreSQL, and Kafka test doubles by default, and isolated container-backed tests only where infrastructure boundaries must be verified.
- Prefer deterministic timestamps and IDs in tests.

## 3. Task Breakdown

### Phase 1: Foundation

#### Task 1. Project scaffolding
- Priority: `P0`
- Goal: create the base C++ project layout aligned with the design
- Scope:
  - create `include/`, `src/`, `tests/`, `configs/`, `design/`
  - update `CMakeLists.txt`
  - add module directories for `core`, `market_data`, `ingestion`, `strategy`, `risk`, `execution`, `portfolio`, `storage`, `monitoring`, `exchange`, `app`
  - add `docker/` artifacts for Redis, PostgreSQL, and Kafka local runtime
- Unit tests with mock data:
  - add a sample test target and one smoke test
  - verify test discovery and execution work in the build
- Deliverable:
  - project builds cleanly with placeholder targets
  - unit-test target is wired into the build

#### Task 2. Core domain model
- Priority: `P0`
- Goal: define the core business types used across the engine
- Scope:
  - `Instrument`
  - `MarketEvent`
  - `OrderRequest`
  - `OrderUpdate`
  - `FillEvent`
  - `Position`
  - `BalanceSnapshot`
  - enums for side, order type, order status, event type
- Unit tests with mock data:
  - construct each core type with sample values
  - verify enum transitions and default invariants
  - verify invalid or incomplete objects are rejected where validation exists
- Deliverable:
  - shared headers and basic serialization-friendly structs
  - tests confirm model construction and invariants

#### Task 3. Configuration system
- Priority: `P0`
- Goal: load runtime config for engine mode, strategy, and risk
- Scope:
  - configuration file format
  - config reader
  - validation rules
  - environment variable override support for secrets
  - Redis, PostgreSQL, and Kafka connection settings
- Unit tests with mock data:
  - parse valid sample config files
  - verify missing required fields fail validation
  - verify environment overrides replace file values deterministically
- Deliverable:
  - engine can start with a valid config file
  - tests cover valid and invalid config cases

#### Task 4. Logging and error handling
- Priority: `P0`
- Goal: establish production-usable logging and failure reporting
- Scope:
  - structured logger setup
  - common error categories
  - startup and shutdown logging
- Unit tests with mock data:
  - verify error categories map to expected codes or messages
  - verify logger can be initialized in test mode
  - verify representative failures generate expected structured fields where practical
- Deliverable:
  - uniform logging available to all modules
  - tests cover error classification and logger initialization

#### Task 5. Build and test harness
- Priority: `P0`
- Goal: enable repeatable builds and unit tests
- Scope:
  - test framework integration
  - base test target
  - CI-friendly build layout
- Unit tests with mock data:
  - add representative sample tests in at least two modules
  - verify tests run as part of the build workflow
- Deliverable:
  - sample tests compile and run
  - test harness is ready for all subsequent tasks

### Phase 2: Event Pipeline and Simulation

#### Task 6. Event dispatcher / engine loop
- Priority: `P0`
- Goal: create the main event-driven runtime pipeline
- Scope:
  - event queue
  - dispatch rules
  - timer event support
  - deterministic processing order
- Unit tests with mock data:
  - feed synthetic events into the dispatcher
  - verify handler invocation order
  - verify timer events are emitted deterministically
  - verify unknown or invalid events are handled safely
- Deliverable:
  - normalized events flow through the engine
  - tests confirm dispatch correctness and ordering

#### Task 6A. Kafka transaction ingestion baseline
- Priority: `P0`
- Goal: consume transaction messages from Kafka and feed them into the engine one by one
- Scope:
  - Kafka consumer abstraction
  - transaction message schema
  - sequential transaction dispatch
  - offset commit policy
  - malformed-message rejection path
- Unit tests with mock data:
  - consume fixed transaction fixtures from a mock Kafka source
  - verify transactions are processed strictly in order
  - verify malformed messages are rejected safely
  - verify offsets are only marked complete at the intended processing checkpoint
- Deliverable:
  - transaction ingestion path is defined and test-covered
  - engine can process inbound transactions sequentially

#### Task 7. Market state store
- Priority: `P0`
- Goal: maintain in-memory market state per instrument
- Scope:
  - top-of-book state
  - last trade tracking
  - timestamps and stale-state markers
- Unit tests with mock data:
  - apply synthetic quote and trade events
  - verify best bid and ask updates
  - verify stale detection from controlled timestamps
  - verify out-of-order events do not corrupt state
- Deliverable:
  - strategies and risk checks can query current market state
  - tests confirm state updates and stale-data behavior

#### Task 8. Strategy interface and sample strategy
- Priority: `P0`
- Goal: define strategy integration points and prove strategy execution works
- Scope:
  - strategy interface
  - strategy context
  - one sample strategy, such as threshold or market-maker simulator
- Unit tests with mock data:
  - pass synthetic market events into the sample strategy
  - verify expected order requests are generated
  - verify no orders are emitted when conditions are not met
  - verify strategy reacts correctly to order and fill callbacks
- Deliverable:
  - strategy receives market events and emits order requests
  - tests prove strategy decisions against fixed inputs

#### Task 9. Risk engine baseline
- Priority: `P0`
- Goal: prevent unsafe orders before execution
- Scope:
  - max order size
  - max notional
  - max position limit
  - kill switch
  - stale market data guard
- Unit tests with mock data:
  - verify valid orders are approved
  - verify oversize orders are rejected
  - verify stale-market orders are rejected
  - verify kill switch blocks all submissions
- Deliverable:
  - order requests are approved or rejected with reasons
  - tests cover approval and rejection paths

#### Task 10. Simulated execution engine
- Priority: `P0`
- Goal: validate order lifecycle before live exchange integration
- Scope:
  - simulated order submit
  - order acknowledgement
  - partial and full fill generation
  - cancel handling
- Unit tests with mock data:
  - submit fake orders and verify lifecycle transitions
  - verify partial fill then full fill behavior
  - verify cancel transitions
  - verify duplicate updates are handled idempotently if supported
- Deliverable:
  - end-to-end paper workflow without exchange dependency
  - tests confirm lifecycle behavior using simulated events

#### Task 11. Portfolio and PnL service
- Priority: `P0`
- Goal: track positions and PnL from fills
- Scope:
  - net position calculation
  - average entry price
  - realized and unrealized PnL
  - fees
- Unit tests with mock data:
  - apply sequences of buy and sell fills
  - verify position quantity and average price
  - verify realized PnL and fee accounting
  - verify flat-position reset behavior
- Deliverable:
  - portfolio state updates from simulated fills
  - tests validate accounting logic with fixed fill sequences

#### Task 12. Persistence baseline
- Priority: `P1`
- Goal: persist enough state for replay and debugging
- Scope:
  - Redis cache layer for real-time market and hot portfolio state
  - PostgreSQL persistence for orders, fills, balances, positions, users, and transactions
  - Kafka transaction receipt and processing-status persistence
  - durable event and snapshot persistence needed for replay
- Unit tests with mock data:
  - verify Redis cache updates from mock market and position events
  - verify PostgreSQL repository mapping with deterministic fixtures
  - verify Kafka transaction metadata is persisted with deterministic topic, partition, and offset values
  - read persisted records back and verify integrity
- Deliverable:
  - critical engine events recorded in the selected storage layers
  - tests validate cache and durable persistence behavior

#### Task 13. Replay tool
- Priority: `P1`
- Goal: replay stored events through the engine
- Scope:
  - event reader
  - deterministic replay mode
  - replay entrypoint
- Unit tests with mock data:
  - replay a fixed event sequence
  - verify resulting state matches expected market, order, and position outcomes
  - verify invalid records are rejected safely
- Deliverable:
  - engine can replay prior sessions for debugging
  - tests prove deterministic replay on sample sessions

### Phase 3: Live Exchange Integration

#### Task 14. Exchange abstraction
- Priority: `P0`
- Goal: stabilize the interface between engine core and exchange adapters
- Scope:
  - market data adapter interface
  - execution adapter interface
  - exchange-specific error mapping
- Unit tests with mock data:
  - verify mock adapters satisfy interface contracts
  - verify normalized outputs from sample exchange messages
  - verify exchange error mapping behavior
- Deliverable:
  - exchange adapter contract finalized
  - tests lock down adapter-facing contracts

#### Task 15. Market data adapter for first exchange
- Priority: `P1`
- Goal: connect to one real crypto venue and normalize live market data
- Scope:
  - WebSocket session management
  - subscription management
  - message parsing
  - reconnect logic
  - heartbeat / stale detection
- Unit tests with mock data:
  - parse captured sample exchange messages
  - verify normalized internal events
  - verify heartbeat timeout and reconnect decision logic with fake clocks
- Deliverable:
  - live market events available through internal event model
  - tests cover parsing and connection-state logic without live network use

#### Task 16. Live order execution adapter
- Priority: `P1`
- Goal: submit and manage real orders through the first exchange
- Scope:
  - authenticated order submission
  - cancel requests
  - order status updates
  - execution report parsing
  - idempotent client order IDs
- Unit tests with mock data:
  - verify request signing or request building with deterministic fixtures
  - verify captured execution reports map to internal order updates
  - verify duplicate exchange updates do not double-apply state changes
- Deliverable:
  - paper-to-live interface complete
  - tests validate request/response normalization and idempotency rules

#### Task 17. Reconciliation and recovery
- Priority: `P1`
- Goal: align engine state with exchange state across restarts and disconnects
- Scope:
  - open order sync
  - balance snapshot pull
  - position recovery
  - duplicate event handling
  - Redis cache warmup from PostgreSQL durable state on startup
  - Kafka offset recovery and safe restart position
- Unit tests with mock data:
  - load mock persisted state and mock exchange snapshots
  - verify recovered open orders and balances
  - verify duplicate recovery inputs do not corrupt state
- Deliverable:
  - restart behavior is controlled and auditable
  - tests prove recovery logic against fixed snapshots

### Phase 4: Observability and Hardening

#### Task 18. Metrics and health checks
- Priority: `P1`
- Goal: expose engine health and performance indicators
- Scope:
  - feed status metrics
  - risk reject count
  - order latency
  - fill rate
  - PnL and exposure metrics
- Unit tests with mock data:
  - inject sample events and verify metric counters and gauges update correctly
  - verify unhealthy states are reported from fixed failure inputs
- Deliverable:
  - operators can monitor engine state in real time
  - tests validate metric calculations and health transitions

#### Task 19. Operational controls
- Priority: `P1`
- Goal: provide safe runtime control surfaces
- Scope:
  - manual kill switch
  - trading pause
  - mode display
  - startup safety checks
- Unit tests with mock data:
  - verify pause and kill-switch state transitions
  - verify startup safety checks reject invalid modes or missing confirmations
  - verify control state affects order admission paths
- Deliverable:
  - controlled transitions between safe and active states
  - tests prove control behavior and guardrails

#### Task 20. Failure-path testing
- Priority: `P1`
- Goal: prove the engine handles realistic failure scenarios
- Scope:
  - disconnects
  - Kafka consumer disconnects or rebalance events
  - stale feeds
  - exchange rejects
  - duplicate fills
  - malformed payloads
- Unit tests with mock data:
  - simulate each failure path with controlled fixtures
  - verify safe rejection, retry, or ignore behavior as designed
  - verify state remains consistent after failure handling
- Deliverable:
  - resilience coverage for major operational risks
  - tests cover major failure scenarios with deterministic inputs

### Phase 5: Post-V1 Improvements

#### Task 21. Storage upgrade
- Priority: `P2`
- Goal: optimize and evolve the Redis/PostgreSQL storage stack after the first stable release
- Scope:
  - PostgreSQL schema optimization and migration flow
  - Redis key design refinement and expiration strategy
  - Kafka topic and consumer-group operational tuning
  - optional partitioning or archival strategy for historical data
- Unit tests with mock data:
  - verify writes, reads, and migrations using isolated test databases
  - verify schema constraints and recovery of expected records
  - verify Redis key lifecycle behavior where applicable
- Deliverable:
  - stronger recovery and analysis support
  - tests validate upgraded storage behavior

#### Task 22. Multi-strategy support
- Priority: `P2`
- Goal: run multiple strategies within the same engine
- Scope:
  - strategy registry
  - per-strategy risk limits
  - per-strategy position accounting
- Unit tests with mock data:
  - run multiple mock strategies against the same event stream
  - verify strategy isolation for orders, limits, and positions
  - verify one strategy failure does not corrupt another strategy state
- Deliverable:
  - strategy isolation within a shared engine
  - tests verify per-strategy isolation rules

#### Task 23. Multi-exchange support
- Priority: `P2`
- Goal: support more than one venue cleanly
- Scope:
  - exchange adapter registry
  - routing layer
  - unified symbol mapping
- Unit tests with mock data:
  - feed mock events from multiple exchanges
  - verify symbol normalization and routing decisions
  - verify exchange-specific failures stay isolated
- Deliverable:
  - engine can connect to multiple exchanges
  - tests validate multi-exchange routing and normalization

#### Task 24. Historical backtesting mode
- Priority: `P2`
- Goal: use the same core logic for offline evaluation
- Scope:
  - historical event ingestion
  - deterministic replay controls
  - performance reporting
- Unit tests with mock data:
  - replay fixed historical fixtures
  - verify strategy outputs and final PnL
  - verify backtest summary metrics from deterministic data
- Deliverable:
  - one strategy code path shared by simulation and backtest
  - tests confirm deterministic offline execution

## 4. Recommended Execution Order

The recommended implementation order is:

1. Task 1: Project scaffolding
2. Task 2: Core domain model
3. Task 3: Configuration system
4. Task 4: Logging and error handling
5. Task 5: Build and test harness
6. Task 6: Event dispatcher / engine loop
7. Task 6A: Kafka transaction ingestion baseline
8. Task 7: Market state store
9. Task 8: Strategy interface and sample strategy
10. Task 9: Risk engine baseline
11. Task 10: Simulated execution engine
12. Task 11: Portfolio and PnL service
13. Task 12: Persistence baseline
14. Task 13: Replay tool
15. Task 14: Exchange abstraction
16. Task 15: Market data adapter for first exchange
17. Task 16: Live order execution adapter
18. Task 17: Reconciliation and recovery
19. Task 18: Metrics and health checks
20. Task 19: Operational controls
21. Task 20: Failure-path testing
22. Task 21: Storage upgrade
23. Task 22: Multi-strategy support
24. Task 23: Multi-exchange support
25. Task 24: Historical backtesting mode

## 5. Suggested Milestones

### Milestone A: Engine Skeleton
- Includes tasks: 1 to 5
- Outcome:
  - codebase structure exists
  - build and tests are working
  - shared types and config are ready
  - every completed task includes mock-data unit tests

### Milestone B: Simulated Trading MVP
- Includes tasks: 6, 6A, 7 to 11
- Outcome:
  - market events can drive a strategy
  - Kafka transactions can be consumed and processed one by one
  - risk checks work
  - simulated orders and fills update positions
  - core simulation pipeline is covered by deterministic unit tests

### Milestone C: Persistence and Replay
- Includes tasks: 12 to 13
- Outcome:
  - engine actions are durable
  - sessions can be replayed for debugging
  - persistence and replay paths are covered by fixture-based tests

### Milestone D: First Live Exchange
- Includes tasks: 14 to 17
- Outcome:
  - one exchange is supported for live or paper trading
  - restart and recovery are defined
  - live adapter logic is validated with captured mock payloads

### Milestone E: Production Readiness
- Includes tasks: 18 to 20
- Outcome:
  - operators can monitor and control the engine
  - failure handling is tested
  - operational behavior is guarded by automated unit coverage

## 6. Highest-Priority Immediate Backlog

If implementation starts now, the first backlog should be:

1. `P0` Task 1: Project scaffolding
2. `P0` Task 2: Core domain model
3. `P0` Task 5: Build and test harness
4. `P0` Task 6: Event dispatcher / engine loop
5. `P0` Task 6A: Kafka transaction ingestion baseline
6. `P0` Task 8: Strategy interface and sample strategy
7. `P0` Task 9: Risk engine baseline
8. `P0` Task 10: Simulated execution engine

This sequence is the fastest path to an end-to-end runnable prototype.
