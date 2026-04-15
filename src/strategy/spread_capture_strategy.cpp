#include "strategy/spread_capture_strategy.hpp"

#include <sstream>
#include <type_traits>
#include <variant>

namespace trading::strategy {

std::vector<trading::core::OrderRequest> SpreadCaptureStrategy::on_event(
    const trading::core::EngineEvent& event,
    const StrategyContext& context) {
    std::vector<trading::core::OrderRequest> requests;

    std::visit([this, &context, &requests](const auto& concrete_event) {
        using EventType = std::decay_t<decltype(concrete_event)>;

        if constexpr (std::is_same_v<EventType, trading::core::MarketEvent>) {
            if (has_emitted_signal_ || concrete_event.instrument.instrument_id != config_.instrument.instrument_id) {
                return;
            }

            const auto state = context.market_state_store.get(config_.instrument.instrument_id);
            if (!state.has_value() || !state->best_bid.has_value() || !state->best_ask.has_value()) {
                return;
            }

            const auto spread = *state->best_ask - *state->best_bid;
            if (spread < config_.min_spread) {
                return;
            }

            const auto price = config_.side == trading::core::OrderSide::buy ? *state->best_bid : *state->best_ask;
            requests.push_back(build_order_request(price));
            has_emitted_signal_ = true;
        } else if constexpr (std::is_same_v<EventType, trading::core::TransactionCommand>) {
            if (concrete_event.command_type == "reset_strategy") {
                has_emitted_signal_ = false;
            }
        }
    }, event);

    return requests;
}

trading::core::OrderRequest SpreadCaptureStrategy::build_order_request(const double price) {
    std::stringstream request_id;
    request_id << config_.strategy_id << "-request-" << next_request_id_++;

    return {
        .request_id = request_id.str(),
        .strategy_id = config_.strategy_id,
        .instrument = config_.instrument,
        .side = config_.side,
        .type = trading::core::OrderType::limit,
        .quantity = config_.order_quantity,
        .price = price,
    };
}

}  // namespace trading::strategy
