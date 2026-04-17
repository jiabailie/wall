#pragma once

#include "config/app_config.hpp"
#include "core/types.hpp"

#include <cstdint>
#include <string>

namespace trading::infrastructure {

// Serializes one transaction command into the key-value payload consumed by KafkaTransactionConsumer.
[[nodiscard]] std::string serialize_transaction_command_payload(const trading::core::TransactionCommand& command);

// Builds the Kafka message key as transaction_id|yyyyMMddHHmmSS using a UTC timestamp.
[[nodiscard]] std::string serialize_transaction_command_key(const trading::core::TransactionCommand& command,
                                                            std::int64_t timestamp_ms);

// librdkafka-backed producer for outbound transaction commands.
class RdKafkaTransactionProducer {
public:
    explicit RdKafkaTransactionProducer(const trading::config::KafkaConfig& config);
    ~RdKafkaTransactionProducer();

    // Publishes one transaction command to the configured Kafka topic.
    void publish(const trading::core::TransactionCommand& command);

    // Flushes any buffered records before shutdown.
    void flush(int timeout_ms = 5000);

private:
    class Impl;
    Impl* impl_ {nullptr};
};

}  // namespace trading::infrastructure
