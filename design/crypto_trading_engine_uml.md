# Crypto Trading Engine UML

## 1. Purpose

This document supplements the main design with UML-style diagrams that describe the planned implementation of the C++ crypto trading engine.

The diagrams are written in PlantUML.

## 2. Component Diagram

This diagram shows the main runtime components and their dependencies.

```plantuml
@startuml
skinparam componentStyle rectangle
left to right direction

package "Crypto Trading Engine" {
  component "Configuration Loader" as CFG
  component "Engine Controller" as CTRL
  component "Market Data Gateway" as MD
  component "Market State Store" as MS
  component "Transaction Ingestion" as INGEST
  component "Strategy Engine" as STRAT
  component "Risk Engine" as RISK
  component "Execution Engine" as EXEC
  component "Portfolio Service" as PORT
  component "Persistence Layer" as STORE
  component "Monitoring and Control" as MON
}

component "Crypto Exchange" as EX
queue "Kafka\n(Docker)" as KAFKA
database "Redis Cache\n(Docker)" as REDIS
database "PostgreSQL\n(Docker)" as POSTGRES

CFG --> CTRL
CTRL --> MD
CTRL --> INGEST
CTRL --> STRAT
CTRL --> RISK
CTRL --> EXEC
CTRL --> PORT
CTRL --> STORE
CTRL --> MON

EX --> MD
KAFKA --> INGEST
MD --> MS
INGEST --> STRAT
MS --> STRAT
MS --> RISK

STRAT --> RISK
RISK --> EXEC
EXEC --> EX

EX --> EXEC
EXEC --> PORT
EXEC --> STORE
MD --> STORE
STRAT --> STORE
RISK --> STORE
PORT --> STORE
STORE --> REDIS
STORE --> POSTGRES

MD --> MON
RISK --> MON
EXEC --> MON
PORT --> MON
@enduml
```

### Component Notes

- `Configuration Loader` reads startup configuration, risk limits, exchange credentials, and strategy parameters.
- `Engine Controller` wires the system together and owns lifecycle management.
- `Market Data Gateway` handles WebSocket subscriptions and normalizes exchange market payloads.
- `Market State Store` provides the latest in-memory market view for each instrument.
- `Transaction Ingestion` consumes Kafka transaction messages and feeds them into the engine one by one.
- `Strategy Engine` converts market events into order intents.
- `Risk Engine` evaluates intents before any live submission.
- `Execution Engine` maps approved intents to exchange API commands and tracks order lifecycle.
- `Portfolio Service` updates positions, balances, and PnL from fills and snapshots.
- `Persistence Layer` coordinates Redis cache writes and PostgreSQL durable writes.
- `Monitoring and Control` exposes metrics, health checks, and kill-switch controls.
- `Redis Cache` stores hot market and runtime state with low latency.
- `PostgreSQL` stores durable business entities and transaction history.
- `Kafka` is the ingress queue for transaction messages to be processed sequentially.

## 3. ER Diagram

This ER diagram models the core durable entities stored in PostgreSQL for trading, recovery, and replay.

```plantuml
@startuml
hide methods
hide stereotypes

entity EXCHANGE {
  *exchange_id : string
  --
  name : string
  venue_type : string
  rest_endpoint : string
  websocket_endpoint : string
}

entity ACCOUNT {
  *account_id : string
  --
  exchange_id : string <<FK>>
  user_id : string <<FK>>
  mode : string
  base_currency : string
}

entity USER {
  *user_id : string
  --
  email : string
  status : string
  created_ts : long
}

entity INSTRUMENT {
  *instrument_id : string
  --
  exchange_id : string <<FK>>
  symbol : string
  asset_class : string
  base_asset : string
  quote_asset : string
  tick_size : decimal
  lot_size : decimal
}

entity STRATEGY {
  *strategy_id : string
  --
  name : string
  version : string
  status : string
}

entity MARKET_EVENT {
  *market_event_id : string
  --
  exchange_id : string <<FK>>
  instrument_id : string <<FK>>
  event_type : string
  exchange_ts : long
  receive_ts : long
  process_ts : long
  payload_ref : string
}

entity ORDER {
  *order_id : string
  --
  client_order_id : string
  exchange_id : string <<FK>>
  instrument_id : string <<FK>>
  strategy_id : string <<FK>>
  side : string
  order_type : string
  price : decimal
  quantity : decimal
  filled_quantity : decimal
  status : string
  created_ts : long
}

entity ORDER_EVENT {
  *order_event_id : string
  --
  order_id : string <<FK>>
  event_type : string
  reason : string
  event_ts : long
}

entity FILL {
  *fill_id : string
  --
  order_id : string <<FK>>
  price : decimal
  quantity : decimal
  fee : decimal
  liquidity_flag : string
  fill_ts : long
}

entity POSITION {
  *position_id : string
  --
  account_id : string <<FK>>
  instrument_id : string <<FK>>
  strategy_id : string <<FK>>
  net_quantity : decimal
  avg_entry_price : decimal
  realized_pnl : decimal
  unrealized_pnl : decimal
  updated_ts : long
}

entity BALANCE_SNAPSHOT {
  *snapshot_id : string
  --
  account_id : string <<FK>>
  asset : string
  total_balance : decimal
  available_balance : decimal
  snapshot_ts : long
}

entity TRANSACTION {
  *transaction_id : string
  --
  account_id : string <<FK>>
  order_id : string <<FK>>
  fill_id : string <<FK>>
  kafka_topic : string
  kafka_partition : int
  kafka_offset : long
  transaction_type : string
  amount : decimal
  asset : string
  processing_status : string
  transaction_ts : long
}

entity AUDIT_LOG {
  *audit_log_id : string
  --
  account_id : string <<FK>>
  entity_type : string
  entity_id : string
  action : string
  created_ts : long
}

entity TRANSACTION_CONSUMER_OFFSET {
  *consumer_offset_id : string
  --
  consumer_group : string
  kafka_topic : string
  kafka_partition : int
  committed_offset : long
  updated_ts : long
}

USER ||--o{ ACCOUNT : owns
EXCHANGE ||--o{ INSTRUMENT : lists
STRATEGY ||--o{ ORDER : creates
EXCHANGE ||--o{ ORDER : routes
INSTRUMENT ||--o{ ORDER : traded_on
ORDER ||--o{ FILL : generates
INSTRUMENT ||--o{ MARKET_EVENT : produces
EXCHANGE ||--o{ MARKET_EVENT : sources
INSTRUMENT ||--o{ POSITION : tracked_as
STRATEGY ||--o{ POSITION : owns_exposure
ORDER ||--o{ ORDER_EVENT : transitions
ACCOUNT ||--o{ POSITION : holds
ACCOUNT ||--o{ BALANCE_SNAPSHOT : records
EXCHANGE ||--o{ ACCOUNT : serves
ACCOUNT ||--o{ TRANSACTION : records
ORDER ||--o{ TRANSACTION : settles
FILL ||--o{ TRANSACTION : produces
ACCOUNT ||--o{ AUDIT_LOG : owns
ACCOUNT ||--o{ TRANSACTION_CONSUMER_OFFSET : tracks
@enduml
```

### ER Notes

- The ER model represents PostgreSQL durable storage.
- `MARKET_EVENT` is stored so the engine can replay normalized input rather than depend on raw exchange payloads alone.
- `ORDER_EVENT` captures the order state machine as an append-only history.
- `POSITION` is the current derived state, while `FILL` and `ORDER_EVENT` form the event history behind it.
- `ACCOUNT` is included so the design can later support multiple accounts or modes without redesigning storage.
- Redis is intentionally excluded from the ER model because it is used as a cache rather than the durable system of record.
- Kafka is represented through durable transaction metadata and consumer offset tracking stored in PostgreSQL.

## 4. Sequence Diagram

This sequence diagram shows the main runtime flow from market data receipt to order fill and portfolio update.

```plantuml
@startuml
autonumber

participant Exchange as EX
participant "Market Data Gateway" as MD
participant "Market State Store" as MS
queue Kafka as KAFKA
participant "Transaction Ingestion" as INGEST
participant "Strategy Engine" as STR
participant "Risk Engine" as RK
participant "Execution Engine" as EE
participant "Portfolio Service" as PS
participant "Persistence Layer" as DB
database "Redis Cache" as REDIS
database "PostgreSQL" as PG
participant Monitoring as MON

EX -> MD : Market data message
MD -> MD : Parse and normalize payload
MD -> MS : Update instrument market state
MD -> DB : Store normalized market event
DB -> REDIS : Cache latest market state
DB -> PG : Persist selected market event or audit record
MD -> MON : Emit feed metrics

KAFKA -> INGEST : Transaction message
INGEST -> INGEST : Validate schema and ordering
INGEST -> DB : Persist transaction receipt
DB -> PG : Write transaction record
INGEST -> STR : Submit TransactionCommandEvent

MS -> STR : Publish normalized market event
STR -> STR : Evaluate strategy logic
STR -> RK : Submit OrderRequest
STR -> DB : Persist order intent
DB -> PG : Write order intent record

RK -> RK : Validate risk limits
alt Risk approved
  RK -> EE : Forward approved order
  RK -> DB : Persist risk approval
  DB -> PG : Write risk decision
  EE -> EX : Submit order
  EE -> DB : Persist submit event
  DB -> PG : Write order event
  EE -> MON : Emit execution metrics

  EX --> EE : Ack / Reject / Fill update
  EE -> DB : Persist order event
  DB -> PG : Write order lifecycle update

  alt Order acknowledged
    EE -> STR : OrderUpdate
    EE -> MON : Emit order state metric
  else Order rejected
    EE -> STR : Reject update
    EE -> MON : Emit reject metric
  else Fill received
    EE -> PS : FillEvent
    PS -> PS : Recalculate position and PnL
    PS -> DB : Persist position snapshot
    DB -> REDIS : Refresh hot position state
    DB -> PG : Persist fill and position snapshot
    PS -> STR : Fill notification
    PS -> MON : Emit PnL and exposure metrics
  end
  INGEST -> DB : Mark transaction processed
  DB -> PG : Update transaction status and consumer offset
else Risk rejected
  RK -> DB : Persist risk rejection
  DB -> PG : Write risk rejection
  RK -> STR : Risk rejection
  RK -> MON : Emit reject metric
  INGEST -> DB : Mark transaction rejected
  DB -> PG : Update transaction status and consumer offset
end
@enduml
```

## 5. Implementation Mapping

The diagrams map directly to the planned code modules:

- `Component Diagram` maps to `core`, `market_data`, `strategy`, `risk`, `execution`, `portfolio`, `storage`, and `monitoring`.
- `ER Diagram` maps to persisted domain state and replay/recovery support.
- `Sequence Diagram` maps to the event-driven runtime behavior that the engine loop and adapters must implement.

## 6. Planned Deliverables From This Design

The implementation implied by these diagrams includes:

- Core domain models for instruments, market events, orders, fills, positions, and balances.
- A normalized event pipeline from market data ingestion to strategy and risk evaluation.
- An order state machine with durable event history.
- A portfolio accounting subsystem driven by fills.
- Persistent logs and snapshots to support replay and recovery.
- Monitoring hooks for connectivity, risk, execution, and PnL visibility.
