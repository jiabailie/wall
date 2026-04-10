#include "exchange/exchange_execution_adapter.hpp"

#include <algorithm>
#include <cctype>

namespace trading::exchange {

namespace {

// Returns a lower-cased copy used for simple substring error mapping.
std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

}  // namespace

ExchangeAdapterErrorCode map_exchange_error(const std::string& exchange_reason) {
    const auto normalized = to_lower_copy(exchange_reason);

    if (normalized.find("unsupported") != std::string::npos
        || normalized.find("invalid type") != std::string::npos) {
        return ExchangeAdapterErrorCode::unsupported_message_type;
    }
    if (normalized.find("insufficient") != std::string::npos
        || normalized.find("rejected") != std::string::npos
        || normalized.find("risk") != std::string::npos) {
        return ExchangeAdapterErrorCode::rejected_by_exchange;
    }
    if (normalized.find("timeout") != std::string::npos
        || normalized.find("connection") != std::string::npos
        || normalized.find("unavailable") != std::string::npos) {
        return ExchangeAdapterErrorCode::transport_failure;
    }

    return ExchangeAdapterErrorCode::unknown;
}

ExchangeExecutionCapabilities SimulatedExchangeExecutionAdapter::capabilities() const {
    return ExchangeExecutionCapabilities {
        .supports_cancel = true,
        .supports_replace = false,
        .supports_market_orders = false,
    };
}

trading::execution::ExecutionResult SimulatedExchangeExecutionAdapter::submit(const trading::core::OrderRequest& request) {
    return engine_.submit(request);
}

trading::execution::ExecutionResult SimulatedExchangeExecutionAdapter::cancel(const std::string& order_id,
                                                                              const std::string& client_order_id) {
    if (order_id.empty()) {
        return trading::execution::ExecutionResult {
            .order_id = order_id,
            .client_order_id = client_order_id,
            .updates = {
                trading::core::OrderUpdate {
                    .order_id = order_id,
                    .client_order_id = client_order_id,
                    .status = trading::core::OrderStatus::rejected,
                    .filled_quantity = 0.0,
                    .reason = "missing order_id for cancel",
                },
            },
            .fills = {},
        };
    }

    return engine_.cancel_open_order(order_id);
}

}  // namespace trading::exchange
