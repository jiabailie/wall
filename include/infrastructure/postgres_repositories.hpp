#pragma once

#include "storage/storage_interfaces.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace trading::infrastructure {

// File-backed adapter that simulates PostgreSQL persistence for transaction records.
class PostgresTransactionRepository final : public trading::storage::ITransactionRepository {
public:
    explicit PostgresTransactionRepository(std::filesystem::path root_directory);

    void save_received(const trading::core::TransactionCommand& command) override;
    void save_processed(const std::string& transaction_id, const std::string& status) override;

    // Returns the current persisted record for test verification.
    [[nodiscard]] std::optional<trading::storage::TransactionRecord> get_record(const std::string& transaction_id) const;

private:
    void load();
    void flush() const;

    std::filesystem::path file_path_;
    std::unordered_map<std::string, trading::storage::TransactionRecord> records_;
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

private:
    void load();
    void flush() const;

    std::filesystem::path file_path_;
    std::unordered_map<std::string, trading::storage::OrderRecord> orders_;
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

// File-backed adapter that simulates PostgreSQL persistence for position records.
class PostgresPositionRepository final : public trading::storage::IPositionRepository {
public:
    explicit PostgresPositionRepository(std::filesystem::path root_directory);

    void save_position(const trading::core::Position& position) override;
    [[nodiscard]] std::optional<trading::core::Position> get_position(const std::string& instrument_id) const override;

private:
    void load();
    void flush() const;

    std::filesystem::path file_path_;
    std::unordered_map<std::string, trading::core::Position> positions_;
};

}  // namespace trading::infrastructure
