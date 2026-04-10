#include "infrastructure/redis_cache.hpp"

namespace trading::infrastructure {

// Stores or replaces one transaction status in the cache.
void RedisTransactionCache::set_status(const std::string& transaction_id, const std::string& status) {
    statuses_[transaction_id] = status;
}

// Returns the cached status for the provided transaction id.
std::string RedisTransactionCache::get_status(const std::string& transaction_id) const {
    if (const auto iterator = statuses_.find(transaction_id); iterator != statuses_.end()) {
        return iterator->second;
    }

    return "";
}

// Stores or replaces one market snapshot in the cache.
void RedisMarketStateCache::upsert_snapshot(const trading::storage::MarketSnapshot& snapshot) {
    snapshots_[snapshot.instrument_id] = snapshot;
}

// Returns the cached market snapshot for the provided instrument id.
std::optional<trading::storage::MarketSnapshot> RedisMarketStateCache::get_snapshot(const std::string& instrument_id) const {
    if (const auto iterator = snapshots_.find(instrument_id); iterator != snapshots_.end()) {
        return iterator->second;
    }

    return std::nullopt;
}

}  // namespace trading::infrastructure
