#include "exchange/exchange_market_data_adapter.hpp"

#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace trading::exchange {

namespace {

// Parses one semicolon-delimited key-value payload into a lookup map.
std::optional<std::unordered_map<std::string, std::string>> parse_fields(const std::string& raw_payload) {
    std::unordered_map<std::string, std::string> fields;

    std::size_t segment_start = 0;
    while (segment_start < raw_payload.size()) {
        const auto segment_end = raw_payload.find(';', segment_start);
        const auto token_end = segment_end == std::string::npos ? raw_payload.size() : segment_end;
        const auto next_start = segment_end == std::string::npos ? raw_payload.size() : segment_end + 1;

        const auto token = raw_payload.substr(segment_start, token_end - segment_start);
        if (!token.empty()) {
            const auto equals_pos = token.find('=');
            if (equals_pos == std::string::npos || equals_pos == 0 || equals_pos == token.size() - 1) {
                return std::nullopt;
            }
            fields.emplace(token.substr(0, equals_pos), token.substr(equals_pos + 1));
        }

        segment_start = next_start;
    }

    return fields;
}

// Parses one required signed 64-bit integer field.
std::optional<std::int64_t> parse_int64(const std::unordered_map<std::string, std::string>& fields,
                                        const std::string& key) {
    const auto iterator = fields.find(key);
    if (iterator == fields.end()) {
        return std::nullopt;
    }

    try {
        return std::stoll(iterator->second);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Parses one optional floating-point field.
std::optional<double> parse_optional_double(const std::unordered_map<std::string, std::string>& fields,
                                            const std::string& key,
                                            bool& parse_error) {
    const auto iterator = fields.find(key);
    if (iterator == fields.end()) {
        return std::nullopt;
    }

    try {
        return std::stod(iterator->second);
    } catch (const std::exception&) {
        parse_error = true;
        return std::nullopt;
    }
}

}  // namespace

std::variant<trading::core::MarketEvent, ExchangeAdapterError> MockExchangeMarketDataAdapter::normalize(
    const std::string& raw_payload,
    const std::int64_t receive_timestamp,
    const std::int64_t process_timestamp) const {
    const auto parsed_fields = parse_fields(raw_payload);
    if (!parsed_fields.has_value()) {
        return ExchangeAdapterError {
            .code = ExchangeAdapterErrorCode::malformed_payload,
            .message = "malformed market payload",
            .retryable = false,
        };
    }

    const auto& fields = *parsed_fields;
    const auto type_iterator = fields.find("type");
    const auto event_id_iterator = fields.find("event_id");
    const auto symbol_iterator = fields.find("symbol");

    if (type_iterator == fields.end() || event_id_iterator == fields.end() || symbol_iterator == fields.end()) {
        return ExchangeAdapterError {
            .code = ExchangeAdapterErrorCode::missing_required_field,
            .message = "missing required market payload field",
            .retryable = false,
        };
    }

    if (symbol_iterator->second != instrument_.symbol) {
        return ExchangeAdapterError {
            .code = ExchangeAdapterErrorCode::symbol_mismatch,
            .message = "symbol does not match configured instrument",
            .retryable = false,
        };
    }

    auto event_type = trading::core::MarketEventType::trade;
    if (type_iterator->second == "trade") {
        event_type = trading::core::MarketEventType::trade;
    } else if (type_iterator->second == "ticker") {
        event_type = trading::core::MarketEventType::ticker;
    } else if (type_iterator->second == "book_snapshot") {
        event_type = trading::core::MarketEventType::book_snapshot;
    } else {
        return ExchangeAdapterError {
            .code = ExchangeAdapterErrorCode::unsupported_message_type,
            .message = "unsupported market message type",
            .retryable = false,
        };
    }

    const auto exchange_timestamp = parse_int64(fields, "exchange_ts");
    if (!exchange_timestamp.has_value()) {
        return ExchangeAdapterError {
            .code = ExchangeAdapterErrorCode::missing_required_field,
            .message = "missing required exchange_ts field",
            .retryable = false,
        };
    }

    bool parse_error = false;
    const auto price = parse_optional_double(fields, "price", parse_error);
    const auto quantity = parse_optional_double(fields, "quantity", parse_error);
    const auto bid = parse_optional_double(fields, "bid", parse_error);
    const auto ask = parse_optional_double(fields, "ask", parse_error);
    if (parse_error) {
        return ExchangeAdapterError {
            .code = ExchangeAdapterErrorCode::invalid_numeric_field,
            .message = "invalid numeric market payload field",
            .retryable = false,
        };
    }

    if (event_type == trading::core::MarketEventType::trade && (!price.has_value() || !quantity.has_value())) {
        return ExchangeAdapterError {
            .code = ExchangeAdapterErrorCode::missing_required_field,
            .message = "trade payload requires price and quantity",
            .retryable = false,
        };
    }

    return trading::core::MarketEvent {
        .event_id = event_id_iterator->second,
        .type = event_type,
        .instrument = instrument_,
        .price = price.value_or(0.0),
        .quantity = quantity.value_or(0.0),
        .bid_price = bid,
        .ask_price = ask,
        .exchange_timestamp = *exchange_timestamp,
        .receive_timestamp = receive_timestamp,
        .process_timestamp = process_timestamp,
    };
}

}  // namespace trading::exchange
