#pragma once

#include "app/application.hpp"
#include "app/engine_controller.hpp"
#include "app/mock_event_sources.hpp"
#include "app/recovery_service.hpp"
#include "app/simulation_runtime.hpp"
#include "app/backtest_runner.hpp"
#include "app/transaction_publisher_application.hpp"
#include "config/config_loader.hpp"
#include "core/clock.hpp"
#include "core/event_dispatcher.hpp"
#include "core/types.hpp"
#include "exchange/exchange_execution_adapter.hpp"
#include "exchange/exchange_market_data_adapter.hpp"
#include "exchange/exchange_router.hpp"
#include "exchange/live_market_data_feed.hpp"
#include "execution/live_execution_tracker.hpp"
#include "execution/matching_engine.hpp"
#include "execution/order_book.hpp"
#include "execution/simulated_execution_engine.hpp"
#include "infrastructure/postgres_repositories.hpp"
#include "infrastructure/kafka_transaction_consumer.hpp"
#include "infrastructure/kafka_transaction_producer.hpp"
#include "infrastructure/redis_cache.hpp"
#include "ingestion/transaction_consumer.hpp"
#include "ingestion/transaction_ingestor.hpp"
#include "market_data/market_state_store.hpp"
#include "market_data/feed_health_tracker.hpp"
#include "monitoring/health_status.hpp"
#include "monitoring/logger.hpp"
#include "monitoring/metrics.hpp"
#include "portfolio/portfolio_service.hpp"
#include "risk/risk_engine.hpp"
#include "storage/storage_interfaces.hpp"
#include "storage/replay_service.hpp"
#include "strategy/sample_threshold_strategy.hpp"
#include "strategy/spread_capture_strategy.hpp"
#include "strategy/strategy_coordinator.hpp"

#include <cstdlib>
#include <unistd.h>
#include <cmath>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace wall_test {


inline thread_local std::string g_current_test_step;

// Records and prints one human-readable test step for detailed debug output.
inline void log_test_step(const std::string& message) {
    g_current_test_step = message;
    std::cout << "  [STEP] " << message << '\n';
}

// Builds a failure message with current test and step context.
inline std::string contextualize_failure(const std::string& message) {
    std::stringstream stream;
    if (const auto* info = ::testing::UnitTest::GetInstance()->current_test_info()) {
        stream << "[" << info->name() << "] ";
    }
    if (!g_current_test_step.empty()) {
        stream << g_current_test_step << ": ";
    }
    stream << message;
    return stream.str();
}

// Creates a unique temporary directory path for storage adapter tests.
inline std::filesystem::path make_temp_directory(const std::string& prefix) {
    static std::size_t counter = 0;
    const auto path = std::filesystem::temp_directory_path() /
        (prefix + "-" + std::to_string(::getpid()) + "-" + std::to_string(++counter));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

// Provides a deterministic low-level Kafka client double for adapter tests.
class FakeKafkaConsumerClient final : public trading::infrastructure::IKafkaConsumerClient {
public:
    explicit FakeKafkaConsumerClient(std::vector<trading::infrastructure::RawKafkaMessage> messages)
        : messages_(std::move(messages)) {}

    std::optional<trading::infrastructure::RawKafkaMessage> poll() override {
        if (next_index_ >= messages_.size()) {
            return std::nullopt;
        }

        return messages_[next_index_++];
    }

    void commit(const std::string& topic, int partition, std::int64_t offset) override {
        commits_.push_back({
            .topic = topic,
            .partition = partition,
            .offset = offset,
            .payload = "",
        });
    }

    void seek(const std::string& topic, int partition, std::int64_t next_offset) override {
        seeks_.push_back({
            .topic = topic,
            .partition = partition,
            .offset = next_offset,
            .payload = "",
        });
    }

    [[nodiscard]] const std::vector<trading::infrastructure::RawKafkaMessage>& commits() const { return commits_; }
    [[nodiscard]] const std::vector<trading::infrastructure::RawKafkaMessage>& seeks() const { return seeks_; }

private:
    std::vector<trading::infrastructure::RawKafkaMessage> messages_;
    std::vector<trading::infrastructure::RawKafkaMessage> commits_;
    std::vector<trading::infrastructure::RawKafkaMessage> seeks_;
    std::size_t next_index_ {0};
};

class SteppingClock final : public trading::core::IClock {
public:
    explicit SteppingClock(const std::int64_t start_ms, const std::int64_t step_ms)
        : current_ms_(start_ms), step_ms_(step_ms) {}

    std::int64_t now_ms() const override {
        const auto value = current_ms_;
        current_ms_ += step_ms_;
        return value;
    }

private:
    mutable std::int64_t current_ms_;
    std::int64_t step_ms_ {0};
};

class ThrowingTransactionRepository final : public trading::storage::ITransactionRepository {
public:
    void save_received(const trading::core::TransactionCommand&) override {
        throw std::runtime_error("save_received failed");
    }

    void save_processed(const std::string&, const std::string&) override {
        throw std::runtime_error("save_processed failed");
    }

    std::vector<trading::storage::TransactionRecord> all_records() const override {
        return {};
    }

    void save_checkpoint(const trading::storage::KafkaCheckpoint&) override {}

    std::vector<trading::storage::KafkaCheckpoint> all_checkpoints() const override {
        return {};
    }
};

class ThrowingStrategy final : public trading::strategy::IStrategy {
public:
    explicit ThrowingStrategy(std::string strategy_id) : strategy_id_(std::move(strategy_id)) {}

    std::string strategy_id() const override { return strategy_id_; }

    std::vector<trading::core::OrderRequest> on_event(const trading::core::EngineEvent&,
                                                      const trading::strategy::StrategyContext&) override {
        throw std::runtime_error("strategy failure");
    }

private:
    std::string strategy_id_;
};

// Returns a reusable BTC instrument fixture for tests.
inline trading::core::Instrument make_btc_instrument() {
    return {
        .instrument_id = "binance:BTCUSDT",
        .exchange = "binance",
        .symbol = "BTCUSDT",
        .base_asset = "BTC",
        .quote_asset = "USDT",
        .tick_size = 0.1,
        .lot_size = 0.0001,
    };
}

inline trading::core::Instrument make_coinbase_eth_instrument() {
    return {
        .instrument_id = "coinbase:ETHUSD",
        .exchange = "coinbase",
        .symbol = "ETHUSD",
        .base_asset = "ETH",
        .quote_asset = "USD",
        .tick_size = 0.01,
        .lot_size = 0.001,
    };
}

// Returns a reusable valid transaction fixture for tests.
inline trading::core::TransactionCommand make_transaction(const std::string& transaction_id,
                                                   const std::int64_t kafka_offset) {
    return {
        .transaction_id = transaction_id,
        .user_id = "user-1",
        .account_id = "account-1",
        .command_type = "place_order",
        .instrument_symbol = "BTCUSDT",
        .quantity = 1.5,
        .price = 42000.0,
        .kafka_topic = "trading-transactions",
        .kafka_partition = 0,
        .kafka_offset = kafka_offset,
    };
}

// Fails the current test when the condition is false.
inline void expect_true(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(contextualize_failure(message));
    }
}

// Fails the current test when the two values differ.
template <typename T>
inline void expect_equal(const T& actual, const T& expected, const std::string& message) {
    if (!(actual == expected)) {
        throw std::runtime_error(contextualize_failure(message));
    }
}

// Fails the current test when the two floating-point values differ beyond tolerance.
inline void expect_near(const double actual, const double expected, const double tolerance, const std::string& message) {
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(contextualize_failure(message));
    }
}

}  // namespace wall_test
