#include "storage/replay_service.hpp"

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace trading::storage {

namespace {

// Splits a tab-delimited line into fields.
std::vector<std::string> split_tsv(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, '\t')) {
        fields.push_back(field);
    }
    return fields;
}

// Converts an optional number into a storage-safe string.
std::string encode_optional_double(const std::optional<double>& value) {
    return value.has_value() ? std::to_string(*value) : "";
}

// Parses an optional number from storage.
std::optional<double> decode_optional_double(const std::string& value) {
    if (value.empty()) {
        return std::nullopt;
    }

    return std::stod(value);
}

}  // namespace

ReplayService::ReplayService(std::filesystem::path log_path) : log_path_(std::move(log_path)) {}

// Removes any existing persisted event log.
void ReplayService::reset_log() const {
    std::filesystem::remove(log_path_);
}

// Appends one normalized runtime event to the event log.
void ReplayService::append_event(const trading::core::EngineEvent& event) const {
    if (const auto parent = log_path_.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream output(log_path_, std::ios::app);
    output << serialize_event(event) << '\n';
}

// Replays persisted events in file order.
ReplayStats ReplayService::replay(const std::function<void(const trading::core::EngineEvent&)>& on_event) const {
    ReplayStats stats;
    if (!std::filesystem::exists(log_path_)) {
        return stats;
    }

    std::ifstream input(log_path_);
    std::string line;
    while (std::getline(input, line)) {
        ++stats.total_records;
        if (const auto event = deserialize_event(line); event.has_value()) {
            on_event(*event);
            ++stats.replayed_records;
        } else {
            ++stats.skipped_records;
        }
    }

    return stats;
}

// Serializes one runtime event as a tab-delimited line.
std::string ReplayService::serialize_event(const trading::core::EngineEvent& event) const {
    return std::visit([](const auto& concrete_event) -> std::string {
        using EventType = std::decay_t<decltype(concrete_event)>;

        if constexpr (std::is_same_v<EventType, trading::core::MarketEvent>) {
            std::stringstream stream;
            stream
                << "market" << '\t'
                << concrete_event.event_id << '\t'
                << static_cast<int>(concrete_event.type) << '\t'
                << concrete_event.instrument.instrument_id << '\t'
                << concrete_event.instrument.exchange << '\t'
                << concrete_event.instrument.symbol << '\t'
                << concrete_event.instrument.base_asset << '\t'
                << concrete_event.instrument.quote_asset << '\t'
                << concrete_event.instrument.tick_size << '\t'
                << concrete_event.instrument.lot_size << '\t'
                << concrete_event.price << '\t'
                << concrete_event.quantity << '\t'
                << encode_optional_double(concrete_event.bid_price) << '\t'
                << encode_optional_double(concrete_event.ask_price) << '\t'
                << concrete_event.exchange_timestamp << '\t'
                << concrete_event.receive_timestamp << '\t'
                << concrete_event.process_timestamp;
            return stream.str();
        } else if constexpr (std::is_same_v<EventType, trading::core::TransactionCommand>) {
            std::stringstream stream;
            stream
                << "transaction" << '\t'
                << concrete_event.transaction_id << '\t'
                << concrete_event.user_id << '\t'
                << concrete_event.account_id << '\t'
                << concrete_event.command_type << '\t'
                << concrete_event.instrument_symbol << '\t'
                << concrete_event.quantity << '\t'
                << encode_optional_double(concrete_event.price) << '\t'
                << concrete_event.kafka_topic << '\t'
                << concrete_event.kafka_partition << '\t'
                << concrete_event.kafka_offset;
            return stream.str();
        } else {
            std::stringstream stream;
            stream
                << "timer" << '\t'
                << concrete_event.timer_id << '\t'
                << concrete_event.timestamp;
            return stream.str();
        }
    }, event);
}

// Deserializes one runtime event from a tab-delimited line.
std::optional<trading::core::EngineEvent> ReplayService::deserialize_event(const std::string& line) const {
    const auto fields = split_tsv(line);
    if (fields.empty()) {
        return std::nullopt;
    }

    try {
        if (fields[0] == "market") {
            if (fields.size() != 17) {
                return std::nullopt;
            }

            return trading::core::MarketEvent {
                .event_id = fields[1],
                .type = static_cast<trading::core::MarketEventType>(std::stoi(fields[2])),
                .instrument = {
                    .instrument_id = fields[3],
                    .exchange = fields[4],
                    .symbol = fields[5],
                    .base_asset = fields[6],
                    .quote_asset = fields[7],
                    .tick_size = std::stod(fields[8]),
                    .lot_size = std::stod(fields[9]),
                },
                .price = std::stod(fields[10]),
                .quantity = std::stod(fields[11]),
                .bid_price = decode_optional_double(fields[12]),
                .ask_price = decode_optional_double(fields[13]),
                .exchange_timestamp = std::stoll(fields[14]),
                .receive_timestamp = std::stoll(fields[15]),
                .process_timestamp = std::stoll(fields[16]),
            };
        }

        if (fields[0] == "transaction") {
            if (fields.size() != 11) {
                return std::nullopt;
            }

            return trading::core::TransactionCommand {
                .transaction_id = fields[1],
                .user_id = fields[2],
                .account_id = fields[3],
                .command_type = fields[4],
                .instrument_symbol = fields[5],
                .quantity = std::stod(fields[6]),
                .price = decode_optional_double(fields[7]),
                .kafka_topic = fields[8],
                .kafka_partition = std::stoi(fields[9]),
                .kafka_offset = std::stoll(fields[10]),
            };
        }

        if (fields[0] == "timer") {
            if (fields.size() != 3) {
                return std::nullopt;
            }

            return trading::core::TimerEvent {
                .timer_id = fields[1],
                .timestamp = std::stoll(fields[2]),
            };
        }
    } catch (...) {
        return std::nullopt;
    }

    return std::nullopt;
}

}  // namespace trading::storage
