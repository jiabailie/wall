#pragma once

#include "config/app_config.hpp"
#include "core/clock.hpp"
#include "core/types.hpp"
#include "market_data/market_state_store.hpp"

#include <optional>
#include <string>

namespace trading::risk {

// Stores one risk-check outcome for an order request.
struct RiskDecision {
    bool approved {false};
    std::optional<std::string> reason;
};

// Evaluates order requests against baseline pre-trade risk rules.
class RiskEngine {
public:
    RiskEngine(const trading::config::RiskConfig& config,
               const trading::market_data::MarketStateStore& market_state_store,
               const trading::core::IClock& clock)
        : config_(config), market_state_store_(market_state_store), clock_(clock) {}

    // Returns the explicit approval or rejection decision for the provided order request.
    [[nodiscard]] RiskDecision evaluate(const trading::core::OrderRequest& request) const;

private:
    [[nodiscard]] std::optional<double> resolve_reference_price(const trading::core::OrderRequest& request) const;

    const trading::config::RiskConfig& config_;
    const trading::market_data::MarketStateStore& market_state_store_;
    const trading::core::IClock& clock_;
};

}  // namespace trading::risk
