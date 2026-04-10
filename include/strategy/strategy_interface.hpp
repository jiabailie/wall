#pragma once

#include "core/types.hpp"
#include "strategy/strategy_context.hpp"

#include <string>
#include <vector>

namespace trading::strategy {

// Defines the common interface implemented by all strategy modules.
class IStrategy {
public:
    virtual ~IStrategy() = default;

    // Returns the stable identifier of the strategy implementation.
    [[nodiscard]] virtual std::string strategy_id() const = 0;

    // Consumes one engine event and returns any generated order requests.
    [[nodiscard]] virtual std::vector<trading::core::OrderRequest> on_event(
        const trading::core::EngineEvent& event,
        const StrategyContext& context) = 0;
};

}  // namespace trading::strategy
