# Crypto Trading Engine

## Overview

This project is a C++20 crypto trading engine prototype designed around an event-driven architecture.

The current implementation provides:

- core domain models for market events, orders, fills, positions, and transaction commands
- a deterministic event dispatcher
- a market state store for real-time instrument state
- a Kafka-style transaction ingestion pipeline that processes transactions one by one
- a Kafka transaction producer application that reads JSONL transaction requests and publishes them to Kafka
- storage abstractions plus native Redis, PostgreSQL, and Kafka adapters
- an end-to-end simulated trading path with strategy, risk, execution, portfolio, and replay support
- local Docker infrastructure definitions for Redis, PostgreSQL, and Kafka
- unit tests for the current simulation and infrastructure boundary layer

The current codebase is beyond the initial scaffold stage, but it is not yet a full live trading engine. Live exchange connectivity, reconciliation, and production runtime wiring are still incomplete.

## Quick Start

From the project root:

```bash
docker compose -f wall/docker-compose.yml up -d
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/wall_benchmark
./build/wall
./build/wall_tx_producer configs/sample_transactions.jsonl
```

To stop the local environment:

```bash
docker compose -f wall/docker-compose.yml down
```

## Components

### Application

Files:

- [main.cpp](/Users/yangruiguo/Documents/wall/main.cpp)
- [application.cpp](/Users/yangruiguo/Documents/wall/src/app/application.cpp)

Responsibility:

- starts the program
- loads configuration
- validates startup settings
- prints a minimal startup banner for the current scaffold

### Core

Files:

- [types.hpp](/Users/yangruiguo/Documents/wall/include/core/types.hpp)
- [clock.hpp](/Users/yangruiguo/Documents/wall/include/core/clock.hpp)
- [event_dispatcher.hpp](/Users/yangruiguo/Documents/wall/include/core/event_dispatcher.hpp)

Responsibility:

- defines the shared engine domain types
- provides a deterministic dispatcher for runtime events
- provides clock abstractions for time-sensitive logic and tests

### Configuration

Files:

- [app_config.hpp](/Users/yangruiguo/Documents/wall/include/config/app_config.hpp)
- [config_loader.hpp](/Users/yangruiguo/Documents/wall/include/config/config_loader.hpp)
- [config_loader.cpp](/Users/yangruiguo/Documents/wall/src/config/config_loader.cpp)
- [development.cfg](/Users/yangruiguo/Documents/wall/configs/development.cfg)

Responsibility:

- loads runtime configuration from a local config file
- supports environment overrides for selected values
- validates Redis, PostgreSQL, and Kafka settings

### Market Data

Files:

- [market_state_store.hpp](/Users/yangruiguo/Documents/wall/include/market_data/market_state_store.hpp)
- [market_state_store.cpp](/Users/yangruiguo/Documents/wall/src/market_data/market_state_store.cpp)

Responsibility:

- stores the latest market state per instrument
- tracks bid, ask, trade, and last update time
- supports stale-data checks

### Transaction Ingestion

Files:

- [transaction_consumer.hpp](/Users/yangruiguo/Documents/wall/include/ingestion/transaction_consumer.hpp)
- [transaction_ingestor.hpp](/Users/yangruiguo/Documents/wall/include/ingestion/transaction_ingestor.hpp)
- [transaction_consumer.cpp](/Users/yangruiguo/Documents/wall/src/ingestion/transaction_consumer.cpp)
- [transaction_ingestor.cpp](/Users/yangruiguo/Documents/wall/src/ingestion/transaction_ingestor.cpp)

Responsibility:

- represents the Kafka consumer boundary
- consumes inbound transaction commands
- validates transaction messages
- processes them sequentially, one by one
- persists transaction receipt and processing status through storage interfaces
- publishes valid transaction commands into the engine dispatcher

### Storage

Files:

- [storage_interfaces.hpp](/Users/yangruiguo/Documents/wall/include/storage/storage_interfaces.hpp)
- [storage_interfaces.cpp](/Users/yangruiguo/Documents/wall/src/storage/storage_interfaces.cpp)

Responsibility:

- defines repository and cache boundaries
- represents PostgreSQL-style durable transaction persistence
- represents Redis-style runtime caching
- currently provides in-memory implementations for scaffolding and tests

### Docker Infrastructure

Files:

- [docker-compose.yml](/Users/yangruiguo/Documents/wall/docker/docker-compose.yml)

Responsibility:

- starts Redis
- starts PostgreSQL
- starts Zookeeper and Kafka

## Infrastructure Roles

### Redis

Used for:

- low-latency cache
- hot runtime state
- current market and transaction status caching

### PostgreSQL

Used for:

- durable business data
- user and account records
- transaction history
- orders, fills, positions, and audit-friendly records

### Kafka

Used for:

- inbound transaction queue
- ordered transaction consumption
- sequential processing by the trading engine

## Project Structure

```text
.
├── CMakeLists.txt
├── README.md
├── configs/
│   └── development.cfg
├── design/
├── docker/
│   └── docker-compose.yml
├── include/
│   ├── app/
│   ├── config/
│   ├── core/
│   ├── ingestion/
│   ├── market_data/
│   └── storage/
├── src/
│   ├── app/
│   ├── config/
│   ├── ingestion/
│   ├── market_data/
│   └── storage/
└── tests/
    └── test_main.cpp
```

## Prerequisites

You need:

- CMake 3.20 or later
- a C++20 compiler
- Docker
- Docker Compose support

## Step-By-Step Startup Guide

This section describes the recommended startup order for the whole local environment.

### Step 1: Start Docker Infrastructure

The project uses these Docker components:

- Kafka for inbound transaction messages
- Zookeeper for Kafka coordination
- Redpanda Console for Kafka topic and message inspection
- Redis for low-latency cache state
- PostgreSQL for durable data

From the project root, start all containers:

```bash
docker compose -f docker/docker-compose.yml up -d
```

Check container status:

```bash
docker compose -f docker/docker-compose.yml ps
```

Expected services:

- `redis`
- `postgres`
- `zookeeper`
- `kafka`
- `redpanda-console`

To stop the containers later:

```bash
docker compose -f docker/docker-compose.yml down
```

### Step 2: Review Local Configuration

The default runtime config is:

- [development.cfg](/Users/yangruiguo/Documents/wall/configs/development.cfg)

It contains:

- Redis host and port
- PostgreSQL host, port, database, user, and password
- Kafka broker list, topic, and consumer group
- exchange name and instrument list

Optional environment overrides:

- `TRADING_MODE`
- `TRADING_POSTGRES_PASSWORD`
- `TRADING_KAFKA_TOPIC`

Example:

```bash
export TRADING_MODE=paper
export TRADING_KAFKA_TOPIC=trading-transactions
```

### Step 3: Build The Program

From the project root:

```bash
cmake -S . -B build
cmake --build build
```

This creates:

- `build/wall`
- `build/wall_tests`
- `build/wall_benchmark`
- `build/wall_replay`
- `build/wall_tx_producer`

### Step 4: Run Unit Tests

Run the test suite before starting the program:

```bash
ctest --test-dir build --output-on-failure
```

The current tests verify:

- core model construction
- configuration loading and environment overrides
- event dispatcher ordering
- market state updates and stale checks
- strategy, risk, execution, and portfolio behavior
- replay behavior
- Kafka-style sequential transaction ingestion
- producer payload serialization and JSON transaction parsing
- invalid transaction rejection behavior

### Step 5: Run The Ingestion Benchmark

Run the dedicated benchmark executable from the project root:

```bash
./build/wall_benchmark
```

This benchmark:

- exercises `TransactionIngestor::process_next()` against in-memory boundaries
- prints per-step timings for poll, persistence, cache updates, dispatch, and commit
- reports average and max durations for each measured step

The default run uses `2000` transactions. To change the sample size, pass the iteration count as the first argument:

```bash
./build/wall_benchmark 10000
```

Example output shape:

```text
[BENCH] transaction_ingestor_step_benchmark iterations=2000
[BENCH] process_next_total calls=2000 avg_us=66.121 max_us=622.958 total_ms=132.243
[BENCH] step_1_poll calls=2000 avg_us=0.079 max_us=0.416 total_ms=0.159
[BENCH] step_2_save_received calls=2000 avg_us=0.604 max_us=200.167 total_ms=1.208
```

Use this benchmark to compare relative step costs inside the ingestion path. Because it runs in-process against local machine resources, the exact numbers will vary by hardware and system load.

### Step 6: Open Redpanda Console For Kafka Inspection

After the Docker stack is running, open Redpanda Console in your browser:

```text
http://localhost:8080
```

Use it to inspect Kafka locally:

- open the `Topics` page to see available topics
- select `trading-transactions` to inspect partitions, offsets, and retained messages
- use the consumer-group view to inspect the `trading-engine` group when the engine is running

This does not replace Kafka in the stack. It is only a UI connected to the existing local broker at `kafka:29092`.

### Step 7: Publish Transactions To Kafka

Sample transaction fixtures are stored in:

- [sample_transactions.jsonl](/Users/yangruiguo/Documents/wall/configs/sample_transactions.jsonl)

Publish the default sample data into Kafka with the C++ producer application:

```bash
./build/wall_tx_producer configs/sample_transactions.jsonl
```

Or stream transactions from standard input:

```bash
cat configs/sample_transactions.jsonl | ./build/wall_tx_producer
```

The producer application:

- reads line-delimited JSON transaction requests
- parses them into `TransactionCommand` values
- serializes them into the key-value payload schema consumed by the Kafka ingestion path
- publishes a Kafka message key as `transaction_id + yyyyMMddHHmmSS` in UTC
- publishes them into the configured Kafka topic using `librdkafka`

The existing shell helper is still available for container-driven manual publishing:

```bash
bash scripts/send_sample_transactions.sh
```

### Step 6: Start The Program

From the project root:

```bash
./build/wall
```

Expected behavior in the current scaffold:

- the program loads [development.cfg](/Users/yangruiguo/Documents/wall/configs/development.cfg)
- validates Redis, PostgreSQL, and Kafka configuration
- runs a single mock-driven simulation cycle
- logs processed market, transaction, and timer events
- writes a replayable event log

The current engine executable is still a development-oriented bootstrap, not a long-running live trading process.

Startup infrastructure behavior:

- when PostgreSQL, Redis, and Kafka are reachable, `wall` uses the native adapters
- when native infrastructure initialization fails, `wall` falls back to:
  - file-backed local repositories
  - in-memory cache adapters
  - mock-driven startup processing without Kafka resume

In fallback mode, startup logs include:

- `message=infrastructure_fallback_enabled`
- `infrastructure_mode=fallback`

This lets local development continue even when Docker services are not running.

### Step 9: Shut Everything Down

When you are done:

```bash
docker compose -f docker/docker-compose.yml down
```

## Docker Components Summary

### Kafka

Container role:

- receives inbound transaction messages
- acts as the source queue the trading engine will consume from

Current status:

- Docker service is available
- C++ transaction producer application is available
- native Kafka consumer and producer adapters are implemented
- live engine wiring to consume Kafka continuously is still incomplete
- the main app falls back to local startup mode when Kafka is unavailable

### Redpanda Console

Container role:

- provides a browser UI for inspecting Kafka topics, partitions, offsets, and consumer groups
- helps verify that transaction messages were published into Kafka during local development

Current status:

- Docker service is available
- connects to the existing Kafka broker inside the local Docker network
- exposed locally at `http://localhost:8080`

### Redis

Container role:

- stores low-latency cache data
- intended for hot market state and runtime status

Current status:

- Docker service is available
- native Redis-backed cache adapters are implemented
- the main runtime is not yet fully wired to use Redis in end-to-end live mode
- the main app falls back to in-memory cache adapters when Redis is unavailable

### PostgreSQL

Container role:

- stores durable system-of-record data
- intended for users, accounts, transactions, fills, positions, and audit history

Current status:

- Docker service is available
- native PostgreSQL-backed repository adapters are implemented
- the main runtime is not yet fully wired to use PostgreSQL in end-to-end live mode
- the main app falls back to file-backed local repositories when PostgreSQL is unavailable

## How To Send Sample Transactions To Kafka

Sample transaction fixtures are stored in:

- [sample_transactions.jsonl](/Users/yangruiguo/Documents/wall/configs/sample_transactions.jsonl)

A C++ producer executable is available after building the project:

- `build/wall_tx_producer`

A shell helper is also available at:

- [send_sample_transactions.sh](/Users/yangruiguo/Documents/wall/scripts/send_sample_transactions.sh)

Before using it, start the Docker infrastructure:

```bash
docker compose -f wall/docker-compose.yml up -d
```

Then publish the default sample transactions with the C++ producer:

```bash
./build/wall_tx_producer configs/sample_transactions.jsonl
```

You can also use standard input:

```bash
cat configs/sample_transactions.jsonl | ./build/wall_tx_producer
```

The producer application:

- reads line-delimited JSON transactions from a local file
- parses each line into a transaction command
- publishes each command to the configured Kafka topic using `librdkafka`

After publishing, you can verify the messages in Redpanda Console:

- open `http://localhost:8080`
- navigate to the `trading-transactions` topic
- inspect the latest records, offsets, and message keys

The shell helper script:

- opens `kafka-console-producer` inside the Kafka container
- writes each line into the Kafka topic without C++ parsing

## Configuration

The default config file is:

- [development.cfg](/Users/yangruiguo/Documents/wall/configs/development.cfg)

Supported environment overrides currently include:

- `TRADING_MODE`
- `TRADING_POSTGRES_PASSWORD`
- `TRADING_KAFKA_TOPIC`

Example:

```bash
export TRADING_MODE=live
export TRADING_KAFKA_TOPIC=custom-transactions
./build/wall
```

## Current Status

Implemented:

- project scaffold
- config loader
- event dispatcher
- market state store
- sample strategy
- baseline risk engine
- simulated execution engine
- portfolio and PnL service
- replay service
- Kafka transaction ingestion interfaces and native Kafka consumer client
- native Kafka transaction producer application
- in-memory repository and cache scaffolding
- native Redis cache adapters
- native PostgreSQL repository adapters
- startup reconciliation and recovery scaffolding
- startup fallback from native infrastructure to local file-backed and in-memory adapters
- local Docker stack
- unit tests with mock data

Not implemented yet:

- live exchange adapter
- long-running live runtime wiring
- production-grade operational controls
- metrics and alerting

## Next Steps

The next development slice should add:

1. replace mock sources with real live market-data and transaction ingestion loops
2. replace mock event sources with real live ingestion and market-data flows
3. add exchange-authoritative reconciliation and restart recovery
4. add a first real exchange adapter
5. add operational controls and metrics
