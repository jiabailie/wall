#pragma once

#include "core/types.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading::storage {

// Stores one persisted transaction-processing record.
struct TransactionRecord {
    std::string transaction_id;
    std::string status;
    std::string kafka_topic;
    int kafka_partition {0};
    std::int64_t kafka_offset {0};
};

// Stores one restart checkpoint per Kafka topic partition.
struct KafkaCheckpoint {
    std::string topic;
    int partition {0};
    std::int64_t offset {0};
};

// Stores one persisted order record.
struct OrderRecord {
    std::string order_id;
    std::string client_order_id;
    std::string strategy_id;
    std::string instrument_id;
    trading::core::OrderSide side {trading::core::OrderSide::buy};
    trading::core::OrderType order_type {trading::core::OrderType::limit};
    trading::core::OrderStatus status {trading::core::OrderStatus::created};
    double quantity {0.0};
    std::optional<double> price;
    double filled_quantity {0.0};
};

// Stores one cached market snapshot record.
struct MarketSnapshot {
    std::string instrument_id;
    std::optional<double> best_bid;
    std::optional<double> best_ask;
    std::optional<double> last_trade_price;
    std::optional<double> last_trade_quantity;
    std::int64_t last_process_timestamp {0};
};

// Defines the durable repository boundary for transaction records.
class ITransactionRepository {
public:
    virtual ~ITransactionRepository() = default;

    // Persists the inbound transaction receipt before engine processing.
    virtual void save_received(const trading::core::TransactionCommand& command) = 0;

    // Persists the final processing status after engine handling completes.
    virtual void save_processed(const std::string& transaction_id, const std::string& status) = 0;

    // Returns all durable transaction-processing records.
    [[nodiscard]] virtual std::vector<TransactionRecord> all_records() const = 0;
};

// Defines the cache boundary for hot transaction state.
class ITransactionCache {
public:
    virtual ~ITransactionCache() = default;

    // Caches the latest transaction status for fast runtime lookups.
    virtual void set_status(const std::string& transaction_id, const std::string& status) = 0;
};

// Defines the durable repository boundary for order records.
class IOrderRepository {
public:
    virtual ~IOrderRepository() = default;

    // Persists a newly created order request with generated order identifiers.
    virtual void save_order(const trading::core::OrderRequest& request,
                            const std::string& order_id,
                            const std::string& client_order_id) = 0;

    // Persists one order lifecycle update.
    virtual void save_order_update(const trading::core::OrderUpdate& update) = 0;

    // Returns the latest stored order record, if available.
    [[nodiscard]] virtual std::optional<OrderRecord> get_order(const std::string& order_id) const = 0;

    // Returns all currently open orders that require runtime recovery.
    [[nodiscard]] virtual std::vector<OrderRecord> all_open_orders() const = 0;
};

// Defines the durable repository boundary for fill records.
class IFillRepository {
public:
    virtual ~IFillRepository() = default;

    // Persists one executed fill.
    virtual void append_fill(const trading::core::FillEvent& fill) = 0;

    // Returns all stored fills in insertion order.
    [[nodiscard]] virtual std::vector<trading::core::FillEvent> all_fills() const = 0;
};

// Defines the durable repository boundary for position records.
class IPositionRepository {
public:
    virtual ~IPositionRepository() = default;

    // Persists the latest position state.
    virtual void save_position(const trading::core::Position& position) = 0;

    // Returns the latest stored position for the instrument, if available.
    [[nodiscard]] virtual std::optional<trading::core::Position> get_position(const std::string& instrument_id) const = 0;

    // Returns all durable positions.
    [[nodiscard]] virtual std::vector<trading::core::Position> all_positions() const = 0;
};

// Defines the durable repository boundary for balance snapshots.
class IBalanceRepository {
public:
    virtual ~IBalanceRepository() = default;

    // Persists the latest balance snapshot for one asset.
    virtual void save_balance(const trading::core::BalanceSnapshot& balance) = 0;

    // Returns the latest stored balance for the asset, if available.
    [[nodiscard]] virtual std::optional<trading::core::BalanceSnapshot> get_balance(const std::string& asset) const = 0;

    // Returns all durable balance snapshots.
    [[nodiscard]] virtual std::vector<trading::core::BalanceSnapshot> all_balances() const = 0;
};

// Defines the cache boundary for market snapshots.
class IMarketStateCache {
public:
    virtual ~IMarketStateCache() = default;

    // Stores one latest market snapshot for an instrument.
    virtual void upsert_snapshot(const MarketSnapshot& snapshot) = 0;

    // Returns the cached market snapshot for the instrument, if available.
    [[nodiscard]] virtual std::optional<MarketSnapshot> get_snapshot(const std::string& instrument_id) const = 0;
};

// Provides an in-memory repository implementation for tests and local scaffolding.
class InMemoryTransactionRepository final : public ITransactionRepository {
public:
    // Stores a newly received transaction record in memory.
    void save_received(const trading::core::TransactionCommand& command) override;

    // Updates the status of an existing transaction record in memory.
    void save_processed(const std::string& transaction_id, const std::string& status) override;

    [[nodiscard]] std::vector<TransactionRecord> all_records() const override;

    // Returns the stored records so tests can verify persistence behavior.
    [[nodiscard]] const std::vector<TransactionRecord>& records() const { return records_; }

private:
    std::vector<TransactionRecord> records_;
};

// Provides an in-memory cache implementation for tests and local scaffolding.
class InMemoryTransactionCache final : public ITransactionCache {
public:
    // Stores the latest status in memory for the given transaction id.
    void set_status(const std::string& transaction_id, const std::string& status) override;

    // Returns the cached status for the given transaction id, or an empty string.
    [[nodiscard]] std::string get_status(const std::string& transaction_id) const;

private:
    std::vector<std::pair<std::string, std::string>> statuses_;
};

// Provides an in-memory order repository implementation for tests and local scaffolding.
class InMemoryOrderRepository final : public IOrderRepository {
public:
    void save_order(const trading::core::OrderRequest& request,
                    const std::string& order_id,
                    const std::string& client_order_id) override;
    void save_order_update(const trading::core::OrderUpdate& update) override;
    [[nodiscard]] std::optional<OrderRecord> get_order(const std::string& order_id) const override;
    [[nodiscard]] std::vector<OrderRecord> all_open_orders() const override;

private:
    std::unordered_map<std::string, OrderRecord> orders_;
};

// Provides an in-memory fill repository implementation for tests and local scaffolding.
class InMemoryFillRepository final : public IFillRepository {
public:
    void append_fill(const trading::core::FillEvent& fill) override;
    [[nodiscard]] std::vector<trading::core::FillEvent> all_fills() const override;

private:
    std::vector<trading::core::FillEvent> fills_;
};

// Provides an in-memory position repository implementation for tests and local scaffolding.
class InMemoryPositionRepository final : public IPositionRepository {
public:
    void save_position(const trading::core::Position& position) override;
    [[nodiscard]] std::optional<trading::core::Position> get_position(const std::string& instrument_id) const override;
    [[nodiscard]] std::vector<trading::core::Position> all_positions() const override;

private:
    std::unordered_map<std::string, trading::core::Position> positions_;
};

// Provides an in-memory balance repository implementation for tests and local scaffolding.
class InMemoryBalanceRepository final : public IBalanceRepository {
public:
    void save_balance(const trading::core::BalanceSnapshot& balance) override;
    [[nodiscard]] std::optional<trading::core::BalanceSnapshot> get_balance(const std::string& asset) const override;
    [[nodiscard]] std::vector<trading::core::BalanceSnapshot> all_balances() const override;

private:
    std::unordered_map<std::string, trading::core::BalanceSnapshot> balances_;
};

// Provides an in-memory market snapshot cache implementation for tests and local scaffolding.
class InMemoryMarketStateCache final : public IMarketStateCache {
public:
    void upsert_snapshot(const MarketSnapshot& snapshot) override;
    [[nodiscard]] std::optional<MarketSnapshot> get_snapshot(const std::string& instrument_id) const override;

private:
    std::unordered_map<std::string, MarketSnapshot> snapshots_;
};

}  // namespace trading::storage
