#include "app/transaction_publisher_application.hpp"

#include "config/config_loader.hpp"
#include "infrastructure/kafka_transaction_producer.hpp"

#include <cctype>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace trading::app {

namespace {

std::string trim(const std::string& value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }

    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::optional<std::unordered_map<std::string, std::string>> parse_flat_json_object(const std::string& line) {
    const auto json = trim(line);
    if (json.empty()) {
        return std::nullopt;
    }
    if (json.front() != '{' || json.back() != '}') {
        return std::nullopt;
    }

    std::unordered_map<std::string, std::string> fields;
    std::size_t cursor = 1;
    while (cursor + 1 < json.size()) {
        while (cursor < json.size() && (std::isspace(static_cast<unsigned char>(json[cursor])) || json[cursor] == ',')) {
            ++cursor;
        }
        if (cursor >= json.size() - 1) {
            break;
        }
        if (json[cursor] != '"') {
            return std::nullopt;
        }

        const auto key_end = json.find('"', cursor + 1);
        if (key_end == std::string::npos) {
            return std::nullopt;
        }
        const auto key = json.substr(cursor + 1, key_end - cursor - 1);
        cursor = key_end + 1;

        while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor]))) {
            ++cursor;
        }
        if (cursor >= json.size() || json[cursor] != ':') {
            return std::nullopt;
        }
        ++cursor;
        while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor]))) {
            ++cursor;
        }
        if (cursor >= json.size()) {
            return std::nullopt;
        }

        std::string value;
        if (json[cursor] == '"') {
            const auto value_end = json.find('"', cursor + 1);
            if (value_end == std::string::npos) {
                return std::nullopt;
            }
            value = json.substr(cursor + 1, value_end - cursor - 1);
            cursor = value_end + 1;
        } else {
            const auto value_end = json.find_first_of(",}", cursor);
            if (value_end == std::string::npos) {
                return std::nullopt;
            }
            value = trim(json.substr(cursor, value_end - cursor));
            cursor = value_end;
        }

        fields.emplace(key, value);
    }

    return fields;
}

}  // namespace

std::optional<trading::core::TransactionCommand> parse_json_transaction_command_line(const std::string& line) {
    const auto fields = parse_flat_json_object(line);
    if (!fields.has_value()) {
        return std::nullopt;
    }

    const auto transaction_id = fields->find("transaction_id");
    const auto user_id = fields->find("user_id");
    const auto account_id = fields->find("account_id");
    const auto command_type = fields->find("command_type");
    const auto instrument_symbol = fields->find("instrument_symbol");
    const auto quantity = fields->find("quantity");
    if (transaction_id == fields->end() ||
        user_id == fields->end() ||
        account_id == fields->end() ||
        command_type == fields->end() ||
        instrument_symbol == fields->end() ||
        quantity == fields->end()) {
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
        };

        if (const auto price = fields->find("price"); price != fields->end() && !price->second.empty() && price->second != "null") {
            command.price = std::stod(price->second);
        }

        return command;
    } catch (...) {
        return std::nullopt;
    }
}

trading::config::AppConfig TransactionPublisherApplication::load_config() const {
    trading::config::ConfigLoader loader;
    const auto config = loader.load_from_file(std::string(TRADING_SOURCE_DIR) + "/configs/development.cfg");
    const auto validation = loader.validate(config);
    if (!validation.valid) {
        throw std::runtime_error(*validation.error);
    }

    return config;
}

void TransactionPublisherApplication::run(const int argc, char** argv) const {
    const auto config = load_config();
    trading::infrastructure::RdKafkaTransactionProducer producer(config.kafka);

    std::ifstream input_file;
    std::istream* input = &std::cin;
    std::string input_name = "stdin";
    if (argc > 1) {
        input_file.open(argv[1]);
        if (!input_file.is_open()) {
            throw std::runtime_error("Unable to open transaction input file: " + std::string(argv[1]));
        }
        input = &input_file;
        input_name = argv[1];
    }

    std::size_t published_count = 0;
    std::size_t skipped_count = 0;
    std::string line;
    while (std::getline(*input, line)) {
        const auto trimmed = trim(line);
        if (trimmed.empty()) {
            continue;
        }

        const auto command = parse_json_transaction_command_line(trimmed);
        if (!command.has_value()) {
            ++skipped_count;
            continue;
        }

        producer.publish(*command);
        ++published_count;
    }

    producer.flush();

    std::cout
        << "Transaction publishing complete"
        << " input=" << input_name
        << " topic=" << config.kafka.transaction_topic
        << " published=" << published_count
        << " skipped=" << skipped_count
        << '\n';
}

}  // namespace trading::app
