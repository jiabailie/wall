#include "app/engine_controller.hpp"
#include "app/mock_event_sources.hpp"
#include "app/recovery_service.hpp"
#include "app/simulation_runtime.hpp"
#include "app/transaction_publisher_application.hpp"
#include "config/config_loader.hpp"
#include "core/clock.hpp"
#include "core/event_dispatcher.hpp"
#include "core/types.hpp"
#include "exchange/exchange_execution_adapter.hpp"
#include "exchange/live_market_data_feed.hpp"
#include "exchange/exchange_market_data_adapter.hpp"
#include "execution/live_execution_tracker.hpp"
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

#include <cstdlib>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using TestFunction = std::function<void()>;

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
        throw std::runtime_error(message);
    }
}

// Fails the current test when the two values differ.
template <typename T>
void expect_equal(const T& actual, const T& expected, const std::string& message) {
    if (!(actual == expected)) {
        throw std::runtime_error(message);
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

    runtime.on_event(trading::core::MarketEvent {
        .event_id = "event-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

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

// Verifies partial then completion fills are both applied into portfolio through the pipeline.
void test_simulation_runtime_applies_partial_and_completion_fills() {
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

    runtime.on_event(trading::core::MarketEvent {
        .event_id = "event-1",
        .type = trading::core::MarketEventType::trade,
        .instrument = make_btc_instrument(),
        .price = 41990.0,
        .quantity = 0.5,
        .process_timestamp = 4900,
    });

    expect_equal(runtime.risk_approved_count(), std::size_t {1}, "partial/completion path should approve one request");
    expect_equal(runtime.applied_fill_count(), std::size_t {2}, "partial/completion path should apply two fills");
    const auto position = runtime.portfolio().get_position("binance:BTCUSDT");
    expect_true(position.has_value(), "position should exist");
    expect_equal(position->net_quantity, 1.0, "partial and completion fills should produce full target quantity");
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
    cache.upsert_snapshot({
        .instrument_id = "binance:BTCUSDT",
        .best_bid = 42000.0,
        .best_ask = 42010.0,
        .last_trade_price = 42005.0,
        .last_trade_quantity = 0.25,
        .last_process_timestamp = 5000,
    });

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
        const auto order = repository.get_order("order-1");
        expect_true(order.has_value(), "postgres order adapter should persist records");
        expect_equal(order->status, trading::core::OrderStatus::acknowledged, "persisted order status should reload");
    }
    std::filesystem::remove_all(temp_dir);
}

// Verifies Redis-style cache adapters read/write transaction and market state values.
void test_redis_cache_adapters_write_read() {
    trading::infrastructure::RedisTransactionCache transaction_cache;
    transaction_cache.set_status("tx-1", "processed");
    expect_equal(transaction_cache.get_status("tx-1"), std::string("processed"), "redis transaction cache should store statuses");

    trading::infrastructure::RedisMarketStateCache market_cache;
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
        repository.save_received(command);
        repository.save_processed("tx-1", "processed");
    }
    {
        trading::infrastructure::PostgresTransactionRepository repository(temp_dir);
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
        repository.save_checkpoint({
            .topic = "trading-transactions",
            .partition = 2,
            .offset = 144,
        });
    }
    {
        trading::infrastructure::PostgresTransactionRepository repository(temp_dir);
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
        repository.save_balance({
            .asset = "USDT",
            .total_balance = 10500.5,
            .available_balance = 10400.25,
        });
    }
    {
        trading::infrastructure::PostgresBalanceRepository repository(temp_dir);
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
    recovery_service.restore_runtime(snapshot, runtime);

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
    transaction_repository.save_received(tx);
    transaction_repository.save_processed("tx-1", "processed");

    trading::app::RecoveryService recovery_service(
        transaction_repository,
        order_repository,
        position_repository,
        balance_repository,
        market_cache,
        transaction_cache);
    const auto snapshot = recovery_service.recover();

    FakeKafkaConsumerClient kafka_client({});
    recovery_service.resume_kafka(snapshot, kafka_client);

    expect_equal(kafka_client.seeks().size(), std::size_t {1}, "one kafka partition should be resumed");
    expect_equal(kafka_client.seeks()[0].partition, 2, "recovery should resume the recovered partition");
    expect_equal(kafka_client.seeks()[0].offset, std::int64_t {78}, "recovery should seek to the next offset");
}

// Verifies replay reproduces the same state as direct fixed-session processing.
void test_replay_service_reproduces_fixed_session_state() {
    const auto temp_dir = make_temp_directory("replay-fixed-session");
    const auto log_path = temp_dir / "events.tsv";

    const std::vector<trading::core::EngineEvent> events {
        trading::core::MarketEvent {
            .event_id = "event-1",
            .type = trading::core::MarketEventType::trade,
            .instrument = make_btc_instrument(),
            .price = 41990.0,
            .quantity = 0.5,
            .process_timestamp = 4900,
        },
        trading::core::TimerEvent {
            .timer_id = "timer-1",
            .timestamp = 5000,
        },
    };

    trading::storage::ReplayService replay_service(log_path);
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

    const auto stats = replay_service.replay([&replay_runtime](const trading::core::EngineEvent& event) {
        replay_runtime.on_event(event);
    });

    expect_equal(stats.replayed_records, std::size_t {2}, "replay should apply both records");
    expect_equal(replay_runtime.risk_approved_count(), direct_runtime.risk_approved_count(), "replay should match approved count");
    expect_equal(replay_runtime.applied_fill_count(), direct_runtime.applied_fill_count(), "replay should match fill count");

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
    const auto stats_a = replay_service.replay([&runtime_a](const trading::core::EngineEvent& event) {
        runtime_a.on_event(event);
    });

    trading::core::FixedClock clock_b(9999999);
    trading::app::SimulationRuntime runtime_b(clock_b, risk_config, runtime_config);
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

    const auto command = consumer.poll();
    expect_true(command.has_value(), "valid payload should be returned");
    expect_equal(fake_client.commits().size(), std::size_t {0}, "poll should not commit valid payload");

    consumer.commit(*command);
    expect_equal(fake_client.commits().size(), std::size_t {1}, "explicit commit should forward offset to client");
    expect_equal(fake_client.commits()[0].offset, std::int64_t {77}, "committed offset should match payload");
}

// Verifies producer payload serialization matches the consumer-side key-value schema.
void test_kafka_transaction_producer_serializes_payload() {
    const auto command = make_transaction("tx-serializer", 0);

    const auto payload = trading::infrastructure::serialize_transaction_command_payload(command);
    expect_true(payload.find("transaction_id=tx-serializer") != std::string::npos, "payload should contain transaction id");
    expect_true(payload.find("user_id=user-1") != std::string::npos, "payload should contain user id");
    expect_true(payload.find("instrument_symbol=BTCUSDT") != std::string::npos, "payload should contain instrument symbol");
    expect_true(payload.find("quantity=1.5") != std::string::npos, "payload should contain quantity");
    expect_true(payload.find("price=42000") != std::string::npos, "payload should contain price");
}

// Verifies the transaction publisher can parse one flat JSON input line.
void test_transaction_publisher_parses_json_line() {
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

    // Step 1: Process both mock transactions through the ingestor.
    expect_true(ingestor.process_next(), "first transaction should be processed");
    expect_true(ingestor.process_next(), "second transaction should be processed");
    expect_true(!ingestor.process_next(), "no third transaction should exist");

    // Step 2: Verify ordered processing and ordered commits.
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

    // Step 1: Process the invalid message.
    expect_true(ingestor.process_next(), "invalid transaction should still be handled");

    // Step 2: Verify it was rejected and never dispatched to runtime subscribers.
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
        {"sample_threshold_strategy_reset_command", test_sample_threshold_strategy_reset_command},
        {"risk_engine_approves_valid_order", test_risk_engine_approves_valid_order},
        {"risk_engine_rejects_oversize_order", test_risk_engine_rejects_oversize_order},
        {"risk_engine_rejects_stale_market", test_risk_engine_rejects_stale_market},
        {"risk_engine_kill_switch_blocks_orders", test_risk_engine_kill_switch_blocks_orders},
        {"simulated_execution_engine_ack_and_fill", test_simulated_execution_engine_ack_and_fill},
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
        {"simulation_runtime_applies_partial_and_completion_fills", test_simulation_runtime_applies_partial_and_completion_fills},
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
        {"replay_service_reproduces_fixed_session_state", test_replay_service_reproduces_fixed_session_state},
        {"replay_service_skips_invalid_records", test_replay_service_skips_invalid_records},
        {"replay_service_isolated_from_live_clock", test_replay_service_isolated_from_live_clock},
        {"kafka_transaction_consumer_parses_payload", test_kafka_transaction_consumer_parses_payload},
        {"kafka_transaction_consumer_skips_malformed_messages", test_kafka_transaction_consumer_skips_malformed_messages},
        {"kafka_transaction_consumer_skips_malformed_payload_burst", test_kafka_transaction_consumer_skips_malformed_payload_burst},
        {"kafka_transaction_consumer_commit_checkpoint_behavior", test_kafka_transaction_consumer_commit_checkpoint_behavior},
        {"kafka_transaction_producer_serializes_payload", test_kafka_transaction_producer_serializes_payload},
        {"transaction_publisher_parses_json_line", test_transaction_publisher_parses_json_line},
        {"transaction_publisher_rejects_invalid_json_line", test_transaction_publisher_rejects_invalid_json_line},
        {"transaction_ingestor_order_and_commit", test_transaction_ingestor_order_and_commit},
        {"transaction_ingestor_rejects_invalid_messages", test_transaction_ingestor_rejects_invalid_messages},
        {"transaction_ingestor_records_metrics", test_transaction_ingestor_records_metrics},
        {"transaction_ingestor_counts_persistence_failures", test_transaction_ingestor_counts_persistence_failures},
        {"exchange_adapter_stub_integration", test_exchange_adapter_stub_integration},
        {"exchange_market_payload_normalization_contract", test_exchange_market_payload_normalization_contract},
        {"exchange_error_mapping_behavior", test_exchange_error_mapping_behavior},
    };

    int failures = 0;

    // Step 1: Execute each test and print a simple result line.
    for (const auto& [name, test] : tests) {
        try {
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
