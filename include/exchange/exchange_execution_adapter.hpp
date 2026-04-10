#pragma once

#include "exchange/exchange_market_data_adapter.hpp"
#include "execution/simulated_execution_engine.hpp"

#include <string>

namespace trading::exchange {

// Stores exchange execution capability flags exposed to the application layer.
struct ExchangeExecutionCapabilities {
    bool supports_cancel {false};
    bool supports_replace {false};
    bool supports_market_orders {false};
};

// Defines the boundary used to submit and cancel orders on an exchange adapter.
class IExchangeExecutionAdapter {
public:
    virtual ~IExchangeExecutionAdapter() = default;

    // Returns capability flags for runtime routing and validation.
    [[nodiscard]] virtual ExchangeExecutionCapabilities capabilities() const = 0;

    // Submits one normalized internal order request into the exchange adapter.
    [[nodiscard]] virtual trading::execution::ExecutionResult submit(const trading::core::OrderRequest& request) = 0;

    // Requests cancellation of an existing order by internal order id.
    [[nodiscard]] virtual trading::execution::ExecutionResult cancel(const std::string& order_id,
                                                                     const std::string& client_order_id) = 0;
};

// Maps one exchange-specific reason string to a stable internal adapter error code.
[[nodiscard]] ExchangeAdapterErrorCode map_exchange_error(const std::string& exchange_reason);

// Wraps the deterministic simulator behind the execution adapter boundary.
class SimulatedExchangeExecutionAdapter final : public IExchangeExecutionAdapter {
public:
    explicit SimulatedExchangeExecutionAdapter(trading::execution::SimulatedExecutionConfig config)
        : engine_(config) {}

    [[nodiscard]] ExchangeExecutionCapabilities capabilities() const override;
    [[nodiscard]] trading::execution::ExecutionResult submit(const trading::core::OrderRequest& request) override;
    [[nodiscard]] trading::execution::ExecutionResult cancel(const std::string& order_id,
                                                             const std::string& client_order_id) override;

private:
    trading::execution::SimulatedExecutionEngine engine_;
};

}  // namespace trading::exchange
