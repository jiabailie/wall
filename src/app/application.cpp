#include "app/application.hpp"

#include "app/engine_controller.hpp"
#include "app/mock_event_sources.hpp"
#include "app/recovery_service.hpp"
#include "app/simulation_runtime.hpp"
#include "config/config_loader.hpp"
#include "exchange/exchange_execution_adapter.hpp"
#include "exchange/exchange_market_data_adapter.hpp"
#include "infrastructure/kafka_transaction_consumer.hpp"
#include "infrastructure/postgres_repositories.hpp"
#include "infrastructure/redis_cache.hpp"
#include "monitoring/health_status.hpp"
#include "monitoring/logger.hpp"
#include "monitoring/metrics.hpp"
#include "storage/replay_service.hpp"
#include "strategy/sample_threshold_strategy.hpp"
#include "strategy/spread_capture_strategy.hpp"
#include "strategy/strategy_coordinator.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace trading::app {

namespace {

// Maps config side strings to domain enum values.
trading::core::OrderSide parse_order_side(const std::string& side) {
    return side == "sell" ? trading::core::OrderSide::sell : trading::core::OrderSide::buy;
}

trading::core::Instrument build_instrument(const std::string& exchange, const std::string& symbol) {
    return {
        .instrument_id = exchange + ":" + symbol,
        .exchange = exchange,
        .symbol = symbol,
        .base_asset = symbol.size() >= 3 ? symbol.substr(0, 3) : symbol,
        .quote_asset = symbol.size() > 3 ? symbol.substr(3) : "USD",
        .tick_size = 0.1,
        .lot_size = 0.0001,
    };
}

std::shared_ptr<trading::strategy::StrategyCoordinator> build_strategy_coordinator(const trading::config::AppConfig& config) {
    auto coordinator = std::make_shared<trading::strategy::StrategyCoordinator>();
    const auto primary_symbol = config.instruments.empty() ? "BTCUSDT" : config.instruments.front();
    const auto primary_instrument = build_instrument(config.exchange_name, primary_symbol);

    coordinator->add_strategy(std::make_unique<trading::strategy::SampleThresholdStrategy>(
        trading::strategy::SampleThresholdStrategyConfig {
            .strategy_id = config.strategy.strategy_id,
            .instrument_id = primary_instrument.instrument_id,
            .instrument = primary_instrument,
            .trigger_price = config.strategy.trigger_price,
            .order_quantity = config.strategy.order_quantity,
            .side = parse_order_side(config.strategy.side),
        }));
    coordinator->add_strategy(std::make_unique<trading::strategy::SpreadCaptureStrategy>(
        trading::strategy::SpreadCaptureStrategyConfig {
            .strategy_id = config.strategy.strategy_id + "-spread",
            .instrument = primary_instrument,
            .min_spread = primary_instrument.tick_size * 5.0,
            .order_quantity = config.strategy.order_quantity * 0.5,
            .side = parse_order_side(config.strategy.side),
        }));

    return coordinator;
}

}  // namespace

// Loads configuration and prints a minimal startup summary for the current scaffold.
void Application::run() {
    trading::config::ConfigLoader loader;

    // Step 1: Load the development configuration file.
    const auto config = loader.load_from_file(std::string(TRADING_SOURCE_DIR) + "/configs/development.cfg");

    // Step 2: Validate the required settings before proceeding.
    const auto validation = loader.validate(config);
    if (!validation.valid) {
        throw std::runtime_error(*validation.error);
    }

    // Step 3: Build the runtime controller with durable infrastructure adapters.
    trading::core::SystemClock clock;
    trading::app::EngineController controller(clock);
    trading::monitoring::ConsoleStructuredLogger logger(std::cout);
    trading::monitoring::RuntimeHealthStatus health_status;
    trading::monitoring::InMemoryMetricsCollector metrics;
    trading::app::RuntimeOperationalControls controls;
    const auto event_log_path = std::filesystem::temp_directory_path() / "wall-runtime-events.tsv";
    const auto local_state_root = std::filesystem::temp_directory_path() / "wall-local-state";
    trading::storage::ReplayService replay_service(event_log_path);
    replay_service.reset_log();
    logger.log(trading::monitoring::LogLevel::info, "application_startup");
    std::unique_ptr<trading::storage::ITransactionRepository> transaction_repository;
    std::unique_ptr<trading::storage::IOrderRepository> order_repository;
    std::unique_ptr<trading::storage::IPositionRepository> position_repository;
    std::unique_ptr<trading::storage::IBalanceRepository> balance_repository;
    std::unique_ptr<trading::storage::ITransactionCache> transaction_cache;
    std::unique_ptr<trading::storage::IMarketStateCache> market_cache;
    std::unique_ptr<trading::infrastructure::IKafkaConsumerClient> kafka_client;
    std::string infrastructure_mode = "native";
    std::string infrastructure_fallback_reason;

    try {
        transaction_repository = std::make_unique<trading::infrastructure::LibpqTransactionRepository>(config.postgres);
        order_repository = std::make_unique<trading::infrastructure::LibpqOrderRepository>(config.postgres);
        position_repository = std::make_unique<trading::infrastructure::LibpqPositionRepository>(config.postgres);
        balance_repository = std::make_unique<trading::infrastructure::LibpqBalanceRepository>(config.postgres);
        transaction_cache = std::make_unique<trading::infrastructure::HiredisTransactionCache>(config.redis);
        market_cache = std::make_unique<trading::infrastructure::HiredisMarketStateCache>(config.redis);
        kafka_client = std::make_unique<trading::infrastructure::RdKafkaConsumerClient>(config.kafka);
    } catch (const std::exception& exception) {
        infrastructure_mode = "fallback";
        infrastructure_fallback_reason = exception.what();

        transaction_repository = std::make_unique<trading::infrastructure::PostgresTransactionRepository>(local_state_root);
        order_repository = std::make_unique<trading::infrastructure::PostgresOrderRepository>(local_state_root);
        position_repository = std::make_unique<trading::infrastructure::PostgresPositionRepository>(local_state_root);
        balance_repository = std::make_unique<trading::infrastructure::PostgresBalanceRepository>(local_state_root);
        transaction_cache = std::make_unique<trading::infrastructure::RedisTransactionCache>();
        market_cache = std::make_unique<trading::infrastructure::RedisMarketStateCache>();

        logger.log(
            trading::monitoring::LogLevel::warn,
            "infrastructure_fallback_enabled",
            {
                {"reason", infrastructure_fallback_reason},
                {"local_state_root", local_state_root.string()},
            });
    }

    const trading::core::Instrument instrument {
        .instrument_id = "mock:" + config.exchange_name + ":" + config.instruments.front(),
        .exchange = config.exchange_name,
        .symbol = config.instruments.front(),
        .base_asset = "BTC",
        .quote_asset = "USDT",
        .tick_size = 0.1,
        .lot_size = 0.0001,
    };

    trading::exchange::MockExchangeMarketDataAdapter market_data_adapter(instrument);
    std::stringstream raw_market_payload;
    raw_market_payload
        << "type=trade;"
        << "event_id=market-1;"
        << "symbol=" << instrument.symbol << ';'
        << "price=42010.0;"
        << "quantity=0.25;"
        << "bid=42000.0;"
        << "ask=42020.0;"
        << "exchange_ts=" << clock.now_ms();
    const auto normalized_market_event = market_data_adapter.normalize(raw_market_payload.str(), clock.now_ms(), clock.now_ms());
    if (const auto* adapter_error = std::get_if<trading::exchange::ExchangeAdapterError>(&normalized_market_event)) {
        throw std::runtime_error("failed to normalize startup market event: " + adapter_error->message);
    }
    const auto startup_market_event = std::get<trading::core::MarketEvent>(normalized_market_event);
    const trading::storage::MarketSnapshot startup_market_snapshot {
        .instrument_id = startup_market_event.instrument.instrument_id,
        .best_bid = startup_market_event.bid_price,
        .best_ask = startup_market_event.ask_price,
        .last_trade_price = startup_market_event.price > 0.0 ? std::optional<double>(startup_market_event.price) : std::nullopt,
        .last_trade_quantity = startup_market_event.quantity > 0.0 ? std::optional<double>(startup_market_event.quantity) : std::nullopt,
        .last_process_timestamp = startup_market_event.process_timestamp,
    };

    trading::app::MockMarketDataSource market_source({
        startup_market_event,
    });
    trading::app::MockTransactionEventSource transaction_source({
        trading::core::TransactionCommand {
            .transaction_id = "mock-tx-1",
            .user_id = "dev-user",
            .account_id = "paper-account",
            .command_type = "place_order",
            .instrument_symbol = instrument.symbol,
            .quantity = 0.10,
            .price = 42000.0,
            .kafka_topic = config.kafka.transaction_topic,
            .kafka_partition = 0,
            .kafka_offset = 0,
        },
    });
    trading::exchange::SimulatedExchangeExecutionAdapter execution_adapter({
        .partial_fill_threshold = config.simulation.partial_fill_threshold,
        .partial_fill_ratio = config.simulation.partial_fill_ratio,
    });
    const auto execution_capabilities = execution_adapter.capabilities();

    controller.add_source(market_source);
    controller.add_source(transaction_source);
    controller.schedule_timer("startup-health-check", clock.now_ms());

    trading::app::SimulationRuntime runtime(
        clock,
        config.risk,
        {
            .strategy = {
                .strategy_id = config.strategy.strategy_id,
                .instrument_id = config.strategy.trigger_instrument_id,
                .instrument = instrument,
                .trigger_price = config.strategy.trigger_price,
                .order_quantity = config.strategy.order_quantity,
                .side = parse_order_side(config.strategy.side),
            },
            .strategy_coordinator = build_strategy_coordinator(config),
            .execution = {
                .partial_fill_threshold = config.simulation.partial_fill_threshold,
                .partial_fill_ratio = config.simulation.partial_fill_ratio,
            },
            .auto_complete_partial_fills = config.simulation.auto_complete_partial_fills,
        },
        &controls,
        &metrics);

    trading::app::RecoveryService recovery_service(
        *transaction_repository,
        *order_repository,
        *position_repository,
        *balance_repository,
        *market_cache,
        *transaction_cache);
    const auto recovery_snapshot = recovery_service.recover({
        startup_market_snapshot,
    });
    recovery_service.restore_runtime(recovery_snapshot, runtime);
    if (kafka_client != nullptr) {
        recovery_service.resume_kafka(recovery_snapshot, *kafka_client);
    }

    controller.subscribe([&runtime, &health_status](const trading::core::EngineEvent& event) {
        const auto approved_before = runtime.risk_approved_count();
        const auto rejected_before = runtime.risk_rejected_count();
        const auto fills_before = runtime.applied_fill_count();

        runtime.on_event(event);

        if (std::get_if<trading::core::MarketEvent>(&event) != nullptr) {
            health_status.set_status(trading::monitoring::RuntimeComponent::market_data, trading::monitoring::HealthStatus::healthy);
        }
        if (std::get_if<trading::core::TransactionCommand>(&event) != nullptr) {
            health_status.set_status(trading::monitoring::RuntimeComponent::ingestion, trading::monitoring::HealthStatus::healthy);
        }
        if (runtime.risk_rejected_count() > rejected_before) {
            health_status.set_status(trading::monitoring::RuntimeComponent::risk, trading::monitoring::HealthStatus::degraded);
        } else if (runtime.risk_approved_count() > approved_before) {
            health_status.set_status(trading::monitoring::RuntimeComponent::risk, trading::monitoring::HealthStatus::healthy);
        }
        if (runtime.applied_fill_count() > fills_before) {
            health_status.set_status(trading::monitoring::RuntimeComponent::execution, trading::monitoring::HealthStatus::healthy);
        }
    });
    controller.subscribe([&replay_service](const trading::core::EngineEvent& event) {
        replay_service.append_event(event);
    });
    controller.subscribe([&logger](const trading::core::EngineEvent& event) {
        std::visit([&logger](const auto& concrete_event) {
            using EventType = std::decay_t<decltype(concrete_event)>;

            if constexpr (std::is_same_v<EventType, trading::core::MarketEvent>) {
                logger.log(
                    trading::monitoring::LogLevel::info,
                    "processed_market_event",
                    {
                        {"event_id", concrete_event.event_id},
                        {"instrument_id", concrete_event.instrument.instrument_id},
                    });
            } else if constexpr (std::is_same_v<EventType, trading::core::TransactionCommand>) {
                logger.log(
                    trading::monitoring::LogLevel::info,
                    "processed_transaction_command",
                    {
                        {"transaction_id", concrete_event.transaction_id},
                        {"command_type", concrete_event.command_type},
                    });
            } else if constexpr (std::is_same_v<EventType, trading::core::TimerEvent>) {
                logger.log(
                    trading::monitoring::LogLevel::info,
                    "processed_timer",
                    {
                        {"timer_id", concrete_event.timer_id},
                    });
            }
        }, event);
    });

    const auto processed_events = controller.run_once();

    for (const auto& position : runtime.portfolio().all_positions()) {
        position_repository->save_position(position);
    }
    for (const auto& balance : runtime.portfolio().all_balances()) {
        balance_repository->save_balance(balance);
    }
    market_cache->upsert_snapshot(startup_market_snapshot);

    // Step 4: Print a startup summary for the current scaffold runtime.
    logger.log(
        trading::monitoring::LogLevel::info,
        "application_startup_complete",
        {
            {"mode", config.mode},
            {"exchange", config.exchange_name},
            {"kafka_topic", config.kafka.transaction_topic},
            {"processed_events", std::to_string(processed_events)},
            {"risk_approved", std::to_string(runtime.risk_approved_count())},
            {"risk_rejected", std::to_string(runtime.risk_rejected_count())},
            {"fills_applied", std::to_string(runtime.applied_fill_count())},
            {"recovered_open_orders", std::to_string(recovery_snapshot.open_orders.size())},
            {"recovered_positions", std::to_string(recovery_snapshot.positions.size())},
            {"recovered_balances", std::to_string(recovery_snapshot.balances.size())},
            {"recovered_kafka_checkpoints", std::to_string(recovery_snapshot.kafka_checkpoints.size())},
            {"infrastructure_mode", infrastructure_mode},
            {"infrastructure_fallback_reason", infrastructure_fallback_reason.empty() ? "none" : infrastructure_fallback_reason},
            {"health_overall", trading::monitoring::to_string(health_status.overall_status())},
            {"metric_transaction_events", std::to_string(metrics.counter("transaction_events"))},
            {"metric_risk_rejects", std::to_string(metrics.counter("risk_rejects"))},
            {"metric_fills_applied", std::to_string(metrics.counter("fills_applied"))},
            {"metric_runtime_event_latency_max_ms", std::to_string(metrics.latency("runtime_event_latency_ms").max_ms)},
            {"event_log_path", event_log_path.string()},
            {"exchange_adapter_supports_cancel", execution_capabilities.supports_cancel ? "true" : "false"},
            {"exchange_adapter_supports_replace", execution_capabilities.supports_replace ? "true" : "false"},
            {"exchange_adapter_supports_market_orders", execution_capabilities.supports_market_orders ? "true" : "false"},
        });
}

}  // namespace trading::app
