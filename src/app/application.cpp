#include "app/application.hpp"

#include "app/engine_controller.hpp"
#include "app/mock_event_sources.hpp"
#include "app/simulation_runtime.hpp"
#include "config/config_loader.hpp"
#include "exchange/exchange_execution_adapter.hpp"
#include "exchange/exchange_market_data_adapter.hpp"
#include "monitoring/health_status.hpp"
#include "monitoring/logger.hpp"
#include "storage/replay_service.hpp"

#include <filesystem>
#include <iostream>
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

    // Step 3: Build the runtime controller with mock development data sources.
    trading::core::SystemClock clock;
    trading::app::EngineController controller(clock);
    trading::monitoring::ConsoleStructuredLogger logger(std::cout);
    trading::monitoring::RuntimeHealthStatus health_status;
    const auto event_log_path = std::filesystem::temp_directory_path() / "wall-runtime-events.tsv";
    trading::storage::ReplayService replay_service(event_log_path);
    replay_service.reset_log();
    logger.log(trading::monitoring::LogLevel::info, "application_startup");

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
                .trigger_price = config.strategy.trigger_price,
                .order_quantity = config.strategy.order_quantity,
                .side = parse_order_side(config.strategy.side),
            },
            .execution = {
                .partial_fill_threshold = config.simulation.partial_fill_threshold,
                .partial_fill_ratio = config.simulation.partial_fill_ratio,
            },
            .auto_complete_partial_fills = config.simulation.auto_complete_partial_fills,
        });

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
            {"health_overall", trading::monitoring::to_string(health_status.overall_status())},
            {"event_log_path", event_log_path.string()},
            {"exchange_adapter_supports_cancel", execution_capabilities.supports_cancel ? "true" : "false"},
            {"exchange_adapter_supports_replace", execution_capabilities.supports_replace ? "true" : "false"},
            {"exchange_adapter_supports_market_orders", execution_capabilities.supports_market_orders ? "true" : "false"},
        });
}

}  // namespace trading::app
