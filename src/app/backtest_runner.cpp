#include "app/backtest_runner.hpp"

namespace trading::app {

BacktestSummary BacktestRunner::run(trading::app::SimulationRuntime& runtime) const {
    trading::storage::ReplayService replay_service(historical_log_path_);
    const auto replay_stats = replay_service.replay([&runtime](const trading::core::EngineEvent& event) {
        runtime.on_event(event);
    });

    const auto positions = runtime.portfolio().all_positions();
    double total_realized_pnl = 0.0;
    double total_unrealized_pnl = 0.0;
    for (const auto& position : positions) {
        total_realized_pnl += position.realized_pnl;
        total_unrealized_pnl += position.unrealized_pnl;
    }

    const auto strategy_stats = runtime.strategy_stats();
    std::size_t paused_strategies = 0;
    std::size_t strategy_failures = 0;
    for (const auto& stats : strategy_stats) {
        if (stats.paused) {
            ++paused_strategies;
        }
        strategy_failures += stats.failures;
    }

    return {
        .replay_stats = replay_stats,
        .risk_approved = runtime.risk_approved_count(),
        .risk_rejected = runtime.risk_rejected_count(),
        .fills_applied = runtime.applied_fill_count(),
        .resulting_positions = positions.size(),
        .configured_strategies = runtime.strategy_count(),
        .active_strategies = runtime.active_strategy_count(),
        .paused_strategies = paused_strategies,
        .strategy_failures = strategy_failures,
        .total_realized_pnl = total_realized_pnl,
        .total_unrealized_pnl = total_unrealized_pnl,
        .strategy_stats = strategy_stats,
    };
}

}  // namespace trading::app
