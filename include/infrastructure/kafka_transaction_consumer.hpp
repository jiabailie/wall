#pragma once

#include "ingestion/transaction_consumer.hpp"

#include <optional>
#include <string>
#include <vector>

namespace trading::infrastructure {

// Stores one raw Kafka message as consumed by the low-level client.
struct RawKafkaMessage {
    std::string topic;
    int partition {0};
    std::int64_t offset {0};
    std::string payload;
};

// Defines the low-level Kafka client boundary used by the higher-level consumer adapter.
class IKafkaConsumerClient {
public:
    virtual ~IKafkaConsumerClient() = default;

    // Polls one raw Kafka message from the subscribed topics, if available.
    virtual std::optional<RawKafkaMessage> poll() = 0;

    // Commits the provided topic/partition/offset as processed.
    virtual void commit(const std::string& topic, int partition, std::int64_t offset) = 0;
};

// Adapts raw Kafka messages into TransactionCommand values used by the runtime.
class KafkaTransactionConsumer final : public trading::ingestion::ITransactionConsumer {
public:
    KafkaTransactionConsumer(IKafkaConsumerClient& client, std::string transaction_topic)
        : client_(client), transaction_topic_(std::move(transaction_topic)) {}

    // Returns the next valid transaction command, skipping malformed messages safely.
    std::optional<trading::core::TransactionCommand> poll() override;

    // Commits the consumed offset for the provided transaction command.
    void commit(const trading::core::TransactionCommand& command) override;

private:
    [[nodiscard]] std::optional<trading::core::TransactionCommand> parse_raw_message(
        const RawKafkaMessage& message) const;

    IKafkaConsumerClient& client_;
    std::string transaction_topic_;
};

}  // namespace trading::infrastructure
