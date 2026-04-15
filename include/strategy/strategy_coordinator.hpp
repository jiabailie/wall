#pragma once

#include "strategy/strategy_interface.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading::strategy {

// Stores orchestration state for one strategy instance inside the shared runtime.
struct StrategyRuntimeStats {
    std::string strategy_id;
    std::size_t handled_events {0};
    std::size_t emitted_requests {0};
    std::size_t failures {0};
    bool paused {false};
};

// Runs multiple strategies against the same event stream and merges their order intents.
class StrategyCoordinator {
public:
    void add_strategy(std::unique_ptr<IStrategy> strategy);

    [[nodiscard]] std::vector<trading::core::OrderRequest> on_event(
        const trading::core::EngineEvent& event,
        const StrategyContext& context);

    [[nodiscard]] std::size_t strategy_count() const { return strategies_.size(); }
    [[nodiscard]] std::size_t active_strategy_count() const;
    [[nodiscard]] std::vector<StrategyRuntimeStats> all_stats() const;
    [[nodiscard]] bool pause_strategy(const std::string& strategy_id);
    [[nodiscard]] bool resume_strategy(const std::string& strategy_id);
    [[nodiscard]] bool is_paused(const std::string& strategy_id) const;

private:
    struct StrategySlot {
        std::unique_ptr<IStrategy> strategy;
        StrategyRuntimeStats stats;
    };

    std::vector<StrategySlot> strategies_;
};

}  // namespace trading::strategy
