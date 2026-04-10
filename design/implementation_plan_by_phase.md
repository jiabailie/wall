# Crypto Trading Engine Implementation Plan by Phase

## 1. Purpose

This document converts the task backlog into an execution plan with several phases. Each phase has a clear objective, scope, exit criteria, and testing expectations.

The plan is designed to reduce risk by building the engine in vertical slices:

- foundation first
- simulation before live trading
- observability before production use
- tests included in every phase

## 2. Planning Principles

- Complete `P0` work before `P1` unless there is a clear dependency reason to change order.
- Keep each phase independently testable.
- Prefer one runnable milestone at the end of each phase.
- Add unit tests with mock data in the same phase as the implementation.
- Do not connect to a live exchange until simulation, risk, and persistence are stable.
- Standardize local infrastructure on Redis in Docker, PostgreSQL in Docker, and Kafka in Docker from the beginning.

## 3. Phase Overview

| Phase | Name | Focus | Main Outcome |
|---|---|---|---|
| 1 | Foundation | project skeleton and shared types | buildable codebase with test harness |
| 2 | Core Runtime | event pipeline and in-memory state | deterministic engine loop |
| 3 | Trading Simulation | strategy, risk, execution, portfolio | end-to-end simulated trading |
| 4 | Durability | persistence and replay | recoverable and replayable engine |
| 5 | Live Integration | exchange adapter and reconciliation | first real exchange support |
| 6 | Hardening | metrics, controls, failure handling | safer production-ready behavior |
| 7 | Expansion | multi-strategy, multi-exchange, backtest | post-v1 capability growth |

## 4. Detailed Phase Plan

### Phase 1: Foundation

#### Objective

Create the project structure, core shared models, config system, logging, test harness, and local infrastructure baseline.

#### Tasks

- Task 1. Project scaffolding
- Task 2. Core domain model
- Task 3. Configuration system
- Task 4. Logging and error handling
- Task 5. Build and test harness

#### Implementation Notes

- Create a modular directory layout that already matches the architecture document.
- Establish naming conventions for domain types and module boundaries.
- Keep the first version of domain models simple and serialization-friendly.
- Add a single executable target and at least one test target.
- Add Docker Compose or equivalent local definitions for Redis, PostgreSQL, and Kafka.

#### Required Unit Tests

- smoke test for the build and test runner
- model construction tests for core structs and enums
- config parsing tests using sample config files
- logger initialization and error classification tests
- configuration tests for Redis, PostgreSQL, and Kafka connection settings

#### Exit Criteria

- the project builds cleanly
- the test suite runs from CMake
- shared domain types are available to all modules
- configuration validation works on valid and invalid examples
- local infrastructure configuration is defined for Redis, PostgreSQL, and Kafka

#### Deliverable

A buildable C++ project skeleton with a working automated test harness and local Docker infrastructure definitions.

### Phase 2: Core Runtime

#### Objective

Implement the event-driven engine loop, Kafka transaction ingestion, and the in-memory market state store, with storage abstractions ready for Redis-backed caching.

#### Tasks

- Task 6. Event dispatcher / engine loop
- Task 7. Market state store

#### Implementation Notes

- Use deterministic event ordering.
- Keep runtime coordination simple, ideally a single logical event-processing path.
- Make timestamps injectable for testing.
- Keep the market-state interface independent from the concrete cache backend.
- Process Kafka transactions sequentially through the same deterministic pipeline.

#### Required Unit Tests

- event dispatch ordering tests with synthetic events
- Kafka ingestion ordering tests with fixed transaction fixtures
- timer event tests with controlled clocks
- market state update tests for bids, asks, and trades
- stale-market detection tests using fixed timestamps
- out-of-order market event safety tests

#### Exit Criteria

- synthetic market events can flow through the dispatcher
- Kafka transaction messages can be consumed and processed one by one
- market state is updated correctly
- stale data is detectable from state queries
- all runtime behavior is covered by deterministic tests

#### Deliverable

A deterministic engine runtime that can ingest market events and process Kafka transactions sequentially.

### Phase 3: Trading Simulation

#### Objective

Implement the minimum complete trading workflow: strategy, risk checks, simulated execution, and portfolio accounting.

#### Tasks

- Task 8. Strategy interface and sample strategy
- Task 9. Risk engine baseline
- Task 10. Simulated execution engine
- Task 11. Portfolio and PnL service

#### Implementation Notes

- Start with one simple sample strategy.
- Keep risk decisions explicit and explainable.
- Simulated execution should model realistic lifecycle transitions, including partial fills.
- Portfolio logic should be deterministic and independent from exchange code.
- Keep portfolio and order persistence behind interfaces that can target PostgreSQL later in the same design.
- Strategy input should accept both market-triggered events and Kafka transaction commands where required by the workflow.

#### Required Unit Tests

- strategy decision tests with synthetic market events
- no-trade tests for non-trigger conditions
- risk approval and rejection tests
- simulated lifecycle tests for ack, partial fill, full fill, and cancel
- position, average price, fee, and PnL tests using fixed fill sequences

#### Exit Criteria

- a market event can trigger strategy output
- risk can accept or reject orders
- approved orders move through a simulated lifecycle
- fills update positions and PnL correctly

#### Deliverable

A runnable simulated trading engine that works end to end without a live exchange.

### Phase 4: Durability

#### Objective

Add Redis cache integration, PostgreSQL persistence, and Kafka transaction-state persistence so sessions can be audited, debugged, and reconstructed.

#### Tasks

- Task 12. Persistence baseline
- Task 13. Replay tool

#### Implementation Notes

- Start with append-only event logs and snapshot files.
- Persist normalized events rather than raw exchange payloads whenever possible.
- Keep replay deterministic and isolated from wall-clock time.
- Use Redis for current market state and hot operational state.
- Use PostgreSQL for users, accounts, orders, fills, balances, positions, transactions, and replay-relevant records.
- Persist Kafka transaction receipt and processing status in PostgreSQL for recovery.

#### Required Unit Tests

- Redis cache write and read tests using isolated fixtures
- PostgreSQL repository write and read tests using deterministic fixtures
- Kafka transaction metadata persistence tests with deterministic topic, partition, and offset values
- replay tests for fixed event sessions
- invalid-record handling tests

#### Exit Criteria

- Redis cache is updated correctly for hot runtime state
- PostgreSQL stores durable trading and business records correctly
- Kafka transaction processing status is recoverable after restart
- persisted sessions can be replayed
- replay reproduces expected state from fixed inputs

#### Deliverable

A replayable engine with Redis cache support, PostgreSQL-backed durability, and Kafka transaction recovery metadata.

### Phase 5: Live Integration

#### Objective

Connect the engine to a first real exchange through stable internal adapter interfaces.

#### Tasks

- Task 14. Exchange abstraction
- Task 15. Market data adapter for first exchange
- Task 16. Live order execution adapter
- Task 17. Reconciliation and recovery

#### Implementation Notes

- Finalize adapter interfaces before writing exchange-specific code.
- Keep exchange normalization at the boundary.
- Use captured payload fixtures and fake clocks in tests.
- Make startup reconciliation mandatory before trading is enabled.
- Warm Redis from PostgreSQL or exchange snapshots during startup reconciliation where needed.
- Resume Kafka consumption from the intended committed position after recovery.

#### Required Unit Tests

- adapter contract tests using stubs
- market data parser tests from captured exchange samples
- request-building and signing tests with deterministic fixtures
- execution report normalization tests
- recovery tests with mock persisted state and exchange snapshots
- Redis cache warmup tests from durable state fixtures
- Kafka offset recovery tests with deterministic consumer metadata

#### Exit Criteria

- one exchange can provide live normalized market data
- orders can be submitted and tracked through the adapter layer
- restart recovery can reconstruct open orders, balances, and positions
- all live-integration logic is testable without real network access

#### Deliverable

The first exchange-connected version of the engine with controlled recovery behavior.

### Phase 6: Hardening

#### Objective

Make the engine safer to operate with monitoring, controls, and failure-path coverage.

#### Tasks

- Task 18. Metrics and health checks
- Task 19. Operational controls
- Task 20. Failure-path testing

#### Implementation Notes

- Health reporting should be driven by explicit state, not logs alone.
- Kill switch and pause logic should be centralized.
- Failure handling should prefer safe rejection over undefined behavior.

#### Required Unit Tests

- metric counter and gauge update tests
- health-state transition tests
- pause and kill-switch behavior tests
- disconnect, stale-feed, reject, duplicate-fill, and malformed-payload tests

#### Exit Criteria

- the engine exposes meaningful operational state
- trading can be paused or disabled safely
- failure paths leave the engine in a controlled state

#### Deliverable

A safer operational engine suitable for controlled live usage.

### Phase 7: Expansion

#### Objective

Extend the engine after the first stable release.

#### Tasks

- Task 21. Storage upgrade
- Task 22. Multi-strategy support
- Task 23. Multi-exchange support
- Task 24. Historical backtesting mode

#### Implementation Notes

- Expand only after the single-strategy, single-exchange core is stable.
- Preserve the same internal event model across live trading, simulation, and backtesting.
- Keep strategy isolation explicit if multiple strategies are introduced.

#### Required Unit Tests

- PostgreSQL read/write and migration tests
- Redis key lifecycle and cache invalidation tests
- Kafka consumer-group and retry-policy tests
- multi-strategy isolation tests
- multi-exchange symbol normalization and routing tests
- deterministic backtest replay and PnL tests

#### Exit Criteria

- the new capability does not break the stable v1 path
- each added feature is independently testable with deterministic fixtures

#### Deliverable

A broader platform that extends beyond the first production milestone.

## 5. Recommended Timeline Order

The recommended implementation sequence is:

1. Phase 1: Foundation
2. Phase 2: Core Runtime
3. Phase 3: Trading Simulation
4. Phase 4: Durability
5. Phase 5: Live Integration
6. Phase 6: Hardening
7. Phase 7: Expansion

This order minimizes risk because it proves correctness locally before introducing exchange dependencies.

## 6. Phase Gates

Each phase should only begin when the previous phase gate is passed.

### Gate A

Before starting Phase 2:

- build passes
- tests pass
- shared domain types are stable enough for downstream modules

### Gate B

Before starting Phase 3:

- event dispatch is deterministic
- market state queries are correct under mock inputs

### Gate C

Before starting Phase 4:

- simulated strategy-to-portfolio flow works end to end
- risk rejection paths are already covered by tests

### Gate D

Before starting Phase 5:

- persistence format is stable enough for replay
- replay can reproduce a simulated session

### Gate E

Before starting Phase 6:

- live adapter logic passes fixture-based tests
- recovery behavior is defined and testable

### Gate F

Before starting Phase 7:

- production-readiness controls and failure tests are in place

## 7. Immediate Execution Plan

If implementation begins now, the practical next sequence is:

1. Complete Phase 1 fully, including tests.
2. Implement Phase 2 with deterministic event and clock abstractions.
3. Build a minimal but complete Phase 3 simulated trading path.
4. Add persistence and replay before any real exchange work.
5. Start Phase 5 only after the simulator is stable and test-covered.

## 8. Summary

The recommended implementation plan uses seven phases. The first four phases build a correct and testable trading core without live exchange risk. The fifth and sixth phases add real connectivity and operational safety. The final phase expands the platform after the single-exchange, single-strategy version is stable.
