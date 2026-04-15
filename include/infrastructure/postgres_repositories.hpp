#pragma once

#include "config/app_config.hpp"
#include "storage/storage_interfaces.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading::infrastructure {

class LibpqSession;

// File-backed adapter that simulates PostgreSQL persistence for transaction records.
class PostgresTransactionRepository final : public trading::storage::ITransactionRepository {
public:
    explicit PostgresTransactionRepository(std::filesystem::path root_directory);

    void save_received(const trading::core::TransactionCommand& command) override;
    void save_processed(const std::string& transaction_id, const std::string& status) override;
    [[nodiscard]] std::vector<trading::storage::TransactionRecord> all_records() const override;

    // Returns the current persisted record for test verification.
    [[nodiscard]] std::optional<trading::storage::TransactionRecord> get_record(const std::string& transaction_id) const;

private:
    void load();
    void flush() const;

    std::filesystem::path file_path_;
    std::unordered_map<std::string, trading::storage::TransactionRecord> records_;
};

// PostgreSQL-backed repository for transaction checkpoints using libpq.
class LibpqTransactionRepository final : public trading::storage::ITransactionRepository {
public:
    explicit LibpqTransactionRepository(const trading::config::PostgresConfig& config);
    ~LibpqTransactionRepository();

    void save_received(const trading::core::TransactionCommand& command) override;
    void save_processed(const std::string& transaction_id, const std::string& status) override;
    [[nodiscard]] std::vector<trading::storage::TransactionRecord> all_records() const override;
    [[nodiscard]] std::optional<trading::storage::TransactionRecord> get_record(const std::string& transaction_id) const;

private:
    std::shared_ptr<LibpqSession> session_;
};

// File-backed adapter that simulates PostgreSQL persistence for order records.
class PostgresOrderRepository final : public trading::storage::IOrderRepository {
public:
    explicit PostgresOrderRepository(std::filesystem::path root_directory);

    void save_order(const trading::core::OrderRequest& request,
                    const std::string& order_id,
                    const std::string& client_order_id) override;
    void save_order_update(const trading::core::OrderUpdate& update) override;
    [[nodiscard]] std::optional<trading::storage::OrderRecord> get_order(const std::string& order_id) const override;
    [[nodiscard]] std::vector<trading::storage::OrderRecord> all_open_orders() const override;

private:
    void load();
    void flush() const;

    std::filesystem::path file_path_;
    std::unordered_map<std::string, trading::storage::OrderRecord> orders_;
};

// PostgreSQL-backed repository for order records using libpq.
class LibpqOrderRepository final : public trading::storage::IOrderRepository {
public:
    explicit LibpqOrderRepository(const trading::config::PostgresConfig& config);
    ~LibpqOrderRepository();

    void save_order(const trading::core::OrderRequest& request,
                    const std::string& order_id,
                    const std::string& client_order_id) override;
    void save_order_update(const trading::core::OrderUpdate& update) override;
    [[nodiscard]] std::optional<trading::storage::OrderRecord> get_order(const std::string& order_id) const override;
    [[nodiscard]] std::vector<trading::storage::OrderRecord> all_open_orders() const override;

private:
    std::shared_ptr<LibpqSession> session_;
};

// File-backed adapter that simulates PostgreSQL persistence for fill records.
class PostgresFillRepository final : public trading::storage::IFillRepository {
public:
    explicit PostgresFillRepository(std::filesystem::path root_directory);

    void append_fill(const trading::core::FillEvent& fill) override;
    [[nodiscard]] std::vector<trading::core::FillEvent> all_fills() const override;

private:
    void load();
    void flush() const;

    std::filesystem::path file_path_;
    std::vector<trading::core::FillEvent> fills_;
};

// PostgreSQL-backed repository for fill records using libpq.
class LibpqFillRepository final : public trading::storage::IFillRepository {
public:
    explicit LibpqFillRepository(const trading::config::PostgresConfig& config);
    ~LibpqFillRepository();

    void append_fill(const trading::core::FillEvent& fill) override;
    [[nodiscard]] std::vector<trading::core::FillEvent> all_fills() const override;

private:
    std::shared_ptr<LibpqSession> session_;
};

// File-backed adapter that simulates PostgreSQL persistence for position records.
class PostgresPositionRepository final : public trading::storage::IPositionRepository {
public:
    explicit PostgresPositionRepository(std::filesystem::path root_directory);

    void save_position(const trading::core::Position& position) override;
    [[nodiscard]] std::optional<trading::core::Position> get_position(const std::string& instrument_id) const override;
    [[nodiscard]] std::vector<trading::core::Position> all_positions() const override;

private:
    void load();
    void flush() const;

    std::filesystem::path file_path_;
    std::unordered_map<std::string, trading::core::Position> positions_;
};

// PostgreSQL-backed repository for position snapshots using libpq.
class LibpqPositionRepository final : public trading::storage::IPositionRepository {
public:
    explicit LibpqPositionRepository(const trading::config::PostgresConfig& config);
    ~LibpqPositionRepository();

    void save_position(const trading::core::Position& position) override;
    [[nodiscard]] std::optional<trading::core::Position> get_position(const std::string& instrument_id) const override;
    [[nodiscard]] std::vector<trading::core::Position> all_positions() const override;

private:
    std::shared_ptr<LibpqSession> session_;
};

// File-backed adapter that simulates PostgreSQL persistence for balances.
class PostgresBalanceRepository final : public trading::storage::IBalanceRepository {
public:
    explicit PostgresBalanceRepository(std::filesystem::path root_directory);

    void save_balance(const trading::core::BalanceSnapshot& balance) override;
    [[nodiscard]] std::optional<trading::core::BalanceSnapshot> get_balance(const std::string& asset) const override;
    [[nodiscard]] std::vector<trading::core::BalanceSnapshot> all_balances() const override;

private:
    void load();
    void flush() const;

    std::filesystem::path file_path_;
    std::unordered_map<std::string, trading::core::BalanceSnapshot> balances_;
};

// PostgreSQL-backed repository for balances using libpq.
class LibpqBalanceRepository final : public trading::storage::IBalanceRepository {
public:
    explicit LibpqBalanceRepository(const trading::config::PostgresConfig& config);
    ~LibpqBalanceRepository();

    void save_balance(const trading::core::BalanceSnapshot& balance) override;
    [[nodiscard]] std::optional<trading::core::BalanceSnapshot> get_balance(const std::string& asset) const override;
    [[nodiscard]] std::vector<trading::core::BalanceSnapshot> all_balances() const override;

private:
    std::shared_ptr<LibpqSession> session_;
};

}  // namespace trading::infrastructure
