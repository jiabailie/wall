# Crypto Trading Engine

## Overview

This project is a C++20 crypto trading engine scaffold designed around an event-driven architecture.

The current implementation provides:

- core domain models for market events, orders, fills, positions, and transaction commands
- a deterministic event dispatcher
- a market state store for real-time instrument state
- a Kafka-style transaction ingestion pipeline that processes transactions one by one
- storage abstractions for Redis-style cache usage and PostgreSQL-style durable persistence
- local Docker infrastructure definitions for Redis, PostgreSQL, and Kafka
- unit tests with mock data for the current foundation layer

The current codebase is the Phase 1 foundation plus the initial Kafka ingestion slice. It is not yet a full live trading engine.

## Quick Start

From the project root:

```bash
docker compose -f docker/docker-compose.yml up -d
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/wall
```

To stop the local environment:

```bash
docker compose -f docker/docker-compose.yml down
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
- Kafka-style sequential transaction ingestion
- invalid transaction rejection behavior

### Step 5: Optionally Send Sample Transactions To Kafka

Sample transaction fixtures are stored in:

- [sample_transactions.jsonl](/Users/yangruiguo/Documents/wall/configs/sample_transactions.jsonl)

Publish the default sample data into Kafka:

```bash
bash scripts/send_sample_transactions.sh
```

Or publish a custom file and topic:

```bash
bash scripts/send_sample_transactions.sh configs/sample_transactions.jsonl trading-transactions
```

This step is optional for the current scaffold, because the current C++ program does not yet include a real Kafka client. It is still useful for validating the local Kafka container and preparing for the next implementation step.

### Step 6: Start The Program

From the project root:

```bash
./build/wall
```

Expected behavior in the current scaffold:

- the program loads [development.cfg](/Users/yangruiguo/Documents/wall/configs/development.cfg)
- validates Redis, PostgreSQL, and Kafka configuration
- prints a startup line with mode, exchange, and Kafka topic

Example:

```text
Trading engine scaffold started in mode=paper, exchange=binance, kafka_topic=trading-transactions
```

### Step 7: Shut Everything Down

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
- sample producer helper is available
- real C++ Kafka consumer is not implemented yet

### Redis

Container role:

- stores low-latency cache data
- intended for hot market state and runtime status

Current status:

- Docker service is available
- real C++ Redis client is not implemented yet

### PostgreSQL

Container role:

- stores durable system-of-record data
- intended for users, accounts, transactions, fills, positions, and audit history

Current status:

- Docker service is available
- real C++ PostgreSQL repository is not implemented yet

## How To Send Sample Transactions To Kafka

Sample transaction fixtures are stored in:

- [sample_transactions.jsonl](/Users/yangruiguo/Documents/wall/configs/sample_transactions.jsonl)

A helper script is available at:

- [send_sample_transactions.sh](/Users/yangruiguo/Documents/wall/scripts/send_sample_transactions.sh)

Before using it, start the Docker infrastructure:

```bash
docker compose -f docker/docker-compose.yml up -d
```

Then publish the default sample transactions:

```bash
bash scripts/send_sample_transactions.sh
```

You can also publish a different file or topic:

```bash
bash scripts/send_sample_transactions.sh configs/sample_transactions.jsonl trading-transactions
```

The helper script:

- reads line-delimited JSON transactions from a local file
- opens `kafka-console-producer` inside the Kafka container
- writes each line into the Kafka topic

Current note:

- the C++ scaffold does not yet connect to a real Kafka client
- this helper is for local infrastructure testing and manual message injection

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
- Kafka-style transaction ingestion interfaces
- in-memory repository and cache scaffolding
- local Docker stack
- unit tests with mock data

Not implemented yet:

- real Kafka client integration
- real Redis client integration
- real PostgreSQL repository implementation
- strategy engine
- risk engine
- simulated execution engine
- live exchange adapter

## Next Steps

The next development slice should add:

1. strategy interfaces
2. risk validation
3. simulated execution
4. portfolio accounting
5. real Redis, PostgreSQL, and Kafka adapter implementations
