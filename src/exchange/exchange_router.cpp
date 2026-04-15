#include "exchange/exchange_router.hpp"

#include <algorithm>
#include <utility>

namespace trading::exchange {

void ExchangeMarketDataRouter::register_adapter(const std::string& exchange_name,
                                                std::unique_ptr<IExchangeMarketDataAdapter> adapter) {
    if (!exchange_name.empty() && adapter != nullptr) {
        adapters_[exchange_name] = std::move(adapter);
    }
}

std::variant<trading::core::MarketEvent, ExchangeAdapterError> ExchangeMarketDataRouter::normalize(
    const std::string& exchange_name,
    const std::string& raw_payload,
    const std::int64_t receive_timestamp,
    const std::int64_t process_timestamp) const {
    const auto iterator = adapters_.find(exchange_name);
    if (iterator == adapters_.end()) {
        return ExchangeAdapterError {
            .code = ExchangeAdapterErrorCode::unknown,
            .message = "no market-data adapter registered for exchange",
            .retryable = false,
        };
    }

    return iterator->second->normalize(raw_payload, receive_timestamp, process_timestamp);
}

std::vector<std::string> ExchangeMarketDataRouter::registered_exchanges() const {
    std::vector<std::string> exchanges;
    exchanges.reserve(adapters_.size());
    for (const auto& [exchange_name, _] : adapters_) {
        exchanges.push_back(exchange_name);
    }
    std::sort(exchanges.begin(), exchanges.end());
    return exchanges;
}

void ExchangeExecutionRouter::register_adapter(const std::string& exchange_name,
                                               std::unique_ptr<IExchangeExecutionAdapter> adapter) {
    if (!exchange_name.empty() && adapter != nullptr) {
        adapters_[exchange_name] = std::move(adapter);
    }
}

trading::execution::ExecutionResult ExchangeExecutionRouter::submit(const trading::core::OrderRequest& request) {
    const auto iterator = adapters_.find(request.instrument.exchange);
    if (iterator == adapters_.end()) {
        return missing_exchange_result(request.instrument.exchange, "", request.request_id);
    }

    return iterator->second->submit(request);
}

trading::execution::ExecutionResult ExchangeExecutionRouter::cancel(const std::string& exchange_name,
                                                                    const std::string& order_id,
                                                                    const std::string& client_order_id) {
    const auto iterator = adapters_.find(exchange_name);
    if (iterator == adapters_.end()) {
        return missing_exchange_result(exchange_name, client_order_id, order_id);
    }

    return iterator->second->cancel(order_id, client_order_id);
}

std::optional<ExchangeExecutionCapabilities> ExchangeExecutionRouter::capabilities(const std::string& exchange_name) const {
    const auto iterator = adapters_.find(exchange_name);
    if (iterator == adapters_.end()) {
        return std::nullopt;
    }

    return iterator->second->capabilities();
}

std::vector<std::string> ExchangeExecutionRouter::registered_exchanges() const {
    std::vector<std::string> exchanges;
    exchanges.reserve(adapters_.size());
    for (const auto& [exchange_name, _] : adapters_) {
        exchanges.push_back(exchange_name);
    }
    std::sort(exchanges.begin(), exchanges.end());
    return exchanges;
}

trading::execution::ExecutionResult ExchangeExecutionRouter::missing_exchange_result(
    const std::string& exchange_name,
    const std::string& client_order_id,
    std::optional<std::string> order_id) const {
    return trading::execution::ExecutionResult {
        .order_id = order_id.value_or(""),
        .client_order_id = client_order_id,
        .updates = {
            trading::core::OrderUpdate {
                .order_id = order_id.value_or(""),
                .client_order_id = client_order_id,
                .status = trading::core::OrderStatus::rejected,
                .filled_quantity = 0.0,
                .reason = "no execution adapter registered for exchange " + exchange_name,
            },
        },
        .fills = {},
    };
}

}  // namespace trading::exchange
