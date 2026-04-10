#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace trading::core {

// Declares the supported market-event categories used inside the engine.
enum class MarketEventType {
    book_snapshot,
    trade,
    ticker,
    timer
};

// Declares the supported order sides.
enum class OrderSide {
    buy,
    sell
};

// Declares the supported order types.
enum class OrderType {
    market,
    limit
};

// Declares the lifecycle states for orders handled by the engine.
enum class OrderStatus {
    created,
    pending_submit,
    acknowledged,
    partially_filled,
    filled,
    pending_cancel,
    canceled,
    rejected,
    expired
};

// Stores the basic venue and symbol identity for one tradable instrument.
struct Instrument {
    std::string instrument_id;
    std::string exchange;
    std::string symbol;
    std::string base_asset;
    std::string quote_asset;
    double tick_size {0.0};
    double lot_size {0.0};
};

// Stores one normalized inbound market event.
struct MarketEvent {
    std::string event_id;
    MarketEventType type {MarketEventType::trade};
    Instrument instrument;
    double price {0.0};
    double quantity {0.0};
    std::optional<double> bid_price;
    std::optional<double> ask_price;
    std::int64_t exchange_timestamp {0};
    std::int64_t receive_timestamp {0};
    std::int64_t process_timestamp {0};
};

// Stores one order request emitted by strategy logic.
struct OrderRequest {
    std::string request_id;
    std::string strategy_id;
    Instrument instrument;
    OrderSide side {OrderSide::buy};
    OrderType type {OrderType::limit};
    double quantity {0.0};
    std::optional<double> price;
};

// Stores one exchange or simulator update for an order.
struct OrderUpdate {
    std::string order_id;
    std::string client_order_id;
    OrderStatus status {OrderStatus::created};
    double filled_quantity {0.0};
    std::optional<std::string> reason;
};

// Stores one executed fill.
struct FillEvent {
    std::string fill_id;
    std::string order_id;
    Instrument instrument;
    OrderSide side {OrderSide::buy};
    double price {0.0};
    double quantity {0.0};
    double fee {0.0};
};

// Stores one current position view.
struct Position {
    std::string position_id;
    Instrument instrument;
    double net_quantity {0.0};
    double average_entry_price {0.0};
    double realized_pnl {0.0};
    double unrealized_pnl {0.0};
};

// Stores one account balance snapshot.
struct BalanceSnapshot {
    std::string asset;
    double total_balance {0.0};
    double available_balance {0.0};
};

// Stores one timer-triggered internal event.
struct TimerEvent {
    std::string timer_id;
    std::int64_t timestamp {0};
};

// Stores one normalized transaction command consumed from Kafka.
struct TransactionCommand {
    std::string transaction_id;
    std::string user_id;
    std::string account_id;
    std::string command_type;
    std::string instrument_symbol;
    double quantity {0.0};
    std::optional<double> price;
    std::string kafka_topic;
    int kafka_partition {0};
    std::int64_t kafka_offset {0};
};

// Stores one event emitted by the runtime dispatcher.
using EngineEvent = std::variant<MarketEvent, TimerEvent, TransactionCommand>;

}  // namespace trading::core
