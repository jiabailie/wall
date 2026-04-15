#include "strategy/strategy_coordinator.hpp"

#include <algorithm>
#include <utility>

namespace trading::strategy {

void StrategyCoordinator::add_strategy(std::unique_ptr<IStrategy> strategy) {
    if (strategy != nullptr) {
        strategies_.push_back(StrategySlot {
            .strategy = std::move(strategy),
            .stats = {},
        });
        auto& slot = strategies_.back();
        slot.stats.strategy_id = slot.strategy->strategy_id();
    }
}

std::vector<trading::core::OrderRequest> StrategyCoordinator::on_event(
    const trading::core::EngineEvent& event,
    const StrategyContext& context) {
    std::vector<trading::core::OrderRequest> requests;
    for (auto& slot : strategies_) {
        if (slot.stats.paused) {
            continue;
        }

        ++slot.stats.handled_events;
        try {
            auto strategy_requests = slot.strategy->on_event(event, context);
            slot.stats.emitted_requests += strategy_requests.size();
            requests.insert(
                requests.end(),
                std::make_move_iterator(strategy_requests.begin()),
                std::make_move_iterator(strategy_requests.end()));
        } catch (...) {
            ++slot.stats.failures;
            slot.stats.paused = true;
        }
    }

    return requests;
}

std::size_t StrategyCoordinator::active_strategy_count() const {
    return static_cast<std::size_t>(std::count_if(
        strategies_.begin(),
        strategies_.end(),
        [](const StrategySlot& slot) {
            return !slot.stats.paused;
        }));
}

std::vector<StrategyRuntimeStats> StrategyCoordinator::all_stats() const {
    std::vector<StrategyRuntimeStats> stats;
    stats.reserve(strategies_.size());
    for (const auto& slot : strategies_) {
        stats.push_back(slot.stats);
    }

    return stats;
}

bool StrategyCoordinator::pause_strategy(const std::string& strategy_id) {
    for (auto& slot : strategies_) {
        if (slot.stats.strategy_id == strategy_id) {
            slot.stats.paused = true;
            return true;
        }
    }

    return false;
}

bool StrategyCoordinator::resume_strategy(const std::string& strategy_id) {
    for (auto& slot : strategies_) {
        if (slot.stats.strategy_id == strategy_id) {
            slot.stats.paused = false;
            return true;
        }
    }

    return false;
}

bool StrategyCoordinator::is_paused(const std::string& strategy_id) const {
    for (const auto& slot : strategies_) {
        if (slot.stats.strategy_id == strategy_id) {
            return slot.stats.paused;
        }
    }

    return false;
}

}  // namespace trading::strategy
