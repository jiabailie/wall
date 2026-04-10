#pragma once

#include "storage/storage_interfaces.hpp"

#include <optional>
#include <string>
#include <unordered_map>

namespace trading::infrastructure {

// In-memory adapter that simulates Redis hot-cache behavior for transaction status.
class RedisTransactionCache final : public trading::storage::ITransactionCache {
public:
    void set_status(const std::string& transaction_id, const std::string& status) override;

    // Returns the cached status for test verification.
    [[nodiscard]] std::string get_status(const std::string& transaction_id) const;

private:
    std::unordered_map<std::string, std::string> statuses_;
};

// In-memory adapter that simulates Redis hot-cache behavior for market snapshots.
class RedisMarketStateCache final : public trading::storage::IMarketStateCache {
public:
    void upsert_snapshot(const trading::storage::MarketSnapshot& snapshot) override;
    [[nodiscard]] std::optional<trading::storage::MarketSnapshot> get_snapshot(const std::string& instrument_id) const override;

private:
    std::unordered_map<std::string, trading::storage::MarketSnapshot> snapshots_;
};

}  // namespace trading::infrastructure
