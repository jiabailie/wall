#include "infrastructure/kafka_transaction_consumer.hpp"

#include <sstream>
#include <string>
#include <unordered_map>

namespace trading::infrastructure {

namespace {

// Splits a payload string into key/value tokens separated by ';'.
std::unordered_map<std::string, std::string> parse_key_value_payload(const std::string& payload) {
    std::unordered_map<std::string, std::string> fields;
    std::stringstream payload_stream(payload);
    std::string token;

    while (std::getline(payload_stream, token, ';')) {
        if (token.empty()) {
            continue;
        }

        const auto separator = token.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const auto key = token.substr(0, separator);
        const auto value = token.substr(separator + 1);
        fields[key] = value;
    }

    return fields;
}

}  // namespace

// Returns the next valid transaction command, skipping malformed messages safely.
std::optional<trading::core::TransactionCommand> KafkaTransactionConsumer::poll() {
    // Keep polling until a valid message is found or source is exhausted.
    while (true) {
        const auto raw_message = client_.poll();
        if (!raw_message.has_value()) {
            return std::nullopt;
        }

        if (raw_message->topic != transaction_topic_) {
            // Commit unrelated topic messages to avoid re-reading them forever.
            client_.commit(raw_message->topic, raw_message->partition, raw_message->offset);
            continue;
        }

        if (const auto parsed = parse_raw_message(*raw_message); parsed.has_value()) {
            return parsed;
        }

        // Commit malformed payloads as rejected so consumption can continue safely.
        client_.commit(raw_message->topic, raw_message->partition, raw_message->offset);
    }
}

// Commits the consumed offset for the provided transaction command.
void KafkaTransactionConsumer::commit(const trading::core::TransactionCommand& command) {
    client_.commit(command.kafka_topic, command.kafka_partition, command.kafka_offset);
}

// Converts one raw Kafka payload into a normalized transaction command.
std::optional<trading::core::TransactionCommand> KafkaTransactionConsumer::parse_raw_message(
    const RawKafkaMessage& message) const {
    const auto fields = parse_key_value_payload(message.payload);

    const auto transaction_id = fields.find("transaction_id");
    const auto user_id = fields.find("user_id");
    const auto account_id = fields.find("account_id");
    const auto command_type = fields.find("command_type");
    const auto instrument_symbol = fields.find("instrument_symbol");
    const auto quantity = fields.find("quantity");
    if (transaction_id == fields.end() ||
        user_id == fields.end() ||
        account_id == fields.end() ||
        command_type == fields.end() ||
        instrument_symbol == fields.end() ||
        quantity == fields.end()) {
        return std::nullopt;
    }

    try {
        trading::core::TransactionCommand command {
            .transaction_id = transaction_id->second,
            .user_id = user_id->second,
            .account_id = account_id->second,
            .command_type = command_type->second,
            .instrument_symbol = instrument_symbol->second,
            .quantity = std::stod(quantity->second),
            .price = std::nullopt,
            .kafka_topic = message.topic,
            .kafka_partition = message.partition,
            .kafka_offset = message.offset,
        };

        if (const auto price = fields.find("price"); price != fields.end() && !price->second.empty()) {
            command.price = std::stod(price->second);
        }

        return command;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace trading::infrastructure
