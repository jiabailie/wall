#pragma once

#include "core/types.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace trading::exchange {

// Declares normalized adapter error categories shared by exchange boundaries.
enum class ExchangeAdapterErrorCode {
    malformed_payload,
    missing_required_field,
    invalid_numeric_field,
    unsupported_message_type,
    symbol_mismatch,
    rejected_by_exchange,
    transport_failure,
    unknown,
};

// Stores one normalized adapter error payload.
struct ExchangeAdapterError {
    ExchangeAdapterErrorCode code {ExchangeAdapterErrorCode::unknown};
    std::string message;
    bool retryable {false};
};

// Defines the boundary used to normalize raw exchange market messages.
class IExchangeMarketDataAdapter {
public:
    virtual ~IExchangeMarketDataAdapter() = default;

    // Converts a raw exchange payload into a normalized internal market event.
    [[nodiscard]] virtual std::variant<trading::core::MarketEvent, ExchangeAdapterError> normalize(
        const std::string& raw_payload,
        std::int64_t receive_timestamp,
        std::int64_t process_timestamp) const = 0;
};

// Provides a deterministic key-value parser adapter for development and tests.
class MockExchangeMarketDataAdapter final : public IExchangeMarketDataAdapter {
public:
    explicit MockExchangeMarketDataAdapter(trading::core::Instrument instrument)
        : instrument_(std::move(instrument)) {}

    [[nodiscard]] std::variant<trading::core::MarketEvent, ExchangeAdapterError> normalize(
        const std::string& raw_payload,
        std::int64_t receive_timestamp,
        std::int64_t process_timestamp) const override;

private:
    trading::core::Instrument instrument_;
};

}  // namespace trading::exchange
