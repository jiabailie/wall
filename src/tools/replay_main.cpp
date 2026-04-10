#include "app/simulation_runtime.hpp"
#include "config/config_loader.hpp"
#include "core/clock.hpp"
#include "storage/replay_service.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

trading::core::OrderSide parse_order_side(const std::string& side) {
    return side == "sell" ? trading::core::OrderSide::sell : trading::core::OrderSide::buy;
}

}  // namespace

// Replays a persisted event session into the simulation runtime.
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

        trading::core::FixedClock replay_clock(0);
        trading::app::SimulationRuntime runtime(
            replay_clock,
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

        trading::storage::ReplayService replay_service(log_path);
        const auto stats = replay_service.replay([&runtime](const trading::core::EngineEvent& event) {
            runtime.on_event(event);
        });

        std::cout
            << "Replay complete"
            << " log_path=" << log_path.string()
            << " total_records=" << stats.total_records
            << " replayed_records=" << stats.replayed_records
            << " skipped_records=" << stats.skipped_records
            << " risk_approved=" << runtime.risk_approved_count()
            << " risk_rejected=" << runtime.risk_rejected_count()
            << " fills_applied=" << runtime.applied_fill_count()
            << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Replay failed: " << exception.what() << '\n';
        return 1;
    }
}
