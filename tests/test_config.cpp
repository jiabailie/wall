#include "test_support.hpp"

using namespace wall_test;

// Verifies the config loader parses files and environment overrides deterministically.
TEST(Config, config_loader) {
    trading::config::ConfigLoader loader;

    // Step 1: Override selected fields through environment variables.
    setenv("TRADING_MODE", "live", 1);
    setenv("TRADING_POSTGRES_PASSWORD", "override-pass", 1);
    setenv("TRADING_KAFKA_TOPIC", "override-topic", 1);
    setenv("TRADING_STRATEGY_TRIGGER_PRICE", "41950.0", 1);
    setenv("TRADING_KILL_SWITCH_ENABLED", "true", 1);

    // Step 2: Load and validate the config file.
    const auto config = loader.load_from_file(std::string(TRADING_SOURCE_DIR) + "/configs/development.cfg");
    const auto validation = loader.validate(config);

    // Step 3: Assert the overrides and required fields are applied.
    expect_true(validation.valid, "config should validate");
    expect_equal(config.mode, std::string("live"), "mode override should apply");
    expect_equal(config.postgres.password, std::string("override-pass"), "postgres password override should apply");
    expect_equal(config.kafka.transaction_topic, std::string("override-topic"), "kafka topic override should apply");
    expect_equal(config.risk.max_order_quantity, 1.0, "risk max order quantity should parse");
    expect_equal(config.risk.kill_switch_enabled, true, "risk kill switch override should apply");
    expect_equal(config.strategy.strategy_id, std::string("sample-threshold"), "strategy id should parse");
    expect_equal(config.strategy.trigger_instrument_id, std::string("mock:binance:BTCUSDT"), "strategy instrument should parse");
    expect_equal(config.strategy.trigger_price, 41950.0, "strategy trigger price override should apply");
    expect_equal(config.strategy.side, std::string("buy"), "strategy side should parse");
    expect_equal(config.simulation.partial_fill_threshold, 0.10, "simulation partial-fill threshold should parse");
    expect_equal(config.simulation.auto_complete_partial_fills, true, "simulation auto-complete flag should parse");

    // Step 4: Clean up the environment for following tests.
    unsetenv("TRADING_MODE");
    unsetenv("TRADING_POSTGRES_PASSWORD");
    unsetenv("TRADING_KAFKA_TOPIC");
    unsetenv("TRADING_STRATEGY_TRIGGER_PRICE");
    unsetenv("TRADING_KILL_SWITCH_ENABLED");
}

// Verifies config validation fails when required strategy settings are missing.
TEST(Config, config_loader_rejects_missing_strategy_fields) {
    trading::config::ConfigLoader loader;
    auto config = loader.load_from_file(std::string(TRADING_SOURCE_DIR) + "/configs/development.cfg");
    config.strategy.order_quantity = 0.0;

    const auto validation = loader.validate(config);
    expect_true(!validation.valid, "config with missing strategy settings should fail validation");
    expect_equal(*validation.error, std::string("strategy settings are required"), "strategy validation error should match");
}

// Verifies malformed numeric config values fail with the offending key name.
TEST(Config, config_loader_rejects_malformed_number) {
    trading::config::ConfigLoader loader;
    const auto path = std::filesystem::temp_directory_path() / ("wall-test-bad-number-" + std::to_string(::getpid()) + ".cfg");
    {
        std::ofstream output(path);
        output << "mode=paper\nredis.host=localhost\nredis.port=63x79\n";
    }

    bool threw = false;
    try {
        loader.load_from_file(path.string());
    } catch (const std::runtime_error& error) {
        threw = std::string(error.what()).find("redis.port") != std::string::npos;
    }
    std::filesystem::remove(path);
    expect_true(threw, "malformed number should fail naming the config key");
}

// Verifies generic TRADING_<KEY> overrides and the allow_fallback flag.
TEST(Config, config_loader_generic_env_override_and_fallback_flag) {
    trading::config::ConfigLoader loader;
    const auto config_path = std::string(TRADING_SOURCE_DIR) + "/configs/development.cfg";

    setenv("TRADING_REDIS_HOST", "redis.internal", 1);
    setenv("TRADING_REDIS_PORT", "7000", 1);
    setenv("TRADING_INFRASTRUCTURE_ALLOW_FALLBACK", "false", 1);
    const auto config = loader.load_from_file(config_path);
    unsetenv("TRADING_REDIS_HOST");
    unsetenv("TRADING_REDIS_PORT");
    unsetenv("TRADING_INFRASTRUCTURE_ALLOW_FALLBACK");

    expect_equal(config.redis.host, std::string("redis.internal"), "generic redis host override should apply");
    expect_equal(config.redis.port, 7000, "generic redis port override should apply");
    expect_true(!config.allow_fallback, "allow_fallback override should apply");
    expect_true(loader.load_from_file(config_path).allow_fallback, "development config should allow fallback");
}
