#include "app/backtest_runner.hpp"
#include "config/config_loader.hpp"
#include "core/clock.hpp"
#include "strategy/sample_threshold_strategy.hpp"
#include "strategy/spread_capture_strategy.hpp"
#include "strategy/strategy_coordinator.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

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

int main(int argc, char** argv) {
    try {
        trading::config::ConfigLoader loader;
        const auto config = loader.load_from_file(std::string(TRADING_SOURCE_DIR) + "/configs/development.cfg");
        const auto validation = loader.validate(config);
        if (!validation.valid) {
            throw std::runtime_error(*validation.error);
        }

        const std::filesystem::path log_path = argc > 1
            ? std::filesystem::path(argv[1])
            : (std::filesystem::temp_directory_path() / "wall-runtime-events.tsv");

        trading::core::FixedClock clock(0);
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
                .strategy_coordinator = build_strategy_coordinator(config),
                .execution = {
                    .partial_fill_threshold = config.simulation.partial_fill_threshold,
                    .partial_fill_ratio = config.simulation.partial_fill_ratio,
                },
                .auto_complete_partial_fills = config.simulation.auto_complete_partial_fills,
            });

        trading::app::BacktestRunner runner(log_path);
        const auto summary = runner.run(runtime);
        std::cout
            << "Backtest complete"
            << " log_path=" << log_path.string()
            << " total_records=" << summary.replay_stats.total_records
            << " replayed_records=" << summary.replay_stats.replayed_records
            << " skipped_records=" << summary.replay_stats.skipped_records
            << " configured_strategies=" << summary.configured_strategies
            << " active_strategies=" << summary.active_strategies
            << " paused_strategies=" << summary.paused_strategies
            << " strategy_failures=" << summary.strategy_failures
            << " risk_approved=" << summary.risk_approved
            << " risk_rejected=" << summary.risk_rejected
            << " fills_applied=" << summary.fills_applied
            << " resulting_positions=" << summary.resulting_positions
            << " total_realized_pnl=" << summary.total_realized_pnl
            << " total_unrealized_pnl=" << summary.total_unrealized_pnl
            << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Backtest failed: " << exception.what() << '\n';
        return 1;
    }
}
