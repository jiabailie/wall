#include "app/recovery_service.hpp"

#include <unordered_map>

namespace trading::app {

namespace {

std::string checkpoint_key(const std::string& topic, const int partition) {
    return topic + "#" + std::to_string(partition);
}

}  // namespace

RecoverySnapshot RecoveryService::recover(const std::vector<trading::storage::MarketSnapshot>& exchange_snapshots) const {
    RecoverySnapshot snapshot;
    snapshot.open_orders = order_repository_.all_open_orders();
    snapshot.positions = position_repository_.all_positions();
    snapshot.balances = balance_repository_.all_balances();

    const auto explicit_checkpoints = transaction_repository_.all_checkpoints();
    std::unordered_map<std::string, trading::storage::KafkaCheckpoint> checkpoints_by_partition;
    for (const auto& record : transaction_repository_.all_records()) {
        transaction_cache_.set_status(record.transaction_id, record.status);
        if (!explicit_checkpoints.empty() || record.status != "processed") {
            continue;
        }

        const auto key = checkpoint_key(record.kafka_topic, record.kafka_partition);
        const auto iterator = checkpoints_by_partition.find(key);
        if (iterator == checkpoints_by_partition.end() || record.kafka_offset > iterator->second.offset) {
            checkpoints_by_partition[key] = {
                .topic = record.kafka_topic,
                .partition = record.kafka_partition,
                .offset = record.kafka_offset,
            };
        }
    }

    if (!explicit_checkpoints.empty()) {
        snapshot.kafka_checkpoints = explicit_checkpoints;
    } else {
        snapshot.kafka_checkpoints.reserve(checkpoints_by_partition.size());
        for (const auto& [_, checkpoint] : checkpoints_by_partition) {
            snapshot.kafka_checkpoints.push_back(checkpoint);
        }
    }

    if (!exchange_snapshots.empty()) {
        for (const auto& exchange_snapshot : exchange_snapshots) {
            market_cache_.upsert_snapshot(exchange_snapshot);
        }
    } else {
        for (const auto& position : snapshot.positions) {
            market_cache_.upsert_snapshot({
                .instrument_id = position.instrument.instrument_id,
                .best_bid = std::nullopt,
                .best_ask = std::nullopt,
                .last_trade_price = position.average_entry_price > 0.0
                    ? std::optional<double>(position.average_entry_price)
                    : std::nullopt,
                .last_trade_quantity = std::nullopt,
                .last_process_timestamp = 0,
            });
        }
    }

    return snapshot;
}

void RecoveryService::restore_runtime(const RecoverySnapshot& snapshot, trading::app::SimulationRuntime& runtime) const {
    for (const auto& position : snapshot.positions) {
        runtime.restore_position(position);
    }
    for (const auto& balance : snapshot.balances) {
        runtime.restore_balance(balance);
    }
    for (const auto& order : snapshot.open_orders) {
        runtime.restore_open_order(order);
    }

    for (const auto& position : snapshot.positions) {
        const auto market_snapshot = market_cache_.get_snapshot(position.instrument.instrument_id);
        if (market_snapshot.has_value()) {
            runtime.restore_market_snapshot(*market_snapshot);
        }
    }
}

void RecoveryService::resume_kafka(const RecoverySnapshot& snapshot, trading::infrastructure::IKafkaConsumerClient& client) const {
    for (const auto& checkpoint : snapshot.kafka_checkpoints) {
        client.seek(checkpoint.topic, checkpoint.partition, checkpoint.offset + 1);
    }
}

}  // namespace trading::app
