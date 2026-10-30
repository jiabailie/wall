#include "config/config_loader.hpp"

#include <cctype>
#include <cstdint>
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

using ValueMap = std::unordered_map<std::string, std::string>;

// Lists every config key that can be overridden by a TRADING_<KEY> environment variable.
constexpr const char* kKnownKeys[] = {
    "mode",
    "exchange.name",
    "instruments",
    "infrastructure.allow_fallback",
    "redis.host",
    "redis.port",
    "postgres.host",
    "postgres.port",
    "postgres.database",
    "postgres.user",
    "postgres.password",
    "kafka.brokers",
    "kafka.transaction_topic",
    "kafka.consumer_group",
    "risk.max_order_quantity",
    "risk.max_order_notional",
    "risk.max_position_quantity",
    "risk.stale_after_ms",
    "risk.kill_switch_enabled",
    "strategy.id",
    "strategy.trigger_instrument_id",
    "strategy.trigger_price",
    "strategy.order_quantity",
    "strategy.side",
    "simulation.partial_fill_threshold",
    "simulation.partial_fill_ratio",
    "simulation.auto_complete_partial_fills",
};

// Builds the environment variable name for a config key.
std::string env_name_for_key(const std::string& key) {
    std::string name = "TRADING_";
    for (const char c : key) {
        name += (c == '.') ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return name;
}

// Returns the value for a key without inserting missing entries.
std::string get_string(const ValueMap& values, const std::string& key) {
    const auto it = values.find(key);
    return it == values.end() ? std::string() : it->second;
}

// Parses a numeric value with a key-named error, using the default when the key is empty or absent.
template <typename T, typename Parser>
T parse_number(const ValueMap& values, const std::string& key, T default_value, Parser parser) {
    const auto text = get_string(values, key);
    if (text.empty()) {
        return default_value;
    }

    try {
        std::size_t consumed = 0;
        const T result = parser(text, &consumed);
        if (consumed != text.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return result;
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid numeric value for config key '" + key + "': " + text);
    }
}

int get_int(const ValueMap& values, const std::string& key, int default_value) {
    return parse_number<int>(values, key, default_value, [](const std::string& t, std::size_t* n) { return std::stoi(t, n); });
}

std::int64_t get_int64(const ValueMap& values, const std::string& key, std::int64_t default_value) {
    return parse_number<std::int64_t>(
        values, key, default_value, [](const std::string& t, std::size_t* n) { return static_cast<std::int64_t>(std::stoll(t, n)); });
}

double get_double(const ValueMap& values, const std::string& key, double default_value) {
    return parse_number<double>(values, key, default_value, [](const std::string& t, std::size_t* n) { return std::stod(t, n); });
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

    ValueMap values;
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
    // Every known key can be overridden with TRADING_<KEY> (dots become underscores, upper-cased).
    for (const auto* key : kKnownKeys) {
        if (const auto value = get_env_value(env_name_for_key(key).c_str())) {
            values[key] = *value;
        }
    }
    // Legacy override names kept for backward compatibility.
    if (const auto value = get_env_value("TRADING_KAFKA_TOPIC")) {
        values["kafka.transaction_topic"] = *value;
    }
    if (const auto value = get_env_value("TRADING_KILL_SWITCH_ENABLED")) {
        values["risk.kill_switch_enabled"] = *value;
    }

    // Step 3: Map the parsed values into the typed config structure.
    AppConfig config;
    config.mode = get_string(values, "mode");
    config.exchange_name = get_string(values, "exchange.name");
    config.instruments = split_csv(get_string(values, "instruments"));
    config.redis.host = get_string(values, "redis.host");
    config.redis.port = get_int(values, "redis.port", 6379);
    config.postgres.host = get_string(values, "postgres.host");
    config.postgres.port = get_int(values, "postgres.port", 5432);
    config.postgres.database = get_string(values, "postgres.database");
    config.postgres.user = get_string(values, "postgres.user");
    config.postgres.password = get_string(values, "postgres.password");
    config.kafka.brokers = split_csv(get_string(values, "kafka.brokers"));
    config.kafka.transaction_topic = get_string(values, "kafka.transaction_topic");
    config.kafka.consumer_group = get_string(values, "kafka.consumer_group");
    config.risk.max_order_quantity = get_double(values, "risk.max_order_quantity", 0.0);
    config.risk.max_order_notional = get_double(values, "risk.max_order_notional", 0.0);
    config.risk.max_position_quantity = get_double(values, "risk.max_position_quantity", 0.0);
    config.risk.stale_after_ms = get_int64(values, "risk.stale_after_ms", 0);
    config.risk.kill_switch_enabled = parse_bool(get_string(values, "risk.kill_switch_enabled"));
    config.strategy.strategy_id = get_string(values, "strategy.id");
    config.strategy.trigger_instrument_id = get_string(values, "strategy.trigger_instrument_id");
    config.strategy.trigger_price = get_double(values, "strategy.trigger_price", 0.0);
    config.strategy.order_quantity = get_double(values, "strategy.order_quantity", 0.0);
    config.strategy.side = get_string(values, "strategy.side");
    config.simulation.partial_fill_threshold = get_double(values, "simulation.partial_fill_threshold", 0.0);
    config.simulation.partial_fill_ratio = get_double(values, "simulation.partial_fill_ratio", 0.0);
    config.simulation.auto_complete_partial_fills = parse_bool(get_string(values, "simulation.auto_complete_partial_fills"));
    config.allow_fallback = parse_bool(get_string(values, "infrastructure.allow_fallback"));
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
