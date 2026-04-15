#pragma once

#include "exchange/exchange_execution_adapter.hpp"
#include "exchange/exchange_market_data_adapter.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading::exchange {

// Routes raw market-data payloads to the correct exchange-specific adapter.
class ExchangeMarketDataRouter {
public:
    void register_adapter(const std::string& exchange_name, std::unique_ptr<IExchangeMarketDataAdapter> adapter);

    [[nodiscard]] std::variant<trading::core::MarketEvent, ExchangeAdapterError> normalize(
        const std::string& exchange_name,
        const std::string& raw_payload,
        std::int64_t receive_timestamp,
        std::int64_t process_timestamp) const;

    [[nodiscard]] std::vector<std::string> registered_exchanges() const;

private:
    std::unordered_map<std::string, std::unique_ptr<IExchangeMarketDataAdapter>> adapters_;
};

// Routes normalized order requests to the correct execution venue adapter.
class ExchangeExecutionRouter {
public:
    void register_adapter(const std::string& exchange_name, std::unique_ptr<IExchangeExecutionAdapter> adapter);

    [[nodiscard]] trading::execution::ExecutionResult submit(const trading::core::OrderRequest& request);
    [[nodiscard]] trading::execution::ExecutionResult cancel(const std::string& exchange_name,
                                                             const std::string& order_id,
                                                             const std::string& client_order_id);
    [[nodiscard]] std::optional<ExchangeExecutionCapabilities> capabilities(const std::string& exchange_name) const;
    [[nodiscard]] std::vector<std::string> registered_exchanges() const;

private:
    [[nodiscard]] trading::execution::ExecutionResult missing_exchange_result(const std::string& exchange_name,
                                                                              const std::string& client_order_id,
                                                                              std::optional<std::string> order_id = std::nullopt) const;

    std::unordered_map<std::string, std::unique_ptr<IExchangeExecutionAdapter>> adapters_;
};

}  // namespace trading::exchange
