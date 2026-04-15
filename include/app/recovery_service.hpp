#pragma once

#include "app/simulation_runtime.hpp"
#include "core/types.hpp"
#include "infrastructure/kafka_transaction_consumer.hpp"
#include "storage/storage_interfaces.hpp"

#include <vector>

namespace trading::app {

// Stores the recovered startup state used to resume the runtime safely.
struct RecoverySnapshot {
    std::vector<trading::storage::OrderRecord> open_orders;
    std::vector<trading::core::Position> positions;
    std::vector<trading::core::BalanceSnapshot> balances;
    std::vector<trading::storage::KafkaCheckpoint> kafka_checkpoints;
};

// Rebuilds runtime state from durable storage and warms caches before consumption resumes.
class RecoveryService {
public:
    RecoveryService(const trading::storage::ITransactionRepository& transaction_repository,
                    const trading::storage::IOrderRepository& order_repository,
                    const trading::storage::IPositionRepository& position_repository,
                    const trading::storage::IBalanceRepository& balance_repository,
                    trading::storage::IMarketStateCache& market_cache,
                    trading::storage::ITransactionCache& transaction_cache)
        : transaction_repository_(transaction_repository),
          order_repository_(order_repository),
          position_repository_(position_repository),
          balance_repository_(balance_repository),
          market_cache_(market_cache),
          transaction_cache_(transaction_cache) {}

    // Rebuilds durable state into a recovery snapshot and warms caches.
    [[nodiscard]] RecoverySnapshot recover(
        const std::vector<trading::storage::MarketSnapshot>& exchange_snapshots = {}) const;

    // Applies the recovered snapshot to the in-memory simulation runtime.
    void restore_runtime(const RecoverySnapshot& snapshot, trading::app::SimulationRuntime& runtime) const;

    // Seeks the Kafka client to the intended next offsets after recovery.
    void resume_kafka(const RecoverySnapshot& snapshot, trading::infrastructure::IKafkaConsumerClient& client) const;

private:
    const trading::storage::ITransactionRepository& transaction_repository_;
    const trading::storage::IOrderRepository& order_repository_;
    const trading::storage::IPositionRepository& position_repository_;
    const trading::storage::IBalanceRepository& balance_repository_;
    trading::storage::IMarketStateCache& market_cache_;
    trading::storage::ITransactionCache& transaction_cache_;
};

}  // namespace trading::app
