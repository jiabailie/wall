#pragma once

#include "strategy/strategy_interface.hpp"

#include <utility>

namespace trading::strategy {

// Stores the configurable behavior for a simple spread-capture strategy.
struct SpreadCaptureStrategyConfig {
    std::string strategy_id;
    trading::core::Instrument instrument;
    double min_spread {0.0};
    double order_quantity {0.0};
    trading::core::OrderSide side {trading::core::OrderSide::buy};
};

// Generates one order when the observed top-of-book spread exceeds a configured threshold.
class SpreadCaptureStrategy final : public IStrategy {
public:
    explicit SpreadCaptureStrategy(SpreadCaptureStrategyConfig config) : config_(std::move(config)) {}

    [[nodiscard]] std::string strategy_id() const override { return config_.strategy_id; }

    [[nodiscard]] std::vector<trading::core::OrderRequest> on_event(
        const trading::core::EngineEvent& event,
        const StrategyContext& context) override;

private:
    [[nodiscard]] trading::core::OrderRequest build_order_request(double price);

    SpreadCaptureStrategyConfig config_;
    bool has_emitted_signal_ {false};
    std::size_t next_request_id_ {1};
};

}  // namespace trading::strategy
