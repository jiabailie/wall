#include "infrastructure/kafka_transaction_producer.hpp"

#include <librdkafka/rdkafka.h>

#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace trading::infrastructure {

namespace {

std::string join_brokers(const std::vector<std::string>& brokers) {
    std::stringstream stream;
    for (std::size_t index = 0; index < brokers.size(); ++index) {
        if (index > 0) {
            stream << ',';
        }
        stream << brokers[index];
    }
    return stream.str();
}

void set_conf(rd_kafka_conf_t* conf,
              const char* key,
              const std::string& value,
              char* error_buffer,
              const std::size_t error_buffer_size) {
    if (rd_kafka_conf_set(conf, key, value.c_str(), error_buffer, error_buffer_size) != RD_KAFKA_CONF_OK) {
        throw std::runtime_error("failed to configure Kafka producer setting " + std::string(key) + ": " + error_buffer);
    }
}

}  // namespace

std::string serialize_transaction_command_payload(const trading::core::TransactionCommand& command) {
    std::stringstream payload;
    payload
        << "transaction_id=" << command.transaction_id
        << ";user_id=" << command.user_id
        << ";account_id=" << command.account_id
        << ";command_type=" << command.command_type
        << ";instrument_symbol=" << command.instrument_symbol
        << ";quantity=" << command.quantity;
    if (command.price.has_value()) {
        payload << ";price=" << *command.price;
    }

    return payload.str();
}

class RdKafkaTransactionProducer::Impl {
public:
    explicit Impl(const trading::config::KafkaConfig& config)
        : topic_(config.transaction_topic) {
        char error_buffer[512];
        rd_kafka_conf_t* conf = rd_kafka_conf_new();

        set_conf(conf, "bootstrap.servers", join_brokers(config.brokers), error_buffer, sizeof(error_buffer));

        producer_ = rd_kafka_new(RD_KAFKA_PRODUCER, conf, error_buffer, sizeof(error_buffer));
        if (producer_ == nullptr) {
            rd_kafka_conf_destroy(conf);
            throw std::runtime_error("failed to create Kafka producer: " + std::string(error_buffer));
        }
    }

    ~Impl() {
        if (producer_ != nullptr) {
            rd_kafka_destroy(producer_);
        }
    }

    void publish(const trading::core::TransactionCommand& command) {
        const auto payload = serialize_transaction_command_payload(command);
        const auto result = rd_kafka_producev(
            producer_,
            RD_KAFKA_V_TOPIC(topic_.c_str()),
            RD_KAFKA_V_PARTITION(RD_KAFKA_PARTITION_UA),
            RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
            RD_KAFKA_V_VALUE(const_cast<char*>(payload.data()), payload.size()),
            RD_KAFKA_V_END);

        if (result != RD_KAFKA_RESP_ERR_NO_ERROR) {
            throw std::runtime_error("failed to publish Kafka transaction: " + std::string(rd_kafka_err2str(result)));
        }

        rd_kafka_poll(producer_, 0);
    }

    void flush(const int timeout_ms) {
        const auto result = rd_kafka_flush(producer_, timeout_ms);
        if (result != RD_KAFKA_RESP_ERR_NO_ERROR) {
            throw std::runtime_error("failed to flush Kafka producer: " + std::string(rd_kafka_err2str(result)));
        }
    }

private:
    rd_kafka_t* producer_ {nullptr};
    std::string topic_;
};

RdKafkaTransactionProducer::RdKafkaTransactionProducer(const trading::config::KafkaConfig& config)
    : impl_(new Impl(config)) {}

RdKafkaTransactionProducer::~RdKafkaTransactionProducer() {
    delete impl_;
}

void RdKafkaTransactionProducer::publish(const trading::core::TransactionCommand& command) {
    impl_->publish(command);
}

void RdKafkaTransactionProducer::flush(const int timeout_ms) {
    impl_->flush(timeout_ms);
}

}  // namespace trading::infrastructure
