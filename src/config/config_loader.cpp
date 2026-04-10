#include "config/config_loader.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace trading::config {

namespace {

// Removes leading and trailing spaces from the provided string.
std::string trim(const std::string& value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }

    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

// Splits a comma-separated list into trimmed items.
std::vector<std::string> split_csv(const std::string& value) {
    std::vector<std::string> result;
    std::stringstream stream(value);
    std::string item;

    // Step 1: Read each comma-delimited item.
    while (std::getline(stream, item, ',')) {
        // Step 2: Trim the item before storing it.
        const auto trimmed = trim(item);
        if (!trimmed.empty()) {
            result.push_back(trimmed);
        }
    }

    return result;
}

// Returns the environment variable value when it exists.
std::optional<std::string> get_env_value(const char* key) {
    if (const char* value = std::getenv(key); value != nullptr) {
        return std::string(value);
    }

    return std::nullopt;
}

// Parses a string boolean using common config spellings.
bool parse_bool(const std::string& value) {
    return value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "on";
}

}  // namespace

// Reads the key-value config file and applies environment overrides.
AppConfig ConfigLoader::load_from_file(const std::string& path) const {
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("Unable to open config file: " + path);
    }

    std::unordered_map<std::string, std::string> values;
    std::string line;

    // Step 1: Parse every non-empty, non-comment line into key/value pairs.
    while (std::getline(input, line)) {
        const auto trimmed = trim(line);
        if (trimmed.empty() || trimmed.starts_with('#')) {
            continue;
        }

        const auto separator = trimmed.find('=');
        if (separator == std::string::npos) {
            throw std::runtime_error("Invalid config line: " + trimmed);
        }

        values[trim(trimmed.substr(0, separator))] = trim(trimmed.substr(separator + 1));
    }

    // Step 2: Apply environment overrides where present.
    if (const auto value = get_env_value("TRADING_MODE")) {
        values["mode"] = *value;
    }
    if (const auto value = get_env_value("TRADING_POSTGRES_PASSWORD")) {
        values["postgres.password"] = *value;
    }
    if (const auto value = get_env_value("TRADING_KAFKA_TOPIC")) {
        values["kafka.transaction_topic"] = *value;
    }
    if (const auto value = get_env_value("TRADING_STRATEGY_TRIGGER_PRICE")) {
        values["strategy.trigger_price"] = *value;
    }
    if (const auto value = get_env_value("TRADING_KILL_SWITCH_ENABLED")) {
        values["risk.kill_switch_enabled"] = *value;
    }

    // Step 3: Map the parsed values into the typed config structure.
    AppConfig config;
    config.mode = values["mode"];
    config.exchange_name = values["exchange.name"];
    config.instruments = split_csv(values["instruments"]);
    config.redis.host = values["redis.host"];
    config.redis.port = values["redis.port"].empty() ? 6379 : std::stoi(values["redis.port"]);
    config.postgres.host = values["postgres.host"];
    config.postgres.port = values["postgres.port"].empty() ? 5432 : std::stoi(values["postgres.port"]);
    config.postgres.database = values["postgres.database"];
    config.postgres.user = values["postgres.user"];
    config.postgres.password = values["postgres.password"];
    config.kafka.brokers = split_csv(values["kafka.brokers"]);
    config.kafka.transaction_topic = values["kafka.transaction_topic"];
    config.kafka.consumer_group = values["kafka.consumer_group"];
    config.risk.max_order_quantity = values["risk.max_order_quantity"].empty() ? 0.0 : std::stod(values["risk.max_order_quantity"]);
    config.risk.max_order_notional = values["risk.max_order_notional"].empty() ? 0.0 : std::stod(values["risk.max_order_notional"]);
    config.risk.max_position_quantity = values["risk.max_position_quantity"].empty() ? 0.0 : std::stod(values["risk.max_position_quantity"]);
    config.risk.stale_after_ms = values["risk.stale_after_ms"].empty() ? 0 : std::stoll(values["risk.stale_after_ms"]);
    config.risk.kill_switch_enabled = parse_bool(values["risk.kill_switch_enabled"]);
    config.strategy.strategy_id = values["strategy.id"];
    config.strategy.trigger_instrument_id = values["strategy.trigger_instrument_id"];
    config.strategy.trigger_price = values["strategy.trigger_price"].empty() ? 0.0 : std::stod(values["strategy.trigger_price"]);
    config.strategy.order_quantity = values["strategy.order_quantity"].empty() ? 0.0 : std::stod(values["strategy.order_quantity"]);
    config.strategy.side = values["strategy.side"];
    config.simulation.partial_fill_threshold =
        values["simulation.partial_fill_threshold"].empty() ? 0.0 : std::stod(values["simulation.partial_fill_threshold"]);
    config.simulation.partial_fill_ratio =
        values["simulation.partial_fill_ratio"].empty() ? 0.0 : std::stod(values["simulation.partial_fill_ratio"]);
    config.simulation.auto_complete_partial_fills = parse_bool(values["simulation.auto_complete_partial_fills"]);
    return config;
}

// Checks that the minimum required config fields are populated.
ConfigValidationResult ConfigLoader::validate(const AppConfig& config) const {
    // Step 1: Verify the top-level runtime mode.
    if (config.mode.empty()) {
        return {.valid = false, .error = "mode is required"};
    }

    // Step 2: Verify infrastructure endpoints.
    if (config.redis.host.empty()) {
        return {.valid = false, .error = "redis.host is required"};
    }
    if (config.postgres.host.empty() || config.postgres.database.empty() || config.postgres.user.empty()) {
        return {.valid = false, .error = "postgres connection settings are required"};
    }
    if (config.kafka.brokers.empty() || config.kafka.transaction_topic.empty() || config.kafka.consumer_group.empty()) {
        return {.valid = false, .error = "kafka settings are required"};
    }
    if (config.risk.max_order_quantity <= 0.0 ||
        config.risk.max_order_notional <= 0.0 ||
        config.risk.max_position_quantity <= 0.0 ||
        config.risk.stale_after_ms <= 0) {
        return {.valid = false, .error = "risk settings are required"};
    }
    if (config.strategy.strategy_id.empty() ||
        config.strategy.trigger_instrument_id.empty() ||
        config.strategy.trigger_price <= 0.0 ||
        config.strategy.order_quantity <= 0.0) {
        return {.valid = false, .error = "strategy settings are required"};
    }
    if (config.strategy.side != "buy" && config.strategy.side != "sell") {
        return {.valid = false, .error = "strategy.side must be buy or sell"};
    }
    if (config.simulation.partial_fill_threshold <= 0.0 ||
        config.simulation.partial_fill_ratio <= 0.0 ||
        config.simulation.partial_fill_ratio > 1.0) {
        return {.valid = false, .error = "simulation settings are required"};
    }

    // Step 3: Return success when all required values are present.
    return {.valid = true, .error = std::nullopt};
}

}  // namespace trading::config
