#include "infrastructure/postgres_repositories.hpp"

#include <libpq-fe.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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

std::string encode_optional_double(const std::optional<double>& value) {
    return value.has_value() ? std::to_string(*value) : "";
}

std::optional<double> decode_optional_double(const char* value) {
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }

    return std::stod(value);
}

void ensure_result_ok(PGresult* result, const std::string_view context) {
    if (result == nullptr) {
        throw std::runtime_error("PostgreSQL operation failed during " + std::string(context));
    }

    const auto status = PQresultStatus(result);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        const std::string message = PQresultErrorMessage(result);
        PQclear(result);
        throw std::runtime_error("PostgreSQL operation failed during " + std::string(context) + ": " + message);
    }
}

}  // namespace

class LibpqSession {
public:
    explicit LibpqSession(const trading::config::PostgresConfig& config) {
        const std::string port = std::to_string(config.port);

        const char* keywords[] = {
            "host",
            "port",
            "dbname",
            "user",
            "password",
            nullptr,
        };
        const char* values[] = {
            config.host.c_str(),
            port.c_str(),
            config.database.c_str(),
            config.user.c_str(),
            config.password.c_str(),
            nullptr,
        };

        connection_ = PQconnectdbParams(keywords, values, 0);
        if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK) {
            const std::string message = connection_ == nullptr ? "null connection" : PQerrorMessage(connection_);
            if (connection_ != nullptr) {
                PQfinish(connection_);
                connection_ = nullptr;
            }
            throw std::runtime_error("failed to connect to PostgreSQL: " + message);
        }

        exec_command(
            "CREATE TABLE IF NOT EXISTS transaction_records ("
            "transaction_id TEXT PRIMARY KEY,"
            "status TEXT NOT NULL,"
            "kafka_topic TEXT NOT NULL,"
            "kafka_partition INTEGER NOT NULL,"
            "kafka_offset BIGINT NOT NULL"
            ")");
        exec_command(
            "CREATE TABLE IF NOT EXISTS kafka_checkpoints ("
            "topic TEXT NOT NULL,"
            "partition_id INTEGER NOT NULL,"
            "offset_value BIGINT NOT NULL,"
            "PRIMARY KEY (topic, partition_id)"
            ")");
        exec_command(
            "CREATE TABLE IF NOT EXISTS order_records ("
            "order_id TEXT PRIMARY KEY,"
            "client_order_id TEXT NOT NULL,"
            "strategy_id TEXT NOT NULL,"
            "instrument_id TEXT NOT NULL,"
            "side INTEGER NOT NULL,"
            "order_type INTEGER NOT NULL,"
            "status INTEGER NOT NULL,"
            "quantity DOUBLE PRECISION NOT NULL,"
            "price DOUBLE PRECISION NULL,"
            "filled_quantity DOUBLE PRECISION NOT NULL"
            ")");
        exec_command(
            "CREATE TABLE IF NOT EXISTS fill_records ("
            "sequence_id BIGSERIAL UNIQUE,"
            "fill_id TEXT PRIMARY KEY,"
            "order_id TEXT NOT NULL,"
            "instrument_id TEXT NOT NULL,"
            "exchange TEXT NOT NULL,"
            "symbol TEXT NOT NULL,"
            "base_asset TEXT NOT NULL,"
            "quote_asset TEXT NOT NULL,"
            "tick_size DOUBLE PRECISION NOT NULL,"
            "lot_size DOUBLE PRECISION NOT NULL,"
            "side INTEGER NOT NULL,"
            "price DOUBLE PRECISION NOT NULL,"
            "quantity DOUBLE PRECISION NOT NULL,"
            "fee DOUBLE PRECISION NOT NULL"
            ")");
        exec_command(
            "CREATE TABLE IF NOT EXISTS position_records ("
            "instrument_id TEXT PRIMARY KEY,"
            "position_id TEXT NOT NULL,"
            "exchange TEXT NOT NULL,"
            "symbol TEXT NOT NULL,"
            "base_asset TEXT NOT NULL,"
            "quote_asset TEXT NOT NULL,"
            "tick_size DOUBLE PRECISION NOT NULL,"
            "lot_size DOUBLE PRECISION NOT NULL,"
            "net_quantity DOUBLE PRECISION NOT NULL,"
            "average_entry_price DOUBLE PRECISION NOT NULL,"
            "realized_pnl DOUBLE PRECISION NOT NULL,"
            "unrealized_pnl DOUBLE PRECISION NOT NULL"
            ")");
        exec_command(
            "CREATE TABLE IF NOT EXISTS order_intent_records ("
            "sequence_id BIGSERIAL UNIQUE,"
            "request_id TEXT PRIMARY KEY,"
            "strategy_id TEXT NOT NULL,"
            "instrument_id TEXT NOT NULL,"
            "side INTEGER NOT NULL,"
            "order_type INTEGER NOT NULL,"
            "quantity DOUBLE PRECISION NOT NULL,"
            "price DOUBLE PRECISION NULL"
            ")");
        exec_command(
            "CREATE TABLE IF NOT EXISTS execution_report_records ("
            "sequence_id BIGSERIAL UNIQUE,"
            "report_id TEXT PRIMARY KEY,"
            "order_id TEXT NOT NULL,"
            "client_order_id TEXT NOT NULL,"
            "instrument_id TEXT NOT NULL,"
            "side INTEGER NOT NULL,"
            "status INTEGER NOT NULL,"
            "cumulative_filled_quantity DOUBLE PRECISION NOT NULL,"
            "last_fill_price DOUBLE PRECISION NULL,"
            "last_fill_fee DOUBLE PRECISION NOT NULL,"
            "reason TEXT NULL,"
            "exchange_timestamp BIGINT NOT NULL"
            ")");
    }

    ~LibpqSession() {
        if (connection_ != nullptr) {
            PQfinish(connection_);
        }
    }

    LibpqSession(const LibpqSession&) = delete;
    LibpqSession& operator=(const LibpqSession&) = delete;

    [[nodiscard]] PGresult* exec(const std::string& sql) const {
        auto* result = PQexec(connection_, sql.c_str());
        ensure_result_ok(result, sql);
        return result;
    }

    void exec_command(const std::string& sql) const {
        auto* result = exec(sql);
        PQclear(result);
    }

    [[nodiscard]] PGresult* exec_params(const std::string& sql,
                                        const std::vector<std::string>& values,
                                        const std::vector<int>& formats = {}) const {
        std::vector<const char*> raw_values;
        raw_values.reserve(values.size());
        for (const auto& value : values) {
            raw_values.push_back(value.c_str());
        }

        std::vector<int> actual_formats = formats;
        if (actual_formats.empty()) {
            actual_formats.resize(values.size(), 0);
        }

        auto* result = PQexecParams(
            connection_,
            sql.c_str(),
            static_cast<int>(values.size()),
            nullptr,
            raw_values.data(),
            nullptr,
            actual_formats.data(),
            0);
        ensure_result_ok(result, sql);
        return result;
    }

private:
    PGconn* connection_ {nullptr};
};

PostgresTransactionRepository::PostgresTransactionRepository(std::filesystem::path root_directory)
    : file_path_(std::move(root_directory) / "transactions.tsv"),
      checkpoint_file_path_(file_path_.parent_path() / "kafka_checkpoints.tsv") {
    load();
    load_checkpoints();
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

std::vector<trading::storage::TransactionRecord> PostgresTransactionRepository::all_records() const {
    std::vector<trading::storage::TransactionRecord> records;
    records.reserve(records_.size());
    for (const auto& [_, record] : records_) {
        records.push_back(record);
    }

    return records;
}

void PostgresTransactionRepository::save_checkpoint(const trading::storage::KafkaCheckpoint& checkpoint) {
    checkpoints_[checkpoint.topic + "#" + std::to_string(checkpoint.partition)] = checkpoint;
    flush_checkpoints();
}

std::vector<trading::storage::KafkaCheckpoint> PostgresTransactionRepository::all_checkpoints() const {
    std::vector<trading::storage::KafkaCheckpoint> checkpoints;
    checkpoints.reserve(checkpoints_.size());
    for (const auto& [_, checkpoint] : checkpoints_) {
        checkpoints.push_back(checkpoint);
    }

    return checkpoints;
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

void PostgresTransactionRepository::load_checkpoints() {
    checkpoints_.clear();
    if (!std::filesystem::exists(checkpoint_file_path_)) {
        return;
    }

    std::ifstream input(checkpoint_file_path_);
    std::string line;
    while (std::getline(input, line)) {
        const auto fields = split_tsv(line);
        if (fields.size() != 3) {
            continue;
        }

        trading::storage::KafkaCheckpoint checkpoint {
            .topic = fields[0],
            .partition = std::stoi(fields[1]),
            .offset = std::stoll(fields[2]),
        };
        checkpoints_[checkpoint.topic + "#" + std::to_string(checkpoint.partition)] = checkpoint;
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

void PostgresTransactionRepository::flush_checkpoints() const {
    ensure_parent_directory(checkpoint_file_path_);

    std::vector<std::string> keys;
    keys.reserve(checkpoints_.size());
    for (const auto& [key, _] : checkpoints_) {
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());

    std::ofstream output(checkpoint_file_path_, std::ios::trunc);
    for (const auto& key : keys) {
        const auto& checkpoint = checkpoints_.at(key);
        output
            << checkpoint.topic << '\t'
            << checkpoint.partition << '\t'
            << checkpoint.offset << '\n';
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

std::vector<trading::storage::OrderRecord> PostgresOrderRepository::all_open_orders() const {
    std::vector<trading::storage::OrderRecord> open_orders;
    for (const auto& [_, order] : orders_) {
        switch (order.status) {
            case trading::core::OrderStatus::created:
            case trading::core::OrderStatus::pending_submit:
            case trading::core::OrderStatus::acknowledged:
            case trading::core::OrderStatus::partially_filled:
            case trading::core::OrderStatus::pending_cancel:
                open_orders.push_back(order);
                break;
            default:
                break;
        }
    }

    return open_orders;
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

PostgresOrderIntentRepository::PostgresOrderIntentRepository(std::filesystem::path root_directory)
    : file_path_(std::move(root_directory) / "order_intents.tsv") {
    load();
}

void PostgresOrderIntentRepository::append_intent(const trading::storage::OrderIntentRecord& intent) {
    intents_.push_back(intent);
    flush();
}

std::vector<trading::storage::OrderIntentRecord> PostgresOrderIntentRepository::all_intents() const {
    return intents_;
}

void PostgresOrderIntentRepository::load() {
    intents_.clear();
    if (!std::filesystem::exists(file_path_)) {
        return;
    }

    std::ifstream input(file_path_);
    std::string line;
    while (std::getline(input, line)) {
        const auto fields = split_tsv(line);
        if (fields.size() != 7) {
            continue;
        }

        intents_.push_back({
            .request_id = fields[0],
            .strategy_id = fields[1],
            .instrument_id = fields[2],
            .side = static_cast<trading::core::OrderSide>(std::stoi(fields[3])),
            .order_type = static_cast<trading::core::OrderType>(std::stoi(fields[4])),
            .quantity = std::stod(fields[5]),
            .price = fields[6].empty() ? std::nullopt : std::optional<double>(std::stod(fields[6])),
        });
    }
}

void PostgresOrderIntentRepository::flush() const {
    ensure_parent_directory(file_path_);

    std::ofstream output(file_path_, std::ios::trunc);
    output << std::setprecision(17);
    for (const auto& intent : intents_) {
        output
            << intent.request_id << '\t'
            << intent.strategy_id << '\t'
            << intent.instrument_id << '\t'
            << static_cast<int>(intent.side) << '\t'
            << static_cast<int>(intent.order_type) << '\t'
            << intent.quantity << '\t'
            << encode_optional_double(intent.price) << '\n';
    }
}

PostgresExecutionReportRepository::PostgresExecutionReportRepository(std::filesystem::path root_directory)
    : file_path_(std::move(root_directory) / "execution_reports.tsv") {
    load();
}

void PostgresExecutionReportRepository::append_report(const trading::storage::ExecutionReportRecord& report) {
    reports_.push_back(report);
    flush();
}

std::vector<trading::storage::ExecutionReportRecord> PostgresExecutionReportRepository::all_reports() const {
    return reports_;
}

void PostgresExecutionReportRepository::load() {
    reports_.clear();
    if (!std::filesystem::exists(file_path_)) {
        return;
    }

    std::ifstream input(file_path_);
    std::string line;
    while (std::getline(input, line)) {
        const auto fields = split_tsv(line);
        if (fields.size() != 11) {
            continue;
        }

        reports_.push_back({
            .report_id = fields[0],
            .order_id = fields[1],
            .client_order_id = fields[2],
            .instrument_id = fields[3],
            .side = static_cast<trading::core::OrderSide>(std::stoi(fields[4])),
            .status = static_cast<trading::core::OrderStatus>(std::stoi(fields[5])),
            .cumulative_filled_quantity = std::stod(fields[6]),
            .last_fill_price = fields[7].empty() ? std::nullopt : std::optional<double>(std::stod(fields[7])),
            .last_fill_fee = std::stod(fields[8]),
            .reason = fields[9].empty() ? std::nullopt : std::optional<std::string>(fields[9]),
            .exchange_timestamp = std::stoll(fields[10]),
        });
    }
}

void PostgresExecutionReportRepository::flush() const {
    ensure_parent_directory(file_path_);

    std::ofstream output(file_path_, std::ios::trunc);
    output << std::setprecision(17);
    for (const auto& report : reports_) {
        output
            << report.report_id << '\t'
            << report.order_id << '\t'
            << report.client_order_id << '\t'
            << report.instrument_id << '\t'
            << static_cast<int>(report.side) << '\t'
            << static_cast<int>(report.status) << '\t'
            << report.cumulative_filled_quantity << '\t'
            << encode_optional_double(report.last_fill_price) << '\t'
            << report.last_fill_fee << '\t'
            << (report.reason.has_value() ? *report.reason : "") << '\t'
            << report.exchange_timestamp << '\n';
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

std::vector<trading::core::Position> PostgresPositionRepository::all_positions() const {
    std::vector<trading::core::Position> positions;
    positions.reserve(positions_.size());
    for (const auto& [_, position] : positions_) {
        positions.push_back(position);
    }

    return positions;
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

PostgresBalanceRepository::PostgresBalanceRepository(std::filesystem::path root_directory)
    : file_path_(std::move(root_directory) / "balances.tsv") {
    load();
}

void PostgresBalanceRepository::save_balance(const trading::core::BalanceSnapshot& balance) {
    balances_[balance.asset] = balance;
    flush();
}

std::optional<trading::core::BalanceSnapshot> PostgresBalanceRepository::get_balance(const std::string& asset) const {
    if (const auto iterator = balances_.find(asset); iterator != balances_.end()) {
        return iterator->second;
    }

    return std::nullopt;
}

std::vector<trading::core::BalanceSnapshot> PostgresBalanceRepository::all_balances() const {
    std::vector<trading::core::BalanceSnapshot> balances;
    balances.reserve(balances_.size());
    for (const auto& [_, balance] : balances_) {
        balances.push_back(balance);
    }

    return balances;
}

void PostgresBalanceRepository::load() {
    balances_.clear();
    if (!std::filesystem::exists(file_path_)) {
        return;
    }

    std::ifstream input(file_path_);
    std::string line;
    while (std::getline(input, line)) {
        const auto fields = split_tsv(line);
        if (fields.size() != 3) {
            continue;
        }

        balances_[fields[0]] = {
            .asset = fields[0],
            .total_balance = std::stod(fields[1]),
            .available_balance = std::stod(fields[2]),
        };
    }
}

void PostgresBalanceRepository::flush() const {
    ensure_parent_directory(file_path_);

    std::vector<std::string> keys;
    keys.reserve(balances_.size());
    for (const auto& [asset, _] : balances_) {
        keys.push_back(asset);
    }
    std::sort(keys.begin(), keys.end());

    std::ofstream output(file_path_, std::ios::trunc);
    output << std::setprecision(17);
    for (const auto& key : keys) {
        const auto& balance = balances_.at(key);
        output
            << balance.asset << '\t'
            << balance.total_balance << '\t'
            << balance.available_balance << '\n';
    }
}

LibpqTransactionRepository::LibpqTransactionRepository(const trading::config::PostgresConfig& config)
    : session_(std::make_shared<LibpqSession>(config)) {}

LibpqTransactionRepository::~LibpqTransactionRepository() = default;

void LibpqTransactionRepository::save_received(const trading::core::TransactionCommand& command) {
    auto* result = session_->exec_params(
        "INSERT INTO transaction_records (transaction_id, status, kafka_topic, kafka_partition, kafka_offset) "
        "VALUES ($1, $2, $3, $4, $5) "
        "ON CONFLICT (transaction_id) DO UPDATE SET "
        "status = EXCLUDED.status, "
        "kafka_topic = EXCLUDED.kafka_topic, "
        "kafka_partition = EXCLUDED.kafka_partition, "
        "kafka_offset = EXCLUDED.kafka_offset",
        {
            command.transaction_id,
            "received",
            command.kafka_topic,
            std::to_string(command.kafka_partition),
            std::to_string(command.kafka_offset),
        });
    PQclear(result);
}

void LibpqTransactionRepository::save_processed(const std::string& transaction_id, const std::string& status) {
    auto* result = session_->exec_params(
        "INSERT INTO transaction_records (transaction_id, status, kafka_topic, kafka_partition, kafka_offset) "
        "VALUES ($1, $2, '', 0, 0) "
        "ON CONFLICT (transaction_id) DO UPDATE SET status = EXCLUDED.status",
        {
            transaction_id,
            status,
        });
    PQclear(result);
}

void LibpqTransactionRepository::save_checkpoint(const trading::storage::KafkaCheckpoint& checkpoint) {
    auto* result = session_->exec_params(
        "INSERT INTO kafka_checkpoints (topic, partition_id, offset_value) "
        "VALUES ($1, $2, $3) "
        "ON CONFLICT (topic, partition_id) DO UPDATE SET "
        "offset_value = EXCLUDED.offset_value",
        {
            checkpoint.topic,
            std::to_string(checkpoint.partition),
            std::to_string(checkpoint.offset),
        });
    PQclear(result);
}

std::vector<trading::storage::KafkaCheckpoint> LibpqTransactionRepository::all_checkpoints() const {
    auto* result = session_->exec(
        "SELECT topic, partition_id, offset_value "
        "FROM kafka_checkpoints");

    std::vector<trading::storage::KafkaCheckpoint> checkpoints;
    checkpoints.reserve(static_cast<std::size_t>(PQntuples(result)));
    for (int row = 0; row < PQntuples(result); ++row) {
        checkpoints.push_back({
            .topic = PQgetvalue(result, row, 0),
            .partition = std::stoi(PQgetvalue(result, row, 1)),
            .offset = std::stoll(PQgetvalue(result, row, 2)),
        });
    }
    PQclear(result);
    return checkpoints;
}

std::vector<trading::storage::TransactionRecord> LibpqTransactionRepository::all_records() const {
    auto* result = session_->exec(
        "SELECT transaction_id, status, kafka_topic, kafka_partition, kafka_offset "
        "FROM transaction_records");

    std::vector<trading::storage::TransactionRecord> records;
    records.reserve(static_cast<std::size_t>(PQntuples(result)));
    for (int row = 0; row < PQntuples(result); ++row) {
        records.push_back({
            .transaction_id = PQgetvalue(result, row, 0),
            .status = PQgetvalue(result, row, 1),
            .kafka_topic = PQgetvalue(result, row, 2),
            .kafka_partition = std::stoi(PQgetvalue(result, row, 3)),
            .kafka_offset = std::stoll(PQgetvalue(result, row, 4)),
        });
    }
    PQclear(result);
    return records;
}

std::optional<trading::storage::TransactionRecord> LibpqTransactionRepository::get_record(
    const std::string& transaction_id) const {
    auto* result = session_->exec_params(
        "SELECT transaction_id, status, kafka_topic, kafka_partition, kafka_offset "
        "FROM transaction_records WHERE transaction_id = $1",
        {
            transaction_id,
        });

    if (PQntuples(result) == 0) {
        PQclear(result);
        return std::nullopt;
    }

    trading::storage::TransactionRecord record {
        .transaction_id = PQgetvalue(result, 0, 0),
        .status = PQgetvalue(result, 0, 1),
        .kafka_topic = PQgetvalue(result, 0, 2),
        .kafka_partition = std::stoi(PQgetvalue(result, 0, 3)),
        .kafka_offset = std::stoll(PQgetvalue(result, 0, 4)),
    };
    PQclear(result);
    return record;
}

LibpqOrderRepository::LibpqOrderRepository(const trading::config::PostgresConfig& config)
    : session_(std::make_shared<LibpqSession>(config)) {}

LibpqOrderRepository::~LibpqOrderRepository() = default;

void LibpqOrderRepository::save_order(const trading::core::OrderRequest& request,
                                      const std::string& order_id,
                                      const std::string& client_order_id) {
    auto* result = session_->exec_params(
        "INSERT INTO order_records "
        "(order_id, client_order_id, strategy_id, instrument_id, side, order_type, status, quantity, price, filled_quantity) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, NULLIF($9, '')::DOUBLE PRECISION, $10) "
        "ON CONFLICT (order_id) DO UPDATE SET "
        "client_order_id = EXCLUDED.client_order_id, "
        "strategy_id = EXCLUDED.strategy_id, "
        "instrument_id = EXCLUDED.instrument_id, "
        "side = EXCLUDED.side, "
        "order_type = EXCLUDED.order_type, "
        "status = EXCLUDED.status, "
        "quantity = EXCLUDED.quantity, "
        "price = EXCLUDED.price, "
        "filled_quantity = EXCLUDED.filled_quantity",
        {
            order_id,
            client_order_id,
            request.strategy_id,
            request.instrument.instrument_id,
            std::to_string(static_cast<int>(request.side)),
            std::to_string(static_cast<int>(request.type)),
            std::to_string(static_cast<int>(trading::core::OrderStatus::created)),
            std::to_string(request.quantity),
            encode_optional_double(request.price),
            "0",
        });
    PQclear(result);
}

void LibpqOrderRepository::save_order_update(const trading::core::OrderUpdate& update) {
    auto* result = session_->exec_params(
        "INSERT INTO order_records "
        "(order_id, client_order_id, strategy_id, instrument_id, side, order_type, status, quantity, price, filled_quantity) "
        "VALUES ($1, $2, '', '', 0, 1, $3, 0, NULL, $4) "
        "ON CONFLICT (order_id) DO UPDATE SET "
        "client_order_id = EXCLUDED.client_order_id, "
        "status = EXCLUDED.status, "
        "filled_quantity = EXCLUDED.filled_quantity",
        {
            update.order_id,
            update.client_order_id,
            std::to_string(static_cast<int>(update.status)),
            std::to_string(update.filled_quantity),
        });
    PQclear(result);
}

std::optional<trading::storage::OrderRecord> LibpqOrderRepository::get_order(const std::string& order_id) const {
    auto* result = session_->exec_params(
        "SELECT order_id, client_order_id, strategy_id, instrument_id, side, order_type, status, quantity, price, filled_quantity "
        "FROM order_records WHERE order_id = $1",
        {
            order_id,
        });

    if (PQntuples(result) == 0) {
        PQclear(result);
        return std::nullopt;
    }

    trading::storage::OrderRecord record {
        .order_id = PQgetvalue(result, 0, 0),
        .client_order_id = PQgetvalue(result, 0, 1),
        .strategy_id = PQgetvalue(result, 0, 2),
        .instrument_id = PQgetvalue(result, 0, 3),
        .side = static_cast<trading::core::OrderSide>(std::stoi(PQgetvalue(result, 0, 4))),
        .order_type = static_cast<trading::core::OrderType>(std::stoi(PQgetvalue(result, 0, 5))),
        .status = static_cast<trading::core::OrderStatus>(std::stoi(PQgetvalue(result, 0, 6))),
        .quantity = std::stod(PQgetvalue(result, 0, 7)),
        .price = decode_optional_double(PQgetvalue(result, 0, 8)),
        .filled_quantity = std::stod(PQgetvalue(result, 0, 9)),
    };
    PQclear(result);
    return record;
}

std::vector<trading::storage::OrderRecord> LibpqOrderRepository::all_open_orders() const {
    auto* result = session_->exec(
        "SELECT order_id, client_order_id, strategy_id, instrument_id, side, order_type, status, quantity, price, filled_quantity "
        "FROM order_records WHERE status IN (0, 1, 2, 3, 5)");

    std::vector<trading::storage::OrderRecord> orders;
    orders.reserve(static_cast<std::size_t>(PQntuples(result)));
    for (int row = 0; row < PQntuples(result); ++row) {
        orders.push_back({
            .order_id = PQgetvalue(result, row, 0),
            .client_order_id = PQgetvalue(result, row, 1),
            .strategy_id = PQgetvalue(result, row, 2),
            .instrument_id = PQgetvalue(result, row, 3),
            .side = static_cast<trading::core::OrderSide>(std::stoi(PQgetvalue(result, row, 4))),
            .order_type = static_cast<trading::core::OrderType>(std::stoi(PQgetvalue(result, row, 5))),
            .status = static_cast<trading::core::OrderStatus>(std::stoi(PQgetvalue(result, row, 6))),
            .quantity = std::stod(PQgetvalue(result, row, 7)),
            .price = decode_optional_double(PQgetvalue(result, row, 8)),
            .filled_quantity = std::stod(PQgetvalue(result, row, 9)),
        });
    }
    PQclear(result);
    return orders;
}

LibpqFillRepository::LibpqFillRepository(const trading::config::PostgresConfig& config)
    : session_(std::make_shared<LibpqSession>(config)) {}

LibpqFillRepository::~LibpqFillRepository() = default;

void LibpqFillRepository::append_fill(const trading::core::FillEvent& fill) {
    auto* result = session_->exec_params(
        "INSERT INTO fill_records "
        "(fill_id, order_id, instrument_id, exchange, symbol, base_asset, quote_asset, tick_size, lot_size, side, price, quantity, fee) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13) "
        "ON CONFLICT (fill_id) DO NOTHING",
        {
            fill.fill_id,
            fill.order_id,
            fill.instrument.instrument_id,
            fill.instrument.exchange,
            fill.instrument.symbol,
            fill.instrument.base_asset,
            fill.instrument.quote_asset,
            std::to_string(fill.instrument.tick_size),
            std::to_string(fill.instrument.lot_size),
            std::to_string(static_cast<int>(fill.side)),
            std::to_string(fill.price),
            std::to_string(fill.quantity),
            std::to_string(fill.fee),
        });
    PQclear(result);
}

std::vector<trading::core::FillEvent> LibpqFillRepository::all_fills() const {
    auto* result = session_->exec(
        "SELECT fill_id, order_id, instrument_id, exchange, symbol, base_asset, quote_asset, tick_size, lot_size, side, price, quantity, fee "
        "FROM fill_records ORDER BY sequence_id ASC");

    std::vector<trading::core::FillEvent> fills;
    fills.reserve(static_cast<std::size_t>(PQntuples(result)));
    for (int row = 0; row < PQntuples(result); ++row) {
        fills.push_back({
            .fill_id = PQgetvalue(result, row, 0),
            .order_id = PQgetvalue(result, row, 1),
            .instrument = {
                .instrument_id = PQgetvalue(result, row, 2),
                .exchange = PQgetvalue(result, row, 3),
                .symbol = PQgetvalue(result, row, 4),
                .base_asset = PQgetvalue(result, row, 5),
                .quote_asset = PQgetvalue(result, row, 6),
                .tick_size = std::stod(PQgetvalue(result, row, 7)),
                .lot_size = std::stod(PQgetvalue(result, row, 8)),
            },
            .side = static_cast<trading::core::OrderSide>(std::stoi(PQgetvalue(result, row, 9))),
            .price = std::stod(PQgetvalue(result, row, 10)),
            .quantity = std::stod(PQgetvalue(result, row, 11)),
            .fee = std::stod(PQgetvalue(result, row, 12)),
        });
    }

    PQclear(result);
    return fills;
}

LibpqOrderIntentRepository::LibpqOrderIntentRepository(const trading::config::PostgresConfig& config)
    : session_(std::make_shared<LibpqSession>(config)) {}

LibpqOrderIntentRepository::~LibpqOrderIntentRepository() = default;

void LibpqOrderIntentRepository::append_intent(const trading::storage::OrderIntentRecord& intent) {
    auto* result = session_->exec_params(
        "INSERT INTO order_intent_records "
        "(request_id, strategy_id, instrument_id, side, order_type, quantity, price) "
        "VALUES ($1, $2, $3, $4, $5, $6, NULLIF($7, '')::DOUBLE PRECISION) "
        "ON CONFLICT (request_id) DO UPDATE SET "
        "strategy_id = EXCLUDED.strategy_id, "
        "instrument_id = EXCLUDED.instrument_id, "
        "side = EXCLUDED.side, "
        "order_type = EXCLUDED.order_type, "
        "quantity = EXCLUDED.quantity, "
        "price = EXCLUDED.price",
        {
            intent.request_id,
            intent.strategy_id,
            intent.instrument_id,
            std::to_string(static_cast<int>(intent.side)),
            std::to_string(static_cast<int>(intent.order_type)),
            std::to_string(intent.quantity),
            encode_optional_double(intent.price),
        });
    PQclear(result);
}

std::vector<trading::storage::OrderIntentRecord> LibpqOrderIntentRepository::all_intents() const {
    auto* result = session_->exec(
        "SELECT request_id, strategy_id, instrument_id, side, order_type, quantity, price "
        "FROM order_intent_records ORDER BY sequence_id ASC");

    std::vector<trading::storage::OrderIntentRecord> intents;
    intents.reserve(static_cast<std::size_t>(PQntuples(result)));
    for (int row = 0; row < PQntuples(result); ++row) {
        intents.push_back({
            .request_id = PQgetvalue(result, row, 0),
            .strategy_id = PQgetvalue(result, row, 1),
            .instrument_id = PQgetvalue(result, row, 2),
            .side = static_cast<trading::core::OrderSide>(std::stoi(PQgetvalue(result, row, 3))),
            .order_type = static_cast<trading::core::OrderType>(std::stoi(PQgetvalue(result, row, 4))),
            .quantity = std::stod(PQgetvalue(result, row, 5)),
            .price = decode_optional_double(PQgetvalue(result, row, 6)),
        });
    }
    PQclear(result);
    return intents;
}

LibpqExecutionReportRepository::LibpqExecutionReportRepository(const trading::config::PostgresConfig& config)
    : session_(std::make_shared<LibpqSession>(config)) {}

LibpqExecutionReportRepository::~LibpqExecutionReportRepository() = default;

void LibpqExecutionReportRepository::append_report(const trading::storage::ExecutionReportRecord& report) {
    auto* result = session_->exec_params(
        "INSERT INTO execution_report_records "
        "(report_id, order_id, client_order_id, instrument_id, side, status, cumulative_filled_quantity, "
        "last_fill_price, last_fill_fee, reason, exchange_timestamp) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, NULLIF($8, '')::DOUBLE PRECISION, $9, $10, $11) "
        "ON CONFLICT (report_id) DO UPDATE SET "
        "order_id = EXCLUDED.order_id, "
        "client_order_id = EXCLUDED.client_order_id, "
        "instrument_id = EXCLUDED.instrument_id, "
        "side = EXCLUDED.side, "
        "status = EXCLUDED.status, "
        "cumulative_filled_quantity = EXCLUDED.cumulative_filled_quantity, "
        "last_fill_price = EXCLUDED.last_fill_price, "
        "last_fill_fee = EXCLUDED.last_fill_fee, "
        "reason = EXCLUDED.reason, "
        "exchange_timestamp = EXCLUDED.exchange_timestamp",
        {
            report.report_id,
            report.order_id,
            report.client_order_id,
            report.instrument_id,
            std::to_string(static_cast<int>(report.side)),
            std::to_string(static_cast<int>(report.status)),
            std::to_string(report.cumulative_filled_quantity),
            encode_optional_double(report.last_fill_price),
            std::to_string(report.last_fill_fee),
            report.reason.value_or(""),
            std::to_string(report.exchange_timestamp),
        });
    PQclear(result);
}

std::vector<trading::storage::ExecutionReportRecord> LibpqExecutionReportRepository::all_reports() const {
    auto* result = session_->exec(
        "SELECT report_id, order_id, client_order_id, instrument_id, side, status, cumulative_filled_quantity, "
        "last_fill_price, last_fill_fee, reason, exchange_timestamp "
        "FROM execution_report_records ORDER BY sequence_id ASC");

    std::vector<trading::storage::ExecutionReportRecord> reports;
    reports.reserve(static_cast<std::size_t>(PQntuples(result)));
    for (int row = 0; row < PQntuples(result); ++row) {
        const char* reason = PQgetisnull(result, row, 9) ? nullptr : PQgetvalue(result, row, 9);
        reports.push_back({
            .report_id = PQgetvalue(result, row, 0),
            .order_id = PQgetvalue(result, row, 1),
            .client_order_id = PQgetvalue(result, row, 2),
            .instrument_id = PQgetvalue(result, row, 3),
            .side = static_cast<trading::core::OrderSide>(std::stoi(PQgetvalue(result, row, 4))),
            .status = static_cast<trading::core::OrderStatus>(std::stoi(PQgetvalue(result, row, 5))),
            .cumulative_filled_quantity = std::stod(PQgetvalue(result, row, 6)),
            .last_fill_price = decode_optional_double(PQgetvalue(result, row, 7)),
            .last_fill_fee = std::stod(PQgetvalue(result, row, 8)),
            .reason = reason == nullptr ? std::nullopt : std::optional<std::string>(reason),
            .exchange_timestamp = std::stoll(PQgetvalue(result, row, 10)),
        });
    }
    PQclear(result);
    return reports;
}

LibpqPositionRepository::LibpqPositionRepository(const trading::config::PostgresConfig& config)
    : session_(std::make_shared<LibpqSession>(config)) {}

LibpqPositionRepository::~LibpqPositionRepository() = default;

void LibpqPositionRepository::save_position(const trading::core::Position& position) {
    auto* result = session_->exec_params(
        "INSERT INTO position_records "
        "(instrument_id, position_id, exchange, symbol, base_asset, quote_asset, tick_size, lot_size, net_quantity, average_entry_price, realized_pnl, unrealized_pnl) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12) "
        "ON CONFLICT (instrument_id) DO UPDATE SET "
        "position_id = EXCLUDED.position_id, "
        "exchange = EXCLUDED.exchange, "
        "symbol = EXCLUDED.symbol, "
        "base_asset = EXCLUDED.base_asset, "
        "quote_asset = EXCLUDED.quote_asset, "
        "tick_size = EXCLUDED.tick_size, "
        "lot_size = EXCLUDED.lot_size, "
        "net_quantity = EXCLUDED.net_quantity, "
        "average_entry_price = EXCLUDED.average_entry_price, "
        "realized_pnl = EXCLUDED.realized_pnl, "
        "unrealized_pnl = EXCLUDED.unrealized_pnl",
        {
            position.instrument.instrument_id,
            position.position_id,
            position.instrument.exchange,
            position.instrument.symbol,
            position.instrument.base_asset,
            position.instrument.quote_asset,
            std::to_string(position.instrument.tick_size),
            std::to_string(position.instrument.lot_size),
            std::to_string(position.net_quantity),
            std::to_string(position.average_entry_price),
            std::to_string(position.realized_pnl),
            std::to_string(position.unrealized_pnl),
        });
    PQclear(result);
}

std::optional<trading::core::Position> LibpqPositionRepository::get_position(const std::string& instrument_id) const {
    auto* result = session_->exec_params(
        "SELECT position_id, instrument_id, exchange, symbol, base_asset, quote_asset, tick_size, lot_size, net_quantity, average_entry_price, realized_pnl, unrealized_pnl "
        "FROM position_records WHERE instrument_id = $1",
        {
            instrument_id,
        });

    if (PQntuples(result) == 0) {
        PQclear(result);
        return std::nullopt;
    }

    trading::core::Position position {
        .position_id = PQgetvalue(result, 0, 0),
        .instrument = {
            .instrument_id = PQgetvalue(result, 0, 1),
            .exchange = PQgetvalue(result, 0, 2),
            .symbol = PQgetvalue(result, 0, 3),
            .base_asset = PQgetvalue(result, 0, 4),
            .quote_asset = PQgetvalue(result, 0, 5),
            .tick_size = std::stod(PQgetvalue(result, 0, 6)),
            .lot_size = std::stod(PQgetvalue(result, 0, 7)),
        },
        .net_quantity = std::stod(PQgetvalue(result, 0, 8)),
        .average_entry_price = std::stod(PQgetvalue(result, 0, 9)),
        .realized_pnl = std::stod(PQgetvalue(result, 0, 10)),
        .unrealized_pnl = std::stod(PQgetvalue(result, 0, 11)),
    };
    PQclear(result);
    return position;
}

std::vector<trading::core::Position> LibpqPositionRepository::all_positions() const {
    auto* result = session_->exec(
        "SELECT position_id, instrument_id, exchange, symbol, base_asset, quote_asset, tick_size, lot_size, net_quantity, average_entry_price, realized_pnl, unrealized_pnl "
        "FROM position_records");

    std::vector<trading::core::Position> positions;
    positions.reserve(static_cast<std::size_t>(PQntuples(result)));
    for (int row = 0; row < PQntuples(result); ++row) {
        positions.push_back({
            .position_id = PQgetvalue(result, row, 0),
            .instrument = {
                .instrument_id = PQgetvalue(result, row, 1),
                .exchange = PQgetvalue(result, row, 2),
                .symbol = PQgetvalue(result, row, 3),
                .base_asset = PQgetvalue(result, row, 4),
                .quote_asset = PQgetvalue(result, row, 5),
                .tick_size = std::stod(PQgetvalue(result, row, 6)),
                .lot_size = std::stod(PQgetvalue(result, row, 7)),
            },
            .net_quantity = std::stod(PQgetvalue(result, row, 8)),
            .average_entry_price = std::stod(PQgetvalue(result, row, 9)),
            .realized_pnl = std::stod(PQgetvalue(result, row, 10)),
            .unrealized_pnl = std::stod(PQgetvalue(result, row, 11)),
        });
    }
    PQclear(result);
    return positions;
}

LibpqBalanceRepository::LibpqBalanceRepository(const trading::config::PostgresConfig& config)
    : session_(std::make_shared<LibpqSession>(config)) {
    auto* result = session_->exec(
        "CREATE TABLE IF NOT EXISTS balance_records ("
        "asset TEXT PRIMARY KEY,"
        "total_balance DOUBLE PRECISION NOT NULL,"
        "available_balance DOUBLE PRECISION NOT NULL"
        ")");
    PQclear(result);
}

LibpqBalanceRepository::~LibpqBalanceRepository() = default;

void LibpqBalanceRepository::save_balance(const trading::core::BalanceSnapshot& balance) {
    auto* result = session_->exec_params(
        "INSERT INTO balance_records (asset, total_balance, available_balance) "
        "VALUES ($1, $2, $3) "
        "ON CONFLICT (asset) DO UPDATE SET "
        "total_balance = EXCLUDED.total_balance, "
        "available_balance = EXCLUDED.available_balance",
        {
            balance.asset,
            std::to_string(balance.total_balance),
            std::to_string(balance.available_balance),
        });
    PQclear(result);
}

std::optional<trading::core::BalanceSnapshot> LibpqBalanceRepository::get_balance(const std::string& asset) const {
    auto* result = session_->exec_params(
        "SELECT asset, total_balance, available_balance FROM balance_records WHERE asset = $1",
        {
            asset,
        });
    if (PQntuples(result) == 0) {
        PQclear(result);
        return std::nullopt;
    }

    trading::core::BalanceSnapshot balance {
        .asset = PQgetvalue(result, 0, 0),
        .total_balance = std::stod(PQgetvalue(result, 0, 1)),
        .available_balance = std::stod(PQgetvalue(result, 0, 2)),
    };
    PQclear(result);
    return balance;
}

std::vector<trading::core::BalanceSnapshot> LibpqBalanceRepository::all_balances() const {
    auto* result = session_->exec(
        "SELECT asset, total_balance, available_balance FROM balance_records");

    std::vector<trading::core::BalanceSnapshot> balances;
    balances.reserve(static_cast<std::size_t>(PQntuples(result)));
    for (int row = 0; row < PQntuples(result); ++row) {
        balances.push_back({
            .asset = PQgetvalue(result, row, 0),
            .total_balance = std::stod(PQgetvalue(result, row, 1)),
            .available_balance = std::stod(PQgetvalue(result, row, 2)),
        });
    }
    PQclear(result);
    return balances;
}

}  // namespace trading::infrastructure
