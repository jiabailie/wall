#include "infrastructure/kafka_transaction_consumer.hpp"

#include <librdkafka/rdkafka.h>

#include <memory>
#include <stdexcept>
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

class RdKafkaConsumerHandle {
public:
    explicit RdKafkaConsumerHandle(const trading::config::KafkaConfig& config) {
        char error_buffer[512];
        rd_kafka_conf_t* conf = rd_kafka_conf_new();

        const auto brokers = join_brokers(config.brokers);
        set_conf(conf, "bootstrap.servers", brokers, error_buffer, sizeof(error_buffer));
        set_conf(conf, "group.id", config.consumer_group, error_buffer, sizeof(error_buffer));
        set_conf(conf, "enable.auto.commit", "false", error_buffer, sizeof(error_buffer));
        set_conf(conf, "enable.auto.offset.store", "false", error_buffer, sizeof(error_buffer));
        set_conf(conf, "auto.offset.reset", "earliest", error_buffer, sizeof(error_buffer));

        consumer_ = rd_kafka_new(RD_KAFKA_CONSUMER, conf, error_buffer, sizeof(error_buffer));
        if (consumer_ == nullptr) {
            rd_kafka_conf_destroy(conf);
            throw std::runtime_error("failed to create Kafka consumer: " + std::string(error_buffer));
        }

        rd_kafka_poll_set_consumer(consumer_);

        rd_kafka_topic_partition_list_t* topics = rd_kafka_topic_partition_list_new(1);
        rd_kafka_topic_partition_list_add(topics, config.transaction_topic.c_str(), RD_KAFKA_PARTITION_UA);
        const auto subscribe_error = rd_kafka_subscribe(consumer_, topics);
        rd_kafka_topic_partition_list_destroy(topics);
        if (subscribe_error != RD_KAFKA_RESP_ERR_NO_ERROR) {
            const std::string message = rd_kafka_err2str(subscribe_error);
            rd_kafka_destroy(consumer_);
            consumer_ = nullptr;
            throw std::runtime_error("failed to subscribe Kafka consumer: " + message);
        }
    }

    ~RdKafkaConsumerHandle() {
        if (consumer_ != nullptr) {
            rd_kafka_consumer_close(consumer_);
            rd_kafka_destroy(consumer_);
        }
    }

    RdKafkaConsumerHandle(const RdKafkaConsumerHandle&) = delete;
    RdKafkaConsumerHandle& operator=(const RdKafkaConsumerHandle&) = delete;

    [[nodiscard]] rd_kafka_t* consumer() const { return consumer_; }

private:
    static std::string join_brokers(const std::vector<std::string>& brokers) {
        std::stringstream stream;
        for (std::size_t index = 0; index < brokers.size(); ++index) {
            if (index > 0) {
                stream << ',';
            }
            stream << brokers[index];
        }
        return stream.str();
    }

    static void set_conf(rd_kafka_conf_t* conf,
                         const char* key,
                         const std::string& value,
                         char* error_buffer,
                         const std::size_t error_buffer_size) {
        if (rd_kafka_conf_set(conf, key, value.c_str(), error_buffer, error_buffer_size) != RD_KAFKA_CONF_OK) {
            throw std::runtime_error("failed to configure Kafka consumer setting " + std::string(key) + ": " + error_buffer);
        }
    }

    rd_kafka_t* consumer_ {nullptr};
};

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

RdKafkaConsumerClient::RdKafkaConsumerClient(const trading::config::KafkaConfig& config, const int poll_timeout_ms)
    : handle_(std::make_shared<RdKafkaConsumerHandle>(config)),
      transaction_topic_(config.transaction_topic),
      poll_timeout_ms_(poll_timeout_ms) {}

RdKafkaConsumerClient::~RdKafkaConsumerClient() = default;

std::optional<RawKafkaMessage> RdKafkaConsumerClient::poll() {
    while (true) {
        rd_kafka_message_t* message = rd_kafka_consumer_poll(handle_->consumer(), poll_timeout_ms_);
        if (message == nullptr) {
            return std::nullopt;
        }

        std::unique_ptr<rd_kafka_message_t, decltype(&rd_kafka_message_destroy)> guard(message, &rd_kafka_message_destroy);
        if (message->err != RD_KAFKA_RESP_ERR_NO_ERROR) {
            if (message->err == RD_KAFKA_RESP_ERR__PARTITION_EOF) {
                continue;
            }

            throw std::runtime_error("Kafka consumer poll failed: " + std::string(rd_kafka_message_errstr(message)));
        }

        const char* topic_name = rd_kafka_topic_name(message->rkt);
        if (topic_name == nullptr) {
            continue;
        }

        return RawKafkaMessage {
            .topic = topic_name,
            .partition = static_cast<int>(message->partition),
            .offset = static_cast<std::int64_t>(message->offset),
            .payload = std::string(
                static_cast<const char*>(message->payload),
                static_cast<std::size_t>(message->len)),
        };
    }
}

void RdKafkaConsumerClient::commit(const std::string& topic, const int partition, const std::int64_t offset) {
    rd_kafka_topic_partition_list_t* offsets = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_t* entry = rd_kafka_topic_partition_list_add(offsets, topic.c_str(), partition);
    entry->offset = static_cast<int64_t>(offset + 1);

    const auto error = rd_kafka_commit(handle_->consumer(), offsets, 0);
    rd_kafka_topic_partition_list_destroy(offsets);
    if (error != RD_KAFKA_RESP_ERR_NO_ERROR) {
        throw std::runtime_error("Kafka commit failed: " + std::string(rd_kafka_err2str(error)));
    }
}

void RdKafkaConsumerClient::seek(const std::string& topic, const int partition, const std::int64_t next_offset) {
    rd_kafka_topic_partition_list_t* offsets = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_t* entry = rd_kafka_topic_partition_list_add(offsets, topic.c_str(), partition);
    entry->offset = static_cast<int64_t>(next_offset);

    const auto assign_error = rd_kafka_assign(handle_->consumer(), offsets);
    if (assign_error != RD_KAFKA_RESP_ERR_NO_ERROR) {
        rd_kafka_topic_partition_list_destroy(offsets);
        throw std::runtime_error("Kafka assign failed during recovery: " + std::string(rd_kafka_err2str(assign_error)));
    }

    rd_kafka_error_t* seek_error = rd_kafka_seek_partitions(handle_->consumer(), offsets, 5000);
    rd_kafka_topic_partition_list_destroy(offsets);
    if (seek_error != nullptr) {
        const std::string message = rd_kafka_error_string(seek_error);
        rd_kafka_error_destroy(seek_error);
        throw std::runtime_error("Kafka seek failed during recovery: " + message);
    }
}

}  // namespace trading::infrastructure
