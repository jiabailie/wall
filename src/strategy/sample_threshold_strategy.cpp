#include "strategy/sample_threshold_strategy.hpp"

#include <sstream>
#include <type_traits>
#include <variant>

namespace trading::strategy {

// Consumes one engine event and emits at most one deterministic order request.
std::vector<trading::core::OrderRequest> SampleThresholdStrategy::on_event(
    const trading::core::EngineEvent& event,
    const StrategyContext& context) {
    std::vector<trading::core::OrderRequest> requests;

    std::visit([this, &context, &requests](const auto& concrete_event) {
        using EventType = std::decay_t<decltype(concrete_event)>;

        if constexpr (std::is_same_v<EventType, trading::core::MarketEvent>) {
            if (concrete_event.instrument.instrument_id != config_.instrument_id || concrete_event.price <= 0.0) {
                return;
            }

            if (const auto state = context.market_state_store.get(config_.instrument_id); !state.has_value()) {
                return;
            }

            if (should_emit_for_trade_price(concrete_event.price)) {
                requests.push_back(build_order_request(concrete_event.instrument, concrete_event.price));
                has_emitted_signal_ = true;
            }
        } else if constexpr (std::is_same_v<EventType, trading::core::TransactionCommand>) {
            if (concrete_event.command_type == "reset_strategy") {
                has_emitted_signal_ = false;
            }
        }
    }, event);

    return requests;
}

// Returns true when the trade price crosses the configured threshold.
bool SampleThresholdStrategy::should_emit_for_trade_price(const double trade_price) const {
    if (has_emitted_signal_) {
        return false;
    }

    if (config_.side == trading::core::OrderSide::buy) {
        return trade_price <= config_.trigger_price;
    }

    return trade_price >= config_.trigger_price;
}

// Builds one stable order request for the configured instrument and side.
trading::core::OrderRequest SampleThresholdStrategy::build_order_request(const trading::core::Instrument& instrument,
                                                                         const double trade_price) {
    std::stringstream request_id;
    request_id << config_.strategy_id << "-request-" << next_request_id_++;

    const auto resolved_instrument = config_.instrument.instrument_id.empty()
        || (config_.instrument.base_asset.empty() && config_.instrument.quote_asset.empty())
        ? instrument
        : config_.instrument;

    return {
        .request_id = request_id.str(),
        .strategy_id = config_.strategy_id,
        .instrument = resolved_instrument,
        .side = config_.side,
        .type = trading::core::OrderType::limit,
        .quantity = config_.order_quantity,
        .price = trade_price,
    };
}

}  // namespace trading::strategy
