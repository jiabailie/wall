#include "infrastructure/postgres_repositories.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace trading::infrastructure {

namespace {

// Splits one TSV line into fields.
std::vector<std::string> split_tsv(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, '\t')) {
        fields.push_back(field);
    }
    return fields;
}

// Creates directories required to persist one file.
void ensure_parent_directory(const std::filesystem::path& file_path) {
    const auto parent_path = file_path.parent_path();
    if (!parent_path.empty()) {
        std::filesystem::create_directories(parent_path);
    }
}

}  // namespace

PostgresTransactionRepository::PostgresTransactionRepository(std::filesystem::path root_directory)
    : file_path_(std::move(root_directory) / "transactions.tsv") {
    load();
}

// Persists the received transaction checkpoint.
void PostgresTransactionRepository::save_received(const trading::core::TransactionCommand& command) {
    records_[command.transaction_id] = {
        .transaction_id = command.transaction_id,
        .status = "received",
        .kafka_topic = command.kafka_topic,
        .kafka_partition = command.kafka_partition,
        .kafka_offset = command.kafka_offset,
    };
    flush();
}

// Persists the processed transaction checkpoint.
void PostgresTransactionRepository::save_processed(const std::string& transaction_id, const std::string& status) {
    if (auto iterator = records_.find(transaction_id); iterator != records_.end()) {
        iterator->second.status = status;
    } else {
        records_[transaction_id] = {
            .transaction_id = transaction_id,
            .status = status,
            .kafka_topic = "",
            .kafka_partition = 0,
            .kafka_offset = 0,
        };
    }
    flush();
}

// Returns one transaction record by id.
std::optional<trading::storage::TransactionRecord> PostgresTransactionRepository::get_record(const std::string& transaction_id) const {
    if (const auto iterator = records_.find(transaction_id); iterator != records_.end()) {
        return iterator->second;
    }

    return std::nullopt;
}

// Loads transaction records from file into memory.
void PostgresTransactionRepository::load() {
    records_.clear();
    if (!std::filesystem::exists(file_path_)) {
        return;
    }

    std::ifstream input(file_path_);
    std::string line;
    while (std::getline(input, line)) {
        const auto fields = split_tsv(line);
        if (fields.size() != 5) {
            continue;
        }

        records_[fields[0]] = {
            .transaction_id = fields[0],
            .status = fields[1],
            .kafka_topic = fields[2],
            .kafka_partition = std::stoi(fields[3]),
            .kafka_offset = std::stoll(fields[4]),
        };
    }
}

// Flushes transaction records into deterministic TSV output.
void PostgresTransactionRepository::flush() const {
    ensure_parent_directory(file_path_);

    std::vector<std::string> keys;
    keys.reserve(records_.size());
    for (const auto& [transaction_id, _] : records_) {
        keys.push_back(transaction_id);
    }
    std::sort(keys.begin(), keys.end());

    std::ofstream output(file_path_, std::ios::trunc);
    for (const auto& key : keys) {
        const auto& record = records_.at(key);
        output
            << record.transaction_id << '\t'
            << record.status << '\t'
            << record.kafka_topic << '\t'
            << record.kafka_partition << '\t'
            << record.kafka_offset << '\n';
    }
}

PostgresOrderRepository::PostgresOrderRepository(std::filesystem::path root_directory)
    : file_path_(std::move(root_directory) / "orders.tsv") {
    load();
}

// Persists a newly created order.
void PostgresOrderRepository::save_order(const trading::core::OrderRequest& request,
                                         const std::string& order_id,
                                         const std::string& client_order_id) {
    orders_[order_id] = {
        .order_id = order_id,
        .client_order_id = client_order_id,
        .strategy_id = request.strategy_id,
        .instrument_id = request.instrument.instrument_id,
        .side = request.side,
        .order_type = request.type,
        .status = trading::core::OrderStatus::created,
        .quantity = request.quantity,
        .price = request.price,
        .filled_quantity = 0.0,
    };
    flush();
}

// Persists one order lifecycle update.
void PostgresOrderRepository::save_order_update(const trading::core::OrderUpdate& update) {
    if (auto iterator = orders_.find(update.order_id); iterator != orders_.end()) {
        iterator->second.client_order_id = update.client_order_id;
        iterator->second.status = update.status;
        iterator->second.filled_quantity = update.filled_quantity;
    } else {
        orders_[update.order_id] = {
            .order_id = update.order_id,
            .client_order_id = update.client_order_id,
            .status = update.status,
            .filled_quantity = update.filled_quantity,
        };
    }
    flush();
}

// Returns one order record by id.
std::optional<trading::storage::OrderRecord> PostgresOrderRepository::get_order(const std::string& order_id) const {
    if (const auto iterator = orders_.find(order_id); iterator != orders_.end()) {
        return iterator->second;
    }

    return std::nullopt;
}

// Loads order records from file.
void PostgresOrderRepository::load() {
    orders_.clear();
    if (!std::filesystem::exists(file_path_)) {
        return;
    }

    std::ifstream input(file_path_);
    std::string line;
    while (std::getline(input, line)) {
        const auto fields = split_tsv(line);
        if (fields.size() != 10) {
            continue;
        }

        trading::storage::OrderRecord record {
            .order_id = fields[0],
            .client_order_id = fields[1],
            .strategy_id = fields[2],
            .instrument_id = fields[3],
            .side = static_cast<trading::core::OrderSide>(std::stoi(fields[4])),
            .order_type = static_cast<trading::core::OrderType>(std::stoi(fields[5])),
            .status = static_cast<trading::core::OrderStatus>(std::stoi(fields[6])),
            .quantity = std::stod(fields[7]),
            .price = fields[8].empty() ? std::nullopt : std::optional<double>(std::stod(fields[8])),
            .filled_quantity = std::stod(fields[9]),
        };
        orders_[record.order_id] = record;
    }
}

// Flushes order records into deterministic TSV output.
void PostgresOrderRepository::flush() const {
    ensure_parent_directory(file_path_);

    std::vector<std::string> keys;
    keys.reserve(orders_.size());
    for (const auto& [order_id, _] : orders_) {
        keys.push_back(order_id);
    }
    std::sort(keys.begin(), keys.end());

    std::ofstream output(file_path_, std::ios::trunc);
    for (const auto& key : keys) {
        const auto& record = orders_.at(key);
        output
            << record.order_id << '\t'
            << record.client_order_id << '\t'
            << record.strategy_id << '\t'
            << record.instrument_id << '\t'
            << static_cast<int>(record.side) << '\t'
            << static_cast<int>(record.order_type) << '\t'
            << static_cast<int>(record.status) << '\t'
            << record.quantity << '\t'
            << (record.price.has_value() ? std::to_string(*record.price) : "") << '\t'
            << record.filled_quantity << '\n';
    }
}

PostgresFillRepository::PostgresFillRepository(std::filesystem::path root_directory)
    : file_path_(std::move(root_directory) / "fills.tsv") {
    load();
}

// Persists one fill event.
void PostgresFillRepository::append_fill(const trading::core::FillEvent& fill) {
    fills_.push_back(fill);
    flush();
}

// Returns all fill records in insertion order.
std::vector<trading::core::FillEvent> PostgresFillRepository::all_fills() const {
    return fills_;
}

// Loads fill records from file.
void PostgresFillRepository::load() {
    fills_.clear();
    if (!std::filesystem::exists(file_path_)) {
        return;
    }

    std::ifstream input(file_path_);
    std::string line;
    while (std::getline(input, line)) {
        const auto fields = split_tsv(line);
        if (fields.size() != 13) {
            continue;
        }

        fills_.push_back({
            .fill_id = fields[0],
            .order_id = fields[1],
            .instrument = {
                .instrument_id = fields[2],
                .exchange = fields[3],
                .symbol = fields[4],
                .base_asset = fields[5],
                .quote_asset = fields[6],
                .tick_size = std::stod(fields[7]),
                .lot_size = std::stod(fields[8]),
            },
            .side = static_cast<trading::core::OrderSide>(std::stoi(fields[9])),
            .price = std::stod(fields[10]),
            .quantity = std::stod(fields[11]),
            .fee = std::stod(fields[12]),
        });
    }
}

// Flushes fill records in insertion order.
void PostgresFillRepository::flush() const {
    ensure_parent_directory(file_path_);

    std::ofstream output(file_path_, std::ios::trunc);
    for (const auto& fill : fills_) {
        output
            << fill.fill_id << '\t'
            << fill.order_id << '\t'
            << fill.instrument.instrument_id << '\t'
            << fill.instrument.exchange << '\t'
            << fill.instrument.symbol << '\t'
            << fill.instrument.base_asset << '\t'
            << fill.instrument.quote_asset << '\t'
            << fill.instrument.tick_size << '\t'
            << fill.instrument.lot_size << '\t'
            << static_cast<int>(fill.side) << '\t'
            << fill.price << '\t'
            << fill.quantity << '\t'
            << fill.fee << '\n';
    }
}

PostgresPositionRepository::PostgresPositionRepository(std::filesystem::path root_directory)
    : file_path_(std::move(root_directory) / "positions.tsv") {
    load();
}

// Persists one position snapshot.
void PostgresPositionRepository::save_position(const trading::core::Position& position) {
    positions_[position.instrument.instrument_id] = position;
    flush();
}

// Returns one position by instrument id.
std::optional<trading::core::Position> PostgresPositionRepository::get_position(const std::string& instrument_id) const {
    if (const auto iterator = positions_.find(instrument_id); iterator != positions_.end()) {
        return iterator->second;
    }

    return std::nullopt;
}

// Loads position records from file.
void PostgresPositionRepository::load() {
    positions_.clear();
    if (!std::filesystem::exists(file_path_)) {
        return;
    }

    std::ifstream input(file_path_);
    std::string line;
    while (std::getline(input, line)) {
        const auto fields = split_tsv(line);
        if (fields.size() != 12) {
            continue;
        }

        trading::core::Position position {
            .position_id = fields[0],
            .instrument = {
                .instrument_id = fields[1],
                .exchange = fields[2],
                .symbol = fields[3],
                .base_asset = fields[4],
                .quote_asset = fields[5],
                .tick_size = std::stod(fields[6]),
                .lot_size = std::stod(fields[7]),
            },
            .net_quantity = std::stod(fields[8]),
            .average_entry_price = std::stod(fields[9]),
            .realized_pnl = std::stod(fields[10]),
            .unrealized_pnl = std::stod(fields[11]),
        };
        positions_[position.instrument.instrument_id] = position;
    }
}

// Flushes position records into deterministic TSV output.
void PostgresPositionRepository::flush() const {
    ensure_parent_directory(file_path_);

    std::vector<std::string> keys;
    keys.reserve(positions_.size());
    for (const auto& [instrument_id, _] : positions_) {
        keys.push_back(instrument_id);
    }
    std::sort(keys.begin(), keys.end());

    std::ofstream output(file_path_, std::ios::trunc);
    for (const auto& key : keys) {
        const auto& position = positions_.at(key);
        output
            << position.position_id << '\t'
            << position.instrument.instrument_id << '\t'
            << position.instrument.exchange << '\t'
            << position.instrument.symbol << '\t'
            << position.instrument.base_asset << '\t'
            << position.instrument.quote_asset << '\t'
            << position.instrument.tick_size << '\t'
            << position.instrument.lot_size << '\t'
            << position.net_quantity << '\t'
            << position.average_entry_price << '\t'
            << position.realized_pnl << '\t'
            << position.unrealized_pnl << '\n';
    }
}

}  // namespace trading::infrastructure
