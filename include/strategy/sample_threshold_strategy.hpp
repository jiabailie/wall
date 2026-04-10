#pragma once

#include "strategy/strategy_interface.hpp"

#include <utility>

namespace trading::strategy {

// Stores the configurable behavior for the sample threshold strategy.
struct SampleThresholdStrategyConfig {
    std::string strategy_id;
    std::string instrument_id;
    double trigger_price {0.0};
    double order_quantity {0.0};
    trading::core::OrderSide side {trading::core::OrderSide::buy};
};

// Generates an order when a trade price crosses a configured threshold.
class SampleThresholdStrategy final : public IStrategy {
public:
    explicit SampleThresholdStrategy(SampleThresholdStrategyConfig config) : config_(std::move(config)) {}

    [[nodiscard]] std::string strategy_id() const override { return config_.strategy_id; }

    [[nodiscard]] std::vector<trading::core::OrderRequest> on_event(
        const trading::core::EngineEvent& event,
        const StrategyContext& context) override;

private:
    [[nodiscard]] bool should_emit_for_trade_price(double trade_price) const;
    [[nodiscard]] trading::core::OrderRequest build_order_request(double trade_price);

    SampleThresholdStrategyConfig config_;
    bool has_emitted_signal_ {false};
    std::size_t next_request_id_ {1};
};

}  // namespace trading::strategy
