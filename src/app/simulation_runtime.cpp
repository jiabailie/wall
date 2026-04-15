#include "app/simulation_runtime.hpp"

#include <algorithm>
#include <variant>

namespace trading::app {

SimulationRuntime::SimulationRuntime(const trading::core::IClock& clock,
                                     const trading::config::RiskConfig& risk_config,
                                     SimulationRuntimeConfig runtime_config)
    : market_state_store_(clock),
      strategy_(std::move(runtime_config.strategy)),
      strategy_context_({
          .market_state_store = market_state_store_,
          .clock = clock,
      }),
      risk_engine_(risk_config, market_state_store_, clock),
      execution_engine_(runtime_config.execution),
      auto_complete_partial_fills_(runtime_config.auto_complete_partial_fills) {}

// Handles one incoming engine event through market, strategy, risk, execution, and portfolio.
void SimulationRuntime::on_event(const trading::core::EngineEvent& event) {
    if (const auto* market_event = std::get_if<trading::core::MarketEvent>(&event)) {
        market_state_store_.apply(*market_event);
        if (market_event->price > 0.0) {
            portfolio_service_.set_mark_price(market_event->instrument.instrument_id, market_event->price);
        }
    }

    const auto requests = strategy_.on_event(event, strategy_context_);
    for (const auto& request : requests) {
        const auto decision = risk_engine_.evaluate(request);
        if (!decision.approved) {
            ++risk_rejected_count_;
            continue;
        }

        ++risk_approved_count_;
        const auto result = execution_engine_.submit(request);
        handle_execution_result(result);

        const auto has_partial_update = std::any_of(
            result.updates.begin(),
            result.updates.end(),
            [](const trading::core::OrderUpdate& update) {
                return update.status == trading::core::OrderStatus::partially_filled;
            });
        if (auto_complete_partial_fills_ && has_partial_update) {
            handle_execution_result(execution_engine_.complete_open_order(result.order_id));
        }
    }
}

// Applies execution fills into the portfolio and counters.
void SimulationRuntime::handle_execution_result(const trading::execution::ExecutionResult& result) {
    for (const auto& fill : result.fills) {
        portfolio_service_.apply_fill(fill);
        ++applied_fill_count_;
    }
}

void SimulationRuntime::restore_open_order(const trading::storage::OrderRecord& order) {
    trading::core::OrderRequest request {
        .request_id = order.order_id,
        .strategy_id = order.strategy_id,
        .instrument = {
            .instrument_id = order.instrument_id,
        },
        .side = order.side,
        .type = order.order_type,
        .quantity = order.quantity,
        .price = order.price,
    };
    execution_engine_.restore_open_order(order.order_id, order.client_order_id, request, order.filled_quantity);
}

void SimulationRuntime::restore_position(const trading::core::Position& position) {
    portfolio_service_.restore_position(position);
}

void SimulationRuntime::restore_balance(const trading::core::BalanceSnapshot& balance) {
    portfolio_service_.restore_balance(balance);
}

void SimulationRuntime::restore_market_snapshot(const trading::storage::MarketSnapshot& snapshot) {
    market_state_store_.restore_snapshot(
        snapshot.instrument_id,
        {
            .best_bid = snapshot.best_bid,
            .best_ask = snapshot.best_ask,
            .last_trade_price = snapshot.last_trade_price,
            .last_trade_quantity = snapshot.last_trade_quantity,
            .last_process_timestamp = snapshot.last_process_timestamp,
        });

    if (snapshot.last_trade_price.has_value()) {
        portfolio_service_.set_mark_price(snapshot.instrument_id, *snapshot.last_trade_price);
    }
}

}  // namespace trading::app
