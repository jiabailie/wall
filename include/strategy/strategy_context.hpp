#pragma once

#include "core/clock.hpp"
#include "market_data/market_state_store.hpp"

namespace trading::strategy {

// Exposes the read-only runtime services strategies need for deterministic decisions.
struct StrategyContext {
    const trading::market_data::MarketStateStore& market_state_store;
    const trading::core::IClock& clock;
};

}  // namespace trading::strategy
