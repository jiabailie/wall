#pragma once

#include "app/simulation_runtime.hpp"
#include "storage/replay_service.hpp"

#include <filesystem>
#include <vector>

namespace trading::app {

// Stores the result summary of one historical backtest run.
struct BacktestSummary {
    trading::storage::ReplayStats replay_stats;
    std::size_t risk_approved {0};
    std::size_t risk_rejected {0};
    std::size_t fills_applied {0};
    std::size_t resulting_positions {0};
    std::size_t configured_strategies {0};
    std::size_t active_strategies {0};
    std::size_t paused_strategies {0};
    std::size_t strategy_failures {0};
    double total_realized_pnl {0.0};
    double total_unrealized_pnl {0.0};
    std::vector<trading::strategy::StrategyRuntimeStats> strategy_stats;
};

// Replays historical event data through the live runtime path and returns aggregate results.
class BacktestRunner {
public:
    explicit BacktestRunner(std::filesystem::path historical_log_path)
        : historical_log_path_(std::move(historical_log_path)) {}

    [[nodiscard]] BacktestSummary run(trading::app::SimulationRuntime& runtime) const;

private:
    std::filesystem::path historical_log_path_;
};

}  // namespace trading::app
