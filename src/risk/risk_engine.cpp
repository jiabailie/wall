#include "risk/risk_engine.hpp"

namespace trading::risk {

// Returns the explicit approval or rejection decision for the provided order request.
RiskDecision RiskEngine::evaluate(const trading::core::OrderRequest& request) const {
    if (config_.kill_switch_enabled) {
        return {.approved = false, .reason = "kill switch enabled"};
    }

    if (request.quantity <= 0.0) {
        return {.approved = false, .reason = "quantity must be positive"};
    }
    if (request.quantity > config_.max_order_quantity) {
        return {.approved = false, .reason = "order quantity exceeds limit"};
    }
    if (request.quantity > config_.max_position_quantity) {
        return {.approved = false, .reason = "position quantity exceeds limit"};
    }
    if (market_state_store_.is_stale(request.instrument.instrument_id, config_.stale_after_ms)) {
        return {.approved = false, .reason = "market data is stale"};
    }

    const auto reference_price = resolve_reference_price(request);
    if (!reference_price.has_value()) {
        return {.approved = false, .reason = "reference price unavailable"};
    }

    if ((request.quantity * *reference_price) > config_.max_order_notional) {
        return {.approved = false, .reason = "order notional exceeds limit"};
    }

    static_cast<void>(clock_);
    return {.approved = true, .reason = std::nullopt};
}

// Returns the order price when available, otherwise uses the latest market trade price.
std::optional<double> RiskEngine::resolve_reference_price(const trading::core::OrderRequest& request) const {
    if (request.price.has_value()) {
        return request.price;
    }

    const auto state = market_state_store_.get(request.instrument.instrument_id);
    if (!state.has_value()) {
        return std::nullopt;
    }

    return state->last_trade_price;
}

}  // namespace trading::risk
