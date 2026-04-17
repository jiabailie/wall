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

namespace {

using TestFunction = std::function<void()>;

thread_local std::string g_current_test_name;
thread_local std::string g_current_test_step;

// Records and prints one human-readable test step for detailed debug output.
void log_test_step(const std::string& message) {
    g_current_test_step = message;
    std::cout << "  [STEP] " << message << '\n';
}

// Builds a failure message with current test and step context.
std::string contextualize_failure(const std::string& message) {
    std::stringstream stream;
    if (!g_current_test_name.empty()) {
        stream << "[" << g_current_test_name << "] ";
    }
    if (!g_current_test_step.empty()) {
        stream << g_current_test_step << ": ";
    }
    stream << message;
    return stream.str();
}

// Creates a unique temporary directory path for storage adapter tests.
std::filesystem::path make_temp_directory(const std::string& prefix) {
    static std::size_t counter = 0;
    const auto path = std::filesystem::temp_directory_path() /
        (prefix + "-" + std::to_string(++counter));
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
trading::core::Instrument make_btc_instrument() {
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

trading::core::Instrument make_coinbase_eth_instrument() {
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
trading::core::TransactionCommand make_transaction(const std::string& transaction_id,
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
void expect_true(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(contextualize_failure(message));
    }
}

// Fails the current test when the two values differ.
template <typename T>
void expect_equal(const T& actual, const T& expected, const std::string& message) {
    if (!(actual == expected)) {
        throw std::runtime_error(contextualize_failure(message));
    }
}

// Fails the current test when the two floating-point values differ beyond tolerance.
void expect_near(const double actual, const double expected, const double tolerance, const std::string& message) {
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(contextualize_failure(message));
    }
}

// Verifies the core model fixtures can be constructed with stable defaults.
void test_core_models() {
    const auto instrument = make_btc_instrument();
    const trading::core::OrderRequest request {
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 1.0,
        .price = 42000.0,
    };

    // Step 1: Verify the instrument fixture survived construction.
    expect_equal(instrument.symbol, std::string("BTCUSDT"), "instrument symbol should match");

    // Step 2: Verify the request fields hold the expected values.
    expect_equal(request.request_id, std::string("req-1"), "request id should match");
    expect_true(request.price.has_value(), "limit order price should exist");
}

// Verifies the config loader parses files and environment overrides deterministically.
void test_config_loader() {
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
void test_config_loader_rejects_missing_strategy_fields() {
    trading::config::ConfigLoader loader;
    auto config = loader.load_from_file(std::string(TRADING_SOURCE_DIR) + "/configs/development.cfg");
    config.strategy.order_quantity = 0.0;

    const auto validation = loader.validate(config);
    expect_true(!validation.valid, "config with missing strategy settings should fail validation");
    expect_equal(*validation.error, std::string("strategy settings are required"), "strategy validation error should match");
}

// Verifies event handlers receive events in the same order they were subscribed.
void test_event_dispatcher_order() {
    trading::core::EventDispatcher dispatcher;
    std::vector<std::string> calls;

    // Step 1: Register two handlers in a known order.
    dispatcher.subscribe([&calls](const trading::core::EngineEvent&) { calls.push_back("first"); });
    dispatcher.subscribe([&calls](const trading::core::EngineEvent&) { calls.push_back("second"); });

    // Step 2: Publish a timer event into the dispatcher.
    dispatcher.publish(trading::core::TimerEvent {.timer_id = "t-1", .timestamp = 1000});

    // Step 3: Assert the handlers ran in registration order.
    expect_equal(calls.size(), std::size_t {2}, "two handlers should run");
    expect_equal(calls[0], std::string("first"), "first handler should run first");
    expect_equal(calls[1], std::string("second"), "second handler should run second");
}

// Verifies in-memory structured logging captures level, message, and fields.
void test_in_memory_structured_logger_records() {
    trading::monitoring::InMemoryStructuredLogger logger;
    logger.log(
        trading::monitoring::LogLevel::warn,
        "risk_limit_hit",
        {
            {"request_id", "req-1"},
            {"reason", "max_notional"},
        });

    expect_equal(logger.records().size(), std::size_t {1}, "one log record should be captured");
    expect_equal(logger.records()[0].message, std::string("risk_limit_hit"), "message should match");
    expect_equal(logger.records()[0].level, trading::monitoring::LogLevel::warn, "level should match");
    expect_equal(logger.records()[0].fields.at("request_id"), std::string("req-1"), "field should be captured");
}

// Verifies console logger renders a structured line with key fields.
void test_console_structured_logger_writes_output() {
    std::ostringstream output;
    trading::monitoring::ConsoleStructuredLogger logger(output);
    logger.log(
        trading::monitoring::LogLevel::info,
        "startup_complete",
        {
            {"mode", "paper"},
            {"exchange", "binance"},
        });

    const auto rendered = output.str();
    expect_true(rendered.find("level=info") != std::string::npos, "console log should include level");
    expect_true(rendered.find("message=startup_complete") != std::string::npos, "console log should include message");
    expect_true(rendered.find("mode=paper") != std::string::npos, "console log should include mode field");
}

// Verifies component health transitions and roll-up logic.
void test_runtime_health_status_transitions() {
    trading::monitoring::RuntimeHealthStatus health;
    expect_equal(health.overall_status(), trading::monitoring::HealthStatus::unknown, "default overall status should be unknown");

    health.set_status(trading::monitoring::RuntimeComponent::ingestion, trading::monitoring::HealthStatus::healthy);
    health.set_status(trading::monitoring::RuntimeComponent::market_data, trading::monitoring::HealthStatus::healthy);
    health.set_status(trading::monitoring::RuntimeComponent::risk, trading::monitoring::HealthStatus::healthy);
    health.set_status(trading::monitoring::RuntimeComponent::execution, trading::monitoring::HealthStatus::healthy);
    expect_equal(health.overall_status(), trading::monitoring::HealthStatus::healthy, "all-healthy should roll up healthy");

    health.set_status(trading::monitoring::RuntimeComponent::risk, trading::monitoring::HealthStatus::degraded);
    expect_equal(health.overall_status(), trading::monitoring::HealthStatus::degraded, "degraded component should roll up degraded");

    health.set_status(trading::monitoring::RuntimeComponent::execution, trading::monitoring::HealthStatus::unavailable);
    expect_equal(
        health.get_status(trading::monitoring::RuntimeComponent::execution),
        trading::monitoring::HealthStatus::unavailable,
        "execution component should be unavailable");
    expect_equal(health.overall_status(), trading::monitoring::HealthStatus::unavailable, "unavailable component should roll up unavailable");
}

// Verifies counters and latency samples are aggregated in memory.
void test_in_memory_metrics_collector_records_counters_and_latency() {
    trading::monitoring::InMemoryMetricsCollector metrics;
    metrics.increment("fills_applied");
    metrics.increment("fills_applied", 2);
    metrics.observe_latency("runtime_event_latency_ms", 3);
    metrics.observe_latency("runtime_event_latency_ms", 7);

    expect_equal(metrics.counter("fills_applied"), std::int64_t {3}, "metrics counter should aggregate increments");
    const auto latency = metrics.latency("runtime_event_latency_ms");
    expect_equal(latency.sample_count, std::int64_t {2}, "latency metric should record sample count");
    expect_equal(latency.total_ms, std::int64_t {10}, "latency metric should record total latency");
    expect_equal(latency.max_ms, std::int64_t {7}, "latency metric should record max latency");
}

// Verifies the controller drains queued events in FIFO order.
void test_engine_controller_queue_order() {
    trading::core::FixedClock clock(1000);
    trading::app::EngineController controller(clock);
    std::vector<std::string> calls;

    controller.subscribe([&calls](const trading::core::EngineEvent& event) {
        std::visit([&calls](const auto& concrete_event) {
            using EventType = std::decay_t<decltype(concrete_event)>;

            if constexpr (std::is_same_v<EventType, trading::core::TimerEvent>) {
                calls.push_back("timer:" + concrete_event.timer_id);
            } else if constexpr (std::is_same_v<EventType, trading::core::TransactionCommand>) {
                calls.push_back("tx:" + concrete_event.transaction_id);
            } else if constexpr (std::is_same_v<EventType, trading::core::MarketEvent>) {
                calls.push_back("market:" + concrete_event.event_id);
            }
        }, event);
    });

    controller.enqueue(trading::core::TimerEvent {.timer_id = "first", .timestamp = 1000});
    controller.enqueue(make_transaction("tx-1", 10));
    controller.enqueue(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 42010.0,
        .quantity = 0.5,
        .process_timestamp = 1000,
    });

    expect_equal(controller.drain(), std::size_t {3}, "three events should be drained");
    expect_equal(calls.size(), std::size_t {3}, "three events should be observed");
    expect_equal(calls[0], std::string("timer:first"), "timer should be first");
    expect_equal(calls[1], std::string("tx:tx-1"), "transaction should be second");
    expect_equal(calls[2], std::string("market:market-1"), "market event should be third");
}

// Verifies due timers are enqueued once in timer registration order.
void test_engine_controller_due_timers() {
    trading::core::FixedClock clock(5000);
    trading::app::EngineController controller(clock);
    std::vector<std::string> timer_ids;

    controller.subscribe([&timer_ids](const trading::core::EngineEvent& event) {
        if (const auto* timer = std::get_if<trading::core::TimerEvent>(&event)) {
            timer_ids.push_back(timer->timer_id);
        }
    });

    controller.schedule_timer("early", 4000);
    controller.schedule_timer("late", 5000);

    expect_equal(controller.enqueue_due_timers(), std::size_t {2}, "two timers should be due");
    expect_equal(controller.drain(), std::size_t {2}, "two timer events should be drained");
    expect_equal(timer_ids[0], std::string("early"), "first due timer should preserve registration order");
    expect_equal(timer_ids[1], std::string("late"), "second due timer should preserve registration order");
    expect_equal(controller.enqueue_due_timers(), std::size_t {0}, "timers should not be emitted twice");
}

// Verifies source polling is deterministic and safe when handlers are absent.
void test_engine_controller_mock_sources_and_empty_dispatch() {
    trading::core::FixedClock clock(1000);
    trading::app::EngineController controller(clock);

    trading::app::MockMarketDataSource market_source({
        trading::core::MarketEvent {
            .event_id = "market-1",
            .type = trading::core::MarketEventType::trade,
            .instrument = make_btc_instrument(),
            .price = 42010.0,
            .quantity = 0.25,
            .process_timestamp = 1000,
        },
    });
    trading::app::MockTransactionEventSource transaction_source({
        make_transaction("tx-1", 10),
    });

    controller.add_source(market_source);
    controller.add_source(transaction_source);

    expect_equal(controller.poll_sources_once(), std::size_t {2}, "two source events should be polled");
    expect_equal(controller.queued_event_count(), std::size_t {2}, "two source events should be queued");
    expect_equal(controller.drain(), std::size_t {2}, "drain should be safe without subscribers");
    expect_equal(controller.poll_sources_once(), std::size_t {0}, "sources should be exhausted");
}

// Verifies market state updates and stale detection use deterministic timestamps.
void test_market_state_store() {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);

    const auto instrument = make_btc_instrument();
    const trading::core::MarketEvent event {
        .event_id = "event-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 42010.0,
        .quantity = 0.5,
        .bid_price = 42000.0,
        .ask_price = 42020.0,
        .exchange_timestamp = 1000,
        .receive_timestamp = 1100,
        .process_timestamp = 4500,
    };

    // Step 1: Apply the market event to the state store.
    store.apply(event);

    // Step 2: Read the stored state and verify the updated values.
    const auto state = store.get(instrument.instrument_id);
    expect_true(state.has_value(), "state should exist");
    expect_true(state->best_bid.has_value(), "best bid should exist");
    expect_equal(*state->best_bid, 42000.0, "best bid should match");
    expect_equal(*state->last_trade_price, 42010.0, "last trade price should match");
    expect_equal(state->last_process_timestamp, std::int64_t {4500}, "last process timestamp should match");

    // Step 3: Verify stale checks respect the fixed clock.
    expect_true(!store.is_stale(instrument.instrument_id, 1000), "state should not be stale yet");
    clock.set_now_ms(7001);
    expect_true(store.is_stale(instrument.instrument_id, 2000), "state should become stale");
}

// Verifies older market events cannot overwrite newer stored state.
void test_market_state_store_ignores_out_of_order_events() {
    trading::core::FixedClock clock(9000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();

    store.apply(trading::core::MarketEvent {
        .event_id = "newer",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 43000.0,
        .quantity = 1.0,
        .bid_price = 42990.0,
        .ask_price = 43010.0,
        .exchange_timestamp = 5000,
        .receive_timestamp = 5100,
        .process_timestamp = 5200,
    });
    store.apply(trading::core::MarketEvent {
        .event_id = "older",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41000.0,
        .quantity = 2.0,
        .bid_price = 40990.0,
        .ask_price = 41010.0,
        .exchange_timestamp = 4000,
        .receive_timestamp = 4100,
        .process_timestamp = 4200,
    });

    const auto state = store.get(instrument.instrument_id);
    expect_true(state.has_value(), "state should exist after out-of-order update");
    expect_equal(*state->best_bid, 42990.0, "older event should not overwrite best bid");
    expect_equal(*state->last_trade_price, 43000.0, "older event should not overwrite trade price");
    expect_equal(state->last_process_timestamp, std::int64_t {5200}, "newer process timestamp should remain");
}

// Verifies quote-only updates preserve the last known trade state.
void test_market_state_store_quote_update_preserves_trade_state() {
    trading::core::FixedClock clock(6000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();

    store.apply(trading::core::MarketEvent {
        .event_id = "trade",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 42050.0,
        .quantity = 0.8,
        .exchange_timestamp = 1000,
        .receive_timestamp = 1100,
        .process_timestamp = 1200,
    });
    store.apply(trading::core::MarketEvent {
        .event_id = "quote",
        .type = trading::core::MarketEventType::ticker,
        .instrument = instrument,
        .bid_price = 42040.0,
        .ask_price = 42060.0,
        .exchange_timestamp = 1300,
        .receive_timestamp = 1400,
        .process_timestamp = 1500,
    });

    const auto state = store.get(instrument.instrument_id);
    expect_true(state.has_value(), "state should exist after quote update");
    expect_equal(*state->best_bid, 42040.0, "best bid should update from quote");
    expect_equal(*state->last_trade_price, 42050.0, "quote update should preserve prior trade price");
    expect_equal(*state->last_trade_quantity, 0.8, "quote update should preserve prior trade quantity");
}

// Verifies book snapshots populate richer depth state and best prices.
void test_market_state_store_tracks_book_depth() {
    trading::core::FixedClock clock(6000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();

    store.apply(trading::core::MarketEvent {
        .event_id = "book-1",
        .type = trading::core::MarketEventType::book_snapshot,
        .instrument = instrument,
        .bid_levels = {
            {.price = 42000.0, .quantity = 1.2},
            {.price = 41999.5, .quantity = 2.0},
        },
        .ask_levels = {
            {.price = 42001.0, .quantity = 1.4},
            {.price = 42002.0, .quantity = 2.2},
        },
        .exchange_timestamp = 1000,
        .receive_timestamp = 1100,
        .process_timestamp = 1200,
    });

    const auto state = store.get(instrument.instrument_id);
    expect_true(state.has_value(), "book state should exist");
    expect_equal(state->bid_levels.size(), std::size_t {2}, "bid depth should be tracked");
    expect_equal(state->ask_levels.size(), std::size_t {2}, "ask depth should be tracked");
    expect_equal(*state->best_bid, 42000.0, "best bid should follow top depth level");
    expect_equal(*state->best_ask, 42001.0, "best ask should follow top depth level");
}

// Verifies feed health degrades on stale data and disconnects beyond pure timestamp checks.
void test_feed_health_tracker_detects_stale_and_disconnect_states() {
    trading::core::FixedClock clock(1000);
    trading::market_data::FeedHealthTracker tracker(clock);

    expect_equal(
        tracker.status(500, 200),
        trading::monitoring::HealthStatus::unknown,
        "brand-new tracker should be unknown");

    tracker.on_connect();
    expect_equal(
        tracker.status(500, 200),
        trading::monitoring::HealthStatus::degraded,
        "connected but unsubscribed feed should be degraded");

    tracker.on_subscribe_success();
    tracker.on_message(1000);
    expect_equal(
        tracker.status(500, 200),
        trading::monitoring::HealthStatus::healthy,
        "fresh subscribed feed should be healthy");

    clock.set_now_ms(1700);
    expect_equal(
        tracker.status(500, 200),
        trading::monitoring::HealthStatus::degraded,
        "stale feed should degrade before disconnect");

    tracker.on_disconnect();
    expect_equal(
        tracker.status(500, 200),
        trading::monitoring::HealthStatus::degraded,
        "recent disconnect should degrade first");

    clock.set_now_ms(2001);
    expect_equal(
        tracker.status(500, 200),
        trading::monitoring::HealthStatus::unavailable,
        "extended disconnect should become unavailable");
}

// Verifies the live feed controller reconnects and resubscribes after disconnects.
void test_live_market_data_feed_controller_reconnects_and_resubscribes() {
    trading::exchange::MockMarketDataFeedSession session;
    trading::monitoring::InMemoryMetricsCollector metrics;
    trading::exchange::LiveMarketDataFeedController controller(
        session,
        {"BTCUSDT", "ETHUSDT"},
        2,
        &metrics);

    controller.start();
    expect_true(controller.connected(), "controller should be connected after start");
    expect_equal(session.connect_calls(), std::size_t {1}, "start should connect once");
    expect_equal(session.subscribe_calls(), std::size_t {1}, "start should subscribe once");
    expect_equal(session.last_symbols().size(), std::size_t {2}, "subscription symbols should be forwarded");

    controller.handle_disconnect();
    expect_true(controller.connected(), "controller should reconnect after disconnect");
    expect_equal(controller.reconnect_attempts(), std::size_t {1}, "disconnect should increment reconnect attempts");
    expect_equal(session.disconnect_calls(), std::size_t {1}, "disconnect should be forwarded");
    expect_equal(session.connect_calls(), std::size_t {2}, "reconnect should call connect again");
    expect_equal(session.subscribe_calls(), std::size_t {2}, "reconnect should resubscribe");
    expect_equal(metrics.counter("market_data_disconnects"), std::int64_t {1}, "disconnect metric should increment");
    expect_equal(metrics.counter("market_data_reconnects"), std::int64_t {1}, "reconnect metric should increment");
}

// Verifies the live feed controller stops after exhausting reconnect attempts.
void test_live_market_data_feed_controller_stops_after_retry_budget() {
    trading::exchange::MockMarketDataFeedSession session;
    trading::exchange::LiveMarketDataFeedController controller(
        session,
        {"BTCUSDT"},
        0);

    controller.start();

    bool threw = false;
    try {
        controller.handle_disconnect();
    } catch (const std::runtime_error&) {
        threw = true;
    }

    expect_true(threw, "disconnect beyond retry budget should throw");
}

// Verifies the sample strategy emits a deterministic order when the threshold is crossed.
void test_sample_threshold_strategy_emits_order_on_trigger() {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();

    store.apply(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    trading::strategy::SampleThresholdStrategy strategy({
        .strategy_id = "threshold-1",
        .instrument_id = instrument.instrument_id,
        .trigger_price = 42000.0,
        .order_quantity = 0.25,
        .side = trading::core::OrderSide::buy,
    });
    const trading::strategy::StrategyContext context {
        .market_state_store = store,
        .clock = clock,
    };

    const auto requests = strategy.on_event(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    }, context);

    expect_equal(requests.size(), std::size_t {1}, "strategy should emit one order request");
    expect_equal(requests[0].strategy_id, std::string("threshold-1"), "strategy id should propagate");
    expect_equal(requests[0].quantity, 0.25, "configured order quantity should propagate");
    expect_true(requests[0].price.has_value(), "emitted order should include a limit price");
    expect_equal(*requests[0].price, 41990.0, "emitted price should match the triggering trade");
}

// Verifies the sample strategy stays idle when the trigger condition is not met.
void test_sample_threshold_strategy_no_order_without_trigger() {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();

    store.apply(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 42050.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    trading::strategy::SampleThresholdStrategy strategy({
        .strategy_id = "threshold-1",
        .instrument_id = instrument.instrument_id,
        .trigger_price = 42000.0,
        .order_quantity = 0.25,
        .side = trading::core::OrderSide::buy,
    });
    const trading::strategy::StrategyContext context {
        .market_state_store = store,
        .clock = clock,
    };

    const auto requests = strategy.on_event(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 42050.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    }, context);

    expect_true(requests.empty(), "strategy should not emit when trigger condition is not met");
}

// Verifies emitted orders inherit full market-event instrument metadata when config is sparse.
void test_sample_threshold_strategy_propagates_full_instrument_metadata() {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();

    store.apply(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    trading::strategy::SampleThresholdStrategy strategy({
        .strategy_id = "threshold-1",
        .instrument_id = instrument.instrument_id,
        .instrument = {
            .instrument_id = instrument.instrument_id,
        },
        .trigger_price = 42000.0,
        .order_quantity = 0.25,
        .side = trading::core::OrderSide::buy,
    });
    const trading::strategy::StrategyContext context {
        .market_state_store = store,
        .clock = clock,
    };

    log_test_step("emit threshold-strategy order from sparse configured instrument");
    const auto requests = strategy.on_event(trading::core::MarketEvent {
        .event_id = "market-2",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41990.0,
        .quantity = 0.25,
        .process_timestamp = 4901,
    }, context);

    log_test_step("verify emitted order carries full instrument metadata");
    expect_equal(requests.size(), std::size_t {1}, "strategy should emit one order");
    expect_equal(requests[0].instrument.exchange, std::string("binance"), "emitted order should carry exchange metadata");
    expect_equal(requests[0].instrument.symbol, std::string("BTCUSDT"), "emitted order should carry symbol metadata");
    expect_equal(requests[0].instrument.base_asset, std::string("BTC"), "emitted order should carry base-asset metadata");
    expect_equal(requests[0].instrument.quote_asset, std::string("USDT"), "emitted order should carry quote-asset metadata");
}

// Verifies a transaction command can reset the sample strategy after a prior signal.
void test_sample_threshold_strategy_reset_command() {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();

    store.apply(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    trading::strategy::SampleThresholdStrategy strategy({
        .strategy_id = "threshold-1",
        .instrument_id = instrument.instrument_id,
        .trigger_price = 42000.0,
        .order_quantity = 0.25,
        .side = trading::core::OrderSide::buy,
    });
    const trading::strategy::StrategyContext context {
        .market_state_store = store,
        .clock = clock,
    };

    const auto first_requests = strategy.on_event(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    }, context);
    const auto second_requests = strategy.on_event(trading::core::MarketEvent {
        .event_id = "market-2",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41980.0,
        .quantity = 0.5,
        .process_timestamp = 4950,
    }, context);

    auto reset_command = make_transaction("tx-reset", 30);
    reset_command.command_type = "reset_strategy";
    const auto reset_requests = strategy.on_event(reset_command, context);
    const auto third_requests = strategy.on_event(trading::core::MarketEvent {
        .event_id = "market-3",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41970.0,
        .quantity = 0.5,
        .process_timestamp = 4990,
    }, context);

    expect_equal(first_requests.size(), std::size_t {1}, "first trigger should emit");
    expect_true(second_requests.empty(), "strategy should suppress repeated signals before reset");
    expect_true(reset_requests.empty(), "reset command should not emit an order");
    expect_equal(third_requests.size(), std::size_t {1}, "strategy should emit again after reset");
}

// Verifies one failing strategy is paused without blocking healthy strategies in the same runtime.
void test_strategy_coordinator_isolates_failing_strategy() {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();

    store.apply(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    trading::strategy::StrategyCoordinator coordinator;
    coordinator.add_strategy(std::make_unique<trading::strategy::SampleThresholdStrategy>(
        trading::strategy::SampleThresholdStrategyConfig {
            .strategy_id = "threshold-1",
            .instrument_id = instrument.instrument_id,
            .instrument = instrument,
            .trigger_price = 42000.0,
            .order_quantity = 0.25,
            .side = trading::core::OrderSide::buy,
        }));
    coordinator.add_strategy(std::make_unique<ThrowingStrategy>("failing-1"));

    const trading::strategy::StrategyContext context {
        .market_state_store = store,
        .clock = clock,
    };
    const trading::core::MarketEvent event {
        .event_id = "market-2",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4950,
    };

    const auto requests = coordinator.on_event(event, context);

    expect_equal(requests.size(), std::size_t {1}, "healthy strategy should still emit when another strategy fails");
    expect_equal(coordinator.strategy_count(), std::size_t {2}, "both strategies should remain registered");
    expect_equal(coordinator.active_strategy_count(), std::size_t {1}, "failing strategy should be paused");
    expect_true(coordinator.is_paused("failing-1"), "failing strategy should be marked paused");

    const auto stats = coordinator.all_stats();
    expect_equal(stats.size(), std::size_t {2}, "stats should be present for both strategies");
    expect_true(stats[1].failures == 1 || stats[0].failures == 1, "one strategy should record a failure");
}

// Verifies valid orders pass baseline risk checks.
void test_risk_engine_approves_valid_order() {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();
    store.apply(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 42000.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    const trading::config::RiskConfig risk_config {
        .max_order_quantity = 1.0,
        .max_order_notional = 50000.0,
        .max_position_quantity = 2.0,
        .stale_after_ms = 1000,
        .kill_switch_enabled = false,
    };
    trading::risk::RiskEngine risk_engine(risk_config, store, clock);

    const auto decision = risk_engine.evaluate({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.5,
        .price = 42000.0,
    });

    expect_true(decision.approved, "valid order should be approved");
}

// Verifies oversize orders are rejected by the configured quantity limit.
void test_risk_engine_rejects_oversize_order() {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();
    store.apply(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 42000.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    const trading::config::RiskConfig risk_config {
        .max_order_quantity = 0.25,
        .max_order_notional = 50000.0,
        .max_position_quantity = 2.0,
        .stale_after_ms = 1000,
        .kill_switch_enabled = false,
    };
    trading::risk::RiskEngine risk_engine(risk_config, store, clock);

    const auto decision = risk_engine.evaluate({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.5,
        .price = 42000.0,
    });

    expect_true(!decision.approved, "oversize order should be rejected");
    expect_equal(*decision.reason, std::string("order quantity exceeds limit"), "oversize rejection reason should match");
}

// Verifies stale market data blocks submissions.
void test_risk_engine_rejects_stale_market() {
    trading::core::FixedClock clock(8000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();
    store.apply(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 42000.0,
        .quantity = 0.5,
        .process_timestamp = 1000,
    });

    const trading::config::RiskConfig risk_config {
        .max_order_quantity = 1.0,
        .max_order_notional = 50000.0,
        .max_position_quantity = 2.0,
        .stale_after_ms = 1000,
        .kill_switch_enabled = false,
    };
    trading::risk::RiskEngine risk_engine(risk_config, store, clock);

    const auto decision = risk_engine.evaluate({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.5,
        .price = 42000.0,
    });

    expect_true(!decision.approved, "stale-market order should be rejected");
    expect_equal(*decision.reason, std::string("market data is stale"), "stale-market rejection reason should match");
}

// Verifies the kill switch blocks all submissions.
void test_risk_engine_kill_switch_blocks_orders() {
    trading::core::FixedClock clock(5000);
    trading::market_data::MarketStateStore store(clock);
    const auto instrument = make_btc_instrument();
    store.apply(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = instrument,
        .price = 42000.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    const trading::config::RiskConfig risk_config {
        .max_order_quantity = 1.0,
        .max_order_notional = 50000.0,
        .max_position_quantity = 2.0,
        .stale_after_ms = 1000,
        .kill_switch_enabled = true,
    };
    trading::risk::RiskEngine risk_engine(risk_config, store, clock);

    const auto decision = risk_engine.evaluate({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.5,
        .price = 42000.0,
    });

    expect_true(!decision.approved, "kill switch should reject all orders");
    expect_equal(*decision.reason, std::string("kill switch enabled"), "kill-switch rejection reason should match");
}

// Verifies small limit orders are acknowledged and fully filled immediately.
void test_simulated_execution_engine_ack_and_fill() {
    trading::execution::SimulatedExecutionEngine execution_engine({
        .partial_fill_threshold = 1.0,
        .partial_fill_ratio = 0.5,
    });

    const auto result = execution_engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.25,
        .price = 42000.0,
    });

    expect_equal(result.updates.size(), std::size_t {2}, "small order should produce ack and fill updates");
    expect_equal(result.updates[0].status, trading::core::OrderStatus::acknowledged, "first update should acknowledge");
    expect_equal(result.updates[1].status, trading::core::OrderStatus::filled, "second update should fill");
    expect_equal(result.fills.size(), std::size_t {1}, "small order should produce one fill");
    expect_equal(result.fills[0].quantity, 0.25, "fill quantity should match request");
}

// Verifies the book tracks best bid and ask levels across multiple prices.
void test_order_book_tracks_best_levels() {
    trading::execution::OrderBook book(make_btc_instrument());

    expect_true(book.add_order(
        {
            .request_id = "req-1",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.4,
            .price = 42000.0,
        },
        "order-1",
        "client-1"),
        "first bid should be accepted");
    expect_true(book.add_order(
        {
            .request_id = "req-2",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.2,
            .price = 42100.0,
        },
        "order-2",
        "client-2"),
        "second bid should be accepted");
    expect_true(book.add_order(
        {
            .request_id = "req-3",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::sell,
            .type = trading::core::OrderType::limit,
            .quantity = 0.3,
            .price = 42200.0,
        },
        "order-3",
        "client-3"),
        "first ask should be accepted");
    expect_true(book.add_order(
        {
            .request_id = "req-4",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::sell,
            .type = trading::core::OrderType::limit,
            .quantity = 0.6,
            .price = 42300.0,
        },
        "order-4",
        "client-4"),
        "second ask should be accepted");

    const auto best_bid = book.best_bid();
    const auto best_ask = book.best_ask();
    const auto bid_levels = book.bid_levels();
    const auto ask_levels = book.ask_levels();

    expect_true(best_bid.has_value(), "best bid should exist");
    expect_true(best_ask.has_value(), "best ask should exist");
    expect_equal(best_bid->price, 42100.0, "best bid price should be highest bid");
    expect_equal(best_bid->total_quantity, 0.2, "best bid quantity should match resting quantity");
    expect_equal(best_ask->price, 42200.0, "best ask price should be lowest ask");
    expect_equal(best_ask->total_quantity, 0.3, "best ask quantity should match resting quantity");
    expect_equal(bid_levels.size(), std::size_t {2}, "two bid levels should exist");
    expect_equal(ask_levels.size(), std::size_t {2}, "two ask levels should exist");
    expect_equal(book.order_count(), std::size_t {4}, "four resting orders should exist");
}

// Verifies same-price resting orders preserve insertion order inside one level.
void test_order_book_preserves_fifo_within_price_level() {
    trading::execution::OrderBook book(make_btc_instrument());

    expect_true(book.add_order(
        {
            .request_id = "req-1",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.4,
            .price = 42000.0,
        },
        "order-1",
        "client-1"),
        "first same-price order should be accepted");
    expect_true(book.add_order(
        {
            .request_id = "req-2",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.6,
            .price = 42000.0,
        },
        "order-2",
        "client-2"),
        "second same-price order should be accepted");

    const auto first_order = book.find_order("order-1");
    const auto second_order = book.find_order("order-2");
    const auto best_bid = book.best_bid();

    expect_true(first_order.has_value(), "first order should be findable");
    expect_true(second_order.has_value(), "second order should be findable");
    expect_true(first_order->sequence_number < second_order->sequence_number, "sequence number should preserve FIFO ordering");
    expect_true(best_bid.has_value(), "best bid should exist");
    expect_equal(best_bid->order_count, std::size_t {2}, "best price level should contain both same-price orders");
    expect_equal(best_bid->total_quantity, 1.0, "same-price level quantity should aggregate");
}

// Verifies cancel removes the order and prunes empty levels.
void test_order_book_cancel_prunes_empty_level() {
    trading::execution::OrderBook book(make_btc_instrument());

    expect_true(book.add_order(
        {
            .request_id = "req-1",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.4,
            .price = 42100.0,
        },
        "order-1",
        "client-1"),
        "top bid should be accepted");
    expect_true(book.add_order(
        {
            .request_id = "req-2",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.7,
            .price = 42000.0,
        },
        "order-2",
        "client-2"),
        "second bid should be accepted");

    expect_true(book.cancel_order("order-1"), "existing top-of-book order should cancel");
    expect_true(!book.cancel_order("missing-order"), "missing order should not cancel");

    const auto best_bid = book.best_bid();
    const auto removed_order = book.find_order("order-1");
    const auto remaining_order = book.find_order("order-2");
    const auto bid_levels = book.bid_levels();

    expect_true(best_bid.has_value(), "best bid should still exist after one cancel");
    expect_equal(best_bid->price, 42000.0, "best bid should move to the next populated level");
    expect_true(!removed_order.has_value(), "canceled order should no longer be findable");
    expect_true(remaining_order.has_value(), "uncanceled order should remain");
    expect_equal(bid_levels.size(), std::size_t {1}, "empty canceled level should be pruned");
    expect_equal(book.order_count(), std::size_t {1}, "one resting order should remain after cancel");
}

// Verifies a crossing order fully matches the resting top of book.
void test_matching_engine_fully_matches_crossing_order() {
    trading::execution::MatchingEngine engine;

    log_test_step("submit resting sell order");
    const auto resting_result = engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42000.0,
    });
    log_test_step("submit crossing buy order");
    const auto taker_result = engine.submit({
        .request_id = "req-2",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42050.0,
    });

    const auto* book = engine.find_book(make_btc_instrument().instrument_id);

    log_test_step("verify full match and empty book");
    expect_equal(resting_result.updates.size(), std::size_t {1}, "non-crossing first order should only acknowledge");
    expect_equal(taker_result.updates.size(), std::size_t {2}, "crossing order should ack then fill");
    expect_equal(taker_result.updates[1].status, trading::core::OrderStatus::filled, "crossing order should fill");
    expect_equal(taker_result.fills.size(), std::size_t {1}, "crossing order should emit one fill");
    expect_equal(taker_result.fills[0].price, 42000.0, "fill price should use resting order price");
    expect_equal(taker_result.fills[0].quantity, 0.4, "fill quantity should match the resting quantity");
    expect_true(book != nullptr, "book should exist after matching");
    expect_equal(book->order_count(), std::size_t {0}, "book should be empty after full match");
}

// Verifies partial matching leaves the residual quantity resting on the book.
void test_matching_engine_partially_matches_and_rests_residual() {
    trading::execution::MatchingEngine engine;

    log_test_step("submit resting sell liquidity");
    const auto ignored_resting_result = engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42000.0,
    });
    static_cast<void>(ignored_resting_result);
    log_test_step("submit larger crossing buy order");
    const auto taker_result = engine.submit({
        .request_id = "req-2",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 1.0,
        .price = 42000.0,
    });

    const auto* book = engine.find_book(make_btc_instrument().instrument_id);
    expect_true(book != nullptr, "book should exist after partial match");

    const auto resting_taker_order = book->find_order(taker_result.order_id);
    const auto best_bid = book->best_bid();
    const auto best_ask = book->best_ask();

    log_test_step("verify partial fill and residual resting quantity");
    expect_equal(taker_result.updates.size(), std::size_t {2}, "partially matched order should ack then partially fill");
    expect_equal(taker_result.updates[1].status, trading::core::OrderStatus::partially_filled, "terminal update should be partial");
    expect_equal(taker_result.updates[1].filled_quantity, 0.4, "filled quantity should match executed amount");
    expect_equal(taker_result.fills.size(), std::size_t {1}, "partial match should emit one fill");
    expect_true(resting_taker_order.has_value(), "residual quantity should rest on the book");
    expect_equal(resting_taker_order->remaining_quantity, 0.6, "resting residual should equal remaining quantity");
    expect_true(best_bid.has_value(), "residual should become top bid");
    expect_equal(best_bid->price, 42000.0, "resting residual price should remain on the incoming limit price");
    expect_equal(best_bid->total_quantity, 0.6, "residual best bid quantity should match remaining quantity");
    expect_true(!best_ask.has_value(), "all opposing liquidity should be consumed");
}

// Verifies a crossing order sweeps multiple resting levels in price-time order.
void test_matching_engine_sweeps_multiple_levels_in_price_time_order() {
    trading::execution::MatchingEngine engine;

    const auto first_resting = engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 0.2,
        .price = 42000.0,
    });
    const auto second_resting = engine.submit({
        .request_id = "req-2",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 0.3,
        .price = 42000.0,
    });
    const auto ignored_third_resting = engine.submit({
        .request_id = "req-3",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42100.0,
    });
    static_cast<void>(ignored_third_resting);
    const auto taker_result = engine.submit({
        .request_id = "req-4",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.7,
        .price = 42100.0,
    });

    const auto* book = engine.find_book(make_btc_instrument().instrument_id);
    expect_true(book != nullptr, "book should exist after sweep");

    expect_equal(taker_result.updates.size(), std::size_t {2}, "sweeping order should ack then fill");
    expect_equal(taker_result.updates[1].status, trading::core::OrderStatus::filled, "sweep should fully fill incoming order");
    expect_equal(taker_result.fills.size(), std::size_t {3}, "sweep should produce one fill per matched resting order");
    expect_near(taker_result.fills[0].quantity, 0.2, 1e-9, "first FIFO match should use the first same-price order");
    expect_near(taker_result.fills[1].quantity, 0.3, 1e-9, "second FIFO match should use the second same-price order");
    expect_near(taker_result.fills[2].quantity, 0.2, 1e-9, "final match should consume part of the next price level");
    expect_equal(taker_result.fills[0].price, 42000.0, "first fill price should match the best resting ask");
    expect_equal(taker_result.fills[1].price, 42000.0, "second fill price should preserve same-price FIFO");
    expect_equal(taker_result.fills[2].price, 42100.0, "third fill price should move to the next ask level");
    expect_true(!book->find_order(first_resting.order_id).has_value(), "first same-price resting order should be consumed");
    expect_true(!book->find_order(second_resting.order_id).has_value(), "second same-price resting order should be consumed");
    const auto best_ask = book->best_ask();
    expect_true(best_ask.has_value(), "partially consumed next level should remain");
    expect_equal(best_ask->price, 42100.0, "remaining ask should stay on the partially consumed level");
    expect_near(best_ask->total_quantity, 0.2, 1e-9, "remaining ask quantity should reflect the residual");
}

// Verifies non-crossing orders rest on the book without fills.
void test_matching_engine_leaves_non_crossing_order_resting() {
    trading::execution::MatchingEngine engine;

    const auto ignored_resting_result = engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42100.0,
    });
    static_cast<void>(ignored_resting_result);
    const auto result = engine.submit({
        .request_id = "req-2",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.5,
        .price = 42000.0,
    });

    const auto* book = engine.find_book(make_btc_instrument().instrument_id);
    expect_true(book != nullptr, "book should exist after resting order");

    expect_equal(result.updates.size(), std::size_t {1}, "non-crossing order should only acknowledge");
    expect_equal(result.fills.size(), std::size_t {0}, "non-crossing order should not fill");
    expect_true(book->find_order(result.order_id).has_value(), "non-crossing order should rest on the book");
    expect_equal(book->order_count(), std::size_t {2}, "both sides should remain resting");
}

// Verifies cancel removes a fully resting order and returns the expected lifecycle updates.
void test_matching_engine_cancels_resting_order() {
    trading::execution::MatchingEngine engine;

    const auto resting_result = engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42100.0,
    });
    const auto cancel_result = engine.cancel(resting_result.order_id, resting_result.client_order_id);

    const auto* book = engine.find_book(make_btc_instrument().instrument_id);
    const auto order = engine.get_order(resting_result.order_id);

    expect_equal(cancel_result.updates.size(), std::size_t {2}, "cancel should produce pending-cancel and canceled updates");
    expect_equal(cancel_result.updates[0].status, trading::core::OrderStatus::pending_cancel, "first cancel update should be pending_cancel");
    expect_equal(cancel_result.updates[1].status, trading::core::OrderStatus::canceled, "second cancel update should be canceled");
    expect_true(book != nullptr, "book should still exist");
    expect_equal(book->order_count(), std::size_t {0}, "canceled resting order should be removed from the book");
    expect_true(order.has_value(), "canceled order should remain queryable");
    expect_equal(order->status, trading::core::OrderStatus::canceled, "tracked order state should become canceled");
    expect_equal(order->filled_quantity, 0.0, "canceled resting order should preserve zero filled quantity");
}

// Verifies a partially filled resting order can be canceled without losing cumulative fill state.
void test_matching_engine_cancels_partially_filled_resting_order() {
    trading::execution::MatchingEngine engine;

    log_test_step("submit maker sell order");
    const auto maker_result = engine.submit({
        .request_id = "req-1",
        .strategy_id = "maker-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 1.0,
        .price = 42000.0,
    });
    log_test_step("submit taker buy order for partial maker fill");
    const auto taker_result = engine.submit({
        .request_id = "req-2",
        .strategy_id = "taker-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42000.0,
    });
    log_test_step("cancel remaining maker quantity");
    const auto cancel_result = engine.cancel(maker_result.order_id, maker_result.client_order_id);

    const auto* book = engine.find_book(make_btc_instrument().instrument_id);
    const auto maker_order = engine.get_order(maker_result.order_id);
    const auto taker_order = engine.get_order(taker_result.order_id);

    log_test_step("verify cancel preserves cumulative maker fill state");
    expect_equal(cancel_result.updates.size(), std::size_t {2}, "cancel after partial fill should still produce two cancel updates");
    expect_equal(cancel_result.updates[0].filled_quantity, 0.4, "pending cancel should preserve cumulative filled quantity");
    expect_equal(cancel_result.updates[1].filled_quantity, 0.4, "canceled update should preserve cumulative filled quantity");
    expect_true(book != nullptr, "book should still exist");
    expect_equal(book->order_count(), std::size_t {0}, "residual resting quantity should be removed after cancel");
    expect_true(maker_order.has_value(), "maker order should remain tracked");
    expect_equal(maker_order->status, trading::core::OrderStatus::canceled, "partially filled maker should become canceled");
    expect_equal(maker_order->filled_quantity, 0.4, "maker order should retain its cumulative filled quantity");
    expect_true(taker_order.has_value(), "taker order should remain tracked");
    expect_equal(taker_order->status, trading::core::OrderStatus::filled, "taker order should remain terminal filled");
}

// Verifies internal tracked order state reflects matching outcomes for both maker and taker orders.
void test_matching_engine_tracks_order_lifecycle_state() {
    trading::execution::MatchingEngine engine;

    const auto maker_result = engine.submit({
        .request_id = "req-1",
        .strategy_id = "maker-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 1.0,
        .price = 42000.0,
    });
    const auto taker_result = engine.submit({
        .request_id = "req-2",
        .strategy_id = "taker-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42010.0,
    });

    const auto maker_order = engine.get_order(maker_result.order_id);
    const auto taker_order = engine.get_order(taker_result.order_id);

    expect_true(maker_order.has_value(), "maker order should be queryable");
    expect_true(taker_order.has_value(), "taker order should be queryable");
    expect_equal(maker_order->status, trading::core::OrderStatus::partially_filled, "maker order should become partially filled after being hit");
    expect_equal(maker_order->filled_quantity, 0.4, "maker cumulative filled quantity should track matched volume");
    expect_equal(taker_order->status, trading::core::OrderStatus::filled, "crossing taker order should become filled");
    expect_equal(taker_order->filled_quantity, 0.4, "taker cumulative filled quantity should match executed volume");
}

// Verifies invalid matching-engine requests are rejected cleanly.
void test_matching_engine_rejects_invalid_limit_requests() {
    trading::execution::MatchingEngine engine;

    log_test_step("submit zero-quantity limit order");
    const auto zero_quantity_result = engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.0,
        .price = 42000.0,
    });
    log_test_step("submit zero-price limit order");
    const auto zero_price_result = engine.submit({
        .request_id = "req-2",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.1,
        .price = 0.0,
    });

    log_test_step("verify rejected lifecycle state for invalid orders");
    expect_equal(zero_quantity_result.updates.size(), std::size_t {1}, "zero-quantity order should reject immediately");
    expect_equal(zero_quantity_result.updates[0].status, trading::core::OrderStatus::rejected, "zero-quantity order should be rejected");
    expect_equal(zero_price_result.updates.size(), std::size_t {1}, "zero-price order should reject immediately");
    expect_equal(zero_price_result.updates[0].status, trading::core::OrderStatus::rejected, "zero-price order should be rejected");
    const auto zero_quantity_order = engine.get_order(zero_quantity_result.order_id);
    const auto zero_price_order = engine.get_order(zero_price_result.order_id);
    expect_true(zero_quantity_order.has_value(), "rejected zero-quantity order should remain queryable");
    expect_true(zero_price_order.has_value(), "rejected zero-price order should remain queryable");
    expect_equal(zero_quantity_order->status, trading::core::OrderStatus::rejected, "tracked zero-quantity order should remain rejected");
    expect_equal(zero_price_order->status, trading::core::OrderStatus::rejected, "tracked zero-price order should remain rejected");
}

// Verifies terminal orders cannot be canceled after they are fully filled.
void test_matching_engine_rejects_cancel_for_terminal_order() {
    trading::execution::MatchingEngine engine;

    log_test_step("submit resting sell then fully crossing buy order");
    const auto resting_result = engine.submit({
        .request_id = "req-1",
        .strategy_id = "maker-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::sell,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42000.0,
    });
    const auto taker_result = engine.submit({
        .request_id = "req-2",
        .strategy_id = "taker-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.4,
        .price = 42000.0,
    });
    static_cast<void>(taker_result);

    log_test_step("attempt cancel on terminal filled maker order");
    const auto cancel_result = engine.cancel(resting_result.order_id, resting_result.client_order_id);

    log_test_step("verify cancel rejection preserves filled quantity");
    expect_equal(cancel_result.updates.size(), std::size_t {1}, "terminal cancel should return one rejection update");
    expect_equal(cancel_result.updates[0].status, trading::core::OrderStatus::rejected, "terminal order cancel should reject");
    expect_true(cancel_result.updates[0].reason.has_value(), "terminal cancel rejection should include a reason");
    expect_equal(*cancel_result.updates[0].reason, std::string("order is not cancelable"), "terminal cancel reason should match");
    expect_equal(cancel_result.updates[0].filled_quantity, 0.4, "terminal cancel rejection should preserve cumulative filled quantity");
}

// Verifies invalid open-order recovery input does not leak into tracked state or the book.
void test_matching_engine_restore_open_order_skips_invalid_state() {
    trading::execution::MatchingEngine engine;

    log_test_step("attempt restore with fully filled quantity");
    engine.restore_open_order(
        "restored-order-1",
        "restored-client-1",
        {
            .request_id = "req-restore-1",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.5,
            .price = 42000.0,
        },
        0.5);
    log_test_step("attempt restore with invalid price");
    engine.restore_open_order(
        "restored-order-2",
        "restored-client-2",
        {
            .request_id = "req-restore-2",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.5,
            .price = 0.0,
        },
        0.0);

    log_test_step("verify invalid restore attempts do not populate tracked order state");
    expect_true(!engine.get_order("restored-order-1").has_value(), "fully filled restore input should be ignored");
    expect_true(!engine.get_order("restored-order-2").has_value(), "invalid-price restore input should be ignored");
    const auto* book = engine.find_book(make_btc_instrument().instrument_id);
    expect_true(book == nullptr || book->order_count() == 0, "invalid restore attempts should not leave resting orders");
}

// Verifies market book snapshots can fill resting local orders from external ask liquidity.
void test_matching_engine_processes_market_ask_levels_against_resting_bids() {
    trading::execution::MatchingEngine engine;

    log_test_step("restore resting local buy order");
    engine.restore_open_order(
        "resting-buy-1",
        "resting-client-1",
        {
            .request_id = "req-restore-1",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 1.0,
            .price = 42000.0,
        },
        0.0);

    log_test_step("apply external ask-side book snapshot");
    const auto result = engine.process_market_event({
        .event_id = "book-1",
        .type = trading::core::MarketEventType::book_snapshot,
        .instrument = make_btc_instrument(),
        .ask_levels = {
            {.price = 41990.0, .quantity = 0.4},
            {.price = 42000.0, .quantity = 0.3},
        },
        .process_timestamp = 1000,
    });

    log_test_step("verify external ask liquidity partially fills the resting buy order");
    expect_equal(result.fills.size(), std::size_t {2}, "two executable ask levels should produce two fills");
    expect_equal(result.fills[0].order_id, std::string("resting-buy-1"), "fills should be attributed to the resting local order");
    expect_equal(result.fills[0].side, trading::core::OrderSide::buy, "resting local buy should emit buy-side fill");
    expect_equal(result.fills[0].price, 41990.0, "first fill should use first ask level price");
    expect_equal(result.fills[1].price, 42000.0, "second fill should use second ask level price");
    expect_near(result.fills[0].quantity, 0.4, 1e-9, "first fill quantity should match first ask level quantity");
    expect_near(result.fills[1].quantity, 0.3, 1e-9, "second fill quantity should match second ask level quantity");
    expect_equal(result.updates.size(), std::size_t {1}, "one resting order update should be emitted");
    expect_equal(result.updates[0].status, trading::core::OrderStatus::partially_filled, "resting buy order should become partially filled");
    expect_near(result.updates[0].filled_quantity, 0.7, 1e-9, "cumulative filled quantity should aggregate across ask levels");
    const auto order = engine.get_order("resting-buy-1");
    expect_true(order.has_value(), "resting order should remain tracked after market-driven partial fill");
    expect_equal(order->status, trading::core::OrderStatus::partially_filled, "tracked order state should reflect market-driven partial fill");
    expect_near(order->filled_quantity, 0.7, 1e-9, "tracked filled quantity should aggregate market-driven fills");
}

// Verifies market book snapshots can fill resting local sell orders from external bid liquidity.
void test_matching_engine_processes_market_bid_levels_against_resting_asks() {
    trading::execution::MatchingEngine engine;

    log_test_step("restore resting local sell order");
    engine.restore_open_order(
        "resting-sell-1",
        "resting-client-1",
        {
            .request_id = "req-restore-1",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::sell,
            .type = trading::core::OrderType::limit,
            .quantity = 0.5,
            .price = 42010.0,
        },
        0.0);

    log_test_step("apply external bid-side book snapshot");
    const auto result = engine.process_market_event({
        .event_id = "book-1",
        .type = trading::core::MarketEventType::book_snapshot,
        .instrument = make_btc_instrument(),
        .bid_levels = {
            {.price = 42020.0, .quantity = 0.5},
        },
        .process_timestamp = 1000,
    });

    log_test_step("verify external bid liquidity fully fills the resting sell order");
    expect_equal(result.fills.size(), std::size_t {1}, "one executable bid level should produce one fill");
    expect_equal(result.fills[0].order_id, std::string("resting-sell-1"), "fill should be attributed to the resting local sell order");
    expect_equal(result.fills[0].side, trading::core::OrderSide::sell, "resting local sell should emit sell-side fill");
    expect_equal(result.fills[0].price, 42020.0, "fill should use external bid level price");
    expect_near(result.fills[0].quantity, 0.5, 1e-9, "fill quantity should match remaining resting sell quantity");
    expect_equal(result.updates.size(), std::size_t {1}, "one resting sell update should be emitted");
    expect_equal(result.updates[0].status, trading::core::OrderStatus::filled, "resting sell order should become filled");
    const auto order = engine.get_order("resting-sell-1");
    expect_true(order.has_value(), "filled resting sell should remain tracked");
    expect_equal(order->status, trading::core::OrderStatus::filled, "tracked resting sell should become filled");
}

// Verifies runtime applies market-driven fills from external book snapshots into portfolio state.
void test_simulation_runtime_market_event_applies_market_driven_fill() {
    trading::core::FixedClock clock(5000);
    trading::app::SimulationRuntime runtime(
        clock,
        {
            .max_order_quantity = 1.0,
            .max_order_notional = 50000.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 1000,
            .kill_switch_enabled = false,
        },
        {
            .strategy = {
                .strategy_id = "threshold-1",
                .instrument_id = "binance:BTCUSDT",
                .trigger_price = 41000.0,
                .order_quantity = 0.05,
                .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 1.0,
                .partial_fill_ratio = 0.5,
            },
            .auto_complete_partial_fills = false,
        });

    log_test_step("restore resting local buy order for market-driven fill");
    runtime.restore_open_order({
        .order_id = "resting-buy-1",
        .client_order_id = "resting-client-1",
        .strategy_id = "maker-1",
        .instrument_id = "binance:BTCUSDT",
        .side = trading::core::OrderSide::buy,
        .order_type = trading::core::OrderType::limit,
        .status = trading::core::OrderStatus::acknowledged,
        .quantity = 0.4,
        .price = 42000.0,
        .filled_quantity = 0.0,
    });

    log_test_step("process external book snapshot through simulation runtime");
    runtime.on_event(trading::core::MarketEvent {
        .event_id = "book-1",
        .type = trading::core::MarketEventType::book_snapshot,
        .instrument = make_btc_instrument(),
        .ask_levels = {
            {.price = 41995.0, .quantity = 0.4},
        },
        .process_timestamp = 4900,
    });

    log_test_step("verify market-driven fill is applied into portfolio");
    expect_equal(runtime.applied_fill_count(), std::size_t {1}, "market-driven book snapshot should apply one fill");
    const auto position = runtime.portfolio().get_position("binance:BTCUSDT");
    expect_true(position.has_value(), "market-driven fill should open a position");
    expect_near(position->net_quantity, 0.4, 1e-9, "position quantity should match market-driven fill quantity");
    expect_near(position->average_entry_price, 41995.0, 1e-9, "position entry price should use external ask level price");
}

// Verifies unsupported market orders are rejected by the simulator.
void test_simulated_execution_engine_rejects_market_order() {
    trading::execution::SimulatedExecutionEngine execution_engine({
        .partial_fill_threshold = 1.0,
        .partial_fill_ratio = 0.5,
    });

    const auto result = execution_engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::market,
        .quantity = 0.25,
        .price = std::nullopt,
    });

    expect_equal(result.updates.size(), std::size_t {1}, "market order should be rejected immediately");
    expect_equal(result.updates[0].status, trading::core::OrderStatus::rejected, "market order should be rejected");
    expect_equal(*result.updates[0].reason, std::string("market orders are not supported by the simulator"), "market-order rejection reason should match");
}

// Verifies larger orders can be partially filled and later completed.
void test_simulated_execution_engine_partial_then_full_fill() {
    trading::execution::SimulatedExecutionEngine execution_engine({
        .partial_fill_threshold = 0.50,
        .partial_fill_ratio = 0.40,
    });

    const auto initial_result = execution_engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 1.0,
        .price = 42000.0,
    });
    const auto completion_result = execution_engine.complete_open_order(initial_result.order_id);

    expect_equal(initial_result.updates.size(), std::size_t {2}, "partial order should produce ack and partial-fill updates");
    expect_equal(initial_result.updates[1].status, trading::core::OrderStatus::partially_filled, "second update should be partial fill");
    expect_equal(initial_result.fills[0].quantity, 0.4, "partial fill quantity should respect the configured ratio");
    expect_equal(completion_result.updates.size(), std::size_t {1}, "completion should produce one terminal update");
    expect_equal(completion_result.updates[0].status, trading::core::OrderStatus::filled, "completion should fill the order");
    expect_equal(completion_result.fills[0].quantity, 0.6, "completion fill should use the remaining quantity");
}

// Verifies open orders can be canceled after a partial fill.
void test_simulated_execution_engine_cancel_open_order() {
    trading::execution::SimulatedExecutionEngine execution_engine({
        .partial_fill_threshold = 0.50,
        .partial_fill_ratio = 0.50,
    });

    const auto initial_result = execution_engine.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 1.0,
        .price = 42000.0,
    });
    const auto cancel_result = execution_engine.cancel_open_order(initial_result.order_id);

    expect_equal(cancel_result.updates.size(), std::size_t {2}, "cancel should produce pending-cancel and canceled updates");
    expect_equal(cancel_result.updates[0].status, trading::core::OrderStatus::pending_cancel, "first cancel update should be pending_cancel");
    expect_equal(cancel_result.updates[1].status, trading::core::OrderStatus::canceled, "second cancel update should be canceled");
}

// Verifies live execution reports drive a full local order lifecycle and fill generation.
void test_live_execution_tracker_applies_full_lifecycle() {
    trading::execution::LiveExecutionTracker tracker;
    const auto instrument = make_btc_instrument();

    tracker.register_order(
        {
            .request_id = "req-1",
            .strategy_id = "strategy-1",
            .instrument = instrument,
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 1.0,
            .price = 42000.0,
        },
        "live-order-1",
        "live-client-1");

    const auto ack_result = tracker.apply_report({
        .report_id = "report-ack-1",
        .order_id = "live-order-1",
        .client_order_id = "live-client-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .status = trading::core::OrderStatus::acknowledged,
        .cumulative_filled_quantity = 0.0,
        .exchange_timestamp = 1000,
    });
    const auto partial_result = tracker.apply_report({
        .report_id = "report-fill-1",
        .order_id = "live-order-1",
        .client_order_id = "live-client-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .status = trading::core::OrderStatus::partially_filled,
        .cumulative_filled_quantity = 0.4,
        .last_fill_price = 42000.0,
        .last_fill_fee = 1.0,
        .exchange_timestamp = 1010,
    });
    const auto final_result = tracker.apply_report({
        .report_id = "report-fill-2",
        .order_id = "live-order-1",
        .client_order_id = "live-client-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .status = trading::core::OrderStatus::filled,
        .cumulative_filled_quantity = 1.0,
        .last_fill_price = 42010.0,
        .last_fill_fee = 1.5,
        .exchange_timestamp = 1020,
    });

    expect_true(ack_result.applied, "acknowledgement report should apply");
    expect_equal(ack_result.updates.size(), std::size_t {1}, "acknowledgement should emit one order update");
    expect_equal(ack_result.updates[0].status, trading::core::OrderStatus::acknowledged, "acknowledgement status should match");

    expect_true(partial_result.applied, "partial fill report should apply");
    expect_equal(partial_result.updates[0].status, trading::core::OrderStatus::partially_filled, "partial status should match");
    expect_equal(partial_result.fills.size(), std::size_t {1}, "partial fill should emit one fill");
    expect_equal(partial_result.fills[0].quantity, 0.4, "partial fill delta should match cumulative change");

    expect_true(final_result.applied, "final fill report should apply");
    expect_equal(final_result.updates[0].status, trading::core::OrderStatus::filled, "final status should match");
    expect_equal(final_result.fills.size(), std::size_t {1}, "final fill should emit one fill");
    expect_equal(final_result.fills[0].quantity, 0.6, "final fill delta should be remaining quantity");

    const auto order = tracker.get_order("live-order-1");
    expect_true(order.has_value(), "tracked live order should exist");
    expect_equal(order->status, trading::core::OrderStatus::filled, "tracked live order should reach filled");
    expect_equal(order->filled_quantity, 1.0, "tracked filled quantity should match final cumulative quantity");
}

// Verifies duplicate execution reports are ignored idempotently.
void test_live_execution_tracker_ignores_duplicate_reports() {
    trading::execution::LiveExecutionTracker tracker;
    const auto instrument = make_btc_instrument();

    tracker.register_order(
        {
            .request_id = "req-1",
            .strategy_id = "strategy-1",
            .instrument = instrument,
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 1.0,
            .price = 42000.0,
        },
        "live-order-1",
        "live-client-1");

    const auto first_result = tracker.apply_report({
        .report_id = "report-dup-1",
        .order_id = "live-order-1",
        .client_order_id = "live-client-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .status = trading::core::OrderStatus::partially_filled,
        .cumulative_filled_quantity = 0.5,
        .last_fill_price = 42000.0,
        .exchange_timestamp = 1000,
    });
    const auto duplicate_result = tracker.apply_report({
        .report_id = "report-dup-1",
        .order_id = "live-order-1",
        .client_order_id = "live-client-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .status = trading::core::OrderStatus::partially_filled,
        .cumulative_filled_quantity = 0.5,
        .last_fill_price = 42000.0,
        .exchange_timestamp = 1000,
    });

    expect_true(first_result.applied, "first report should apply");
    expect_true(duplicate_result.ignored_duplicate, "duplicate report should be ignored");
    expect_true(!duplicate_result.applied, "duplicate report should not apply twice");

    const auto order = tracker.get_order("live-order-1");
    expect_true(order.has_value(), "tracked order should still exist");
    expect_equal(order->filled_quantity, 0.5, "duplicate report should not change filled quantity");
}

// Verifies late execution reports do not roll terminal local state backward.
void test_live_execution_tracker_ignores_late_terminal_regressions() {
    trading::execution::LiveExecutionTracker tracker;
    const auto instrument = make_btc_instrument();

    tracker.register_order(
        {
            .request_id = "req-1",
            .strategy_id = "strategy-1",
            .instrument = instrument,
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 1.0,
            .price = 42000.0,
        },
        "live-order-1",
        "live-client-1");

    const auto final_apply = tracker.apply_report({
        .report_id = "report-final-1",
        .order_id = "live-order-1",
        .client_order_id = "live-client-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .status = trading::core::OrderStatus::filled,
        .cumulative_filled_quantity = 1.0,
        .last_fill_price = 42000.0,
        .exchange_timestamp = 2000,
    });
    expect_true(final_apply.applied, "terminal fill report should apply before late regression test");

    const auto late_result = tracker.apply_report({
        .report_id = "report-late-1",
        .order_id = "live-order-1",
        .client_order_id = "live-client-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .status = trading::core::OrderStatus::partially_filled,
        .cumulative_filled_quantity = 0.4,
        .last_fill_price = 41990.0,
        .exchange_timestamp = 1500,
    });

    expect_true(late_result.ignored_stale, "late regressive report should be ignored");
    const auto order = tracker.get_order("live-order-1");
    expect_true(order.has_value(), "tracked order should still exist");
    expect_equal(order->status, trading::core::OrderStatus::filled, "late report should not roll terminal state backward");
    expect_equal(order->filled_quantity, 1.0, "late report should not reduce filled quantity");
}

// Verifies the first buy fill opens a long position and updates balances.
void test_portfolio_service_opens_position_from_first_fill() {
    trading::portfolio::PortfolioService portfolio_service;
    const auto instrument = make_btc_instrument();

    portfolio_service.apply_fill({
        .fill_id = "fill-1",
        .order_id = "order-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .price = 42000.0,
        .quantity = 1.0,
        .fee = 10.0,
    });

    const auto position = portfolio_service.get_position(instrument.instrument_id);
    expect_true(position.has_value(), "position should exist after first fill");
    expect_equal(position->net_quantity, 1.0, "position net quantity should match buy fill");
    expect_equal(position->average_entry_price, 42000.0, "average entry should match first fill price");

    const auto base_balance = portfolio_service.get_balance(instrument.base_asset);
    const auto quote_balance = portfolio_service.get_balance(instrument.quote_asset);
    expect_equal(base_balance.total_balance, 1.0, "base balance should increase on buy");
    expect_equal(quote_balance.total_balance, -42010.0, "quote balance should include notional and fee");
}

// Verifies average entry price updates across additional same-side fills.
void test_portfolio_service_updates_average_entry_price() {
    trading::portfolio::PortfolioService portfolio_service;
    const auto instrument = make_btc_instrument();

    portfolio_service.apply_fill({
        .fill_id = "fill-1",
        .order_id = "order-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .price = 42000.0,
        .quantity = 1.0,
        .fee = 0.0,
    });
    portfolio_service.apply_fill({
        .fill_id = "fill-2",
        .order_id = "order-2",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .price = 43000.0,
        .quantity = 1.0,
        .fee = 0.0,
    });

    const auto position = portfolio_service.get_position(instrument.instrument_id);
    expect_true(position.has_value(), "position should exist");
    expect_equal(position->net_quantity, 2.0, "net quantity should include both buys");
    expect_equal(position->average_entry_price, 42500.0, "average entry should be weighted across fills");
}

// Verifies realized PnL is computed when reducing a long position.
void test_portfolio_service_realized_pnl_on_position_reduction() {
    trading::portfolio::PortfolioService portfolio_service;
    const auto instrument = make_btc_instrument();

    portfolio_service.apply_fill({
        .fill_id = "fill-1",
        .order_id = "order-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .price = 40000.0,
        .quantity = 1.0,
        .fee = 0.0,
    });
    portfolio_service.apply_fill({
        .fill_id = "fill-2",
        .order_id = "order-2",
        .instrument = instrument,
        .side = trading::core::OrderSide::sell,
        .price = 41000.0,
        .quantity = 0.4,
        .fee = 0.0,
    });

    const auto position = portfolio_service.get_position(instrument.instrument_id);
    expect_true(position.has_value(), "position should exist");
    expect_equal(position->net_quantity, 0.6, "position should be reduced after sell");
    expect_equal(position->realized_pnl, 400.0, "realized pnl should reflect closed quantity");
}

// Verifies unrealized PnL updates when mark prices change.
void test_portfolio_service_unrealized_pnl_from_mark_price() {
    trading::portfolio::PortfolioService portfolio_service;
    const auto instrument = make_btc_instrument();

    portfolio_service.apply_fill({
        .fill_id = "fill-1",
        .order_id = "order-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .price = 40000.0,
        .quantity = 1.0,
        .fee = 0.0,
    });
    portfolio_service.set_mark_price(instrument.instrument_id, 40500.0);

    const auto position = portfolio_service.get_position(instrument.instrument_id);
    expect_true(position.has_value(), "position should exist");
    expect_equal(position->unrealized_pnl, 500.0, "unrealized pnl should use mark price");
}

// Verifies fees are reflected in quote balances for both buy and sell fills.
void test_portfolio_service_fee_handling_on_balances() {
    trading::portfolio::PortfolioService portfolio_service;
    const auto instrument = make_btc_instrument();

    portfolio_service.apply_fill({
        .fill_id = "fill-1",
        .order_id = "order-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .price = 100.0,
        .quantity = 1.0,
        .fee = 2.0,
    });
    portfolio_service.apply_fill({
        .fill_id = "fill-2",
        .order_id = "order-2",
        .instrument = instrument,
        .side = trading::core::OrderSide::sell,
        .price = 120.0,
        .quantity = 0.5,
        .fee = 1.0,
    });

    const auto base_balance = portfolio_service.get_balance(instrument.base_asset);
    const auto quote_balance = portfolio_service.get_balance(instrument.quote_asset);
    expect_equal(base_balance.total_balance, 0.5, "base balance should reflect buy then partial sell");
    expect_equal(quote_balance.total_balance, -43.0, "quote balance should account for both fees");
}

// Verifies a full in-process simulation run reaches portfolio updates on the happy path.
void test_simulation_runtime_happy_path() {
    trading::core::FixedClock clock(5000);
    trading::app::SimulationRuntime runtime(
        clock,
        {
            .max_order_quantity = 1.0,
            .max_order_notional = 50000.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 1000,
            .kill_switch_enabled = false,
        },
        {
            .strategy = {
                .strategy_id = "threshold-1",
                .instrument_id = "binance:BTCUSDT",
                .trigger_price = 42000.0,
                .order_quantity = 0.05,
                .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 0.10,
                .partial_fill_ratio = 0.50,
            },
            .auto_complete_partial_fills = false,
        });

    log_test_step("seed restored resting sell order for matching");
    runtime.restore_open_order({
        .order_id = "resting-sell-1",
        .client_order_id = "resting-client-1",
        .strategy_id = "maker-1",
        .instrument_id = "binance:BTCUSDT",
        .side = trading::core::OrderSide::sell,
        .order_type = trading::core::OrderType::limit,
        .status = trading::core::OrderStatus::acknowledged,
        .quantity = 0.05,
        .price = 41990.0,
        .filled_quantity = 0.0,
    });

    log_test_step("process trigger trade event through runtime");
    runtime.on_event(trading::core::MarketEvent {
        .event_id = "event-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    log_test_step("verify approved request and applied fill");
    expect_equal(runtime.risk_approved_count(), std::size_t {1}, "happy path should approve one request");
    expect_equal(runtime.risk_rejected_count(), std::size_t {0}, "happy path should not reject");
    expect_equal(runtime.applied_fill_count(), std::size_t {1}, "happy path should apply one fill");
    const auto position = runtime.portfolio().get_position("binance:BTCUSDT");
    expect_true(position.has_value(), "position should exist after happy path fill");
    expect_equal(position->net_quantity, 0.05, "position size should match strategy request quantity");
}

// Verifies risk rejection stops the pipeline before execution and portfolio updates.
void test_simulation_runtime_risk_rejection_stops_execution() {
    trading::core::FixedClock clock(5000);
    trading::app::SimulationRuntime runtime(
        clock,
        {
            .max_order_quantity = 1.0,
            .max_order_notional = 100.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 1000,
            .kill_switch_enabled = false,
        },
        {
            .strategy = {
                .strategy_id = "threshold-1",
                .instrument_id = "binance:BTCUSDT",
                .trigger_price = 42000.0,
                .order_quantity = 0.05,
                .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 0.10,
                .partial_fill_ratio = 0.50,
            },
            .auto_complete_partial_fills = false,
        });

    runtime.on_event(trading::core::MarketEvent {
        .event_id = "event-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    expect_equal(runtime.risk_approved_count(), std::size_t {0}, "risk-reject path should not approve");
    expect_equal(runtime.risk_rejected_count(), std::size_t {1}, "risk-reject path should reject one request");
    expect_equal(runtime.applied_fill_count(), std::size_t {0}, "risk-reject path should not apply fills");
    const auto position = runtime.portfolio().get_position("binance:BTCUSDT");
    expect_true(!position.has_value(), "risk-reject path should not open a position");
}

// Verifies partial matching applies the executed quantity and leaves the residual resting.
void test_simulation_runtime_applies_partial_match_fill() {
    trading::core::FixedClock clock(5000);
    trading::app::SimulationRuntime runtime(
        clock,
        {
            .max_order_quantity = 2.0,
            .max_order_notional = 100000.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 1000,
            .kill_switch_enabled = false,
        },
        {
            .strategy = {
                .strategy_id = "threshold-1",
                .instrument_id = "binance:BTCUSDT",
                .trigger_price = 42000.0,
                .order_quantity = 1.0,
                .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 0.5,
                .partial_fill_ratio = 0.4,
            },
            .auto_complete_partial_fills = true,
        });

    log_test_step("seed limited opposing sell liquidity");
    runtime.restore_open_order({
        .order_id = "resting-sell-1",
        .client_order_id = "resting-client-1",
        .strategy_id = "maker-1",
        .instrument_id = "binance:BTCUSDT",
        .side = trading::core::OrderSide::sell,
        .order_type = trading::core::OrderType::limit,
        .status = trading::core::OrderStatus::acknowledged,
        .quantity = 0.4,
        .price = 41990.0,
        .filled_quantity = 0.0,
    });

    log_test_step("process strategy-triggering market event");
    runtime.on_event(trading::core::MarketEvent {
        .event_id = "event-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    log_test_step("verify only available liquidity is filled");
    expect_equal(runtime.risk_approved_count(), std::size_t {1}, "partial-match path should approve one request");
    expect_equal(runtime.applied_fill_count(), std::size_t {1}, "partial-match path should apply one fill");
    const auto position = runtime.portfolio().get_position("binance:BTCUSDT");
    expect_true(position.has_value(), "position should exist");
    expect_equal(position->net_quantity, 0.4, "partial match should apply only the available opposing quantity");
}

// Verifies pause trading blocks strategy-generated orders and records operational metrics.
void test_simulation_runtime_pause_trading_blocks_orders() {
    trading::core::FixedClock clock(5000);
    trading::monitoring::InMemoryMetricsCollector metrics;
    trading::app::RuntimeOperationalControls controls;
    controls.pause_trading();

    trading::app::SimulationRuntime runtime(
        clock,
        {
            .max_order_quantity = 1.0,
            .max_order_notional = 50000.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 1000,
            .kill_switch_enabled = false,
        },
        {
            .strategy = {
                .strategy_id = "threshold-1",
                .instrument_id = "binance:BTCUSDT",
                .trigger_price = 42000.0,
                .order_quantity = 0.05,
                .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 1.0,
                .partial_fill_ratio = 0.5,
            },
            .auto_complete_partial_fills = false,
        },
        &controls,
        &metrics);

    runtime.on_event(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    expect_true(runtime.trading_paused(), "runtime should observe paused trading control");
    expect_equal(runtime.risk_approved_count(), std::size_t {0}, "paused trading should block order approval");
    expect_equal(runtime.applied_fill_count(), std::size_t {0}, "paused trading should block fills");
    expect_equal(metrics.counter("orders_paused"), std::int64_t {1}, "paused order metric should increment");
}

// Verifies runtime metrics capture fills, approvals, and per-event latency.
void test_simulation_runtime_records_metrics() {
    SteppingClock clock(5000, 2);
    trading::monitoring::InMemoryMetricsCollector metrics;
    trading::app::RuntimeOperationalControls controls;

    trading::app::SimulationRuntime runtime(
        clock,
        {
            .max_order_quantity = 1.0,
            .max_order_notional = 50000.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 1000,
            .kill_switch_enabled = false,
        },
        {
            .strategy = {
                .strategy_id = "threshold-1",
                .instrument_id = "binance:BTCUSDT",
                .trigger_price = 42000.0,
                .order_quantity = 0.05,
                .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 1.0,
                .partial_fill_ratio = 0.5,
            },
            .auto_complete_partial_fills = false,
        },
        &controls,
        &metrics);

    runtime.restore_open_order({
        .order_id = "resting-sell-1",
        .client_order_id = "resting-client-1",
        .strategy_id = "maker-1",
        .instrument_id = "binance:BTCUSDT",
        .side = trading::core::OrderSide::sell,
        .order_type = trading::core::OrderType::limit,
        .status = trading::core::OrderStatus::acknowledged,
        .quantity = 0.05,
        .price = 41990.0,
        .filled_quantity = 0.0,
    });

    runtime.on_event(trading::core::MarketEvent {
        .event_id = "market-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    expect_equal(metrics.counter("market_events"), std::int64_t {1}, "market event metric should increment");
    expect_equal(metrics.counter("orders_approved"), std::int64_t {1}, "approved order metric should increment");
    expect_equal(metrics.counter("fills_applied"), std::int64_t {1}, "fill metric should increment");
    expect_equal(metrics.latency("runtime_event_latency_ms").sample_count, std::int64_t {1}, "runtime latency should be recorded");
}

// Verifies order repository save and lifecycle update behavior.
void test_in_memory_order_repository_save_and_update() {
    trading::storage::InMemoryOrderRepository repository;
    const auto instrument = make_btc_instrument();

    log_test_step("save initial order and apply lifecycle update");
    repository.save_order(
        {
            .request_id = "req-1",
            .strategy_id = "strategy-1",
            .instrument = instrument,
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.5,
            .price = 42000.0,
        },
        "order-1",
        "client-1");
    repository.save_order_update({
        .order_id = "order-1",
        .client_order_id = "client-1",
        .status = trading::core::OrderStatus::acknowledged,
        .filled_quantity = 0.0,
        .reason = std::nullopt,
    });

    log_test_step("reload saved order and verify updated state");
    const auto order = repository.get_order("order-1");
    expect_true(order.has_value(), "order should exist after save");
    expect_equal(order->strategy_id, std::string("strategy-1"), "strategy id should persist");
    expect_equal(order->status, trading::core::OrderStatus::acknowledged, "order status should update");
}

// Verifies position repository stores and replaces latest position state.
void test_in_memory_position_repository_update() {
    trading::storage::InMemoryPositionRepository repository;
    const auto instrument = make_btc_instrument();

    repository.save_position({
        .position_id = "position-1",
        .instrument = instrument,
        .net_quantity = 1.0,
        .average_entry_price = 42000.0,
        .realized_pnl = 0.0,
        .unrealized_pnl = 0.0,
    });
    repository.save_position({
        .position_id = "position-1",
        .instrument = instrument,
        .net_quantity = 0.5,
        .average_entry_price = 42000.0,
        .realized_pnl = 100.0,
        .unrealized_pnl = 50.0,
    });

    const auto position = repository.get_position(instrument.instrument_id);
    expect_true(position.has_value(), "position should exist");
    expect_equal(position->net_quantity, 0.5, "position repository should store latest net quantity");
    expect_equal(position->realized_pnl, 100.0, "position repository should store latest realized pnl");
}

// Verifies market snapshot cache write and read behavior.
void test_in_memory_market_state_cache_write_read() {
    trading::storage::InMemoryMarketStateCache cache;
    log_test_step("write market snapshot into in-memory cache");
    cache.upsert_snapshot({
        .instrument_id = "binance:BTCUSDT",
        .best_bid = 42000.0,
        .best_ask = 42010.0,
        .last_trade_price = 42005.0,
        .last_trade_quantity = 0.25,
        .last_process_timestamp = 5000,
    });

    log_test_step("read market snapshot back from cache");
    const auto snapshot = cache.get_snapshot("binance:BTCUSDT");
    expect_true(snapshot.has_value(), "snapshot should exist");
    expect_true(snapshot->best_bid.has_value(), "best bid should be present");
    expect_equal(*snapshot->best_bid, 42000.0, "best bid should persist");
    expect_equal(snapshot->last_process_timestamp, std::int64_t {5000}, "process timestamp should persist");
}

// Verifies the Postgres order adapter persists and reads back order updates.
void test_postgres_order_repository_write_read() {
    const auto temp_dir = make_temp_directory("postgres-order-repo");
    {
        trading::infrastructure::PostgresOrderRepository repository(temp_dir);
        const auto instrument = make_btc_instrument();
        log_test_step("persist order and lifecycle update through Postgres-style repository");
        repository.save_order(
            {
                .request_id = "req-1",
                .strategy_id = "strategy-1",
                .instrument = instrument,
                .side = trading::core::OrderSide::buy,
                .type = trading::core::OrderType::limit,
                .quantity = 0.25,
                .price = 42000.0,
            },
            "order-1",
            "client-1");
        repository.save_order_update({
            .order_id = "order-1",
            .client_order_id = "client-1",
            .status = trading::core::OrderStatus::acknowledged,
            .filled_quantity = 0.0,
            .reason = std::nullopt,
        });
    }
    {
        trading::infrastructure::PostgresOrderRepository repository(temp_dir);
        log_test_step("reload persisted order record from repository");
        const auto order = repository.get_order("order-1");
        expect_true(order.has_value(), "postgres order adapter should persist records");
        expect_equal(order->status, trading::core::OrderStatus::acknowledged, "persisted order status should reload");
    }
    std::filesystem::remove_all(temp_dir);
}

// Verifies Redis-style cache adapters read/write transaction and market state values.
void test_redis_cache_adapters_write_read() {
    trading::infrastructure::RedisTransactionCache transaction_cache;
    log_test_step("write transaction status to Redis-style transaction cache");
    transaction_cache.set_status("tx-1", "processed");
    expect_equal(transaction_cache.get_status("tx-1"), std::string("processed"), "redis transaction cache should store statuses");

    trading::infrastructure::RedisMarketStateCache market_cache;
    log_test_step("write and read market snapshot through Redis-style market cache");
    market_cache.upsert_snapshot({
        .instrument_id = "binance:BTCUSDT",
        .best_bid = 42000.0,
        .best_ask = 42010.0,
        .last_trade_price = 42005.0,
        .last_trade_quantity = 0.3,
        .last_process_timestamp = 9000,
    });
    const auto snapshot = market_cache.get_snapshot("binance:BTCUSDT");
    expect_true(snapshot.has_value(), "redis market cache should store snapshots");
    expect_equal(*snapshot->best_ask, 42010.0, "redis market cache should return stored best ask");
}

// Verifies transaction checkpoints persist across repository restarts.
void test_postgres_transaction_checkpoint_persistence() {
    const auto temp_dir = make_temp_directory("postgres-transaction-repo");
    {
        trading::infrastructure::PostgresTransactionRepository repository(temp_dir);
        const auto command = make_transaction("tx-1", 77);
        log_test_step("persist received and processed transaction checkpoint");
        repository.save_received(command);
        repository.save_processed("tx-1", "processed");
    }
    {
        trading::infrastructure::PostgresTransactionRepository repository(temp_dir);
        log_test_step("reload transaction checkpoint from repository");
        const auto record = repository.get_record("tx-1");
        expect_true(record.has_value(), "transaction checkpoint should persist");
        expect_equal(record->status, std::string("processed"), "persisted checkpoint status should reload");
        expect_equal(record->kafka_offset, std::int64_t {77}, "persisted checkpoint offset should reload");
    }
    std::filesystem::remove_all(temp_dir);
}

// Verifies explicit Kafka checkpoints persist across repository restarts.
void test_postgres_explicit_kafka_checkpoint_persistence() {
    const auto temp_dir = make_temp_directory("postgres-explicit-checkpoint-repo");
    {
        trading::infrastructure::PostgresTransactionRepository repository(temp_dir);
        log_test_step("persist explicit Kafka checkpoint");
        repository.save_checkpoint({
            .topic = "trading-transactions",
            .partition = 2,
            .offset = 144,
        });
    }
    {
        trading::infrastructure::PostgresTransactionRepository repository(temp_dir);
        log_test_step("reload explicit Kafka checkpoint");
        const auto checkpoints = repository.all_checkpoints();
        expect_equal(checkpoints.size(), std::size_t {1}, "explicit checkpoint should persist");
        expect_equal(checkpoints[0].partition, 2, "persisted checkpoint partition should reload");
        expect_equal(checkpoints[0].offset, std::int64_t {144}, "persisted explicit checkpoint offset should reload");
    }
    std::filesystem::remove_all(temp_dir);
}

// Verifies balance snapshots persist across repository restarts.
void test_postgres_balance_repository_write_read() {
    const auto temp_dir = make_temp_directory("postgres-balance-repo");
    {
        trading::infrastructure::PostgresBalanceRepository repository(temp_dir);
        log_test_step("persist balance snapshot");
        repository.save_balance({
            .asset = "USDT",
            .total_balance = 10500.5,
            .available_balance = 10400.25,
        });
    }
    {
        trading::infrastructure::PostgresBalanceRepository repository(temp_dir);
        log_test_step("reload balance snapshot");
        const auto balance = repository.get_balance("USDT");
        expect_true(balance.has_value(), "balance snapshot should persist");
        expect_equal(balance->total_balance, 10500.5, "persisted balance total should reload");
        expect_equal(balance->available_balance, 10400.25, "persisted balance available should reload");
    }
    std::filesystem::remove_all(temp_dir);
}

// Verifies order intents persist across repository restarts.
void test_postgres_order_intent_repository_write_read() {
    const auto temp_dir = make_temp_directory("postgres-order-intent-repo");
    {
        trading::infrastructure::PostgresOrderIntentRepository repository(temp_dir);
        log_test_step("persist outbound order intent");
        repository.append_intent({
            .request_id = "req-1",
            .strategy_id = "strategy-1",
            .instrument_id = "binance:BTCUSDT",
            .side = trading::core::OrderSide::buy,
            .order_type = trading::core::OrderType::limit,
            .quantity = 0.25,
            .price = 42010.0,
        });
    }
    {
        trading::infrastructure::PostgresOrderIntentRepository repository(temp_dir);
        log_test_step("reload persisted order intent");
        const auto intents = repository.all_intents();
        expect_equal(intents.size(), std::size_t {1}, "order intent should persist");
        expect_equal(intents[0].request_id, std::string("req-1"), "persisted order intent id should reload");
        expect_true(intents[0].price.has_value(), "persisted order intent price should reload");
        expect_equal(*intents[0].price, 42010.0, "persisted order intent price should match");
    }
    std::filesystem::remove_all(temp_dir);
}

// Verifies execution reports persist across repository restarts.
void test_postgres_execution_report_repository_write_read() {
    const auto temp_dir = make_temp_directory("postgres-execution-report-repo");
    {
        trading::infrastructure::PostgresExecutionReportRepository repository(temp_dir);
        log_test_step("persist execution report");
        repository.append_report({
            .report_id = "report-1",
            .order_id = "order-1",
            .client_order_id = "client-1",
            .instrument_id = "binance:BTCUSDT",
            .side = trading::core::OrderSide::buy,
            .status = trading::core::OrderStatus::partially_filled,
            .cumulative_filled_quantity = 0.4,
            .last_fill_price = 42020.0,
            .last_fill_fee = 0.12,
            .reason = std::string("partial_fill"),
            .exchange_timestamp = 9001,
        });
    }
    {
        trading::infrastructure::PostgresExecutionReportRepository repository(temp_dir);
        log_test_step("reload persisted execution report");
        const auto reports = repository.all_reports();
        expect_equal(reports.size(), std::size_t {1}, "execution report should persist");
        expect_equal(reports[0].report_id, std::string("report-1"), "persisted execution report id should reload");
        expect_equal(reports[0].cumulative_filled_quantity, 0.4, "persisted execution quantity should reload");
        expect_true(reports[0].reason.has_value(), "persisted execution reason should reload");
        expect_equal(*reports[0].reason, std::string("partial_fill"), "persisted execution reason should match");
    }
    std::filesystem::remove_all(temp_dir);
}

// Verifies recovery rebuilds runtime state, warms cache, and derives Kafka checkpoints.
void test_recovery_service_restores_runtime_and_cache() {
    trading::storage::InMemoryTransactionRepository transaction_repository;
    trading::storage::InMemoryOrderRepository order_repository;
    trading::storage::InMemoryPositionRepository position_repository;
    trading::storage::InMemoryBalanceRepository balance_repository;
    trading::storage::InMemoryMarketStateCache market_cache;
    trading::storage::InMemoryTransactionCache transaction_cache;

    auto tx_1 = make_transaction("tx-1", 10);
    auto tx_2 = make_transaction("tx-2", 12);
    log_test_step("seed repositories with processed transactions and open order state");
    transaction_repository.save_received(tx_1);
    transaction_repository.save_processed("tx-1", "processed");
    transaction_repository.save_received(tx_2);
    transaction_repository.save_processed("tx-2", "processed");

    order_repository.save_order(
        {
            .request_id = "req-1",
            .strategy_id = "strategy-1",
            .instrument = make_btc_instrument(),
            .side = trading::core::OrderSide::buy,
            .type = trading::core::OrderType::limit,
            .quantity = 0.5,
            .price = 42000.0,
        },
        "order-open-1",
        "client-1");
    order_repository.save_order_update({
        .order_id = "order-open-1",
        .client_order_id = "client-1",
        .status = trading::core::OrderStatus::partially_filled,
        .filled_quantity = 0.2,
        .reason = std::nullopt,
    });

    position_repository.save_position({
        .position_id = "position-binance:BTCUSDT",
        .instrument = make_btc_instrument(),
        .net_quantity = 0.2,
        .average_entry_price = 42000.0,
        .realized_pnl = 15.0,
        .unrealized_pnl = 0.0,
    });
    balance_repository.save_balance({
        .asset = "USDT",
        .total_balance = 5000.0,
        .available_balance = 4900.0,
    });

    trading::app::RecoveryService recovery_service(
        transaction_repository,
        order_repository,
        position_repository,
        balance_repository,
        market_cache,
        transaction_cache);

    log_test_step("run recovery and derive runtime snapshot");
    const auto snapshot = recovery_service.recover({
        {
            .instrument_id = "binance:BTCUSDT",
            .best_bid = 41995.0,
            .best_ask = 42005.0,
            .last_trade_price = 42000.0,
            .last_trade_quantity = 0.25,
            .last_process_timestamp = 5000,
        },
    });

    expect_equal(snapshot.open_orders.size(), std::size_t {1}, "one open order should be recovered");
    expect_equal(snapshot.positions.size(), std::size_t {1}, "one position should be recovered");
    expect_equal(snapshot.balances.size(), std::size_t {1}, "one balance should be recovered");
    expect_equal(snapshot.kafka_checkpoints.size(), std::size_t {1}, "one partition checkpoint should be derived");
    expect_equal(snapshot.kafka_checkpoints[0].offset, std::int64_t {12}, "latest processed offset should be recovered");
    expect_equal(transaction_cache.get_status("tx-2"), std::string("processed"), "transaction cache should be warmed");

    trading::core::FixedClock clock(5000);
    trading::app::SimulationRuntime runtime(
        clock,
        {
            .max_order_quantity = 1.0,
            .max_order_notional = 50000.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 1000,
            .kill_switch_enabled = false,
        },
        {
            .strategy = {
                .strategy_id = "threshold-1",
                .instrument_id = "binance:BTCUSDT",
                .trigger_price = 42000.0,
                .order_quantity = 0.05,
                .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 1.0,
                .partial_fill_ratio = 0.5,
            },
            .auto_complete_partial_fills = false,
        });
    log_test_step("restore recovered snapshot into simulation runtime");
    recovery_service.restore_runtime(snapshot, runtime);

    log_test_step("verify restored portfolio and warmed market cache");
    const auto position = runtime.portfolio().get_position("binance:BTCUSDT");
    const auto balance = runtime.portfolio().get_balance("USDT");
    expect_true(position.has_value(), "recovered runtime should restore position");
    expect_equal(position->net_quantity, 0.2, "recovered position quantity should match");
    expect_equal(balance.total_balance, 5000.0, "recovered balance should match");

    const auto warmed_snapshot = market_cache.get_snapshot("binance:BTCUSDT");
    expect_true(warmed_snapshot.has_value(), "market cache should be warmed from exchange snapshot");
    expect_equal(*warmed_snapshot->best_bid, 41995.0, "warmed best bid should match exchange snapshot");
}

// Verifies explicit Kafka checkpoints override inferred transaction offsets during recovery.
void test_recovery_service_prefers_explicit_checkpoints() {
    trading::storage::InMemoryTransactionRepository transaction_repository;
    trading::storage::InMemoryOrderRepository order_repository;
    trading::storage::InMemoryPositionRepository position_repository;
    trading::storage::InMemoryBalanceRepository balance_repository;
    trading::storage::InMemoryMarketStateCache market_cache;
    trading::storage::InMemoryTransactionCache transaction_cache;

    auto tx = make_transaction("tx-1", 77);
    tx.kafka_partition = 2;
    log_test_step("seed processed transaction and explicit Kafka checkpoint");
    transaction_repository.save_received(tx);
    transaction_repository.save_processed("tx-1", "processed");
    transaction_repository.save_checkpoint({
        .topic = tx.kafka_topic,
        .partition = tx.kafka_partition,
        .offset = 55,
    });

    trading::app::RecoveryService recovery_service(
        transaction_repository,
        order_repository,
        position_repository,
        balance_repository,
        market_cache,
        transaction_cache);
    log_test_step("recover snapshot and verify explicit checkpoint precedence");
    const auto snapshot = recovery_service.recover();

    expect_equal(snapshot.kafka_checkpoints.size(), std::size_t {1}, "one explicit checkpoint should be recovered");
    expect_equal(snapshot.kafka_checkpoints[0].offset, std::int64_t {55}, "explicit checkpoint should override derived offset");
}

// Verifies Kafka recovery resumes from the next intended offset.
void test_recovery_service_resumes_kafka_from_next_offset() {
    trading::storage::InMemoryTransactionRepository transaction_repository;
    trading::storage::InMemoryOrderRepository order_repository;
    trading::storage::InMemoryPositionRepository position_repository;
    trading::storage::InMemoryBalanceRepository balance_repository;
    trading::storage::InMemoryMarketStateCache market_cache;
    trading::storage::InMemoryTransactionCache transaction_cache;

    auto tx = make_transaction("tx-1", 77);
    tx.kafka_partition = 2;
    log_test_step("seed processed transaction for Kafka resume");
    transaction_repository.save_received(tx);
    transaction_repository.save_processed("tx-1", "processed");

    trading::app::RecoveryService recovery_service(
        transaction_repository,
        order_repository,
        position_repository,
        balance_repository,
        market_cache,
        transaction_cache);
    log_test_step("recover snapshot and resume Kafka consumer");
    const auto snapshot = recovery_service.recover();

    FakeKafkaConsumerClient kafka_client({});
    recovery_service.resume_kafka(snapshot, kafka_client);

    log_test_step("verify recovered Kafka seek offset");
    expect_equal(kafka_client.seeks().size(), std::size_t {1}, "one kafka partition should be resumed");
    expect_equal(kafka_client.seeks()[0].partition, 2, "recovery should resume the recovered partition");
    expect_equal(kafka_client.seeks()[0].offset, std::int64_t {78}, "recovery should seek to the next offset");
}

// Verifies runtime persists local open-order state, fills, portfolio, and market snapshots for recovery.
void test_simulation_runtime_persists_local_state_for_recovery() {
    trading::core::FixedClock clock(5000);
    trading::storage::InMemoryOrderRepository order_repository;
    trading::storage::InMemoryFillRepository fill_repository;
    trading::storage::InMemoryPositionRepository position_repository;
    trading::storage::InMemoryBalanceRepository balance_repository;
    trading::storage::InMemoryMarketStateCache market_cache;

    trading::app::SimulationRuntime runtime(
        clock,
        {
            .max_order_quantity = 1.0,
            .max_order_notional = 50000.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 1000,
            .kill_switch_enabled = false,
        },
        {
            .strategy = {
                .strategy_id = "threshold-1",
                .instrument_id = "binance:BTCUSDT",
                .instrument = make_btc_instrument(),
                .trigger_price = 42000.0,
                .order_quantity = 0.4,
                .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 1.0,
                .partial_fill_ratio = 0.5,
            },
            .auto_complete_partial_fills = false,
            .order_repository = &order_repository,
            .fill_repository = &fill_repository,
            .position_repository = &position_repository,
            .balance_repository = &balance_repository,
            .market_state_cache = &market_cache,
        });

    log_test_step("process trade event that creates a resting local buy order");
    runtime.on_event(trading::core::MarketEvent {
        .event_id = "trade-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    log_test_step("verify resting open order and cached market snapshot were persisted");
    const auto open_orders = order_repository.all_open_orders();
    expect_equal(open_orders.size(), std::size_t {1}, "one resting local order should persist as open");
    expect_equal(open_orders[0].status, trading::core::OrderStatus::acknowledged, "resting local order should persist as acknowledged");
    const auto cached_trade_snapshot = market_cache.get_snapshot("binance:BTCUSDT");
    expect_true(cached_trade_snapshot.has_value(), "market snapshot should be persisted to cache");
    expect_true(cached_trade_snapshot->last_trade_price.has_value(), "persisted market snapshot should retain last trade price");
    expect_equal(*cached_trade_snapshot->last_trade_price, 41990.0, "persisted trade price should match event");

    log_test_step("process book snapshot that fills the persisted resting order");
    runtime.on_event(trading::core::MarketEvent {
        .event_id = "book-1",
        .type = trading::core::MarketEventType::book_snapshot,
        .instrument = make_btc_instrument(),
        .ask_levels = {
            {.price = 41990.0, .quantity = 0.4},
        },
        .process_timestamp = 4901,
    });

    log_test_step("verify filled order, persisted fill, portfolio, and book snapshot state");
    const auto filled_order = order_repository.get_order(open_orders[0].order_id);
    expect_true(filled_order.has_value(), "filled local order should remain persisted");
    expect_equal(filled_order->status, trading::core::OrderStatus::filled, "filled local order should persist terminal status");
    expect_equal(fill_repository.all_fills().size(), std::size_t {1}, "one market-driven fill should be persisted");
    const auto persisted_position = position_repository.get_position("binance:BTCUSDT");
    expect_true(persisted_position.has_value(), "position should be persisted after fill");
    expect_near(persisted_position->net_quantity, 0.4, 1e-9, "persisted position quantity should match fill");
    const auto persisted_balance = balance_repository.get_balance("BTC");
    expect_true(persisted_balance.has_value(), "base-asset balance should be persisted after fill");
    expect_near(persisted_balance->total_balance, 0.4, 1e-9, "persisted base balance should reflect fill quantity");
    const auto cached_book_snapshot = market_cache.get_snapshot("binance:BTCUSDT");
    expect_true(cached_book_snapshot.has_value(), "book snapshot should persist to market cache");
    expect_true(cached_book_snapshot->best_ask.has_value(), "persisted book snapshot should retain best ask");
    expect_equal(*cached_book_snapshot->best_ask, 41990.0, "persisted best ask should match book snapshot");
}

// Verifies recovery restores persisted local open orders and matching can continue after restart.
void test_recovery_service_restores_local_open_order_continuity() {
    trading::storage::InMemoryTransactionRepository transaction_repository;
    trading::storage::InMemoryOrderRepository order_repository;
    trading::storage::InMemoryPositionRepository position_repository;
    trading::storage::InMemoryBalanceRepository balance_repository;
    trading::storage::InMemoryMarketStateCache market_cache;
    trading::storage::InMemoryTransactionCache transaction_cache;

    {
        trading::core::FixedClock first_clock(5000);
        trading::app::SimulationRuntime first_runtime(
            first_clock,
            {
                .max_order_quantity = 1.0,
                .max_order_notional = 50000.0,
                .max_position_quantity = 2.0,
                .stale_after_ms = 1000,
                .kill_switch_enabled = false,
            },
            {
                .strategy = {
                    .strategy_id = "threshold-1",
                    .instrument_id = "binance:BTCUSDT",
                    .instrument = make_btc_instrument(),
                    .trigger_price = 42000.0,
                    .order_quantity = 0.4,
                    .side = trading::core::OrderSide::buy,
                },
                .execution = {
                    .partial_fill_threshold = 1.0,
                    .partial_fill_ratio = 0.5,
                },
                .auto_complete_partial_fills = false,
                .order_repository = &order_repository,
                .fill_repository = nullptr,
                .position_repository = &position_repository,
                .balance_repository = &balance_repository,
                .market_state_cache = &market_cache,
            });

        log_test_step("run first runtime until local order is persisted as resting open state");
        first_runtime.on_event(trading::core::MarketEvent {
            .event_id = "trade-1",
            .type = trading::core::MarketEventType::trade,
            .instrument = make_btc_instrument(),
            .price = 41990.0,
            .quantity = 0.5,
            .process_timestamp = 4900,
        });
    }

    trading::app::RecoveryService recovery_service(
        transaction_repository,
        order_repository,
        position_repository,
        balance_repository,
        market_cache,
        transaction_cache);

    log_test_step("recover persisted open-order snapshot after restart");
    const auto snapshot = recovery_service.recover();
    expect_equal(snapshot.open_orders.size(), std::size_t {1}, "one local open order should be recoverable after restart");

    trading::core::FixedClock second_clock(6000);
    trading::storage::InMemoryFillRepository fill_repository;
    trading::app::SimulationRuntime recovered_runtime(
        second_clock,
        {
            .max_order_quantity = 1.0,
            .max_order_notional = 50000.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 1000,
            .kill_switch_enabled = false,
        },
        {
                .strategy = {
                    .strategy_id = "threshold-1",
                    .instrument_id = "binance:BTCUSDT",
                    .instrument = make_btc_instrument(),
                    .trigger_price = 41000.0,
                    .order_quantity = 0.4,
                    .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 1.0,
                .partial_fill_ratio = 0.5,
            },
            .auto_complete_partial_fills = false,
            .order_repository = &order_repository,
            .fill_repository = &fill_repository,
            .position_repository = &position_repository,
            .balance_repository = &balance_repository,
            .market_state_cache = &market_cache,
        });

    log_test_step("restore recovered snapshot into new runtime");
    recovery_service.restore_runtime(snapshot, recovered_runtime);

    log_test_step("continue matching after restart with external executable ask liquidity");
    recovered_runtime.on_event(trading::core::MarketEvent {
        .event_id = "book-1",
        .type = trading::core::MarketEventType::book_snapshot,
        .instrument = make_btc_instrument(),
        .ask_levels = {
            {.price = 41990.0, .quantity = 0.4},
        },
        .process_timestamp = 5900,
    });

    log_test_step("verify recovered local open order continues to filled state after restart");
    const auto filled_orders = order_repository.all_open_orders();
    expect_equal(filled_orders.size(), std::size_t {0}, "filled recovered order should no longer appear as open");
    expect_equal(fill_repository.all_fills().size(), std::size_t {1}, "continued matching after restart should persist fill");
    const auto position = recovered_runtime.portfolio().get_position("binance:BTCUSDT");
    expect_true(position.has_value(), "recovered runtime should apply continued fill into portfolio");
    expect_near(position->net_quantity, 0.4, 1e-9, "continued post-restart fill should restore intended position");
}

// Verifies replay reproduces the same state as direct fixed-session processing.
void test_replay_service_reproduces_fixed_session_state() {
    const auto temp_dir = make_temp_directory("replay-fixed-session");
    const auto log_path = temp_dir / "events.tsv";

    const std::vector<trading::core::EngineEvent> events {
        trading::core::MarketEvent {
            .event_id = "event-1",
            .type = trading::core::MarketEventType::book_snapshot,
            .instrument = make_btc_instrument(),
            .bid_levels = {
                {.price = 41990.0, .quantity = 0.05},
            },
            .process_timestamp = 4900,
        },
        trading::core::TimerEvent {
            .timer_id = "timer-1",
            .timestamp = 5000,
        },
    };

    trading::storage::ReplayService replay_service(log_path);
    log_test_step("write fixed event log");
    replay_service.reset_log();
    for (const auto& event : events) {
        replay_service.append_event(event);
    }

    trading::core::FixedClock direct_clock(5000);
    trading::app::SimulationRuntime direct_runtime(
        direct_clock,
        {
            .max_order_quantity = 1.0,
            .max_order_notional = 50000.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 1000,
            .kill_switch_enabled = false,
        },
        {
            .strategy = {
                .strategy_id = "threshold-1",
                .instrument_id = "binance:BTCUSDT",
                .trigger_price = 42000.0,
                .order_quantity = 0.05,
                .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 1.0,
                .partial_fill_ratio = 0.5,
            },
            .auto_complete_partial_fills = false,
        });
    log_test_step("seed direct runtime with matching resting liquidity");
    direct_runtime.restore_open_order({
        .order_id = "resting-sell-1",
        .client_order_id = "resting-client-1",
        .strategy_id = "maker-1",
        .instrument_id = "binance:BTCUSDT",
        .side = trading::core::OrderSide::sell,
        .order_type = trading::core::OrderType::limit,
        .status = trading::core::OrderStatus::acknowledged,
        .quantity = 0.05,
        .price = 41990.0,
        .filled_quantity = 0.0,
    });
    for (const auto& event : events) {
        direct_runtime.on_event(event);
    }

    trading::core::FixedClock replay_clock(5000);
    trading::app::SimulationRuntime replay_runtime(
        replay_clock,
        {
            .max_order_quantity = 1.0,
            .max_order_notional = 50000.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 1000,
            .kill_switch_enabled = false,
        },
        {
            .strategy = {
                .strategy_id = "threshold-1",
                .instrument_id = "binance:BTCUSDT",
                .trigger_price = 42000.0,
                .order_quantity = 0.05,
                .side = trading::core::OrderSide::buy,
            },
            .execution = {
                .partial_fill_threshold = 1.0,
                .partial_fill_ratio = 0.5,
            },
            .auto_complete_partial_fills = false,
        });
    log_test_step("seed replay runtime with matching resting liquidity");
    replay_runtime.restore_open_order({
        .order_id = "resting-sell-1",
        .client_order_id = "resting-client-1",
        .strategy_id = "maker-1",
        .instrument_id = "binance:BTCUSDT",
        .side = trading::core::OrderSide::sell,
        .order_type = trading::core::OrderType::limit,
        .status = trading::core::OrderStatus::acknowledged,
        .quantity = 0.05,
        .price = 41990.0,
        .filled_quantity = 0.0,
    });

    log_test_step("replay persisted events into runtime");
    const auto stats = replay_service.replay([&replay_runtime](const trading::core::EngineEvent& event) {
        replay_runtime.on_event(event);
    });

    log_test_step("compare replay results to direct processing");
    expect_equal(stats.replayed_records, std::size_t {2}, "replay should apply both records");
    expect_equal(replay_runtime.risk_approved_count(), direct_runtime.risk_approved_count(), "replay should match approved count");
    expect_equal(replay_runtime.applied_fill_count(), direct_runtime.applied_fill_count(), "replay should match fill count");
    expect_equal(replay_runtime.applied_fill_count(), std::size_t {1}, "book-driven replay should apply one fill");

    const auto direct_position = direct_runtime.portfolio().get_position("binance:BTCUSDT");
    const auto replay_position = replay_runtime.portfolio().get_position("binance:BTCUSDT");
    expect_true(direct_position.has_value() && replay_position.has_value(), "both runs should produce a position");
    expect_equal(replay_position->net_quantity, direct_position->net_quantity, "replayed position should match direct run");

    std::filesystem::remove_all(temp_dir);
}

// Verifies invalid persisted records are skipped without aborting replay.
void test_replay_service_skips_invalid_records() {
    const auto temp_dir = make_temp_directory("replay-invalid-records");
    const auto log_path = temp_dir / "events.tsv";

    trading::storage::ReplayService replay_service(log_path);
    replay_service.reset_log();
    replay_service.append_event(trading::core::TimerEvent {
        .timer_id = "timer-1",
        .timestamp = 1000,
    });
    {
        std::ofstream output(log_path, std::ios::app);
        output << "invalid\tbroken\trecord\n";
    }

    std::size_t replayed = 0;
    const auto stats = replay_service.replay([&replayed](const trading::core::EngineEvent&) {
        ++replayed;
    });

    expect_equal(stats.total_records, std::size_t {2}, "two persisted records should be scanned");
    expect_equal(stats.replayed_records, std::size_t {1}, "one valid record should be replayed");
    expect_equal(stats.skipped_records, std::size_t {1}, "one invalid record should be skipped");
    expect_equal(replayed, std::size_t {1}, "callback should run only for valid records");

    std::filesystem::remove_all(temp_dir);
}

// Verifies replay outcomes are deterministic across different wall-clock values.
void test_replay_service_isolated_from_live_clock() {
    const auto temp_dir = make_temp_directory("replay-clock-isolation");
    const auto log_path = temp_dir / "events.tsv";

    trading::storage::ReplayService replay_service(log_path);
    replay_service.reset_log();
    replay_service.append_event(trading::core::MarketEvent {
        .event_id = "event-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 9999990,
    });

    const auto risk_config = trading::config::RiskConfig {
        .max_order_quantity = 1.0,
        .max_order_notional = 50000.0,
        .max_position_quantity = 2.0,
        .stale_after_ms = 1000,
        .kill_switch_enabled = false,
    };
    const auto runtime_config = trading::app::SimulationRuntimeConfig {
        .strategy = {
            .strategy_id = "threshold-1",
            .instrument_id = "binance:BTCUSDT",
            .trigger_price = 42000.0,
            .order_quantity = 0.05,
            .side = trading::core::OrderSide::buy,
        },
        .execution = {
            .partial_fill_threshold = 1.0,
            .partial_fill_ratio = 0.5,
        },
        .auto_complete_partial_fills = false,
    };

    trading::core::FixedClock clock_a(0);
    trading::app::SimulationRuntime runtime_a(clock_a, risk_config, runtime_config);
    runtime_a.restore_open_order({
        .order_id = "resting-sell-1",
        .client_order_id = "resting-client-1",
        .strategy_id = "maker-1",
        .instrument_id = "binance:BTCUSDT",
        .side = trading::core::OrderSide::sell,
        .order_type = trading::core::OrderType::limit,
        .status = trading::core::OrderStatus::acknowledged,
        .quantity = 0.05,
        .price = 41990.0,
        .filled_quantity = 0.0,
    });
    const auto stats_a = replay_service.replay([&runtime_a](const trading::core::EngineEvent& event) {
        runtime_a.on_event(event);
    });

    trading::core::FixedClock clock_b(9999999);
    trading::app::SimulationRuntime runtime_b(clock_b, risk_config, runtime_config);
    runtime_b.restore_open_order({
        .order_id = "resting-sell-1",
        .client_order_id = "resting-client-1",
        .strategy_id = "maker-1",
        .instrument_id = "binance:BTCUSDT",
        .side = trading::core::OrderSide::sell,
        .order_type = trading::core::OrderType::limit,
        .status = trading::core::OrderStatus::acknowledged,
        .quantity = 0.05,
        .price = 41990.0,
        .filled_quantity = 0.0,
    });
    const auto stats_b = replay_service.replay([&runtime_b](const trading::core::EngineEvent& event) {
        runtime_b.on_event(event);
    });
    expect_equal(stats_a.replayed_records, stats_b.replayed_records, "both replays should process the same record count");

    expect_equal(runtime_a.risk_approved_count(), runtime_b.risk_approved_count(), "replay counts should match across clocks");
    expect_equal(runtime_a.applied_fill_count(), runtime_b.applied_fill_count(), "fill counts should match across clocks");

    const auto position_a = runtime_a.portfolio().get_position("binance:BTCUSDT");
    const auto position_b = runtime_b.portfolio().get_position("binance:BTCUSDT");
    expect_true(position_a.has_value() && position_b.has_value(), "both replays should produce positions");
    expect_equal(position_a->net_quantity, position_b->net_quantity, "position result should match across clocks");

    std::filesystem::remove_all(temp_dir);
}

// Verifies the backtest runner returns replay, strategy, and PnL summary data.
void test_backtest_runner_summarizes_runtime_results() {
    const auto temp_dir = make_temp_directory("backtest-summary");
    const auto log_path = temp_dir / "events.tsv";
    trading::storage::ReplayService replay_service(log_path);
    log_test_step("write historical backtest event log");
    replay_service.reset_log();

    replay_service.append_event(trading::core::MarketEvent {
        .event_id = "event-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 1000,
    });

    auto coordinator = std::make_shared<trading::strategy::StrategyCoordinator>();
    coordinator->add_strategy(std::make_unique<trading::strategy::SampleThresholdStrategy>(
        trading::strategy::SampleThresholdStrategyConfig {
            .strategy_id = "threshold-1",
            .instrument_id = "binance:BTCUSDT",
            .instrument = make_btc_instrument(),
            .trigger_price = 42000.0,
            .order_quantity = 0.05,
            .side = trading::core::OrderSide::buy,
        }));

    trading::core::FixedClock clock(5000);
    trading::app::SimulationRuntime runtime(
        clock,
        {
            .max_order_quantity = 1.0,
            .max_order_notional = 50000.0,
            .max_position_quantity = 2.0,
            .stale_after_ms = 10000,
            .kill_switch_enabled = false,
        },
        {
            .strategy = {
                .strategy_id = "threshold-1",
                .instrument_id = "binance:BTCUSDT",
                .trigger_price = 42000.0,
                .order_quantity = 0.05,
                .side = trading::core::OrderSide::buy,
            },
            .strategy_coordinator = coordinator,
            .execution = {
                .partial_fill_threshold = 1.0,
                .partial_fill_ratio = 0.5,
            },
            .auto_complete_partial_fills = false,
        });
    log_test_step("seed runtime with resting liquidity for backtest matching");
    runtime.restore_open_order({
        .order_id = "resting-sell-1",
        .client_order_id = "resting-client-1",
        .strategy_id = "maker-1",
        .instrument_id = "binance:BTCUSDT",
        .side = trading::core::OrderSide::sell,
        .order_type = trading::core::OrderType::limit,
        .status = trading::core::OrderStatus::acknowledged,
        .quantity = 0.05,
        .price = 41990.0,
        .filled_quantity = 0.0,
    });

    trading::app::BacktestRunner runner(log_path);
    log_test_step("run backtest and collect summary");
    const auto summary = runner.run(runtime);

    log_test_step("verify replay, strategy, and fill summary fields");
    expect_equal(summary.replay_stats.replayed_records, std::size_t {1}, "backtest should replay the market event");
    expect_equal(summary.configured_strategies, std::size_t {1}, "configured strategy count should be reported");
    expect_equal(summary.active_strategies, std::size_t {1}, "healthy strategy should remain active");
    expect_equal(summary.paused_strategies, std::size_t {0}, "no strategy should be paused");
    expect_equal(summary.risk_approved, std::size_t {1}, "threshold event should produce one approved request");
    expect_equal(summary.fills_applied, std::size_t {1}, "backtest should apply one fill");
    expect_true(summary.total_unrealized_pnl == 0.0, "new fill at mark price should have zero unrealized pnl");
    expect_equal(summary.strategy_stats.size(), std::size_t {1}, "summary should expose strategy stats");
    expect_equal(summary.strategy_stats[0].emitted_requests, std::size_t {1}, "strategy stats should include emitted request count");

    std::filesystem::remove_all(temp_dir);
}

// Verifies raw Kafka payloads are adapted into normalized transaction commands.
void test_kafka_transaction_consumer_parses_payload() {
    FakeKafkaConsumerClient fake_client({
        {
            .topic = "trading-transactions",
            .partition = 1,
            .offset = 42,
            .payload =
                "transaction_id=tx-1;"
                "user_id=user-1;"
                "account_id=account-1;"
                "command_type=place_order;"
                "instrument_symbol=BTCUSDT;"
                "quantity=1.25;"
                "price=42000.5",
        },
    });
    trading::infrastructure::KafkaTransactionConsumer consumer(fake_client, "trading-transactions");

    log_test_step("poll and parse valid Kafka payload");
    const auto command = consumer.poll();
    expect_true(command.has_value(), "adapter should parse valid kafka payload");
    expect_equal(command->transaction_id, std::string("tx-1"), "transaction id should match payload");
    expect_equal(command->kafka_partition, 1, "kafka partition should propagate");
    expect_equal(command->kafka_offset, std::int64_t {42}, "kafka offset should propagate");
    expect_true(command->price.has_value(), "price should parse when provided");
}

// Verifies malformed messages are safely skipped and committed as rejected.
void test_kafka_transaction_consumer_skips_malformed_messages() {
    FakeKafkaConsumerClient fake_client({
        {
            .topic = "trading-transactions",
            .partition = 0,
            .offset = 10,
            .payload = "transaction_id=tx-bad;user_id=user-1;quantity=broken",
        },
        {
            .topic = "trading-transactions",
            .partition = 0,
            .offset = 11,
            .payload =
                "transaction_id=tx-good;"
                "user_id=user-1;"
                "account_id=account-1;"
                "command_type=place_order;"
                "instrument_symbol=BTCUSDT;"
                "quantity=0.50",
        },
    });
    trading::infrastructure::KafkaTransactionConsumer consumer(fake_client, "trading-transactions");

    log_test_step("poll malformed then valid Kafka payloads");
    const auto command = consumer.poll();
    expect_true(command.has_value(), "consumer should continue to next message after malformed payload");
    expect_equal(command->transaction_id, std::string("tx-good"), "consumer should return the first valid payload");
    expect_equal(fake_client.commits().size(), std::size_t {1}, "malformed payload should be committed");
    expect_equal(fake_client.commits()[0].offset, std::int64_t {10}, "committed malformed offset should match");
}

// Verifies bursts of malformed Kafka payloads are drained safely before the next valid message.
void test_kafka_transaction_consumer_skips_malformed_payload_burst() {
    FakeKafkaConsumerClient fake_client({
        {
            .topic = "trading-transactions",
            .partition = 0,
            .offset = 20,
            .payload = "broken",
        },
        {
            .topic = "trading-transactions",
            .partition = 0,
            .offset = 21,
            .payload = "transaction_id=tx-bad-2;quantity=oops",
        },
        {
            .topic = "trading-transactions",
            .partition = 0,
            .offset = 22,
            .payload =
                "transaction_id=tx-good-burst;"
                "user_id=user-1;"
                "account_id=account-1;"
                "command_type=place_order;"
                "instrument_symbol=BTCUSDT;"
                "quantity=0.75",
        },
    });
    trading::infrastructure::KafkaTransactionConsumer consumer(fake_client, "trading-transactions");

    log_test_step("drain malformed burst until the next valid Kafka payload");
    const auto command = consumer.poll();
    expect_true(command.has_value(), "consumer should recover after malformed burst");
    expect_equal(command->transaction_id, std::string("tx-good-burst"), "first valid message after malformed burst should be returned");
    expect_equal(fake_client.commits().size(), std::size_t {2}, "all malformed payloads should be committed");
    expect_equal(fake_client.commits()[0].offset, std::int64_t {20}, "first malformed offset should be committed");
    expect_equal(fake_client.commits()[1].offset, std::int64_t {21}, "second malformed offset should be committed");
}

// Verifies commit checkpoints are controlled by ingestor processing, not by poll.
void test_kafka_transaction_consumer_commit_checkpoint_behavior() {
    FakeKafkaConsumerClient fake_client({
        {
            .topic = "trading-transactions",
            .partition = 0,
            .offset = 77,
            .payload =
                "transaction_id=tx-1;"
                "user_id=user-1;"
                "account_id=account-1;"
                "command_type=place_order;"
                "instrument_symbol=BTCUSDT;"
                "quantity=1.00",
        },
    });
    trading::infrastructure::KafkaTransactionConsumer consumer(fake_client, "trading-transactions");

    log_test_step("poll valid payload without auto-commit");
    const auto command = consumer.poll();
    expect_true(command.has_value(), "valid payload should be returned");
    expect_equal(fake_client.commits().size(), std::size_t {0}, "poll should not commit valid payload");

    log_test_step("commit payload explicitly through consumer");
    consumer.commit(*command);
    expect_equal(fake_client.commits().size(), std::size_t {1}, "explicit commit should forward offset to client");
    expect_equal(fake_client.commits()[0].offset, std::int64_t {77}, "committed offset should match payload");
}

// Verifies producer payload serialization matches the consumer-side key-value schema.
void test_kafka_transaction_producer_serializes_payload() {
    const auto command = make_transaction("tx-serializer", 0);

    log_test_step("serialize transaction command into Kafka payload format");
    const auto payload = trading::infrastructure::serialize_transaction_command_payload(command);
    expect_true(payload.find("transaction_id=tx-serializer") != std::string::npos, "payload should contain transaction id");
    expect_true(payload.find("user_id=user-1") != std::string::npos, "payload should contain user id");
    expect_true(payload.find("instrument_symbol=BTCUSDT") != std::string::npos, "payload should contain instrument symbol");
    expect_true(payload.find("quantity=1.5") != std::string::npos, "payload should contain quantity");
    expect_true(payload.find("price=42000") != std::string::npos, "payload should contain price");
}

// Verifies producer key serialization uses transaction_id|yyyyMMddHHmmSS.
void test_kafka_transaction_producer_serializes_key() {
    const auto command = make_transaction("tx-serializer", 0);
    log_test_step("serialize transaction key with UTC timestamp suffix");
    const auto key = trading::infrastructure::serialize_transaction_command_key(command, std::int64_t {1713254950000});

    expect_equal(key, std::string("tx-serializer|20240416080910"), "key should concatenate transaction id, separator, and UTC timestamp");
}

// Verifies the transaction publisher can parse one flat JSON input line.
void test_transaction_publisher_parses_json_line() {
    log_test_step("parse valid flat JSON transaction line");
    const auto command = trading::app::parse_json_transaction_command_line(
        R"({"transaction_id":"tx-2001","user_id":"user-9","account_id":"account-9","command_type":"place_order","instrument_symbol":"ETHUSDT","quantity":1.25,"price":3200.5})");

    expect_true(command.has_value(), "json transaction line should parse");
    expect_equal(command->transaction_id, std::string("tx-2001"), "parsed transaction id should match");
    expect_equal(command->user_id, std::string("user-9"), "parsed user id should match");
    expect_equal(command->instrument_symbol, std::string("ETHUSDT"), "parsed instrument should match");
    expect_equal(command->quantity, 1.25, "parsed quantity should match");
    expect_true(command->price.has_value(), "parsed price should exist");
    expect_equal(*command->price, 3200.5, "parsed price should match");
}

// Verifies malformed JSON transaction lines are rejected safely.
void test_transaction_publisher_rejects_invalid_json_line() {
    log_test_step("reject malformed JSON transaction line");
    const auto command = trading::app::parse_json_transaction_command_line(
        R"({"transaction_id":"tx-2001","user_id":"user-9","quantity":"oops"})");

    expect_true(!command.has_value(), "invalid json transaction line should be rejected");
}

// Verifies Kafka-style transaction consumption preserves order and commit checkpoints.
void test_transaction_ingestor_order_and_commit() {
    trading::core::EventDispatcher dispatcher;
    std::vector<std::string> processed_ids;
    dispatcher.subscribe([&processed_ids](const trading::core::EngineEvent& event) {
        if (const auto* command = std::get_if<trading::core::TransactionCommand>(&event)) {
            processed_ids.push_back(command->transaction_id);
        }
    });

    trading::ingestion::MockTransactionConsumer consumer({
        make_transaction("tx-1", 10),
        make_transaction("tx-2", 11),
    });
    trading::storage::InMemoryTransactionRepository repository;
    trading::storage::InMemoryTransactionCache cache;
    trading::ingestion::TransactionIngestor ingestor(consumer, repository, cache, dispatcher);

    log_test_step("process both mock transactions through ingestor");
    expect_true(ingestor.process_next(), "first transaction should be processed");
    expect_true(ingestor.process_next(), "second transaction should be processed");
    expect_true(!ingestor.process_next(), "no third transaction should exist");

    log_test_step("verify publish order, commits, cache, and repository status");
    expect_equal(processed_ids.size(), std::size_t {2}, "two transactions should be published");
    expect_equal(processed_ids[0], std::string("tx-1"), "first transaction order should match");
    expect_equal(processed_ids[1], std::string("tx-2"), "second transaction order should match");
    expect_equal(consumer.committed().size(), std::size_t {2}, "two transactions should be committed");
    expect_equal(consumer.committed()[0].transaction_id, std::string("tx-1"), "first committed transaction should match");
    expect_equal(cache.get_status("tx-2"), std::string("processed"), "processed status should be cached");
    expect_equal(repository.records()[1].status, std::string("processed"), "repository status should be updated");
}

// Verifies malformed transactions are rejected without dispatcher publication.
void test_transaction_ingestor_rejects_invalid_messages() {
    trading::core::EventDispatcher dispatcher;
    std::vector<std::string> processed_ids;
    dispatcher.subscribe([&processed_ids](const trading::core::EngineEvent& event) {
        if (const auto* command = std::get_if<trading::core::TransactionCommand>(&event)) {
            processed_ids.push_back(command->transaction_id);
        }
    });

    auto invalid = make_transaction("tx-bad", 20);
    invalid.quantity = 0.0;

    trading::ingestion::MockTransactionConsumer consumer({invalid});
    trading::storage::InMemoryTransactionRepository repository;
    trading::storage::InMemoryTransactionCache cache;
    trading::ingestion::TransactionIngestor ingestor(consumer, repository, cache, dispatcher);

    log_test_step("process invalid transaction message");
    expect_true(ingestor.process_next(), "invalid transaction should still be handled");

    log_test_step("verify invalid message rejection and commit behavior");
    expect_true(processed_ids.empty(), "invalid transaction should not be published");
    expect_equal(cache.get_status("tx-bad"), std::string("rejected"), "invalid transaction should be rejected");
    expect_equal(repository.records()[0].status, std::string("rejected"), "repository should record rejection");
    expect_equal(consumer.committed().size(), std::size_t {1}, "invalid transaction should still be committed");
}

// Verifies ingestion metrics capture throughput, rejects, and latency.
void test_transaction_ingestor_records_metrics() {
    trading::core::EventDispatcher dispatcher;
    trading::core::FixedClock clock(1000);
    trading::monitoring::InMemoryMetricsCollector metrics;

    auto invalid = make_transaction("tx-bad", 31);
    invalid.quantity = 0.0;

    trading::ingestion::MockTransactionConsumer consumer({
        make_transaction("tx-good", 30),
        invalid,
    });
    trading::storage::InMemoryTransactionRepository repository;
    trading::storage::InMemoryTransactionCache cache;
    trading::ingestion::TransactionIngestor ingestor(consumer, repository, cache, dispatcher, &clock, &metrics);

    expect_true(ingestor.process_next(), "first message should be processed");
    expect_true(ingestor.process_next(), "second message should be handled as rejected");

    expect_equal(metrics.counter("transactions_received"), std::int64_t {2}, "received counter should include all handled messages");
    expect_equal(metrics.counter("transactions_processed"), std::int64_t {1}, "processed counter should include valid messages");
    expect_equal(metrics.counter("transactions_rejected"), std::int64_t {1}, "rejected counter should include invalid messages");
    expect_equal(metrics.latency("transaction_processing_latency_ms").sample_count, std::int64_t {2}, "latency samples should be recorded per handled message");
}

// Verifies persistence failures are surfaced and counted during ingestion.
void test_transaction_ingestor_counts_persistence_failures() {
    trading::core::EventDispatcher dispatcher;
    trading::core::FixedClock clock(1000);
    trading::monitoring::InMemoryMetricsCollector metrics;
    trading::ingestion::MockTransactionConsumer consumer({make_transaction("tx-1", 40)});
    ThrowingTransactionRepository repository;
    trading::storage::InMemoryTransactionCache cache;
    trading::ingestion::TransactionIngestor ingestor(consumer, repository, cache, dispatcher, &clock, &metrics);

    bool threw = false;
    try {
        ingestor.process_next();
    } catch (const std::runtime_error&) {
        threw = true;
    }

    expect_true(threw, "persistence failure should propagate");
    expect_equal(metrics.counter("persistence_failures"), std::int64_t {1}, "persistence failure metric should increment");
}

// Verifies stub exchange adapters satisfy the market/execution interface contracts.
void test_exchange_adapter_stub_integration() {
    const auto instrument = make_btc_instrument();

    trading::exchange::MockExchangeMarketDataAdapter market_adapter(instrument);
    trading::exchange::IExchangeMarketDataAdapter& market_interface = market_adapter;
    const auto market_result = market_interface.normalize(
        "type=trade;event_id=fixture-1;symbol=BTCUSDT;price=42010.5;quantity=0.75;bid=42010.0;ask=42011.0;exchange_ts=123456",
        123500,
        123501);
    const auto* event = std::get_if<trading::core::MarketEvent>(&market_result);
    expect_true(event != nullptr, "market adapter should normalize fixture payload");
    expect_equal(event->event_id, std::string("fixture-1"), "event id should be parsed");
    expect_equal(event->instrument.instrument_id, instrument.instrument_id, "normalized event should keep configured instrument");

    trading::exchange::SimulatedExchangeExecutionAdapter execution_adapter({
        .partial_fill_threshold = 1.0,
        .partial_fill_ratio = 0.5,
    });
    trading::exchange::IExchangeExecutionAdapter& execution_interface = execution_adapter;
    const auto submit_result = execution_interface.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.25,
        .price = 42010.0,
    });
    expect_equal(submit_result.updates.size(), std::size_t {2}, "execution adapter should proxy simulator lifecycle events");
    expect_equal(submit_result.updates[0].status, trading::core::OrderStatus::acknowledged, "first update should acknowledge");

    trading::exchange::MockLiveExchangeExecutionAdapter live_execution_adapter;
    const auto live_submit = live_execution_adapter.submit({
        .request_id = "req-live-1",
        .strategy_id = "strategy-1",
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.25,
        .price = 42010.0,
    });
    expect_equal(live_submit.updates.size(), std::size_t {1}, "live adapter submit should produce one pending update");
    expect_equal(live_submit.updates[0].status, trading::core::OrderStatus::pending_submit, "live adapter should enter pending_submit before exchange ack");

    const auto reconciliation = live_execution_adapter.apply_exchange_report({
        .report_id = "live-report-1",
        .order_id = live_submit.order_id,
        .client_order_id = live_submit.client_order_id,
        .instrument = instrument,
        .side = trading::core::OrderSide::buy,
        .status = trading::core::OrderStatus::acknowledged,
        .cumulative_filled_quantity = 0.0,
        .exchange_timestamp = 1000,
    });
    expect_true(reconciliation.applied, "live adapter should reconcile exchange acknowledgements");
}

// Verifies market-data and execution routing resolve adapters by exchange name.
void test_exchange_router_routes_multi_exchange_requests() {
    trading::exchange::ExchangeMarketDataRouter market_router;
    market_router.register_adapter(
        "binance",
        std::make_unique<trading::exchange::MockExchangeMarketDataAdapter>(make_btc_instrument()));
    market_router.register_adapter(
        "coinbase",
        std::make_unique<trading::exchange::MockExchangeMarketDataAdapter>(make_coinbase_eth_instrument()));

    const auto binance_event = market_router.normalize(
        "binance",
        "type=trade;event_id=fixture-1;symbol=BTCUSDT;price=42010.5;quantity=0.75;exchange_ts=123456",
        123500,
        123501);
    const auto* normalized_binance_event = std::get_if<trading::core::MarketEvent>(&binance_event);
    expect_true(normalized_binance_event != nullptr, "registered exchange should normalize market payload");
    expect_equal(normalized_binance_event->instrument.exchange, std::string("binance"), "normalized event should keep binance identity");

    const auto missing_market = market_router.normalize("kraken", "type=trade", 1, 1);
    const auto* missing_market_error = std::get_if<trading::exchange::ExchangeAdapterError>(&missing_market);
    expect_true(missing_market_error != nullptr, "unknown market exchange should return an adapter error");

    trading::exchange::ExchangeExecutionRouter execution_router;
    execution_router.register_adapter(
        "binance",
        std::make_unique<trading::exchange::SimulatedExchangeExecutionAdapter>(
            trading::execution::SimulatedExecutionConfig {
                .partial_fill_threshold = 1.0,
                .partial_fill_ratio = 0.5,
            }));

    const auto routed_submit = execution_router.submit({
        .request_id = "req-1",
        .strategy_id = "strategy-1",
        .instrument = make_btc_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.25,
        .price = 42010.0,
    });
    expect_equal(routed_submit.updates.size(), std::size_t {2}, "registered execution adapter should receive routed submit");

    const auto missing_submit = execution_router.submit({
        .request_id = "req-2",
        .strategy_id = "strategy-2",
        .instrument = make_coinbase_eth_instrument(),
        .side = trading::core::OrderSide::buy,
        .type = trading::core::OrderType::limit,
        .quantity = 0.25,
        .price = 3000.0,
    });
    expect_equal(missing_submit.updates.size(), std::size_t {1}, "missing execution route should return one rejection update");
    expect_equal(missing_submit.updates[0].status, trading::core::OrderStatus::rejected, "missing execution route should reject");

    const auto capabilities = execution_router.capabilities("binance");
    expect_true(capabilities.has_value(), "registered execution route should expose capabilities");
    expect_true(capabilities->supports_cancel, "simulated execution route should support cancel");
}

// Verifies payload normalization keeps internal market-event semantics stable.
void test_exchange_market_payload_normalization_contract() {
    trading::exchange::MockExchangeMarketDataAdapter market_adapter(make_btc_instrument());

    const auto result = market_adapter.normalize(
        "type=ticker;event_id=tick-1;symbol=BTCUSDT;bid=42001.0;ask=42003.0;exchange_ts=777",
        800,
        900);
    const auto* event = std::get_if<trading::core::MarketEvent>(&result);
    expect_true(event != nullptr, "ticker payload should normalize");
    expect_equal(event->type, trading::core::MarketEventType::ticker, "event type should map from payload type");
    expect_true(event->bid_price.has_value(), "ticker should carry best bid");
    expect_true(event->ask_price.has_value(), "ticker should carry best ask");
    expect_equal(*event->bid_price, 42001.0, "best bid should parse");
    expect_equal(*event->ask_price, 42003.0, "best ask should parse");
    expect_equal(event->exchange_timestamp, std::int64_t {777}, "exchange timestamp should parse");
    expect_equal(event->receive_timestamp, std::int64_t {800}, "receive timestamp should be adapter input");
    expect_equal(event->process_timestamp, std::int64_t {900}, "process timestamp should be adapter input");
}

// Verifies exchange-specific reasons map to stable internal adapter error codes.
void test_exchange_error_mapping_behavior() {
    expect_equal(
        trading::exchange::map_exchange_error("insufficient margin for order"),
        trading::exchange::ExchangeAdapterErrorCode::rejected_by_exchange,
        "insufficient margin should map to rejected_by_exchange");
    expect_equal(
        trading::exchange::map_exchange_error("connection timeout while submitting"),
        trading::exchange::ExchangeAdapterErrorCode::transport_failure,
        "connection timeout should map to transport_failure");
    expect_equal(
        trading::exchange::map_exchange_error("unsupported order type"),
        trading::exchange::ExchangeAdapterErrorCode::unsupported_message_type,
        "unsupported type should map to unsupported_message_type");
}

}  // namespace

// Runs the registered unit tests and returns a non-zero code on failure.
int main() {
    const std::vector<std::pair<std::string, TestFunction>> tests {
        {"core_models", test_core_models},
        {"config_loader", test_config_loader},
        {"config_loader_rejects_missing_strategy_fields", test_config_loader_rejects_missing_strategy_fields},
        {"event_dispatcher_order", test_event_dispatcher_order},
        {"in_memory_structured_logger_records", test_in_memory_structured_logger_records},
        {"console_structured_logger_writes_output", test_console_structured_logger_writes_output},
        {"runtime_health_status_transitions", test_runtime_health_status_transitions},
        {"in_memory_metrics_collector_records_counters_and_latency", test_in_memory_metrics_collector_records_counters_and_latency},
        {"engine_controller_queue_order", test_engine_controller_queue_order},
        {"engine_controller_due_timers", test_engine_controller_due_timers},
        {"engine_controller_mock_sources_and_empty_dispatch", test_engine_controller_mock_sources_and_empty_dispatch},
        {"market_state_store", test_market_state_store},
        {"market_state_store_ignores_out_of_order_events", test_market_state_store_ignores_out_of_order_events},
        {"market_state_store_quote_update_preserves_trade_state", test_market_state_store_quote_update_preserves_trade_state},
        {"market_state_store_tracks_book_depth", test_market_state_store_tracks_book_depth},
        {"feed_health_tracker_detects_stale_and_disconnect_states", test_feed_health_tracker_detects_stale_and_disconnect_states},
        {"live_market_data_feed_controller_reconnects_and_resubscribes", test_live_market_data_feed_controller_reconnects_and_resubscribes},
        {"live_market_data_feed_controller_stops_after_retry_budget", test_live_market_data_feed_controller_stops_after_retry_budget},
        {"sample_threshold_strategy_emits_order_on_trigger", test_sample_threshold_strategy_emits_order_on_trigger},
        {"sample_threshold_strategy_no_order_without_trigger", test_sample_threshold_strategy_no_order_without_trigger},
        {"sample_threshold_strategy_propagates_full_instrument_metadata", test_sample_threshold_strategy_propagates_full_instrument_metadata},
        {"sample_threshold_strategy_reset_command", test_sample_threshold_strategy_reset_command},
        {"strategy_coordinator_isolates_failing_strategy", test_strategy_coordinator_isolates_failing_strategy},
        {"risk_engine_approves_valid_order", test_risk_engine_approves_valid_order},
        {"risk_engine_rejects_oversize_order", test_risk_engine_rejects_oversize_order},
        {"risk_engine_rejects_stale_market", test_risk_engine_rejects_stale_market},
        {"risk_engine_kill_switch_blocks_orders", test_risk_engine_kill_switch_blocks_orders},
        {"simulated_execution_engine_ack_and_fill", test_simulated_execution_engine_ack_and_fill},
        {"order_book_tracks_best_levels", test_order_book_tracks_best_levels},
        {"order_book_preserves_fifo_within_price_level", test_order_book_preserves_fifo_within_price_level},
        {"order_book_cancel_prunes_empty_level", test_order_book_cancel_prunes_empty_level},
        {"matching_engine_fully_matches_crossing_order", test_matching_engine_fully_matches_crossing_order},
        {"matching_engine_partially_matches_and_rests_residual", test_matching_engine_partially_matches_and_rests_residual},
        {"matching_engine_sweeps_multiple_levels_in_price_time_order", test_matching_engine_sweeps_multiple_levels_in_price_time_order},
        {"matching_engine_leaves_non_crossing_order_resting", test_matching_engine_leaves_non_crossing_order_resting},
        {"matching_engine_cancels_resting_order", test_matching_engine_cancels_resting_order},
        {"matching_engine_cancels_partially_filled_resting_order", test_matching_engine_cancels_partially_filled_resting_order},
        {"matching_engine_tracks_order_lifecycle_state", test_matching_engine_tracks_order_lifecycle_state},
        {"matching_engine_rejects_invalid_limit_requests", test_matching_engine_rejects_invalid_limit_requests},
        {"matching_engine_rejects_cancel_for_terminal_order", test_matching_engine_rejects_cancel_for_terminal_order},
        {"matching_engine_restore_open_order_skips_invalid_state", test_matching_engine_restore_open_order_skips_invalid_state},
        {"matching_engine_processes_market_ask_levels_against_resting_bids", test_matching_engine_processes_market_ask_levels_against_resting_bids},
        {"matching_engine_processes_market_bid_levels_against_resting_asks", test_matching_engine_processes_market_bid_levels_against_resting_asks},
        {"simulated_execution_engine_rejects_market_order", test_simulated_execution_engine_rejects_market_order},
        {"simulated_execution_engine_partial_then_full_fill", test_simulated_execution_engine_partial_then_full_fill},
        {"simulated_execution_engine_cancel_open_order", test_simulated_execution_engine_cancel_open_order},
        {"live_execution_tracker_applies_full_lifecycle", test_live_execution_tracker_applies_full_lifecycle},
        {"live_execution_tracker_ignores_duplicate_reports", test_live_execution_tracker_ignores_duplicate_reports},
        {"live_execution_tracker_ignores_late_terminal_regressions", test_live_execution_tracker_ignores_late_terminal_regressions},
        {"portfolio_service_opens_position_from_first_fill", test_portfolio_service_opens_position_from_first_fill},
        {"portfolio_service_updates_average_entry_price", test_portfolio_service_updates_average_entry_price},
        {"portfolio_service_realized_pnl_on_position_reduction", test_portfolio_service_realized_pnl_on_position_reduction},
        {"portfolio_service_unrealized_pnl_from_mark_price", test_portfolio_service_unrealized_pnl_from_mark_price},
        {"portfolio_service_fee_handling_on_balances", test_portfolio_service_fee_handling_on_balances},
        {"simulation_runtime_happy_path", test_simulation_runtime_happy_path},
        {"simulation_runtime_risk_rejection_stops_execution", test_simulation_runtime_risk_rejection_stops_execution},
        {"simulation_runtime_applies_partial_match_fill", test_simulation_runtime_applies_partial_match_fill},
        {"simulation_runtime_market_event_applies_market_driven_fill", test_simulation_runtime_market_event_applies_market_driven_fill},
        {"simulation_runtime_pause_trading_blocks_orders", test_simulation_runtime_pause_trading_blocks_orders},
        {"simulation_runtime_records_metrics", test_simulation_runtime_records_metrics},
        {"in_memory_order_repository_save_and_update", test_in_memory_order_repository_save_and_update},
        {"in_memory_position_repository_update", test_in_memory_position_repository_update},
        {"in_memory_market_state_cache_write_read", test_in_memory_market_state_cache_write_read},
        {"postgres_order_repository_write_read", test_postgres_order_repository_write_read},
        {"redis_cache_adapters_write_read", test_redis_cache_adapters_write_read},
        {"postgres_transaction_checkpoint_persistence", test_postgres_transaction_checkpoint_persistence},
        {"postgres_explicit_kafka_checkpoint_persistence", test_postgres_explicit_kafka_checkpoint_persistence},
        {"postgres_balance_repository_write_read", test_postgres_balance_repository_write_read},
        {"postgres_order_intent_repository_write_read", test_postgres_order_intent_repository_write_read},
        {"postgres_execution_report_repository_write_read", test_postgres_execution_report_repository_write_read},
        {"recovery_service_restores_runtime_and_cache", test_recovery_service_restores_runtime_and_cache},
        {"recovery_service_prefers_explicit_checkpoints", test_recovery_service_prefers_explicit_checkpoints},
        {"recovery_service_resumes_kafka_from_next_offset", test_recovery_service_resumes_kafka_from_next_offset},
        {"simulation_runtime_persists_local_state_for_recovery", test_simulation_runtime_persists_local_state_for_recovery},
        {"recovery_service_restores_local_open_order_continuity", test_recovery_service_restores_local_open_order_continuity},
        {"replay_service_reproduces_fixed_session_state", test_replay_service_reproduces_fixed_session_state},
        {"replay_service_skips_invalid_records", test_replay_service_skips_invalid_records},
        {"replay_service_isolated_from_live_clock", test_replay_service_isolated_from_live_clock},
        {"backtest_runner_summarizes_runtime_results", test_backtest_runner_summarizes_runtime_results},
        {"kafka_transaction_consumer_parses_payload", test_kafka_transaction_consumer_parses_payload},
        {"kafka_transaction_consumer_skips_malformed_messages", test_kafka_transaction_consumer_skips_malformed_messages},
        {"kafka_transaction_consumer_skips_malformed_payload_burst", test_kafka_transaction_consumer_skips_malformed_payload_burst},
        {"kafka_transaction_consumer_commit_checkpoint_behavior", test_kafka_transaction_consumer_commit_checkpoint_behavior},
        {"kafka_transaction_producer_serializes_payload", test_kafka_transaction_producer_serializes_payload},
        {"kafka_transaction_producer_serializes_key", test_kafka_transaction_producer_serializes_key},
        {"transaction_publisher_parses_json_line", test_transaction_publisher_parses_json_line},
        {"transaction_publisher_rejects_invalid_json_line", test_transaction_publisher_rejects_invalid_json_line},
        {"transaction_ingestor_order_and_commit", test_transaction_ingestor_order_and_commit},
        {"transaction_ingestor_rejects_invalid_messages", test_transaction_ingestor_rejects_invalid_messages},
        {"transaction_ingestor_records_metrics", test_transaction_ingestor_records_metrics},
        {"transaction_ingestor_counts_persistence_failures", test_transaction_ingestor_counts_persistence_failures},
        {"exchange_adapter_stub_integration", test_exchange_adapter_stub_integration},
        {"exchange_router_routes_multi_exchange_requests", test_exchange_router_routes_multi_exchange_requests},
        {"exchange_market_payload_normalization_contract", test_exchange_market_payload_normalization_contract},
        {"exchange_error_mapping_behavior", test_exchange_error_mapping_behavior},
    };

    int failures = 0;

    // Step 1: Execute each test and print a simple result line.
    for (const auto& [name, test] : tests) {
        try {
            g_current_test_name = name;
            g_current_test_step.clear();
            std::cout << "[RUN ] " << name << '\n';
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cout << "[FAIL] " << name << ": " << exception.what() << '\n';
        }
    }

    // Step 2: Return an aggregated process exit code for CTest.
    return failures == 0 ? 0 : 1;
}
