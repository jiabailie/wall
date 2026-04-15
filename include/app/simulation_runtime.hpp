#pragma once

#include "app/runtime_operational_controls.hpp"
#include "config/app_config.hpp"
#include "core/types.hpp"
#include "execution/simulated_execution_engine.hpp"
#include "market_data/market_state_store.hpp"
#include "monitoring/metrics.hpp"
#include "storage/storage_interfaces.hpp"
#include "portfolio/portfolio_service.hpp"
#include "risk/risk_engine.hpp"
#include "strategy/sample_threshold_strategy.hpp"

#include <cstddef>

namespace trading::app {

// Stores configurable behavior for end-to-end in-process simulation.
struct SimulationRuntimeConfig {
    trading::strategy::SampleThresholdStrategyConfig strategy;
    trading::execution::SimulatedExecutionConfig execution;
    bool auto_complete_partial_fills {false};
};

// Wires the full event pipeline for local simulation runs.
class SimulationRuntime {
public:
    SimulationRuntime(const trading::core::IClock& clock,
                      const trading::config::RiskConfig& risk_config,
                      SimulationRuntimeConfig runtime_config,
                      RuntimeOperationalControls* controls = nullptr,
                      trading::monitoring::IMetricsCollector* metrics = nullptr);

    // Handles one incoming engine event through market, strategy, risk, execution, and portfolio.
    void on_event(const trading::core::EngineEvent& event);

    // Returns the current in-memory portfolio state.
    [[nodiscard]] const trading::portfolio::PortfolioService& portfolio() const { return portfolio_service_; }

    // Returns total risk-approved request count since startup.
    [[nodiscard]] std::size_t risk_approved_count() const { return risk_approved_count_; }

    // Returns total risk-rejected request count since startup.
    [[nodiscard]] std::size_t risk_rejected_count() const { return risk_rejected_count_; }

    // Returns total applied fill count since startup.
    [[nodiscard]] std::size_t applied_fill_count() const { return applied_fill_count_; }

    // Restores one persisted open order into the execution engine for startup recovery.
    void restore_open_order(const trading::storage::OrderRecord& order);

    // Restores one persisted position into the portfolio service for startup recovery.
    void restore_position(const trading::core::Position& position);

    // Restores one persisted balance into the portfolio service for startup recovery.
    void restore_balance(const trading::core::BalanceSnapshot& balance);

    // Restores one market snapshot into the market-state store and mark-price state.
    void restore_market_snapshot(const trading::storage::MarketSnapshot& snapshot);

    [[nodiscard]] bool trading_paused() const {
        return controls_ != nullptr ? controls_->trading_paused() : false;
    }

private:
    void handle_execution_result(const trading::execution::ExecutionResult& result);

    const trading::core::IClock& clock_;
    trading::market_data::MarketStateStore market_state_store_;
    trading::strategy::SampleThresholdStrategy strategy_;
    trading::strategy::StrategyContext strategy_context_;
    trading::risk::RiskEngine risk_engine_;
    trading::portfolio::PortfolioService portfolio_service_;
    trading::execution::SimulatedExecutionEngine execution_engine_;
    RuntimeOperationalControls* controls_ {nullptr};
    trading::monitoring::IMetricsCollector* metrics_ {nullptr};
    bool auto_complete_partial_fills_ {false};

    std::size_t risk_approved_count_ {0};
    std::size_t risk_rejected_count_ {0};
    std::size_t applied_fill_count_ {0};
};

}  // namespace trading::app
