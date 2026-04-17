#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trading::config {

// Stores Redis connection settings.
struct RedisConfig {
    std::string host;
    int port {6379};
};

// Stores PostgreSQL connection settings.
struct PostgresConfig {
    std::string host;
    int port {5432};
    std::string database;
    std::string user;
    std::string password;
};

// Stores Kafka consumer settings.
struct KafkaConfig {
    std::vector<std::string> brokers;
    std::string transaction_topic;
    std::string consumer_group;
};

// Stores the baseline risk limits for pre-trade checks.
struct RiskConfig {
    double max_order_quantity {0.0};
    double max_order_notional {0.0};
    double max_position_quantity {0.0};
    std::int64_t stale_after_ms {0};
    bool kill_switch_enabled {false};
};

// Stores strategy settings for local simulation runs.
struct StrategyConfig {
    std::string strategy_id;
    std::string trigger_instrument_id;
    double trigger_price {0.0};
    double order_quantity {0.0};
    std::string side;
};

// Stores simulation runtime execution settings.
struct SimulationConfig {
    double partial_fill_threshold {0.0};
    double partial_fill_ratio {0.0};
    bool auto_complete_partial_fills {false};
};

// Stores the application runtime configuration.
struct AppConfig {
    std::string mode;
    std::string exchange_name;
    std::vector<std::string> instruments;
    RedisConfig redis;
    PostgresConfig postgres;
    KafkaConfig kafka;
    RiskConfig risk;
    StrategyConfig strategy;
    SimulationConfig simulation;
};

// Stores the result of configuration validation.
struct ConfigValidationResult {
    bool valid {false};
    std::optional<std::string> error;
};

}  // namespace trading::config
